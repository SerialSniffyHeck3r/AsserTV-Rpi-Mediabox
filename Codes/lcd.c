// lcd.c
#include "lcd.h"
#include <pigpio.h>
#include <stdio.h>

#define LCD_RS 26
#define LCD_E  19
#define LCD_D4 13
#define LCD_D5 6
#define LCD_D6 5
#define LCD_D7 11

static int lcd_inited = 0;

static void lcd_write4bits(uint8_t nibble)
{
    // nibble의 bit0~3 -> D4~D7
    gpioWrite(LCD_D4, (nibble >> 0) & 0x01);
    gpioWrite(LCD_D5, (nibble >> 1) & 0x01);
    gpioWrite(LCD_D6, (nibble >> 2) & 0x01);
    gpioWrite(LCD_D7, (nibble >> 3) & 0x01);

    gpioWrite(LCD_E, 1);
    gpioDelay(1);          // > 450ns
    gpioWrite(LCD_E, 0);
    gpioDelay(40);         // 대부분 명령 37µs 이상
}

static void lcd_send(uint8_t value, int rs)
{
    gpioWrite(LCD_RS, rs);

    // 상위 nibble
    lcd_write4bits((value >> 4) & 0x0F);
    // 하위 nibble
    lcd_write4bits(value & 0x0F);
}

static void lcd_command(uint8_t cmd)
{
    lcd_send(cmd, 0);
    if (cmd == 0x01 || cmd == 0x02) {
        // Clear / Home 은 1.52ms 이상
        gpioDelay(2000);
    } else {
        gpioDelay(40);
    }
}

static void lcd_data(uint8_t data)
{
    lcd_send(data, 1);
}

int lcd_init(void)
{
    if (lcd_inited) return 0;

    if (gpioInitialise() < 0) {
        fprintf(stderr, "lcd_init: gpioInitialise failed\n");
        return -1;
    }

    // 핀 출력 설정
    gpioSetMode(LCD_RS, PI_OUTPUT);
    gpioSetMode(LCD_E,  PI_OUTPUT);
    gpioSetMode(LCD_D4, PI_OUTPUT);
    gpioSetMode(LCD_D5, PI_OUTPUT);
    gpioSetMode(LCD_D6, PI_OUTPUT);
    gpioSetMode(LCD_D7, PI_OUTPUT);

    gpioWrite(LCD_RS, 0);
    gpioWrite(LCD_E,  0);

    gpioDelay(50000); // 전원 인가 후 >40ms

    // 8비트 모드 명령을 3번 보내서 초기 sync
    lcd_write4bits(0x03);
    gpioDelay(5000);
    lcd_write4bits(0x03);
    gpioDelay(5000);
    lcd_write4bits(0x03);
    gpioDelay(150);

    // 4비트 모드 진입
    lcd_write4bits(0x02);
    gpioDelay(150);

    // Function set: 4bit, 2라인, 5x8 폰트
    lcd_command(0x28);

    // Display on, cursor off, blink off
    lcd_command(0x0C);

    // Clear
    lcd_command(0x01);

    // Entry mode: auto-increment, shift off
    lcd_command(0x06);

    lcd_inited = 1;
    return 0;
}

void lcd_shutdown(void)
{
    if (!lcd_inited) return;
    lcd_clear();
    gpioTerminate();
    lcd_inited = 0;
}

void lcd_clear(void)
{
    if (!lcd_inited) return;
    lcd_command(0x01);
}

void lcd_set_cursor(int col, int row)
{
    if (!lcd_inited) return;

    if (col < 0) col = 0;
    if (col >= LCD_COLS) col = LCD_COLS - 1;
    if (row < 0) row = 0;
    if (row >= LCD_ROWS) row = LCD_ROWS - 1;

    static const uint8_t row_offsets[LCD_ROWS] = {
        0x00, 0x40, 0x14, 0x54   // 20x4 기준
    };

    lcd_command(0x80 | (row_offsets[row] + col));
}

void lcd_write_char(char c)
{
    if (!lcd_inited) return;
    lcd_data((uint8_t)c);
}

void lcd_write_str(const char *s)
{
    if (!lcd_inited || !s) return;
    while (*s) {
        lcd_write_char(*s++);
    }
}

void lcd_puts_at(int col, int row, const char *s)
{
    if (!lcd_inited || !s) return;
    if (col < 0 || col >= LCD_COLS || row < 0 || row >= LCD_ROWS) return;

    lcd_set_cursor(col, row);

    int maxlen = LCD_COLS - col;
    for (int i = 0; i < maxlen && s[i]; i++) {
        lcd_write_char(s[i]);
    }
}

// 커스텀 문자: CGRAM에 0~7번 등록
void lcd_create_char(uint8_t index, const uint8_t pattern[8])
{
    if (!lcd_inited || !pattern) return;

    index &= 0x07; // 0~7만 유효

    // CGRAM 주소: 0x40 | (index << 3)
    lcd_command(0x40 | (index << 3));

    for (int i = 0; i < 8; i++) {
        uint8_t row = pattern[i] & 0x1F; // 5bit
        lcd_data(row);
    }
}
