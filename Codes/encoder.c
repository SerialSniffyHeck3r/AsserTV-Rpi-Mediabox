#include "encoder.h"
#include <pigpio.h>
#include <string.h>

// 버튼 롱프레스 / 디바운스 설정
#define ENC_BTN_LONG_PRESS_MS   700
#define ENC_BTN_LONG_PRESS_US   (ENC_BTN_LONG_PRESS_MS * 1000)
#define ENC_BTN_DEBOUNCE_US     5000

typedef struct {
    int pin_a;
    int pin_b;
    int pin_btn;

    volatile int delta;          // 바깥에서 읽는 step 수 (한 디텐트당 ±1)
    volatile int clicks;         // 짧은 클릭 횟수
    volatile int long_presses;   // 롱프레스 횟수

    // 쿼드러처 상태
    uint8_t last_ab;             // 이전 AB 상태 (0~3)
    int accum;                   // 내부 누적 (edge 단위, ±4에서 1step 생성)

    // 버튼 상태
    uint32_t btn_down_tick;
    int btn_down;
    int btn_long_sent;           // watchdog 타임아웃으로 롱프레스 이미 발행했는지
} encoder_t;

static encoder_t g_enc[ENC_MAX];

// 쿼드러처 테이블 (old_AB << 2 | new_AB 로 index)
// 값: +1 (정방향), -1 (역방향), 0 (무시/불법 전이)
static const int8_t quad_table[16] = {
    0,   // 0000: 00 -> 00
    +1,  // 0001: 00 -> 01
    -1,  // 0010: 00 -> 10
    0,   // 0011: 00 -> 11 (불법)
    -1,  // 0100: 01 -> 00
    0,   // 0101: 01 -> 01
    0,   // 0110: 01 -> 10 (불법)
    +1,  // 0111: 01 -> 11
    +1,  // 1000: 10 -> 00
    0,   // 1001: 10 -> 01 (불법)
    0,   // 1010: 10 -> 10
    -1,  // 1011: 10 -> 11
    0,   // 1100: 11 -> 00 (불법)
    -1,  // 1101: 11 -> 01
    +1,  // 1110: 11 -> 10
    0    // 1111: 11 -> 11
};

// 어떤 GPIO 이벤트가 왔을 때, 여기에 매칭되는 인코더 찾기
static encoder_t* find_encoder_by_pin(int pin)
{
    for (int i = 0; i < ENC_MAX; i++) {
        if (g_enc[i].pin_a == pin ||
            g_enc[i].pin_b == pin ||
            g_enc[i].pin_btn == pin) {
            return &g_enc[i];
        }
    }
    return NULL;
}

// 회전용 콜백: A/B 둘 다 이걸로 들어옴
static void encoder_rotary_alert(int gpio, int level, uint32_t tick)
{
    encoder_t *e = find_encoder_by_pin(gpio);
    if (!e) return;

    if (level == PI_TIMEOUT) return;  // 타임아웃 이벤트 무시

    // A/B 둘 중 하나가 변할 때마다 현재 AB 읽음
    int a = gpioRead(e->pin_a);
    int b = gpioRead(e->pin_b);
    uint8_t ab = (uint8_t)((a << 1) | b) & 0x03;

    uint8_t index = (e->last_ab << 2) | ab;
    int8_t step = quad_table[index];

    e->last_ab = ab;

    if (step != 0) {
        // edge 단위 step 누적 (한 디텐트에 4 step = ±4)
        e->accum += step;

        if (e->accum >= 4) {
            e->delta++;      // 한 클릭 정방향
            e->accum = 0;
        } else if (e->accum <= -4) {
            e->delta--;      // 한 클릭 역방향
            e->accum = 0;
        }
    }
}

// 버튼용 콜백
static void encoder_button_alert(int gpio, int level, uint32_t tick)
{
    encoder_t *e = find_encoder_by_pin(gpio);
    if (!e) return;

    if (gpio != e->pin_btn) return;

    // watchdog 타임아웃: 손을 떼지 않아도 롱프레스 이벤트 1회 발생
    if (level == PI_TIMEOUT) {
        if (e->btn_down && !e->btn_long_sent) {
            e->long_presses++;
            e->btn_long_sent = 1;
        }
        return;
    }

    // 풀업 가정: 평소 HIGH, 누르면 LOW
    if (level == 0) {
        // 버튼 눌림 시작
        e->btn_down = 1;
        e->btn_down_tick = tick;
        e->btn_long_sent = 0;

        // ENC_BTN_LONG_PRESS_MS 이후에 타임아웃 이벤트 발생하도록 설정
        gpioSetWatchdog(gpio, ENC_BTN_LONG_PRESS_MS);
    } else if (level == 1) {
        // 버튼에서 손 뗀 시점
        gpioSetWatchdog(gpio, 0);   // watchdog 해제

        if (e->btn_down) {
            uint32_t dt = tick - e->btn_down_tick; // us 단위
            e->btn_down = 0;

            if (e->btn_long_sent) {
                // 이미 타임아웃에서 롱프레스를 보냈다면 추가 이벤트는 생성 안 함
                return;
            }

            // 아직 롱프레스 안 보냈으면 시간 기준으로 클릭/롱프레스 분류
            if (dt >= ENC_BTN_LONG_PRESS_US) {
                e->long_presses++;
            } else {
                e->clicks++;
            }
        }
    }
}

int encoder_init(encoder_id_t id, int pin_a, int pin_b, int pin_btn)
{
    if (id < 0 || id >= ENC_MAX) return -1;

    encoder_t *e = &g_enc[id];
    memset(e, 0, sizeof(*e));

    e->pin_a = pin_a;
    e->pin_b = pin_b;
    e->pin_btn = pin_btn;
    e->delta = 0;
    e->clicks = 0;
    e->long_presses = 0;
    e->accum = 0;
    e->btn_down = 0;
    e->btn_down_tick = 0;
    e->btn_long_sent = 0;

    // pigpio는 이미 gpioInitialise() 된 상태라고 가정 (lcd_init() 등에서)
    gpioSetMode(pin_a, PI_INPUT);
    gpioSetMode(pin_b, PI_INPUT);
    gpioSetPullUpDown(pin_a, PI_PUD_UP);
    gpioSetPullUpDown(pin_b, PI_PUD_UP);

    // A/B 모두 글리치 필터 (1ms)
    gpioGlitchFilter(pin_a, 1000);
    gpioGlitchFilter(pin_b, 1000);

    // 현재 AB 상태를 읽어서 last_ab 초기화
    int a = gpioRead(pin_a);
    int b = gpioRead(pin_b);
    e->last_ab = (uint8_t)((a << 1) | b) & 0x03;

    // A/B 모두 회전 콜백 연결
    gpioSetAlertFunc(pin_a, encoder_rotary_alert);
    gpioSetAlertFunc(pin_b, encoder_rotary_alert);

    // 버튼이 있으면 버튼 콜백 세팅
    if (pin_btn >= 0) {
        gpioSetMode(pin_btn, PI_INPUT);
        gpioSetPullUpDown(pin_btn, PI_PUD_UP);
        gpioGlitchFilter(pin_btn, ENC_BTN_DEBOUNCE_US);  // 5ms
        gpioSetWatchdog(pin_btn, 0);
        gpioSetAlertFunc(pin_btn, encoder_button_alert);
    }

    return 0;
}

int encoder_get_delta(encoder_id_t id)
{
    if (id < 0 || id >= ENC_MAX) return 0;
    encoder_t *e = &g_enc[id];
    int d = e->delta;
    e->delta = 0;
    return d;
}

int encoder_get_clicks(encoder_id_t id)
{
    if (id < 0 || id >= ENC_MAX) return 0;
    encoder_t *e = &g_enc[id];
    int c = e->clicks;
    e->clicks = 0;
    return c;
}

int encoder_get_long_presses(encoder_id_t id)
{
    if (id < 0 || id >= ENC_MAX) return 0;
    encoder_t *e = &g_enc[id];
    int c = e->long_presses;
    e->long_presses = 0;
    return c;
}

void encoder_shutdown(void)
{
    for (int i = 0; i < ENC_MAX; i++) {
        encoder_t *e = &g_enc[i];
        if (e->pin_a > 0) {
            gpioSetAlertFunc(e->pin_a, NULL);
        }
        if (e->pin_b > 0) {
            gpioSetAlertFunc(e->pin_b, NULL);
        }
        if (e->pin_btn > 0) {
            gpioSetAlertFunc(e->pin_btn, NULL);
            gpioSetWatchdog(e->pin_btn, 0);
        }
    }
}
