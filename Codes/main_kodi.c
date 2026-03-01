#include "lcd.h"
#include "encoder.h"
#include "kodi_rpc.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

// LCD 한 줄을 항상 정확히 LCD_COLS 길이로 맞추는 헬퍼
static void make_line(char *dst, const char *src)
{
    size_t len = strlen(src);
    if (len > LCD_COLS) len = LCD_COLS;
    memcpy(dst, src, len);
    if (len < LCD_COLS) {
        memset(dst + len, ' ', LCD_COLS - len);
    }
    dst[LCD_COLS] = '\0';
}

// Kodi 볼륨(0~100)을 dB 스타일로 보여주기 위해 단순히 -100 오프셋
static void format_vol_line(char *buf, int kodi_volume, int muted)
{
    char tmp[64];
    int db = kodi_volume - 100; // 100 -> 0dB, 80 -> -20dB 느낌

    snprintf(tmp, sizeof(tmp),
             "%s Vol:%4ddB",
             muted ? "MUTE " : "MUSIC",
             db);
    make_line(buf, tmp);
}

static void format_menu_line(char *buf, int menu_index)
{
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "MENU idx:%2d", menu_index);
    make_line(buf, tmp);
}

int main(void)
{
    // pigpio + LCD 초기화
    if (lcd_init() < 0) {
        return 1;
    }

    // Kodi RPC 초기화 (localhost:8080)
    kodi_rpc_init("127.0.0.1", 8080, NULL, NULL);

    // 인코더 핀 매핑 (BCM)
    // 볼륨 인코더: A=17, B=27, BTN=22
    encoder_init(ENC_VOL,  17, 27, 22);
    // 메뉴 인코더: A=16, B=20, BTN=21
    encoder_init(ENC_MENU, 16, 20, 21);

    lcd_clear();

    int kodi_volume = 80;  // 0~100
    int muted = 0;
    int menu_index = 0;

    // Kodi에서 실제 볼륨/뮤트 상태 읽어오기 (실패하면 기본 값 사용)
    kodi_get_volume(&kodi_volume, &muted);
    if (kodi_volume < 0 || kodi_volume > 100) {
        kodi_volume = 80;
    }
    if (muted != 0 && muted != 1) {
        muted = 0;
    }

    char line0[LCD_COLS + 1];
    char line1[LCD_COLS + 1];
    char line2[LCD_COLS + 1];
    char line3[LCD_COLS + 1];

    format_vol_line(line0, kodi_volume, muted);
    format_menu_line(line1, menu_index);
    make_line(line2, "Artist: (dummy)");
    make_line(line3, "[#####-----] 00:00");

    lcd_puts_at(0, 0, line0);
    lcd_puts_at(0, 1, line1);
    lcd_puts_at(0, 2, line2);
    lcd_puts_at(0, 3, line3);

    while (1) {
        // 1) 볼륨 인코더 회전 -> Kodi volume 0~100
        int d_vol = encoder_get_delta(ENC_VOL);
        if (d_vol != 0) {
            kodi_volume += d_vol;

            if (kodi_volume > 100) kodi_volume = 100;
            if (kodi_volume < 0)   kodi_volume = 0;

            format_vol_line(line0, kodi_volume, muted);
            lcd_puts_at(0, 0, line0);

            // Kodi에 실제 볼륨 반영
            kodi_set_volume(kodi_volume);
        }

        // 2) 볼륨 인코더 버튼: short=mute 토글, long=메시지
        int vol_clicks = encoder_get_clicks(ENC_VOL);
        if (vol_clicks > 0) {
            muted = !muted;
            format_vol_line(line0, kodi_volume, muted);
            lcd_puts_at(0, 0, line0);
            kodi_set_mute(muted);
        }

        int vol_longs = encoder_get_long_presses(ENC_VOL);
        if (vol_longs > 0) {
            make_line(line2, "VOL long press");
            lcd_puts_at(0, 2, line2);
        }

        // 3) 메뉴 인코더 회전: 일단 더미 menu_index만
        int d_menu = encoder_get_delta(ENC_MENU);
        if (d_menu != 0) {
            menu_index += d_menu;
            if (menu_index < 0) menu_index = 0;
            if (menu_index > 99) menu_index = 99;

            format_menu_line(line1, menu_index);
            lcd_puts_at(0, 1, line1);

            // TODO: 여기서 Player 상태/트랙 정보 요청해서
            //       다른 줄 업데이트하는 메뉴 시스템 붙이면 됨
        }

        // 4) 메뉴 인코더 버튼
        int menu_clicks = encoder_get_clicks(ENC_MENU);
        if (menu_clicks > 0) {
            make_line(line2, "MENU click select");
            lcd_puts_at(0, 2, line2);
        }

        int menu_longs = encoder_get_long_presses(ENC_MENU);
        if (menu_longs > 0) {
            make_line(line3, "MENU long press");
            lcd_puts_at(0, 3, line3);
        }

        usleep(5000);
    }

    // 도달하진 않겠지만 형식상
    encoder_shutdown();
    kodi_rpc_shutdown();
    lcd_shutdown();
    return 0;
}
