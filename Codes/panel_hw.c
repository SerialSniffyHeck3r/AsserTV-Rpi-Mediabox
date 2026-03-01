#include "panel_hw.h"
#include <pigpio.h>
#include <unistd.h>
#include <stdint.h>
#include <math.h>

// 공통 애노드 RGB 백라이트 핀
#define PIN_R    14  // GPIO14 (핀 8)
#define PIN_G    15  // GPIO15 (핀 10)
#define PIN_B    18  // GPIO18 (핀 12)

// 부저 핀
#define PIN_BUZZ 23  // GPIO23 (핀 16)

// ---------------------------------------------------------------------
// LED 색/슬립 페이드 상태
// ---------------------------------------------------------------------
//  - 모든 값은 "PWM 값" 기준 (공통 애노드): 0 = 가장 밝음, 255 = 꺼짐
// ---------------------------------------------------------------------

static uint8_t cur_r = 255;
static uint8_t cur_g = 255;
static uint8_t cur_b = 255;

// "활성 모드"에서의 색 (재생/정지/일시정지 상태에서 마지막으로 지정된 색)
static uint8_t active_r = 0;
static uint8_t active_g = 0;
static uint8_t active_b = 0;

// 페이드용 시작/목표 색
static uint8_t start_r  = 255;
static uint8_t start_g  = 255;
static uint8_t start_b  = 255;
static uint8_t target_r = 255;
static uint8_t target_g = 255;
static uint8_t target_b = 255;

static int        sleep_mode     = 0;   // 0 = 정상, 1 = 슬립
static int        fading         = 0;   // 0 = 없음, 1 = 페이드 중
static long long  fade_start_ms  = 0;
static int        fade_duration_ms = 0; // 현재 진행 중인 페이드 길이(ms)

#define SLEEP_PWM        200     // 슬립 상태에서의 흰색 밝기 (꽤 어둡게)
#define FADE_DURATION_SLEEP_MS    3000    // 3초: 슬립/웨이크 전용
#define FADE_DURATION_NORMAL_MS   (FADE_DURATION_SLEEP_MS / 2)  // 일반 색 변경은 절반 속도
#define GAMMA            2.0f    // 간단 감마 보정

// ---------------------------------------------------------------------
// 내부 헬퍼: PWM 값에 감마를 적용해서 실제 하드웨어에 쏴 주기
// ---------------------------------------------------------------------

static void apply_pwm(uint8_t r, uint8_t g, uint8_t b)
{
    // 공통 애노드: 0 = 최대 밝기, 255 = 꺼짐
    // 여기서 "밝기"로 변환한 뒤 감마를 씌우고 다시 PWM 값으로 복원

    float ir = 1.0f - (float)r / 255.0f;   // 0~1 (0=꺼짐,1=최대밝기)
    float ig = 1.0f - (float)g / 255.0f;
    float ib = 1.0f - (float)b / 255.0f;

    ir = powf(ir, GAMMA);
    ig = powf(ig, GAMMA);
    ib = powf(ib, GAMMA);

    uint8_t pr = (uint8_t)(255.0f - ir * 255.0f + 0.5f);
    uint8_t pg = (uint8_t)(255.0f - ig * 255.0f + 0.5f);
    uint8_t pb = (uint8_t)(255.0f - ib * 255.0f + 0.5f);

    gpioPWM(PIN_R, pr);
    gpioPWM(PIN_G, pg);
    gpioPWM(PIN_B, pb);
}

static void apply_current(void)
{
    apply_pwm(cur_r, cur_g, cur_b);
}

// ---------------------------------------------------------------------
// 초기화/종료
// ---------------------------------------------------------------------

void panel_hw_init(void)
{
    // RGB 핀 출력 + PWM 설정
    gpioSetMode(PIN_R, PI_OUTPUT);
    gpioSetMode(PIN_G, PI_OUTPUT);
    gpioSetMode(PIN_B, PI_OUTPUT);

    gpioSetPWMfrequency(PIN_R, 1000);
    gpioSetPWMfrequency(PIN_G, 1000);
    gpioSetPWMfrequency(PIN_B, 1000);

    gpioSetPWMrange(PIN_R, 255);
    gpioSetPWMrange(PIN_G, 255);
    gpioSetPWMrange(PIN_B, 255);

    // 부저 핀
    gpioSetMode(PIN_BUZZ, PI_OUTPUT);
    gpioSetPWMfrequency(PIN_BUZZ, 2000); // 기본 2kHz
    gpioSetPWMrange(PIN_BUZZ, 255);
    gpioPWM(PIN_BUZZ, 0);

    // 부팅 직후: LED 꺼진 상태에서 시작
    cur_r = cur_g = cur_b = 255;
    active_r = active_g = active_b = 0;   // 나중에 panel_set_rgb 가 채움
    start_r = start_g = start_b = cur_r;
    target_r = target_g = target_b = cur_r;
    sleep_mode = 0;
    fading     = 0;

    apply_current();
}

void panel_hw_shutdown(void)
{
    cur_r = cur_g = cur_b = 255;   // 모두 꺼짐
    apply_current();
    gpioPWM(PIN_BUZZ, 0);
}

// ---------------------------------------------------------------------
// 색상 설정 / 슬립 모드 제어
// ---------------------------------------------------------------------

// panel_set_rgb:
//  - 인자는 "PWM 값" 기준 (0=밝음, 255=꺼짐)
//  - 슬립 모드가 아닐 때만 즉시 LED 색을 변경
//  - 슬립 모드일 때는 active_* 만 갱신 (깨울 때 이 색으로 돌아옴)


void panel_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    active_r = r;
    active_g = g;
    active_b = b;

    if (!sleep_mode) {
        cur_r = r;
        cur_g = g;
        cur_b = b;
        fading = 0;
        apply_current();
    }
}


// 일반 색 변경용 페이드 (재생/정지/일시정지 등)
//  - duration_ms > 0 이면 부드럽게 페이드
//  - duration_ms <= 0 이면 즉시 변경 (panel_set_rgb 와 동일 동작)
void panel_set_rgb_fade(uint8_t r, uint8_t g, uint8_t b,
                        int duration_ms, long long now_ms)
{
    active_r = r;
    active_g = g;
    active_b = b;

    if (sleep_mode) {
        // 슬립 모드에서는 색만 기억해 두고 실제 LED 는 건드리지 않는다.
        return;
    }

    if (duration_ms <= 0) {
        // duration <= 0 이면 즉시 변경
        cur_r = r;
        cur_g = g;
        cur_b = b;
        fading = 0;
        apply_current();
        return;
    }

    start_r = cur_r;
    start_g = cur_g;
    start_b = cur_b;

    target_r = r;
    target_g = g;
    target_b = b;

    fade_start_ms    = now_ms;
    fade_duration_ms = duration_ms;
    fading           = 1;
}

void panel_set_sleep_mode(int sleep_on, long long now_ms)
{
    if (sleep_on) {
        if (!sleep_mode) {
            sleep_mode = 1;
            // 현재 색에서 점점 "수면용 흰색"으로
            start_r  = cur_r;
            start_g  = cur_g;
            start_b  = cur_b;
            target_r = SLEEP_PWM;
            target_g = SLEEP_PWM;
            target_b = SLEEP_PWM;
            fade_start_ms    = now_ms;
            fade_duration_ms = PANEL_SLEEP_FADE_MS;
            fading           = 1;
        }
    } else {
        if (sleep_mode) {
            sleep_mode = 0;
            // 수면용 흰색에서 마지막 활성 색으로 복귀
            start_r  = cur_r;
            start_g  = cur_g;
            start_b  = cur_b;
            target_r = active_r;
            target_g = active_g;
            target_b = active_b;
            fade_start_ms    = now_ms;
            fade_duration_ms = PANEL_SLEEP_FADE_MS;
            fading           = 1;
        }
    }
}

// 주기적으로 호출되어 페이드 진행
void panel_led_tick(long long now_ms)
{
    if (!fading)
        return;

    long long dt = now_ms - fade_start_ms;
    if (dt <= 0)
        dt = 0;

    if (dt >= fade_duration_ms) {
        cur_r = target_r;
        cur_g = target_g;
        cur_b = target_b;
        fading = 0;
        apply_current();
        return;
    }

    float t = (float)dt / (float)fade_duration_ms;

    uint8_t r = (uint8_t)((1.0f - t) * start_r + t * target_r);
    uint8_t g = (uint8_t)((1.0f - t) * start_g + t * target_g);
    uint8_t b = (uint8_t)((1.0f - t) * start_b + t * target_b);

    cur_r = r;
    cur_g = g;
    cur_b = b;

    apply_current();
}


// ---------------------------------------------------------------------
// 부저 (비프음)
// ---------------------------------------------------------------------
//  - panel_beep_ms: 고정 주파수 짧은 비프
//  - panel_beep_volume_click: 볼륨 비례 400~2000 Hz
//  - panel_beep_menu_click: 메뉴 인코더 좌/우 800 / 1200 Hz
//  - panel_alarm_*: 알람 시계용 10초 "띠딕" 패턴
// ---------------------------------------------------------------------

static void beep_envelope(int freq, int duration_ms, int use_gamma)
{
    const int steps = 20;
    for (int i = 0; i < steps; i++) {
        float t = (float)i / (float)(steps - 1);
        float env = use_gamma ? powf(t, 2.2f) : t;
        int duty = (int)(255.0f * env);

        gpioSetPWMfrequency(PIN_BUZZ, freq);
        gpioPWM(PIN_BUZZ, duty);

        usleep(duration_ms * 1000 / steps);
    }
    gpioPWM(PIN_BUZZ, 0);
}

void panel_beep_ms(int ms)
{
    if (ms <= 0)
        return;

    gpioSetPWMfrequency(PIN_BUZZ, 1000);
    gpioPWM(PIN_BUZZ, 180);
    usleep(ms * 1000);
    gpioPWM(PIN_BUZZ, 0);
}

void panel_beep_short(void)
{
    panel_beep_ms(40);
}

//  - 현재 볼륨(%)에 따라 400Hz~2000Hz 사이로 선형 매핑
//  - 감마형 envelope 로 "톡" 하는 짧은 소리

// 볼륨 인코더 전용:
//  - 현재 볼륨(%)에 따라 400Hz~2000Hz 사이로 선형 매핑
//  - 감마형 envelope 로 "톡" 하는 짧은 소리
void panel_beep_volume_click(int volume_percent)
{
    if (volume_percent < 0)   volume_percent = 0;
    if (volume_percent > 100) volume_percent = 100;

    float t   = (float)volume_percent / 100.0f;   // 0.00 ~ 1.00
    int   freq = (int)(400.0f + t * (2000.0f - 400.0f) + 0.5f);
    // → 0% = 400Hz, 100% = 2000Hz, 중간은 1%마다 다른 값

    beep_envelope(freq, 35, 1);
}


// 메뉴 인코더 전용:
//  - dir < 0 : 800Hz
//  - dir > 0 : 1200Hz
//  - dir == 0 은 무시
void panel_beep_menu_click(int dir)
{
    if (dir == 0)
        return;

    int freq = (dir < 0) ? 800 : 1200;
    beep_envelope(freq, 35, 1);
}

// 알람 시계용 10초 비프 패턴 ("띠딕 띠딕" 반복)
static int       alarm_beep_active   = 0;
static long long alarm_beep_start_ms = 0;

void panel_alarm_start(long long now_ms)
{
    alarm_beep_active   = 1;
    alarm_beep_start_ms = now_ms;
    gpioPWM(PIN_BUZZ, 0);
}

void panel_alarm_tick(long long now_ms)
{
    if (!alarm_beep_active)
        return;

    long long elapsed = now_ms - alarm_beep_start_ms;
    if (elapsed < 0)
        elapsed = 0;

    if (elapsed >= 10000) { // 10초 후 종료
        alarm_beep_active = 0;
        gpioPWM(PIN_BUZZ, 0);
        return;
    }

    long long phase = elapsed % 800;  // 0~799ms 패턴 반복
    int duty = 0;
    int freq = 0;

    if (phase < 120) {
        // 첫 번째 "띠"
        freq = 2000;
        duty = 200;
    } else if (phase < 240) {
        // 짧은 무음
        duty = 0;
    } else if (phase < 360) {
        // 두 번째 "딕"
        freq = 1200;
        duty = 200;
    } else {
        duty = 0;
    }

    if (duty > 0) {
        gpioSetPWMfrequency(PIN_BUZZ, freq);
        gpioPWM(PIN_BUZZ, duty);
    } else {
        gpioPWM(PIN_BUZZ, 0);
    }
}
