#pragma once
#include <stdint.h>

typedef enum {
    BTN_REW = 0,
    BTN_PLAYPAUSE = 1,
    BTN_FF = 2,
    BTN_MAX
} button_id_t;

// GPIO2,3,4를 내부에서 고정으로 사용
int buttons_init(void);

int button_get_clicks(button_id_t id);        // 짧은 클릭 수, 읽으면 0으로 리셋
int button_get_long_presses(button_id_t id);  // 롱프레스 수, 읽으면 0으로 리셋

// ★ 추가: 버튼이 지금 눌려있는지 여부 (눌려있으면 1, 아니면 0)
int button_is_down(button_id_t id);

void buttons_shutdown(void);
