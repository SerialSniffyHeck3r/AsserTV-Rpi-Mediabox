#include <pigpio.h>
#include <stdio.h>
#include <stdint.h>

// BCM GPIO 번호 (배선이 이거랑 맞는지 꼭 확인!)
#define LCD_RS 26
#define LCD_E  19
#define LCD_D4 13
#define LCD_D5 6
#define LCD_D6 5
#define LCD_D7 11

static void lcd_pulse_enable(void)
{
    gpioWrite(LCD_E, 1);
    gpioDelay(5);      // 5us로 넉넉하게
    gpioWrite(LCD_E, 0);
    gpioDelay(100);    // 명령 처리 여유
}

static void lcd_write4bits_raw(uint8_t nibble)
{
    // nibble의 bit0~3 -> D4~D7
    gpioWrite(LCD_D4, (nibble >> 0) & 1);
    gpioWrite(LCD_D5, (nibble >> 1) & 1);
    gpioWrite(LCD_D6, (nibble >> 2) & 1);
    gpioWrite(LCD_D7, (nibble >> 3) & 1);
    lcd_pulse_enable();
}

static void lcd_send(uint8_t value, int rs)
{
    gpioWrite(LCD_RS, rs); // 0 = command, 1 = data
    lcd_write4bits_raw(value >> 4);   // 상위 4비트
    lcd_write4bits_raw(value & 0x0F); // 하위 4비트
    gpioDelay(80);                    // 명령 처리시간 넉넉하게
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
    gpioDelay(3000);   // 2ms 이상
}

static void lcd_set_cursor(int col, int row)
{
    static const int row_offsets[] = {0x00, 0x40, 0x14, 0x54}; // 20x4 표준
    if (row < 0) row = 0;
    if (row > 3) row = 3;
    lcd_command(0x80 | (col + row_offsets[row]));
    gpioDelay(80);
}

static void lcd_init(void)
{
    gpioWrite(LCD_RS, 0);
    gpioWrite(LCD_E,  0);
    gpioWrite(LCD_D4, 0);
    gpioWrite(LCD_D5, 0);
    gpioWrite(LCD_D6, 0);
    gpioWrite(LCD_D7, 0);

    // 전원 인가 후 대기 (이미 켜져있을 수도 있지만 넉넉하게)
    gpioDelay(50000); // 50ms

    // 8bit 모드 가정하고 0x3을 3번 보냄
    lcd_write4bits_raw(0x03);
    gpioDelay(5000);  // 4.1ms 이상

    lcd_write4bits_raw(0x03);
    gpioDelay(5000);

    lcd_write4bits_raw(0x03);
    gpioDelay(5000);

    // 4bit 모드 진입 (0x2)
    lcd_write4bits_raw(0x02);
    gpioDelay(5000);

    // function set: 4bit(0), 2line(1), 5x8(0) => 0x28
    lcd_command(0x28);

    // display off
    lcd_command(0x08);

    // clear
    lcd_clear();

    // entry mode set: increment, no shift
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

    gpioSetMode(LCD_RS, PI_OUTPUT);
    gpioSetMode(LCD_E,  PI_OUTPUT);
    gpioSetMode(LCD_D4, PI_OUTPUT);
    gpioSetMode(LCD_D5, PI_OUTPUT);
    gpioSetMode(LCD_D6, PI_OUTPUT);
    gpioSetMode(LCD_D7, PI_OUTPUT);

    printf("LCD init...\n");
    lcd_init();

    printf("Clear...\n");
    lcd_clear();

    printf("Write test pattern...\n");
    lcd_set_cursor(0, 0);
    for (int i = 0; i < 20; i++) {
        lcd_write_char('A');
    }

    lcd_set_cursor(0, 1);
    for (int i = 0; i < 20; i++) {
        lcd_write_char('B');
    }

    gpioDelay(10000000); // 10초 구경
    gpioTerminate();
    return 0;
}

