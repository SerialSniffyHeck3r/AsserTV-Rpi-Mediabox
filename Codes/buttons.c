#include "buttons.h"
#include <pigpio.h>
#include <string.h>

#define PIN_BTN_REW   2   // GPIO2  (핀 3)
#define PIN_BTN_PLAY  3   // GPIO3  (핀 5)
#define PIN_BTN_FF    4   // GPIO4  (핀 7)

// 롱프레스 기준 및 디바운스
#define BTN_LONG_PRESS_MS   700           // 0.7s 이상 누르면 롱프레스
#define BTN_LONG_PRESS_US   (BTN_LONG_PRESS_MS * 1000)
#define BTN_DEBOUNCE_US     5000          // 5ms glitch filter

typedef struct {
    int pin;

    // 콜백 스레드에서 증가, 메인 루프에서 읽고 0으로 리셋
    volatile int clicks;         // 짧은 클릭 수
    volatile int long_presses;   // 롱프레스 수

    // 콜백 스레드 내부에서만 사용하는 상태
    uint32_t down_tick;          // 눌리기 시작한 시점 (us, gpioTick 기반)
    int      down;               // 내부 상태: 1이면 현재 "눌린 상태"로 간주
    int      long_sent;          // watchdog(PI_TIMEOUT)으로 롱프레스 이미 보냈는지
} button_t;

static button_t g_btn[BTN_MAX];

static button_t* find_button_by_pin(int pin)
{
    for (int i = 0; i < BTN_MAX; i++) {
        if (g_btn[i].pin == pin)
            return &g_btn[i];
    }
    return NULL;
}

// pigpio alert 콜백 (엣지/타임아웃 공통 진입점)
static void button_alert(int gpio, int level, uint32_t tick)
{
    button_t *b = find_button_by_pin(gpio);
    if (!b) return;

    // 1) watchdog 타임아웃 (PI_TIMEOUT == 2)
    if (level == PI_TIMEOUT) {
        if (b->down && !b->long_sent) {
            // 일정 시간 이상 계속 눌려 있으면, 손을 떼지 않아도
            // 롱프레스 이벤트 1회를 바로 발행
            b->long_presses++;
            b->long_sent = 1;
        }
        return;
    }

    // 2) 풀업 가정: 평소 HIGH(1), 누르면 LOW(0)
    if (level == 0) {
        // 버튼 눌림 시작
        b->down      = 1;
        b->down_tick = tick;
        b->long_sent = 0;

        // LONG_PRESS_MS 이후에 타임아웃 이벤트 발생하도록 설정
        gpioSetWatchdog(gpio, BTN_LONG_PRESS_MS);
    }
    else if (level == 1) {
        // 버튼에서 손을 뗀 시점
        gpioSetWatchdog(gpio, 0);   // watchdog 해제

        if (b->down) {
            uint32_t dt = tick - b->down_tick;  // us 단위
            b->down = 0;

            if (b->long_sent) {
                // 이미 PI_TIMEOUT에서 롱프레스 이벤트를 한 번 보냈다면
                // 여기서는 추가 이벤트를 만들지 않는다.
                return;
            }

            // 아직 롱프레스 안 보냈으면, 눌린 시간으로 클릭/롱 구분
            if (dt >= BTN_LONG_PRESS_US) {
                b->long_presses++;
            } else {
                b->clicks++;
            }
        }
    }
}

int buttons_init(void)
{
    memset(g_btn, 0, sizeof(g_btn));

    g_btn[BTN_REW].pin       = PIN_BTN_REW;
    g_btn[BTN_PLAYPAUSE].pin = PIN_BTN_PLAY;
    g_btn[BTN_FF].pin        = PIN_BTN_FF;

    for (int i = 0; i < BTN_MAX; i++) {
        int pin = g_btn[i].pin;

        gpioSetMode(pin, PI_INPUT);
        gpioSetPullUpDown(pin, PI_PUD_UP);

        // 기본 디바운스 (핀 상태가 5ms 이상 변함없을 때만 유효 엣지로 인정)
        gpioGlitchFilter(pin, BTN_DEBOUNCE_US);

        // 처음에는 watchdog 끔
        gpioSetWatchdog(pin, 0);

        // alert 콜백 등록
        gpioSetAlertFunc(pin, button_alert);
    }

    return 0;
}

int button_get_clicks(button_id_t id)
{
    if (id < 0 || id >= BTN_MAX) return 0;
    button_t *b = &g_btn[id];
    int c = b->clicks;
    b->clicks = 0;
    return c;
}

int button_get_long_presses(button_id_t id)
{
    if (id < 0 || id >= BTN_MAX) return 0;
    button_t *b = &g_btn[id];
    int c = b->long_presses;
    b->long_presses = 0;
    return c;
}

// ★ 중요: 길게 누르는 동안 seek을 위해 "현재 눌려있는지"를 직접 GPIO에서 읽는다.
int button_is_down(button_id_t id)
{
    if (id < 0 || id >= BTN_MAX) return 0;
    button_t *b = &g_btn[id];

    // 풀업: 평소 HIGH(1), 눌리면 LOW(0)
    int level = gpioRead(b->pin);
    return (level == 0) ? 1 : 0;
}

void buttons_shutdown(void)
{
    for (int i = 0; i < BTN_MAX; i++) {
        int pin = g_btn[i].pin;
        if (pin > 0) {
            gpioSetAlertFunc(pin, NULL);
            gpioSetWatchdog(pin, 0);
        }
    }
}
