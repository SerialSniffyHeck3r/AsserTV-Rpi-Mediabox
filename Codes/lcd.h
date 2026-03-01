// lcd.h
#pragma once

#include <stdint.h>

#define LCD_COLS 20
#define LCD_ROWS 4

// 초기화 / 종료
int  lcd_init(void);      // 성공 시 0, 실패 시 <0
void lcd_shutdown(void);

// 기본 동작
void lcd_clear(void);
void lcd_set_cursor(int col, int row);
void lcd_write_char(char c);
void lcd_write_str(const char *s);

// 편의 함수: 지정 위치에 문자열 찍기 (줄 넘기지 않고 잘라냄)
void lcd_puts_at(int col, int row, const char *s);

// 커스텀 문자 (CGRAM) 등록
// index: 0~7
// pattern: 각 행의 하위 5비트만 사용 (0~31)
void lcd_create_char(uint8_t index, const uint8_t pattern[8]);

