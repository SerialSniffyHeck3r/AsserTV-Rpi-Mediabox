#pragma once
#include <stdint.h>

typedef enum {
    ENC_VOL = 0,
    ENC_MENU = 1,
    ENC_MAX
} encoder_id_t;

// 인코더 하나 초기화
// pin_btn == -1 이면 버튼 없이 사용
int encoder_init(encoder_id_t id, int pin_a, int pin_b, int pin_btn);

// 회전 변화량: 마지막 호출 이후 누적 delta 반환 후 0으로 리셋
int encoder_get_delta(encoder_id_t id);

// 짧은 클릭 횟수: 호출 시 0으로 리셋
int encoder_get_clicks(encoder_id_t id);

// 롱프레스 횟수: 호출 시 0으로 리셋
int encoder_get_long_presses(encoder_id_t id);

// 종료 시 콜백 해제
void encoder_shutdown(void);
