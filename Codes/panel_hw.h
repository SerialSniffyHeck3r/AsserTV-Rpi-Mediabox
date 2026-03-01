#pragma once
#include <stdint.h>

// 슬립 모드 페이드의 기준 시간 (ms)
#define PANEL_SLEEP_FADE_MS 3000

void panel_hw_init(void);
void panel_hw_shutdown(void);

void panel_set_rgb(uint8_t r, uint8_t g, uint8_t b);
// duration_ms > 0 이면 지정한 시간 동안 부드럽게 페이드
void panel_set_rgb_fade(uint8_t r, uint8_t g, uint8_t b,
                        int duration_ms, long long now_ms);

void panel_set_sleep_mode(int sleep_on, long long now_ms);
void panel_led_tick(long long now_ms);

// 알람(시계)용 비프 패턴
void panel_alarm_start(long long now_ms);
void panel_alarm_tick(long long now_ms);

void panel_beep_ms(int ms);
void panel_beep_short(void);
void panel_beep_volume_click(int volume_percent);
void panel_beep_menu_click(int dir);
