#include <pigpio.h>
#include <stdio.h>
#include <stdint.h>

// BCM GPIO 번호
#define LCD_RS 26
#define LCD_E  19
#define LCD_D4 13
#define LCD_D5 6
#define LCD_D6 5
#define LCD_D7 11

static void lcd_pulse_enable(void)
{
    gpioWrite(LCD_E, 1);
    gpioDelay(1);      // >= 450ns
    gpioWrite(LCD_E, 0);
    gpioDelay(100);    // 명령 처리 여유
}

static void lcd_write4bits(uint8_t value)
{
    gpioWrite(LCD_D4, (value >> 0) & 1);
    gpioWrite(LCD_D5, (value >> 1) & 1);
    gpioWrite(LCD_D6, (value >> 2) & 1);
    gpioWrite(LCD_D7, (value >> 3) & 1);
    lcd_pulse_enable();
}

static void lcd_send(uint8_t value, int rs)
{
    gpioWrite(LCD_RS, rs); // 0 = command, 1 = data
    lcd_write4bits(value >> 4);
    lcd_write4bits(value & 0x0F);
    gpioDelay(40);         // 대부분 명령 37us 정도
}

static void lcd_command(uint8_t cmd)
{
    lcd_send(cmd, 0);
}

static void lcd_write_char(char c)
{
    lcd_send((uint8_t)c, 1);
}

static void lcd_clear(void)
{
    lcd_command(0x01); // clear display
    gpioDelay(2000);   // clear는 1.52ms 이상
}

static void lcd_set_cursor(int col, int row)
{
    static const int row_offsets[] = {0x00, 0x40, 0x14, 0x54}; // 20x4 전형 레이아웃
    if (row < 0) row = 0;
    if (row > 3) row = 3;
    lcd_command(0x80 | (col + row_offsets[row]));
}

static void lcd_init(void)
{
    // 초기 핀 상태
    gpioWrite(LCD_RS, 0);
    gpioWrite(LCD_E,  0);
    gpioWrite(LCD_D4, 0);
    gpioWrite(LCD_D5, 0);
    gpioWrite(LCD_D6, 0);
    gpioWrite(LCD_D7, 0);

    gpioDelay(15000); // 전원 인가 후 15ms 이상

    // 8bit 모드 가정하고 상위 4비트만 3번 보냄
    lcd_write4bits(0x03);
    gpioDelay(4100);
    lcd_write4bits(0x03);
    gpioDelay(4100);
    lcd_write4bits(0x03);
    gpioDelay(150);

    // 4bit 모드 진입
    lcd_write4bits(0x02);
    gpioDelay(150);

    // function set: 4bit, 2 line(20x4도 이 설정), 5x8 dots
    lcd_command(0x28);

    // display off
    lcd_command(0x08);

    // clear
    lcd_clear();

    // entry mode: cursor move right, no display shift
    lcd_command(0x06);

    // display on, cursor off, blink off
    lcd_command(0x0C);
}

int main(void)
{
    if (gpioInitialise() < 0) {
        fprintf(stderr, "pigpio init failed (sudo로 실행했는지 확인)\n");
        return 1;
    }

    // GPIO 출력 설정
    gpioSetMode(LCD_RS, PI_OUTPUT);
    gpioSetMode(LCD_E,  PI_OUTPUT);
    gpioSetMode(LCD_D4, PI_OUTPUT);
    gpioSetMode(LCD_D5, PI_OUTPUT);
    gpioSetMode(LCD_D6, PI_OUTPUT);
    gpioSetMode(LCD_D7, PI_OUTPUT);

    lcd_init();
    lcd_clear();
    lcd_set_cursor(0, 0);

    const char *msg1 = "mediabox_lcd_ctrl";
    for (const char *p = msg1; *p; p++) {
        lcd_write_char(*p);
    }

    lcd_set_cursor(0, 1);
    const char *msg2 = "Hello, Ul-aegi!";
    for (const char *p = msg2; *p; p++) {
        lcd_write_char(*p);
    }

    // 10초 정도 구경
    gpioDelay(10000000);

    gpioTerminate();
    return 0;
}

