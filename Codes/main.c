#include "lcd.h"
#include "buttons.h"
#include "encoder.h"
#include "panel_hw.h"
#include "kodi_rpc.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>

#include <stdlib.h>

// 확장 LCD / Kodi RPC 헬퍼 프로토타입 (이미 다른 C파일에 구현돼 있다고 가정)
void lcd_create_char(uint8_t index, const uint8_t pattern[8]);

// Kodi Input.* (JSON-RPC)
int kodi_input_up(void);
int kodi_input_down(void);
int kodi_input_left(void);
int kodi_input_right(void);
int kodi_input_select(void);
int kodi_input_back(void);

// GUI 정보 (이전 단계에서 추가한 구조체/함수)


static int last_logged_kodi_ok = -1;

// 고정값들
#define LCD_COLS 20
#define LCD_ROWS 4

#define MENU_TIMEOUT_MS         5000
#define POLL_INTERVAL_MS         500
#define SCROLL_INTERVAL_PLAY_MS  300
#define SCROLL_INTERVAL_IDLE_MS  300
#define SCROLL_INTERVAL_MENU_MS  300
// 스크롤이 끝까지 돌고 다시 처음으로 돌아온 뒤 기다리는 시간 (ms)
// 예: 1500 → 1.5초
#define SCROLL_RESTART_PAUSE_MS  5000
// 오디오/비디오 3행 정보 회전 주기(ms)
#define AUDIO_INFO_ROTATE_MS    5000   // 오디오: 5초마다 다음 정보
#define VIDEO_INFO_ROTATE_MS    5000   // 비디오: 5초마다 프레임 토글


// 알람 시계 상태
typedef struct {
    int enabled;            // 알람 on/off
    int hour;               // 알람 시 (0~23)
    int minute;             // 알람 분 (0~59)
    int last_trigger_yday;  // 마지막으로 울린 날짜 (tm_yday)
} alarm_state_t;

// 기본값: 알람 비활성, 시간은 07:00
static alarm_state_t g_alarm = {
    .enabled           = 0,
    .hour              = 7,
    .minute            = 0,
    .last_trigger_yday = -1
};

// 커스텀 글리프 인덱스
#define CG_SPK   0
#define CG_CLOCK 1
#define CG_PB    2
#define CG_MEDIA 3
#define CG_FILE  4
#define CG_INFO  5
#define CG_MORE  6

typedef enum {
    UI_PLAYBACK = 0,
    UI_IDLE,
    UI_MENU
} ui_mode_t;

typedef enum {
    MEDIA_NONE = 0,
    MEDIA_AUDIO,
    MEDIA_VIDEO,
    MEDIA_IMAGE
} media_type_t;

typedef enum {
    PB_ICON_STATE_STOP = 0,
    PB_ICON_STATE_PLAY,
    PB_ICON_STATE_PAUSE,
    PB_ICON_STATE_FF,
    PB_ICON_STATE_REW
} pb_icon_state_t;

// 스크롤 상태
// 스크롤 상태
typedef struct {
    char      text[256];
    int       len;
    int       offset;
    long long last_step_ms;
    int       interval_ms;
    unsigned int wrap_count;   // 한 번 끝까지 스크롤해서 처음으로 돌아간 횟수
} scroll_t;


// 전역 스크롤러들
static scroll_t g_scroll_title;
static scroll_t g_scroll_artist;
static scroll_t g_scroll_idle;
static scroll_t g_scroll_gui_window;
static scroll_t g_scroll_gui_control;



// 파일 번호 (1베이스, soft 카운터)
static int g_file_index = -1;
static pb_icon_state_t g_pb_icon_state = PB_ICON_STATE_STOP;



// --------------------------------------------------------
// UTF-8 → LCD-safe 텍스트 변환
//  - ASCII(0x20~0x7E)는 그대로 통과
//  - 일본어 히라가나 / 가타카나 → 로마자
//  - 그 외 비 ASCII 문자는 문자 하나당 '?' 하나로 대체
// --------------------------------------------------------

#include <stdint.h>

// 히라가나를 카타카나 코드로 정규화한 뒤,
// 카타카나 코드포인트를 로마자로 매핑
static const char *kana_to_romaji(uint32_t cp)
{
    // 히라가나 범위이면 대응하는 카타카나로 변환 (0x60 offset)
    if (cp >= 0x3041 && cp <= 0x3096) {
        cp += 0x60;
    }

    switch (cp) {
    case 0x30A1: return "a";   // ァ
    case 0x30A2: return "a";   // ア
    case 0x30A3: return "i";   // ィ
    case 0x30A4: return "i";   // イ
    case 0x30A5: return "u";   // ゥ
    case 0x30A6: return "u";   // ウ
    case 0x30A7: return "e";   // ェ
    case 0x30A8: return "e";   // エ
    case 0x30A9: return "o";   // ォ
    case 0x30AA: return "o";   // オ

    case 0x30AB: return "ka";  // カ
    case 0x30AC: return "ga";  // ガ
    case 0x30AD: return "ki";  // キ
    case 0x30AE: return "gi";  // ギ
    case 0x30AF: return "ku";  // ク
    case 0x30B0: return "gu";  // グ
    case 0x30B1: return "ke";  // ケ
    case 0x30B2: return "ge";  // ゲ
    case 0x30B3: return "ko";  // コ
    case 0x30B4: return "go";  // ゴ

    case 0x30B5: return "sa";  // サ
    case 0x30B6: return "za";  // ザ
    case 0x30B7: return "shi"; // シ
    case 0x30B8: return "ji";  // ジ
    case 0x30B9: return "su";  // ス
    case 0x30BA: return "zu";  // ズ
    case 0x30BB: return "se";  // セ
    case 0x30BC: return "ze";  // ゼ
    case 0x30BD: return "so";  // ソ
    case 0x30BE: return "zo";  // ゾ

    case 0x30BF: return "ta";  // タ
    case 0x30C0: return "da";  // ダ
    case 0x30C1: return "chi"; // チ
    case 0x30C2: return "ji";  // ヂ
    case 0x30C4: return "tsu"; // ツ
    case 0x30C5: return "zu";  // ヅ
    case 0x30C6: return "te";  // テ
    case 0x30C7: return "de";  // デ
    case 0x30C8: return "to";  // ト
    case 0x30C9: return "do";  // ド

    case 0x30CA: return "na";  // ナ
    case 0x30CB: return "ni";  // ニ
    case 0x30CC: return "nu";  // ヌ
    case 0x30CD: return "ne";  // ネ
    case 0x30CE: return "no";  // ノ

    case 0x30CF: return "ha";  // ハ
    case 0x30D0: return "ba";  // バ
    case 0x30D1: return "pa";  // パ
    case 0x30D2: return "hi";  // ヒ
    case 0x30D3: return "bi";  // ビ
    case 0x30D4: return "pi";  // ピ
    case 0x30D5: return "fu";  // フ
    case 0x30D6: return "bu";  // ブ
    case 0x30D7: return "pu";  // プ
    case 0x30D8: return "he";  // ヘ
    case 0x30D9: return "be";  // ベ
    case 0x30DA: return "pe";  // ペ
    case 0x30DB: return "ho";  // ホ
    case 0x30DC: return "bo";  // ボ
    case 0x30DD: return "po";  // ポ

    case 0x30DE: return "ma";  // マ
    case 0x30DF: return "mi";  // ミ
    case 0x30E0: return "mu";  // ム
    case 0x30E1: return "me";  // メ
    case 0x30E2: return "mo";  // モ

    case 0x30E3: return "ya";  // ャ
    case 0x30E4: return "ya";  // ヤ
    case 0x30E5: return "yu";  // ュ
    case 0x30E6: return "yu";  // ユ
    case 0x30E7: return "yo";  // ョ
    case 0x30E8: return "yo";  // ヨ

    case 0x30E9: return "ra";  // ラ
    case 0x30EA: return "ri";  // リ
    case 0x30EB: return "ru";  // ル
    case 0x30EC: return "re";  // レ
    case 0x30ED: return "ro";  // ロ

    case 0x30EF: return "wa";  // ワ
    case 0x30F2: return "o";   // ヲ
    case 0x30F3: return "n";   // ン
    case 0x30F4: return "vu";  // ヴ

    default:
        return NULL;
    }
}

// UTF-8 문자열을 LCD에 안전한 8비트 문자열로 변환
//  - dst_size는 scroll_t.text 크기(256) 정도로 들어온다고 가정
static void lcd_normalize_text(const char *src, char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0) return;
    if (!src) src = "";

    size_t out = 0;
    dst[0] = '\0';

    size_t len = strlen(src);
    size_t i   = 0;

    int pending_sokuon = 0;  // 작은 っ / ッ 처리용 플래그

    while (i < len && out + 1 < dst_size) {
        unsigned char c = (unsigned char)src[i];
        uint32_t cp = 0;
        size_t seq_len = 1;

        if (c < 0x80) {
            // 1바이트 ASCII
            cp = c;
            seq_len = 1;
        } else if ((c & 0xE0) == 0xC0 && (i + 1) < len) {
            // 2바이트 UTF-8
            unsigned char c1 = (unsigned char)src[i + 1];
            if ((c1 & 0xC0) == 0x80) {
                cp = ((uint32_t)(c  & 0x1F) << 6) |
                     ((uint32_t)(c1 & 0x3F));
                seq_len = 2;
            } else {
                cp = 0xFFFD; // invalid
                seq_len = 1;
            }
        } else if ((c & 0xF0) == 0xE0 && (i + 2) < len) {
            // 3바이트 UTF-8
            unsigned char c1 = (unsigned char)src[i + 1];
            unsigned char c2 = (unsigned char)src[i + 2];
            if (((c1 & 0xC0) == 0x80) && ((c2 & 0xC0) == 0x80)) {
                cp = ((uint32_t)(c  & 0x0F) << 12) |
                     ((uint32_t)(c1 & 0x3F) << 6)  |
                     ((uint32_t)(c2 & 0x3F));
                seq_len = 3;
            } else {
                cp = 0xFFFD;
                seq_len = 1;
            }
        } else if ((c & 0xF8) == 0xF0 && (i + 3) < len) {
            // 4바이트 UTF-8 (이모지 등)
            unsigned char c1 = (unsigned char)src[i + 1];
            unsigned char c2 = (unsigned char)src[i + 2];
            unsigned char c3 = (unsigned char)src[i + 3];
            if (((c1 & 0xC0) == 0x80) &&
                ((c2 & 0xC0) == 0x80) &&
                ((c3 & 0xC0) == 0x80)) {
                cp = ((uint32_t)(c  & 0x07) << 18) |
                     ((uint32_t)(c1 & 0x3F) << 12) |
                     ((uint32_t)(c2 & 0x3F) << 6)  |
                     ((uint32_t)(c3 & 0x3F));
                seq_len = 4;
            } else {
                cp = 0xFFFD;
                seq_len = 1;
            }
        } else {
            // 잘못된 시퀀스 → replacement
            cp = 0xFFFD;
            seq_len = 1;
        }

        i += seq_len;

        // 1) ASCII 영역
        if (cp < 0x80) {
            if (cp < 0x20) {
                // 제어문자 → 공백으로
                dst[out++] = ' ';
            } else {
                dst[out++] = (char)cp;
            }
            continue;
        }

        // 2) 작은 っ / ッ (sokuon) → 다음 음절 자음 두배
        if (cp == 0x3063 || cp == 0x30C3) {
            pending_sokuon = 1;
            continue;
        }

        // 3) 장음부호 ー → 그냥 '-'로
        if (cp == 0x30FC) {
            if (out + 1 >= dst_size) break;
            dst[out++] = '-';
            continue;
        }

        // 4) 히라가나 / 가타카나인지 체크
        int is_kana =
            (cp >= 0x3041 && cp <= 0x309F) ||  // 히라가나
            (cp >= 0x30A1 && cp <= 0x30FF);    // 가타카나

        if (is_kana) {
            const char *roma = kana_to_romaji(cp);
            if (!roma) {
                // 매핑 없는 애는 그냥 '?'
                if (out + 1 >= dst_size) break;
                dst[out++] = '?';
                pending_sokuon = 0;
                continue;
            }

            // sokuon이 켜져 있으면, romaji의 첫 자음 하나를 앞에 붙인다
            if (pending_sokuon) {
                char cons = 0;
                for (const char *p = roma; *p; ++p) {
                    char ch = *p;
                    if ((ch >= 'a' && ch <= 'z') ||
                        (ch >= 'A' && ch <= 'Z')) {
                        cons = ch;
                        break;
                    }
                }
                if (cons && out + 1 < dst_size) {
                    dst[out++] = cons;
                }
                pending_sokuon = 0;
            }

            // romaji 전체를 복사
            for (const char *p = roma; *p && out + 1 < dst_size; ++p) {
                dst[out++] = *p;
            }
            continue;
        }

        // 5) 그 외 모든 비-ASCII 문자 → '?'
        if (out + 1 >= dst_size) break;
        dst[out++] = '?';
    }

    dst[out] = '\0';
}


// --------------------------------------------------------
// 시간 / 스크롤 유틸
// --------------------------------------------------------

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void scroll_init(scroll_t *s, const char *txt, int interval_ms)
{
    if (!s) return;
    if (!txt) txt = "";
    strncpy(s->text, txt, sizeof(s->text) - 1);
    s->text[sizeof(s->text) - 1] = '\0';
    s->len = (int)strlen(s->text);
    s->offset = 0;
    s->last_step_ms = 0;
    s->interval_ms = interval_ms;
    s->wrap_count  = 0;
}

static void scroll_set_text(scroll_t *s, const char *txt)
{
    if (!s) return;
    if (!txt) txt = "";

    // UTF-8 → LCD-safe 변환
    char norm[sizeof(s->text)];
    lcd_normalize_text(txt, norm, sizeof(norm));

    // 내용이 같으면 건드리지 않음
    if (strcmp(s->text, norm) == 0)
        return;

    // 스크롤 버퍼에 복사
    strncpy(s->text, norm, sizeof(s->text) - 1);
    s->text[sizeof(s->text) - 1] = '\0';
    s->len = (int)strlen(s->text);
    s->offset = 0;
    s->last_step_ms = 0;
    // (wrap_count를 쓰고 있다면, 여기서 s->wrap_count = 0; 추가해도 됨)
}

static void scroll_advance(scroll_t *s, long long now, int window_width)
{
    if (!s) return;
    if (s->interval_ms <= 0) return;
    if (s->len == 0) {
        s->offset = 0;
        return;
    }

    // 문자열이 화면보다 짧으면 스크롤할 필요 없음
    if (s->len <= window_width) {
        s->offset = 0;
        return;
    }

    if (s->last_step_ms == 0) {
        // 초기 진입 시: 지금 시간을 기준으로 타이머 시작
        s->last_step_ms = now;
        return;
    }

    if (now - s->last_step_ms < s->interval_ms)
        return;

    // 한 칸 전진
    s->last_step_ms = now;
    s->offset++;

    // ★ 끝까지 흘려보내기:
    // offset 최대값을 len까지 허용
    //  - offset == len: 화면 전체가 공백
    //  - 그 다음 스텝에서 wrap + 딜레이 설정
    int max_offset = s->len;

    if (s->offset > max_offset) {
        s->offset = 0;
        s->wrap_count++;

        // ★ 다시 처음으로 돌아왔을 때, 추가 딜레이 부여
        if (SCROLL_RESTART_PAUSE_MS > 0) {
            // 다음 이동까지 SCROLL_RESTART_PAUSE_MS 만큼 기다리도록
            s->last_step_ms = now + (SCROLL_RESTART_PAUSE_MS - s->interval_ms);
        }
    }
}

static void scroll_render_window(scroll_t *s,
                                 long long now,
                                 char *line,
                                 int start_col,
                                 int width)
{
    if (!s || !line) return;

    // 문자열이 짧으면 스크롤 없이 고정 표시
    if (s->len <= width) {
        for (int i = 0; i < width; i++) {
            char c = (i < s->len) ? s->text[i] : ' ';
            line[start_col + i] = c;
        }
        return;
    }

    // 스크롤 진행
    scroll_advance(s, now, width);
    int off = s->offset;

    for (int i = 0; i < width; i++) {
        int idx = off + i;

        // idx < len이면 실제 텍스트, 그 이후는 공백
        char c = (idx >= 0 && idx < s->len) ? s->text[idx] : ' ';
        line[start_col + i] = c;
    }
}



// --------------------------------------------------------
// IDLE 모드용 날씨/RSS 스크롤 상태
// --------------------------------------------------------

typedef enum {
    IDLE_PHASE_WEATHER = 0,
    IDLE_PHASE_RSS     = 1
} idle_phase_t;

// 기본은 "데이터 없음" 상태로 시작.
// 실제 날씨/RSS 모듈에서 idle_set_weather_text(), idle_set_rss_text()
// 를 호출하면 valid 플래그와 텍스트가 채워진다.
static char g_idle_weather_text[256] = "";
static char g_idle_rss_text[256]     = "";
static int  g_idle_weather_valid     = 0;
static int  g_idle_rss_valid         = 0;

static idle_phase_t g_idle_phase     = IDLE_PHASE_WEATHER;
static unsigned int g_idle_last_wrap = 0;

// 외부에서 날씨 문자열을 업데이트할 때 사용
static void idle_set_weather_text(const char *txt)
{
    if (!txt || !*txt) {
        g_idle_weather_valid = 0;
        g_idle_weather_text[0] = '\0';
        return;
    }
    strncpy(g_idle_weather_text, txt, sizeof(g_idle_weather_text) - 1);
    g_idle_weather_text[sizeof(g_idle_weather_text) - 1] = '\0';
    g_idle_weather_valid = 1;
}

// 외부에서 RSS 제목을 업데이트할 때 사용
static void idle_set_rss_text(const char *txt)
{
    if (!txt || !*txt) {
        g_idle_rss_valid = 0;
        g_idle_rss_text[0] = '\0';
        return;
    }
    strncpy(g_idle_rss_text, txt, sizeof(g_idle_rss_text) - 1);
    g_idle_rss_text[sizeof(g_idle_rss_text) - 1] = '\0';
    g_idle_rss_valid = 1;
}




static void put_right(char *row, int start, int width, unsigned int v)
{
    // start: 0-based index
    char buf[16];
    snprintf(buf, sizeof(buf), "%u", v);
    int len = (int)strlen(buf);
    for (int i = 0; i < width; i++) row[start + i] = ' ';
    int off = width - len;
    if (off < 0) off = 0;
    for (int i = 0; i < len && (off + i) < width; i++) row[start + off + i] = buf[i];
}

static void put_pic_size(char *row, unsigned long long bytes)
{
    // columns(1-based): 15-18 digits, 19-20 unit  => idx 14-17 digits, idx 18-19 unit
    // unknown
    if (bytes == 0) {
        row[14] = row[15] = row[16] = row[17] = '-';
        row[18] = 'K'; row[19] = 'B';
        return;
    }

    unsigned long long kb = (bytes + 512ULL) / 1024ULL; // round
    unsigned long long mb_bytes = 1024ULL * 1024ULL;

    if (kb < 800ULL) {
        // pos15 blank, pos16-18 right-aligned KB (no leading zero), pos19-20 "KB"
        row[14] = ' ';
        // 3 digits field at idx 15-17
        unsigned int v = (kb > 999ULL) ? 999U : (unsigned int)kb;
        put_right(row, 15, 3, v);
        row[18] = 'K'; row[19] = 'B';
        return;
    }

    // MB
    // <80MB: 15-16 int, 17 '.', 18 1 decimal (rounded), 19-20 "MB"
    unsigned long long mb10 = (bytes * 10ULL + (mb_bytes / 20ULL)) / mb_bytes; // round to 0.1MB
    unsigned long long mb_int = mb10 / 10ULL;
    unsigned long long mb_dec = mb10 % 10ULL;

    if (mb_int < 80ULL) {
        // int part into idx 14-15 (2 chars)
        for (int i = 14; i <= 15; i++) row[i] = ' ';
        unsigned int vi = (mb_int > 99ULL) ? 99U : (unsigned int)mb_int;
        put_right(row, 14, 2, vi);

        row[16] = '.';
        row[17] = (char)('0' + (int)mb_dec);
        row[18] = 'M'; row[19] = 'B';
        return;
    }

    // >=80MB: no decimal. use idx 15-17 (3 chars), keep "MB"
    row[14] = ' ';
    unsigned long long mb = (bytes + (mb_bytes / 2ULL)) / mb_bytes; // round to nearest MB
    if (mb > 999ULL) mb = 999ULL;
    put_right(row, 15, 3, (unsigned int)mb);
    row[18] = 'M'; row[19] = 'B';
}

static const char *kodi_basename(const char *path)
{
    if (!path || !*path) return "";
    const char *last = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') last = p + 1;
    }
    return (*last) ? last : path;
}


// --------------------------------------------------------
// 커스텀 글리프
// --------------------------------------------------------

static const uint8_t glyph_speaker[8] = {
    0b00001,
    0b00011,
    0b00111,
    0b11111,
    0b11111,
    0b00111,
    0b00011,
    0b00001,
};

static const uint8_t glyph_clock[8] = {
    0b00000,
    0b01110,
    0b10101,
    0b10111,
    0b10001,
    0b01110,
    0b00000,
    0b00000,
};

static const uint8_t glyph_pb_play[8] = {
    0b00000,
    0b01000,
    0b01100,
    0b01110,
    0b01110,
    0b01100,
    0b01000,
    0b00000,
};

static const uint8_t glyph_pb_pause[8] = {
    0b00000,
    0b01010,
    0b01010,
    0b01010,
    0b01010,
    0b01010,
    0b01010,
    0b00000,
};

static const uint8_t glyph_pb_stop[8] = {
    0b00000,
    0b00000,
    0b01110,
    0b01110,
    0b01110,
    0b01110,
    0b00000,
    0b00000,
};


// FF / REW 아이콘 (4행 1열 등에서 사용할 수 있는 방향 재생 아이콘)
static const uint8_t glyph_pb_ff[8] = {
    0b00000,
    0b11100,
    0b01110,
    0b00111,
    0b01110,
    0b11100,
    0b00000,
    0b00000,
};

static const uint8_t glyph_pb_rew[8] = {
    0b00000,
    0b00111,
    0b01110,
    0b11100,
    0b01110,
    0b00111,
    0b00000,
    0b00000,
};




static const uint8_t glyph_media_video[8] = {
    0b00010,
    0b00100,
    0b01000,
    0b10000,
    0b11111,
    0b11111,
    0b11111,
    0b00000,
};

static const uint8_t glyph_media_music[8] = {
    0b00001,
    0b00011,
    0b00101,
    0b01001,
    0b01001,
    0b01011,
    0b11011,
    0b11000,
};

static const uint8_t glyph_media_image[8] = {
    0b00000,
    0b11111,
    0b10001,
    0b10101,
    0b10101,
    0b10001,
    0b11111,
    0b00000,
};

static const uint8_t glyph_file[8] = {
    0b11111,
    0b10001,
    0b10111,
    0b10001,
    0b10111,
    0b10111,
    0b11111,
    0b00000,
};

static const uint8_t glyph_info[8] = {
    0b01110,
    0b11011,
    0b11111,
    0b11011,
    0b11011,
    0b11011,
    0b01110,
    0b00000,
};

static const uint8_t glyph_title_more[8] = {
    0b00000,
    0b00000,
    0b00000,
    0b00000,
    0b00000,
    0b00000,
    0b10101,
    0b00000,
};

static void init_custom_chars(void)
{
    lcd_create_char(CG_SPK,   glyph_speaker);
    lcd_create_char(CG_CLOCK, glyph_clock);
    lcd_create_char(CG_PB,    glyph_pb_stop);
    lcd_create_char(CG_MEDIA, glyph_media_video);
    lcd_create_char(CG_FILE,  glyph_file);
    lcd_create_char(CG_INFO,  glyph_info);
    lcd_create_char(CG_MORE,  glyph_title_more);
}

// 재생 상태 아이콘
static void set_pb_icon(pb_icon_state_t st)
{
    if (g_pb_icon_state == st)
        return;

    const uint8_t *glyph = glyph_pb_stop;

    switch (st) {
    case PB_ICON_STATE_PLAY:
        glyph = glyph_pb_play;
        break;
    case PB_ICON_STATE_PAUSE:
        glyph = glyph_pb_pause;
        break;
    case PB_ICON_STATE_FF:
        glyph = glyph_pb_ff;
        break;
    case PB_ICON_STATE_REW:
        glyph = glyph_pb_rew;
        break;
    case PB_ICON_STATE_STOP:
    default:
        glyph = glyph_pb_stop;
        break;
    }

    lcd_create_char(CG_PB, glyph);
    g_pb_icon_state = st;
}

static void update_pb_icon_from_np(const kodi_now_playing_t *np)
{
    pb_icon_state_t st = PB_ICON_STATE_STOP;

    if (np && np->active) {
        int spd = np->play_speed;

        if (spd > 1) {
            // 2x, 4x, 8x ... FAST FORWARD
            st = PB_ICON_STATE_FF;
        } else if (spd < 0) {
            // -1, -2 ... REWIND
            st = PB_ICON_STATE_REW;
        } else if (spd == 1) {
            // 정상 재생
            st = PB_ICON_STATE_PLAY;
        } else { // spd == 0
            // 일시정지
            st = PB_ICON_STATE_PAUSE;
        }
    }

    set_pb_icon(st);
}


// 미디어 타입/아이콘
static media_type_t detect_media_type(const kodi_now_playing_t *np)
{
    if (!np || !np->active) return MEDIA_NONE;
    if (strstr(np->type, "audio"))   return MEDIA_AUDIO;
    if (strstr(np->type, "video"))   return MEDIA_VIDEO;
    if (strstr(np->type, "picture") ||
        strstr(np->type, "image"))   return MEDIA_IMAGE;
    return MEDIA_NONE;
}

static void update_media_icon(media_type_t mt)
{
    const uint8_t *glyph = NULL;
    switch (mt) {
    case MEDIA_AUDIO: glyph = glyph_media_music; break;
    case MEDIA_VIDEO: glyph = glyph_media_video; break;
    case MEDIA_IMAGE: glyph = glyph_media_image; break;
    case MEDIA_NONE:
    default:
        return;
    }
    lcd_create_char(CG_MEDIA, glyph);
}

// --------------------------------------------------------
// 공통 라인 포맷터 (1행 / 4행)
// --------------------------------------------------------

static void clear_line(char *line)
{
    for (int i = 0; i < LCD_COLS; i++)
        line[i] = ' ';
}

static void lcd_write_row(int row, const char *buf)
{
    lcd_set_cursor(0, row);
    for (int i = 0; i < LCD_COLS; i++)
        lcd_write_char(buf[i]);
}

// 1행: (Kodi 있을 때) 스피커 + 볼륨 + % + 시계
//      (Kodi 없을 때)        공백          + 시계만
static void format_line1(char *line, int kodi_ok, int volume, int muted)
{
    clear_line(line);

    // --------------------------
    // 공통: 시계 그리기
    // --------------------------
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    if (!lt) return;

    int hour = lt->tm_hour;
    int min  = lt->tm_min;

    // 1,15: 시계 아이콘
    line[14] = CG_CLOCK;

    // 시: 리딩 제로 없이, 2칸
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", hour);
    int len   = (int)strlen(buf);
    int width = 2;
    int start = width - len;
    if (start < 0) start = 0;

    line[15] = line[16] = ' ';
    for (int i = 0; i < len && (start + i) < width; i++)
        line[15 + start + i] = buf[i];

    // 1,18: ':' 0.5초마다 깜빡이기 (1Hz)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    line[17] = ((ms / 500) % 2) ? ':' : ' ';

    // 분: 리딩 제로 있는 2자리
    snprintf(buf, sizeof(buf), "%02d", min);
    line[18] = buf[0];
    line[19] = buf[1];

    // --------------------------
    // Kodi 연결이 없으면 여기까지 (볼륨 영역은 공백 유지)
    // --------------------------
    if (!kodi_ok)
        return;

    // --------------------------
    // Kodi 있을 때만: 1,1~1,5 볼륨 영역
    // --------------------------

    // 1,1: 스피커
    line[0] = CG_SPK;

    // 1,2~4: 볼륨 또는 뮤트 표시
    for (int i = 0; i < 3; i++)
        line[1 + i] = ' ';

    if (muted) {
        // 뮤트일 때는 숫자 대신 ---
        line[1] = '-';
        line[2] = '-';
        line[3] = '-';
    } else {
        if (volume < 0)   volume = 0;
        if (volume > 100) volume = 100;

        char vbuf[8];
        snprintf(vbuf, sizeof(vbuf), "%d", volume);
        int vlen   = (int)strlen(vbuf);
        int vwidth = 3;
        int vstart = vwidth - vlen;
        if (vstart < 0) vstart = 0;

        for (int i = 0; i < vlen && (vstart + i) < vwidth; i++)
            line[1 + vstart + i] = vbuf[i];
    }

    // 1,5: %
    line[4] = '%';
}



// 4행용 보조 (파일 형식/번호/시간)

static void format_file_type(char out4[4], const kodi_now_playing_t *np)
{
    for (int i = 0; i < 4; i++)
        out4[i] = ' ';

    if (!np || !np->active)
        return;

    const char *file = np->file;
    if (!file[0]) return;

    const char *dot = strrchr(file, '.');
    if (!dot || !dot[1]) return;

    char ext[8];
    strncpy(ext, dot + 1, sizeof(ext) - 1);
    ext[sizeof(ext) - 1] = '\0';

    for (char *p = ext; *p; ++p) {
        if (*p >= 'a' && *p <= 'z')
            *p = (char)(*p - 'a' + 'A');
    }

    int len = (int)strlen(ext);
    if (len >= 4) {
        for (int i = 0; i < 4; i++)
            out4[i] = ext[i];
    } else if (len == 3) {
        out4[0] = ext[0];
        out4[1] = ext[1];
        out4[2] = ext[2];
        out4[3] = ' ';
    } else if (len == 2) {
        out4[0] = ext[0];
        out4[1] = ext[1];
        out4[2] = ' ';
        out4[3] = ' ';
    } else if (len == 1) {
        out4[0] = ext[0];
    }
}

static void format_file_index(char out3[3], int file_index, int active)
{
    if (!active || file_index < 1) {
        out3[0] = out3[1] = out3[2] = '-';
        return;
    }

    if (file_index > 999)
        file_index = 999;

    char buf[8];
    snprintf(buf, sizeof(buf), "%d", file_index);
    int len   = (int)strlen(buf);
    int width = 3;
    int start = width - len;
    if (start < 0) start = 0;

    out3[0] = out3[1] = out3[2] = ' ';
    for (int i = 0; i < len && (start + i) < width; i++)
        out3[start + i] = buf[i];
}

static void format_play_time(char *line, const kodi_now_playing_t *np)
{
    // 4,14~16: 분, 4,17: ', 4,18~19: 초, 4,20: "
    if (!np || !np->active || np->total_sec <= 0) {
        line[13] = ' ';
        line[14] = '-';
        line[15] = '-';
        line[16] = '\'';
        line[17] = '-';
        line[18] = '-';
        line[19] = '"';
        return;
    }

    int t = np->time_sec;
    if (t < 0) t = 0;

    int minutes = t / 60;
    int seconds = t % 60;

    char buf[8];

    // 분 3자리 (리딩 제로 한 자리만 허용)
    snprintf(buf, sizeof(buf), "%d", minutes);
    int len   = (int)strlen(buf);
    int width = 3;
    int start = width - len;
    if (start < 0) start = 0;

    line[13] = line[14] = line[15] = ' ';
    for (int i = 0; i < len && (start + i) < width; i++)
        line[13 + start + i] = buf[i];

    line[16] = '\'';

        snprintf(buf, sizeof(buf), "%02d", seconds);
    line[17] = buf[0];
    line[18] = buf[1];
    line[19] = '"';

    // 일시정지 상태일 때에는 분/초 숫자만 0.5초 간격으로 깜빡인다.
    // 1행 시계의 ':' 깜빡임과 동일한 위상으로 맞추기 위해 같은 ms 계산을 사용한다.
    if (np && np->active && !np->playing) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        long long ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        int on = (int)((ms / 500) % 2);  // 0.5초마다 토글

        if (!on) {
            // 숫자 부분만 OFF. ' 와 " 는 그대로 유지
            line[13] = ' ';
            line[14] = ' ';
            line[15] = ' ';
            line[17] = ' ';
            line[18] = ' ';
        }
    }
}

// 4행 전체
static void format_line4(char *line, const kodi_now_playing_t *np)
{
    clear_line(line);

    // 4,1: 재생 상태 아이콘 고정
    line[0] = CG_PB;
    line[1] = ' ';

    // 4,3~6: 예전에는 파일 형식(확장자) 영역이었지만,
    //        이제는 항상 공백 유지 (아무 것도 표시하지 않음)

    // 4,8: 파일 아이콘 (커스텀 글리프)
    line[8] = CG_FILE;

    // 4,9~11: 파일 번호 (기존 10~12에서 한 칸 왼쪽으로 이동)
    char fidx[3];
    format_file_index(fidx, g_file_index, np && np->active);
    line[9]  = fidx[0];
    line[10]  = fidx[1];
    line[11] = fidx[2];

    // 4,14~20: 재생 시간 (기존 format_play_time 그대로 사용)
    format_play_time(line, np);
}




// Kodi 가 꺼져 있을 때의 4행: 알람 상태 표시
static void format_alarm_line(char *line)
{
    clear_line(line);

    // 4행 13~14: 'AL' (1-base 기준이므로 인덱스 12, 13)
    line[12] = 'A';
    line[13] = 'L';

    // 4행 16~20: HH:MM 또는 --:--
    if (!g_alarm.enabled) {
        line[15] = '-';
        line[16] = '-';
        line[17] = ':';
        line[18] = '-';
        line[19] = '-';
    } else {
        char buf[3];

        int hour   = g_alarm.hour;
        int minute = g_alarm.minute;

        if (hour   < 0) hour   = 0;
        if (hour   > 23) hour   = 23;
        if (minute < 0) minute = 0;
        if (minute > 59) minute = 59;

        snprintf(buf, sizeof(buf), "%02d", hour);
        line[15] = buf[0];
        line[16] = buf[1];
        line[17] = ':';
        snprintf(buf, sizeof(buf), "%02d", minute);
        line[18] = buf[0];
        line[19] = buf[1];
    }
}



// Kodi 가 꺼져 있을 때 4행에 표시하는 알람 시계
//  - 4,13~14: "AL"
//  - 4,16~17: 알람 시 (HH)
//  - 4,18   : ':' 고정
//  - 4,19~20: 알람 분 (MM)
//  - 알람이 꺼져 있으면 "--:--" 표시

// Kodi 가 꺼져 있을 때 알람 시간에 도달하면
//  - panel_alarm_start() 를 호출해 10초간 비프 패턴을 울린다.
//  - 하루에 한 번만 울리도록 tm_yday 기준으로 재발동을 막는다.
static void handle_alarm(long long now_ms, int kodi_ok)
{
    // "전원이 꺼진 상태" 전용 동작: Kodi 가 살아있으면 아무 것도 안 함
    if (kodi_ok)
        return;

    if (!g_alarm.enabled)
        return;

    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    if (!lt)
        return;

    int hour = lt->tm_hour;
    int min  = lt->tm_min;
    int yday = lt->tm_yday;

    // 이미 오늘 울렸다면 다시 울리지 않는다.
    if (g_alarm.last_trigger_yday == yday)
        return;

    if (hour == g_alarm.hour && min == g_alarm.minute) {
        g_alarm.last_trigger_yday = yday;

        // 10초간 "띠딕 띠딕" 비프 패턴 시작
        panel_alarm_start(now_ms);

        // 필요하다면 여기에서 CEC / Kodi 재생 시작 로직을 추가할 수 있다.
        //  ex) system("/usr/bin/cec-client ...") 또는 Kodi JSON-RPC Player.Open 등
    }
}



// --------------------------------------------------------
// 2/3행 playback: 제목/메타/비트레이트/FPS/해상도
// --------------------------------------------------------

static void format_bitrate_3digits(char *line, int start_idx, int kbps)
{
    if (kbps <= 0) {
        line[start_idx + 0] = '-';
        line[start_idx + 1] = '-';
        line[start_idx + 2] = '-';
        return;
    }

    if (kbps > 999) kbps = 999;

    char buf[8];
    snprintf(buf, sizeof(buf), "%d", kbps);
    int len   = (int)strlen(buf);
    int width = 3;
    int start = width - len;
    if (start < 0) start = 0;

    for (int i = 0; i < width; i++)
        line[start_idx + i] = ' ';

    for (int i = 0; i < len && (start + i) < width; i++)
        line[start_idx + start + i] = buf[i];
}

static void format_bitrate_4digits(char *line, int start_idx, int kbps)
{
    if (kbps <= 0) {
        for (int i = 0; i < 4; i++)
            line[start_idx + i] = '-';
        return;
    }

    if (kbps > 9999) kbps = 9999;

    char buf[8];
    snprintf(buf, sizeof(buf), "%d", kbps);
    int len   = (int)strlen(buf);
    int width = 4;
    int start = width - len;
    if (start < 0) start = 0;

    for (int i = 0; i < width; i++)
        line[start_idx + i] = ' ';

    for (int i = 0; i < len && (start + i) < width; i++)
        line[start_idx + start + i] = buf[i];
}

static void format_fps(char *line, int start_idx, double fps)
{
    // 출력 포맷: "23.97" (5 chars)
    if (fps <= 0.01) {
        line[start_idx + 0] = '-';
        line[start_idx + 1] = '-';
        line[start_idx + 2] = '.';
        line[start_idx + 3] = '-';
        line[start_idx + 4] = '-';
        return;
    }

    if (fps > 99.99) fps = 99.99;

    int whole = (int)fps;
    int frac  = (int)((fps - (double)whole) * 100.0 + 0.5);
    if (frac >= 100) { whole++; frac = 0; }
    if (whole > 99) { whole = 99; frac = 99; }

    line[start_idx + 0] = (whole >= 10) ? ('0' + (whole / 10)) : ' ';
    line[start_idx + 1] = '0' + (whole % 10);
    line[start_idx + 2] = '.';
    line[start_idx + 3] = '0' + (frac / 10);
    line[start_idx + 4] = '0' + (frac % 10);
}

// "--.-" 형태로 채널수 표시 (예: " 2.0", "11.0")
// 비정상 값(<=0, >16)은 전부 "--.-" 로 처리
static void format_audio_channels(char *line, int start_idx, int channels)
{
    if (channels <= 0 || channels > 16) {
        line[start_idx + 0] = '-';
        line[start_idx + 1] = '-';
        line[start_idx + 2] = '.';
        line[start_idx + 3] = '-';
        return;
    }

    char buf[8];
    snprintf(buf, sizeof(buf), "%d", channels);
    int len = (int)strlen(buf);

    char d0 = ' ';
    char d1 = ' ';

    if (len == 1) {
        d1 = buf[0];          // " 2.0"
    } else {
        d0 = buf[len - 2];    // 끝 두 자리만 사용
        d1 = buf[len - 1];
    }

    line[start_idx + 0] = d0;
    line[start_idx + 1] = d1;
    line[start_idx + 2] = '.';
    line[start_idx + 3] = '0';   // 정수 채널만 알고 있으니 항상 ".0"
}

// "---.-" 형태로 샘플레이트(kHz) 표시 (예: " 44.1", "192.0")
// 비정상 값(<=0) 은 "---.-"로 처리
static void format_samplerate_khz_3_1(char *line, int start_idx, int samplerate_hz)
{
    if (samplerate_hz <= 0) {
        line[start_idx + 0] = '-';
        line[start_idx + 1] = '-';
        line[start_idx + 2] = '-';
        line[start_idx + 3] = '.';
        line[start_idx + 4] = '-';
        return;
    }

    // Hz -> 0.1kHz 로 변환 (반올림): 44100 -> 441, 48000 -> 480
    int khz_x10 = (samplerate_hz + 50) / 100;
    if (khz_x10 > 9999) khz_x10 = 9999;

    int int_part = khz_x10 / 10;
    int dec_part = khz_x10 % 10;

    char buf[16];
    snprintf(buf, sizeof(buf), "%d", int_part);
    int len = (int)strlen(buf);

    char d0 = ' ';
    char d1 = ' ';
    char d2 = ' ';

    if (len == 1) {
        d2 = buf[0];              // "  8.x"
    } else if (len == 2) {
        d1 = buf[0];              // " 44.x"
        d2 = buf[1];
    } else {
        d0 = buf[len - 3];        // 끝 3자리만 사용: 192 -> "192.x"
        d1 = buf[len - 2];
        d2 = buf[len - 1];
    }

    line[start_idx + 0] = d0;
    line[start_idx + 1] = d1;
    line[start_idx + 2] = d2;
    line[start_idx + 3] = '.';
    line[start_idx + 4] = (char)('0' + dec_part);
}



static void format_resolution(char *line, int start_idx, int w, int h)
{
    // 출력 포맷: "1920x1080" (최대 11칸으로 커버, 부족하면 ---)
    if (w <= 0 || h <= 0) {
        line[start_idx + 0]  = '-';
        line[start_idx + 1]  = '-';
        line[start_idx + 2]  = '-';
        line[start_idx + 3]  = '-';
        line[start_idx + 4]  = '-';
        line[start_idx + 5]  = 'x';
        line[start_idx + 6]  = '-';
        line[start_idx + 7]  = '-';
        line[start_idx + 8]  = '-';
        line[start_idx + 9]  = '-';
        line[start_idx + 10] = '-';
        return;
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%dx%d", w, h);

    // 11칸에 맞춰 우측 정렬(대충 예쁘게)
    for (int i = 0; i < 11; i++) line[start_idx + i] = ' ';
    int len = (int)strlen(buf);
    if (len > 11) len = 11;
    memcpy(&line[start_idx + (11 - len)], buf, len);
}


// now playing → 타이틀/아티스트 스크롤 텍스트 설정
// 타이틀/아티스트 스크롤 텍스트 설정
static void update_scrollers_from_np(const kodi_now_playing_t *np,
                                     media_type_t mt)
{
    const char *title  = "";
    const char *artist = "";
    const char *file   = "";

    if (!np || !np->active) {
        scroll_set_text(&g_scroll_title,  "");
        scroll_set_text(&g_scroll_artist, "");
        return;
    }

    // np->file이 비어있지 않으면 basename만 뽑아서 사용
    if (np->file[0]) {
        file = kodi_basename(np->file);
    }

    if (mt == MEDIA_AUDIO) {
        // 1순위: 메타데이터 타이틀
        if (np->title[0]) {
            title = np->title;
        }
        // 2순위: 파일 basename
        else if (file[0]) {
            title = file;
        }
        else {
            title = "(NO TITLE)";
        }

        // 아티스트 표시 우선순위
        if (np->artist[0]) {
            artist = np->artist;
        } else if (np->album[0]) {
            artist = np->album;
        } else {
            artist = "(NO ARTIST)";
        }
    }
    else if (mt == MEDIA_VIDEO) {
        // 영화: 타이틀이 있으면 그걸 쓰고, 없으면 파일 basename
        if (np->title[0]) {
            title = np->title;
        }
        else if (file[0]) {
            title = file;
        }
        else {
            title = "(NO TITLE)";
        }
        artist = "";
    }
    else if (mt == MEDIA_IMAGE) {
        // 사진: 원래부터 file을 우선 사용하던 모드니까
        // 여기서도 basename만 사용하도록 변경
        if (file[0]) {
            title = file;
        }
        else if (np->title[0]) {
            title = np->title;
        }
        else {
            title = "(NO TITLE)";
        }
        artist = "";
    }

    scroll_set_text(&g_scroll_title,  title);
    scroll_set_text(&g_scroll_artist, artist);
}

// playback 모드 2/3행
static void format_playback_rows(const kodi_now_playing_t *np,
                                 media_type_t mt,
                                 long long now,
                                 char *row2,
                                 char *row3)
{
    clear_line(row2);
    clear_line(row3);

    if (!np || !np->active) {
        const char *msg = "NO ACTIVE PLAYER";
        int len = (int)strlen(msg);
        if (len > LCD_COLS) len = LCD_COLS;
        memcpy(row2, msg, len);
        return;
    }

    update_media_icon(mt);
    update_scrollers_from_np(np, mt);

    // 2행: 2,1 미디어 글리프 + 2,3~ 제목 스크롤
    row2[0] = CG_MEDIA;
    row2[1] = ' ';
    scroll_render_window(&g_scroll_title, now, row2, 2, 18);

    // 3행 공통 헤더: 3,1 정보 글리프
    row3[0] = CG_INFO;
    row3[1] = ' ';

    if (mt == MEDIA_AUDIO) {
        // ----------------------------------------------------
        // AUDIO 모드 3행
        //  3,3~3,13 : 아티스트/앨범 스크롤 (폭 11칸)
        //  3,15~3,20: 회전 정보
        //    0: "----Kb" (비트레이트)
        //    1: "--.-ch" (채널)
        //    2: "---.-k" (샘플레이트)
        //    3: "EXT "   (확장자, 3,17~3,20)
        // ----------------------------------------------------
        scroll_render_window(&g_scroll_artist, now, row3, 2, 11);

        // 우측 정보 영역 초기화
        for (int i = 14; i < 20; i++)
            row3[i] = ' ';

        int phase = 0;
        if (AUDIO_INFO_ROTATE_MS > 0) {
            long long slot = now / AUDIO_INFO_ROTATE_MS;
            if (slot < 0) slot = -slot;
            phase = (int)(slot % 4);   // 0..3
        }

        switch (phase) {
        case 0: // 비트레이트 "----Kb"
            format_bitrate_4digits(row3, 14, np ? np->audio_bitrate_kbps : 0);
            row3[18] = 'K';
            row3[19] = 'b';
            break;

        case 1: // 채널 "--.-ch"
            format_audio_channels(row3, 14, np ? np->audio_channels : 0);
            row3[18] = 'c';
            row3[19] = 'h';
            break;

        case 2: // 샘플레이트 "---.-k"
            format_samplerate_khz_3_1(row3, 14, np ? np->audio_samplerate_hz : 0);
            row3[19] = 'k';
            break;

        case 3: // 확장자 (3,17~3,20)
        default:
        {
            char ftype[4];
            format_file_type(ftype, np);
            for (int i = 0; i < 4; i++)
                row3[16 + i] = ftype[i];   // col 17~20
            break;
        }
        }
    }
    else if (mt == MEDIA_VIDEO) {
        // ----------------------------------------------------
        // VIDEO 모드 3행: 2 프레임 회전
        //
        //   프레임 A (phase 0):
        //     3,3~3,6 : Video Width (4자리까지, 우측 정렬)
        //     3,7     : 'x'
        //     3,8~3,11: Video Height (4자리까지, 우측 정렬)
        //     3,15~3,20: "--.-ch" (오디오 채널수)
        //
        //   프레임 B (phase 1):
        //     3,3~3,7 : FPS "--.--" (기존 format_fps 그대로)
        //     3,15~3,19: "---.-K" (샘플레이트 kHz)
        // ----------------------------------------------------
           row3[0] = CG_INFO;
        row3[1] = ' ';

        // 나머지 칸 클리어
        for (int i = 2; i < LCD_COLS; i++)
            row3[i] = ' ';

        // A/B 프레임 토글
        int phase = 0;
        if (VIDEO_INFO_ROTATE_MS > 0) {
            long long slot = now / VIDEO_INFO_ROTATE_MS;
            if (slot < 0) slot = -slot;
            phase = (int)(slot & 1);   // 0 또는 1
        }

        if (phase == 0) {
            // ===== 프레임 A: 해상도 + 채널수 =====
            if (np && np->video_width > 0 && np->video_height > 0) {
                unsigned int w = (np->video_width  > 9999)
                                   ? 9999 : (unsigned int)np->video_width;
                unsigned int h = (np->video_height > 9999)
                                   ? 9999 : (unsigned int)np->video_height;

                // 3,3~3,6: width (4자리, 우측 정렬)
                // row index 2..5
                put_right(row3, 2, 4, w);

                // 3,7: 'x'
                row3[6] = 'x';

                // 3,8~3,11: height (4자리, 우측 정렬)
                // row index 7..10
                put_right(row3, 7, 4, h);
            } else {
                // 값 없으면 "----x----"
                memcpy(row3 + 2, "----x----", 9);
            }

            // 3,15~3,18: 채널수 "--.-"
            // row index 14..17
            format_audio_channels(row3, 14, np ? np->audio_channels : 0);

            // 3,19~3,20: "ch" 고정
            // row index 18,19
            row3[18] = 'c';
            row3[19] = 'h';
        }
        else {
            // ===== 프레임 B: FPS + 샘플레이트 =====

            // 3,3~3,7: FPS "--.--"
            // row index 2..6
            format_fps(row3, 2, np ? np->video_fps : 0.0);

            // 3,9~3,11: "fps" 고정
            // row index 8..10 (앞 7은 공백)
            row3[7]  = ' ';
            row3[8]  = 'f';
            row3[9]  = 'p';
            row3[10] = 's';

            // 3,15~3,19: 샘플레이트 "---.-"
            // row index 14..18
            format_samplerate_khz_3_1(row3, 14,
                                      np ? np->audio_samplerate_hz : 0);

            // 3,20: 'K'
            // row index 19
            row3[19] = 'K';
        }
    }


    else if (mt == MEDIA_IMAGE) {
        // ----------------------------------------------------
        // IMAGE 모드 3행: 기존 레이아웃 유지
        //   3,3~3,13: 해상도 "-----x-----"
        //   3,15~3,20: 파일 크기 (KB/MB)
        // ----------------------------------------------------
        if (np->video_width > 0 && np->video_height > 0) {
            put_right(row3, 2, 5, (unsigned int)np->video_width);   // col 3-7
            row3[7] = 'x';                                          // col 8
            put_right(row3, 8, 5, (unsigned int)np->video_height);  // col 9-13
        } else {
            memcpy(row3 + 2, "-----x-----", 11);
        }

        put_pic_size(row3, np->file_size_bytes);
    }
}


// --------------------------------------------------------
// 메뉴 모드: current window / current control
// --------------------------------------------------------

static void format_menu_rows(long long now,
                             kodi_gui_info_t *gui,
                             char *row2,
                             char *row3)
{
    clear_line(row2);
    clear_line(row3);

    const char *w = (gui && gui->window_label[0])  ? gui->window_label  : "(no window)";
    const char *c = (gui && gui->control_label[0]) ? gui->control_label : "(no control)";

    scroll_set_text(&g_scroll_gui_window,  w);
    scroll_set_text(&g_scroll_gui_control, c);

    // 메뉴 모드에서는 2,1 / 3,1 아이콘 없이 전체 20칸 텍스트
    scroll_render_window(&g_scroll_gui_window,  now, row2, 0, LCD_COLS);
    scroll_render_window(&g_scroll_gui_control, now, row3, 0, LCD_COLS);
}

// --------------------------------------------------------
// 아이들 모드: 2행 비우고 3행만 날씨/뉴스 스크롤
// --------------------------------------------------------

static void format_idle_rows(long long now,
                             char *row2,
                             char *row3)
{
    clear_line(row2);
    clear_line(row3);

    // 날씨/뉴스 데이터가 전혀 없으면 기존 폴백 문자열 사용
    if (!g_idle_weather_valid && !g_idle_rss_valid) {
        scroll_set_text(&g_scroll_idle, "WEATHER N/A  NEWS N/A");
        scroll_render_window(&g_scroll_idle, now, row3, 0, LCD_COLS);
        return;
    }

    const char *target = NULL;

    // 현재 phase에 따라 어떤 텍스트를 쓸지 결정
    if (g_idle_phase == IDLE_PHASE_WEATHER) {
        if (g_idle_weather_valid) {
            target = g_idle_weather_text;
        } else if (g_idle_rss_valid) {
            // 날씨가 없으면 바로 RSS phase로 전환
            g_idle_phase = IDLE_PHASE_RSS;
            target = g_idle_rss_text;
        }
    } else { // IDLE_PHASE_RSS
        if (g_idle_rss_valid) {
            target = g_idle_rss_text;
        } else if (g_idle_weather_valid) {
            // RSS가 없으면 날씨 phase로 되돌림
            g_idle_phase = IDLE_PHASE_WEATHER;
            target = g_idle_weather_text;
        }
    }

    if (!target) {
        // 방어 로직: 둘 다 invalid인 이상 상황
        scroll_set_text(&g_scroll_idle, "WEATHER N/A  NEWS N/A");
        scroll_render_window(&g_scroll_idle, now, row3, 0, LCD_COLS);
        return;
    }

    // 현재 phase에 맞는 텍스트 적용
    scroll_set_text(&g_scroll_idle, target);

    // 스크롤 한 번 돌기 전/후 wrap_count 비교
    unsigned int before_wrap = g_scroll_idle.wrap_count;

    scroll_render_window(&g_scroll_idle, now, row3, 0, LCD_COLS);

    if (g_scroll_idle.wrap_count != before_wrap) {
        // 한 번 완전히 스크롤해서 처음으로 돌아온 시점
        g_idle_last_wrap = g_scroll_idle.wrap_count;

        if (g_idle_phase == IDLE_PHASE_WEATHER) {
            if (g_idle_rss_valid) {
                g_idle_phase = IDLE_PHASE_RSS;
            }
        } else {
            if (g_idle_weather_valid) {
                g_idle_phase = IDLE_PHASE_WEATHER;
            }
        }
    }
}




// --------------------------------------------------------
// Kodi에서 날씨를 받아와서 IDLE 스크롤 텍스트로 꽂기
//  - 너무 자주 부르지 않도록 10분에 한 번만 갱신
//  - 실제 텍스트는 idle_set_weather_text()로 넘김
// --------------------------------------------------------
static void update_idle_weather_from_kodi(long long now_ms, int kodi_ok)
{
    // 10분마다 한 번만 요청
    const long long WEATHER_UPDATE_INTERVAL_MS = 600000; // 10 * 60 * 1000
    static long long last_weather_update_ms = 0;

    if (!kodi_ok) {
        // Kodi 연결 안 되어 있으면 날씨 요청 안 함
        return;
    }

    if (last_weather_update_ms != 0 &&
        (now_ms - last_weather_update_ms) < WEATHER_UPDATE_INTERVAL_MS) {
        // 아직 10분 안 지났으면 그냥 리턴
        return;
    }

    kodi_weather_info_t w;
    if (kodi_get_weather(&w) != 0) {
        // JSON-RPC 실패했으면 건드리지 않음
        return;
    }

    // Kodi가 아직 날씨를 못 받아온 상태면 InfoLabel이 전부 빈 문자열일 수 있으니 체크
    if (!w.location[0] &&
        !w.conditions[0] &&
        !w.temp_current[0] &&
        !w.temp_high_today[0] &&
        !w.temp_low_today[0]) {
        // 전부 비어 있으면 기존 IDLE 텍스트 유지
        return;
    }

    char line[256];

    // 울애기가 말한 포맷:
    // {상태 텍스트} / {현재 온도} {오늘 최고} {오늘 최저} / TOMORROW {내일 최고} {내일 최저}
    // Kodi InfoLabel들은 이미 "21°C" 같은 형태라서 그냥 이어붙이면 됨.
    snprintf(line, sizeof(line),
             "%s / %s / %s %s %s / TOMORROW %s %s",
             w.conditions[0]      ? w.conditions      : "WEATHER",  // 맨 앞: 날씨 상태(없으면 WEATHER)
             w.location[0]        ? w.location        : "",         // 위치(도시 이름)
             w.temp_current[0]    ? w.temp_current    : "",         // 현재 온도
             w.temp_high_today[0] ? w.temp_high_today : "",         // 오늘 최고
             w.temp_low_today[0]  ? w.temp_low_today  : "",         // 오늘 최저
             w.temp_high_tom[0]   ? w.temp_high_tom   : "",         // 내일 최고
             w.temp_low_tom[0]    ? w.temp_low_tom    : "");        // 내일 최저

    // 여기서 idle 스크롤 엔진에 텍스트 꽂아주기
    // (이 함수는 이전에 우리가 만든 idle_phase / wrap_count 시스템이 쓰는 그 함수)
    idle_set_weather_text(line);

    last_weather_update_ms = now_ms;
}

// Kodi 상태에 따라 RGB 백라이트 색상 지정
//  - common-anode: 0 = 가장 밝음, 255 = 꺼짐
//  - 재생 중:   청록색에 R만 80 정도 더 밝게 (살짝 따뜻한 청록)
//  - 일시정지: 노란색에 B만 80 정도 더 밝게 (따뜻한 흰색 쪽)
//  - 정지/아이들: 흰색 최대 밝기
// Kodi 상태에 따라 RGB 백라이트 색상 지정
//  - common-anode: 0 = 가장 밝음, 255 = 꺼짐
//  - 재생 중:   청록색에 R만 80 정도 더 밝게 (살짝 따뜻한 청록)
//  - 일시정지: 노란색에 B만 80 정도 더 밝게 (따뜻한 흰색 쪽)
//  - 정지/아이들: 흰색 최대 밝기
#define PANEL_COLOR_FADE_MS (PANEL_SLEEP_FADE_MS / 2)

static void update_panel_from_state(int kodi_ok, const kodi_now_playing_t *np,
                                    int muted, long long now_ms)
{
    (void)muted;

    static int     prev_valid = 0;
    static uint8_t prev_r     = 255;
    static uint8_t prev_g     = 255;
    static uint8_t prev_b     = 255;

    // Kodi 자체가 죽어 있으면: 현재 색 유지 (sleep 페이드만 사용)
    // → 여기서 더 이상 에러색(청록)으로 바꾸지 않아서 "청록색 점프" 버그 제거
    if (!kodi_ok) {
        return;
    }

    uint8_t r, g, b;

    // 재생 항목이 없거나 완전 정지 상태: 흰색 최대 밝기
    if (!np || !np->active) {
        r = 0;
        g = 0;
        b = 0;
    } else if (!np->playing) {
        // 일시정지: 0,0,175
        r = 0;
        g = 0;
        b = 175;
    } else {
        // 재생 중: 175,0,0
        r = 175;
        g = 0;
        b = 0;
    }

    // 색이 실제로 바뀔 때만 페이드 시작
    if (!prev_valid || r != prev_r || g != prev_g || b != prev_b) {
        panel_set_rgb_fade(r, g, b, PANEL_COLOR_FADE_MS, now_ms);
        prev_r     = r;
        prev_g     = g;
        prev_b     = b;
        prev_valid = 1;
    }
}


// --------------------------------------------------------
// main
// --------------------------------------------------------

int main(void)
{
    if (lcd_init() < 0) {
        fprintf(stderr, "lcd_init failed\n");
        return 1;
    }

    lcd_clear();
    init_custom_chars();

    panel_hw_init();
    buttons_init();

    int kodi_ok    = 0;
    int kodi_volume = 80;
    int muted       = 0;

    kodi_now_playing_t np;
    memset(&np, 0, sizeof(np));

    // Kodi 초기 연결
    if (kodi_rpc_init("127.0.0.1", 8080, NULL, NULL) == 0) {
        int v = -1, m = -1;
        if (kodi_get_volume(&v, &m) == 0 && v >= 0 && v <= 100) {
            kodi_volume = v;
            kodi_ok     = 1;
        }
        if (m == 0 || m == 1)
            muted = m;

        if (kodi_ok)
            kodi_get_now_playing(&np);
    }

    // 인코더 초기화 (핀은 이전과 동일)
    encoder_init(ENC_VOL,  17, 27, 22);
    encoder_init(ENC_MENU, 16, 20, 21);

    // 스크롤러 초기값
    scroll_init(&g_scroll_title,  "", SCROLL_INTERVAL_PLAY_MS);
    scroll_init(&g_scroll_artist, "", SCROLL_INTERVAL_PLAY_MS);
    scroll_init(&g_scroll_idle,
                "WEATHER N/A  NEWS N/A",
                SCROLL_INTERVAL_IDLE_MS);
    scroll_init(&g_scroll_gui_window,  "", SCROLL_INTERVAL_MENU_MS);
    scroll_init(&g_scroll_gui_control, "", SCROLL_INTERVAL_MENU_MS);

        ui_mode_t mode = UI_PLAYBACK;
    int menu_active = 0;
    long long last_menu_input_ms = 0;
    long long last_poll_ms       = now_ms();
    long long last_retry_ms      = 0;        // Kodi 재연결 시도 타이머

    kodi_gui_info_t gui;
    memset(&gui, 0, sizeof(gui));

    int gui_inited   = 0;        // GUI 상태 한 번이라도 받아왔는지 여부
    int prev_kodi_ok = kodi_ok;  // 슬립 모드 페이드를 위한 이전 상태

    // 부팅 직후 Kodi 연결 상태에 맞춰 슬립/정상 밝기 초기화
    panel_set_sleep_mode(kodi_ok ? 0 : 1, now_ms());


    char row0[LCD_COLS];
    char row1[LCD_COLS];
    char row2[LCD_COLS];
    char row3[LCD_COLS];

    char prev0[LCD_COLS];
    char prev1[LCD_COLS];
    char prev2[LCD_COLS];
    char prev3[LCD_COLS];

    memset(prev0, 0xFF, sizeof(prev0));
    memset(prev1, 0xFF, sizeof(prev1));
    memset(prev2, 0xFF, sizeof(prev2));
    memset(prev3, 0xFF, sizeof(prev3));

    while (1) {
        long long now = now_ms();

        // ---------------------------
        // 볼륨 인코더
        // ---------------------------
        int d_vol      = encoder_get_delta(ENC_VOL);
        int vol_clicks = encoder_get_clicks(ENC_VOL);
        int vol_longs  = encoder_get_long_presses(ENC_VOL);

        if (kodi_ok) {
            // Kodi 살아 있을 때: 기존 동작 유지
            if (d_vol != 0) {
                kodi_volume += d_vol;
                if (kodi_volume < 0)   kodi_volume = 0;
                if (kodi_volume > 100) kodi_volume = 100;
                kodi_set_volume(kodi_volume);
                panel_beep_volume_click(kodi_volume);
            }

            if (vol_clicks > 0) {
                muted = !muted;
                kodi_set_mute(muted);
                panel_beep_short();
            }

            if (vol_longs > 0) {
                for (int i = 0; i < vol_longs; i++) {
                    // 재생 중일 때 현재 플레이어의 OSD 띄우기
                    kodi_input_show_osd();
                    panel_beep_ms(120);
                }
            }
        } else {
            // Kodi 꺼진 상태: 볼륨 버튼 클릭 = "전원 켜기"
            if (vol_clicks > 0) {
                // 1) TV 전원 ON (CEC)
                      //          system("sudo -u mediabox HOME=/home/mediabox DISPLAY=:0 /usr/bin/kodi-standalone &");
                // 3) JSON-RPC 재연결을 바로 시도하도록 타이머 리셋
                last_retry_ms = 0;


                panel_beep_short();
            }
            // d_vol, vol_longs 는 전원 꺼진 상태에서는 무시
        }


        // ---------------------------
        // 메뉴 인코더 → Kodi GUI 조작 / 알람 조절
        // ---------------------------
        int d_menu      = encoder_get_delta(ENC_MENU);
        int menu_clicks = encoder_get_clicks(ENC_MENU);
        int menu_longs  = encoder_get_long_presses(ENC_MENU);

        if (kodi_ok) {
            if (d_menu != 0 || menu_clicks > 0 || menu_longs > 0) {
                menu_active        = 1;
                last_menu_input_ms = now;
            }

            if (d_menu != 0) {
                int steps = d_menu;
                if (steps > 10)  steps = 10;
                if (steps < -10) steps = -10;

                int dir   = (steps > 0) ? 1 : -1;
                int count = (steps > 0) ? steps : -steps;

                // 방향에 따라 한 번만 비프 (800 / 1200 Hz)
                panel_beep_menu_click(dir);

                for (int i = 0; i < count; i++) {
                    if (dir > 0) kodi_input_down();
                    else         kodi_input_up();
                    usleep(2000);
                }
            }

            if (menu_clicks > 0) {
                // 클릭 수가 여러 개여도 한 번만 ENTER 로 처리 → 더블 입력 방지
                kodi_input_select();   // ENTER
                panel_beep_short();
            }

            if (menu_longs > 0) {
                for (int i = 0; i < menu_longs; i++) {
                    kodi_input_back();     // PREV
                    panel_beep_ms(120);
                }
            }
        } else {
            // Kodi 가 꺼져 있을 때: 메뉴 인코더로 알람 시간 조절
            if (d_menu != 0) {
                int step = d_menu;
                int delta_minutes = step * 5;   // 한 스텝당 5분씩

                int total = g_alarm.hour * 60 + g_alarm.minute + delta_minutes;
                int day_minutes = 24 * 60;
                // 0~23:59 범위로 wrap
                total %= day_minutes;
                if (total < 0) total += day_minutes;

                g_alarm.hour   = total / 60;
                g_alarm.minute = total % 60;

                // 회전 방향에 따라 800 / 1200 Hz 비프
                panel_beep_menu_click(step);
            }

            if (menu_clicks > 0) {
                // 클릭으로 알람 ON/OFF 토글
                g_alarm.enabled = !g_alarm.enabled;
                panel_beep_short();
            }

            // 롱프레스는 알람 UI에서는 사용하지 않음
        }



        // ---------------------------
        // 전면 버튼 (REW / PLAYPAUSE / FF)
        // ---------------------------
        {
            // 각 버튼별 롱프레스 상태
            static long long rew_hold_start_ms   = 0;
            static long long rew_last_repeat_ms  = 0;
            static int       rew_hold_active     = 0;

            static long long ff_hold_start_ms    = 0;
            static long long ff_last_repeat_ms   = 0;
            static int       ff_hold_active      = 0;

            static long long play_hold_start_ms  = 0;
            static int       play_hold_fired     = 0;

            const long long HOLD_START_MS   = 700;  // 0.7초 이상 눌러야 롱프레스
            const long long SEEK_REPEAT_MS  = 200;  // 재생 중 빨리감기/되감기 반복 간격
            const long long NAV_REPEAT_MS   = 200;  // 메뉴에서 좌우 이동 반복 간격

            // 짧은 클릭 이벤트는 여기서 한 번에 받아서 비움
            int rew_clicks  = button_get_clicks(BTN_REW);
            int ff_clicks   = button_get_clicks(BTN_FF);
            int play_clicks = button_get_clicks(BTN_PLAYPAUSE);

            // 지금 눌려 있는지 (손 떼는 순간만 보는 게 아니라 "현재 상태")
            int rew_down  = button_is_down(BTN_REW);
            int ff_down   = button_is_down(BTN_FF);
            int play_down = button_is_down(BTN_PLAYPAUSE);

            if (kodi_ok && np.active) {
                // =======================
                // 재생 중: 플레이어 제어
                // =======================

                // REW 짧게: 이전 트랙
                if (rew_clicks > 0 && !rew_hold_active) {
                    kodi_player_goto(np.playerid, "previous");
                    panel_beep_short();
                }

                // FF 짧게: 다음 트랙
                if (ff_clicks > 0 && !ff_hold_active) {
                    kodi_player_goto(np.playerid, "next");
                    panel_beep_short();
                }

                // PLAY 짧게: play/pause 토글
                if (play_clicks > 0 && !play_hold_fired) {
                    kodi_player_play_pause(np.playerid);
                    panel_beep_short();
                }

                // ----- REW 길게: 되감기 계속 -----
                if (!rew_down) {
                    rew_hold_start_ms  = 0;
                    rew_last_repeat_ms = 0;
                    rew_hold_active    = 0;
                } else {
                    if (rew_hold_start_ms == 0) {
                        rew_hold_start_ms  = now;
                        rew_last_repeat_ms = 0;
                    } else if (now - rew_hold_start_ms >= HOLD_START_MS) {
                        // 기준 시간 지나면, 손 떼지 않아도 반복 시킹
                        if (!rew_hold_active ||
                            (now - rew_last_repeat_ms) >= SEEK_REPEAT_MS) {
                            kodi_player_seek_small(np.playerid, -1);
                            panel_beep_ms(50);
                            rew_hold_active    = 1;
                            rew_last_repeat_ms = now;
                        }
                    }
                }

                // ----- FF 길게: 빨리감기 계속 -----
                if (!ff_down) {
                    ff_hold_start_ms  = 0;
                    ff_last_repeat_ms = 0;
                    ff_hold_active    = 0;
                } else {
                    if (ff_hold_start_ms == 0) {
                        ff_hold_start_ms  = now;
                        ff_last_repeat_ms = 0;
                    } else if (now - ff_hold_start_ms >= HOLD_START_MS) {
                        if (!ff_hold_active ||
                            (now - ff_last_repeat_ms) >= SEEK_REPEAT_MS) {
                            kodi_player_seek_small(np.playerid, +1);
                            panel_beep_ms(50);
                            ff_hold_active    = 1;
                            ff_last_repeat_ms = now;
                        }
                    }
                }

                // ----- PLAY 길게: STOP (한 번만) -----
                if (!play_down) {
                    play_hold_start_ms = 0;
                    play_hold_fired    = 0;
                } else {
                    if (play_hold_start_ms == 0) {
                        play_hold_start_ms = now;
                    } else if (!play_hold_fired &&
                               (now - play_hold_start_ms) >= HOLD_START_MS) {
                        kodi_player_stop(np.playerid);
                        panel_beep_ms(100);
                        play_hold_fired = 1;
                    }
                }
            } else {
                // =======================
                // 재생 없음(IDLE/메뉴): GUI 제어
                // =======================

                // 눌리지 않은 버튼은 상태 리셋
                if (!rew_down) {
                    rew_hold_start_ms  = 0;
                    rew_last_repeat_ms = 0;
                    rew_hold_active    = 0;
                }
                if (!ff_down) {
                    ff_hold_start_ms  = 0;
                    ff_last_repeat_ms = 0;
                    ff_hold_active    = 0;
                }
                if (!play_down) {
                    play_hold_start_ms = 0;
                    play_hold_fired    = 0;
                }

                // REW 짧게: 메뉴 오른쪽 이동
                if (rew_clicks > 0 && !rew_hold_active) {
                    kodi_input_right();
                    panel_beep_short();
                }

                // FF 짧게: 메뉴 왼쪽 이동
                if (ff_clicks > 0 && !ff_hold_active) {
                    kodi_input_left();
                    panel_beep_short();
                }

                // PLAY 짧게: 컨텍스트 메뉴
                if (play_clicks > 0 && !play_hold_fired) {
                    kodi_input_context_menu();
                    panel_beep_short();
                }

                // REW 길게: 오른쪽으로 계속 이동
                if (rew_down) {
                    if (rew_hold_start_ms == 0) {
                        rew_hold_start_ms  = now;
                        rew_last_repeat_ms = 0;
                    } else if (now - rew_hold_start_ms >= HOLD_START_MS) {
                        if (!rew_hold_active ||
                            (now - rew_last_repeat_ms) >= NAV_REPEAT_MS) {
                            kodi_input_right();
                            panel_beep_short();
                            rew_hold_active    = 1;
                            rew_last_repeat_ms = now;
                        }
                    }
                }

                // FF 길게: 왼쪽으로 계속 이동
                if (ff_down) {
                    if (ff_hold_start_ms == 0) {
                        ff_hold_start_ms  = now;
                        ff_last_repeat_ms = 0;
                    } else if (now - ff_hold_start_ms >= HOLD_START_MS) {
                        if (!ff_hold_active ||
                            (now - ff_last_repeat_ms) >= NAV_REPEAT_MS) {
                            kodi_input_left();
                            panel_beep_short();
                            ff_hold_active    = 1;
                            ff_last_repeat_ms = now;
                        }
                    }
                }

                // PLAY 길게: 컨텍스트 메뉴 한 번 (반복 없음)
                if (play_down) {
                    if (play_hold_start_ms == 0) {
                        play_hold_start_ms = now;
                    } else if (!play_hold_fired &&
                               (now - play_hold_start_ms) >= HOLD_START_MS) {
                        kodi_input_context_menu();
                        panel_beep_short();
                        play_hold_fired = 1;
                    }
                }

                if (!np.active)
                    g_file_index = -1;
            }
        }


        const long long RECONNECT_INTERVAL_MS = 3000;  // Kodi 오프라인일 때 재연결 간격(ms)

        // ---------------------------
        // Kodi 폴링 + 재연결 로직
        // ---------------------------
        if (kodi_ok) {
            // 정상 연결 상태: 짧은 주기로 상태만 폴링
            if ((now - last_poll_ms) >= POLL_INTERVAL_MS) {
                last_poll_ms = now;

                // 1) 재생 상태
                kodi_now_playing_t prev = np;
                int np_ok = (kodi_get_now_playing(&np) == 0);

                if (!np_ok) {
                    // 완전히 끊겼으면 재연결 모드 진입
                    kodi_ok       = 0;
                    gui_inited    = 0;
                    last_retry_ms = now;
                    np.active     = 0;


                } else {
                    // playlist position -> FILE index 동기화
                    if (np.active && np.playlist_pos > 0) {
                        g_file_index = np.playlist_pos;
                    } else {
                        // Kodi가 position을 알 수 없을 때는 stale 값 표시를 피하기 위해 비움
                        g_file_index = -1;
                    }

                    if (!prev.active && np.active) {
                        if (g_file_index < 1)
                            g_file_index = 1;
                    }
                    if (prev.active && !np.active) {
                        g_file_index = -1;
                    }


                    // 2) 볼륨 / 뮤트
                    int v = -1, m = -1;
                    if (kodi_get_volume(&v, &m) == 0 && v >= 0 && v <= 100) {
                        kodi_volume = v;
                    }
                    if (m == 0 || m == 1)
                        muted = m;

                    // 3) GUI 정보 (메뉴 activity 검출)
                    kodi_gui_info_t newgui;
                    memset(&newgui, 0, sizeof(newgui));
                    if (kodi_get_gui_info(&newgui) == 0) {
                        if (gui_inited) {
                            if (strcmp(gui.window_label,  newgui.window_label)  != 0 ||
                                strcmp(gui.control_label, newgui.control_label) != 0) {
                                menu_active        = 1;
                                last_menu_input_ms = now;
                            }
                        } else {
                            gui_inited = 1;  // 첫 번째는 baseline
                        }
                        gui = newgui;
                    }
                }
            }
        } else {
            // kodi_ok == 0 : Kodi가 꺼져 있거나 아직 안 뜬 상태 → 주기적으로 재연결
            const long long RECONNECT_INTERVAL_MS = 2000;  // 2초마다 한 번

            if (last_retry_ms == 0 || (now - last_retry_ms) >= RECONNECT_INTERVAL_MS) {
                last_retry_ms = now;

                // Kodi JSON-RPC 서버에 다시 붙어보기
                // (이전에 열려 있던 curl 핸들을 정리한 뒤 재초기화해서,
                //  장시간 Kodi 다운 후에도 깨끗하게 다시 연결되도록 한다)
                kodi_rpc_shutdown();

                if (kodi_rpc_init("127.0.0.1", 8080, NULL, NULL) == 0) {
                    int v = -1, m = -1;

                    if (kodi_get_volume(&v, &m) == 0 && v >= 0 && v <= 100) {
                        kodi_volume = v;
                        kodi_ok     = 1;
                    }
                    if (m == 0 || m == 1)
                        muted = m;

                    if (kodi_ok) {
                        // 연결되자마자 재생/GUI 상태를 한 번 읽어서
                        // LCD를 바로 IDLE/PLAYBACK 모드로 깨운다.
                        kodi_get_now_playing(&np);

                        kodi_gui_info_t newgui;
                        memset(&newgui, 0, sizeof(newgui));
                        if (kodi_get_gui_info(&newgui) == 0) {
                            gui        = newgui;
                            gui_inited = 1;
                        }
                    }
                }
            }
        }

        // ★ 여기 한 줄 추가 ★
        update_idle_weather_from_kodi(now, kodi_ok);
        handle_alarm(now, kodi_ok);

        // Kodi 연결 상태 변화에 따라 슬립 모드 전환 (최신 상태 기준)
        if (kodi_ok != prev_kodi_ok) {
            panel_set_sleep_mode(kodi_ok ? 0 : 1, now);
            prev_kodi_ok = kodi_ok;
        }


        // 메뉴 타임아웃
        if (menu_active && (now - last_menu_input_ms) > MENU_TIMEOUT_MS)
            menu_active = 0;

        media_type_t mt = detect_media_type(&np);
        update_pb_icon_from_np(&np);
        update_media_icon(mt);
        update_panel_from_state(kodi_ok, &np, muted, now);
        panel_led_tick(now);
	handle_alarm(now, kodi_ok);
	panel_alarm_tick(now);

        if (menu_active)
            mode = UI_MENU;
        else if (kodi_ok && np.active)
            mode = UI_PLAYBACK;
        else
            mode = UI_IDLE;

        // ------------------------------------------------
        // 라인 포맷
        // ------------------------------------------------
        format_line1(row0, kodi_ok, kodi_volume, muted);
                

	if (kodi_ok) {
            // Kodi 연결 있을 때만 4행 상태바 표시
            format_line4(row3, &np);
        } else {
            // Kodi 없으면 4행 전체 공백
            format_alarm_line(row3);
        }

        if (mode == UI_PLAYBACK) {
            format_playback_rows(&np, mt, now, row1, row2);
        } else if (mode == UI_MENU) {
            format_menu_rows(now, &gui, row1, row2);
        } else { // UI_IDLE
            format_idle_rows(now, row1, row2);
        }

        // ------------------------------------------------
        // 변경된 행만 LCD에 raw로 출력
        // (커스텀 글리프 0번 때문에 strcmp/널 종료 사용 금지)
        // ------------------------------------------------
        if (memcmp(row0, prev0, LCD_COLS) != 0) {
            lcd_write_row(0, row0);
            memcpy(prev0, row0, LCD_COLS);
        }
        if (memcmp(row1, prev1, LCD_COLS) != 0) {
            lcd_write_row(1, row1);
            memcpy(prev1, row1, LCD_COLS);
        }
        if (memcmp(row2, prev2, LCD_COLS) != 0) {
            lcd_write_row(2, row2);
            memcpy(prev2, row2, LCD_COLS);
        }
        if (memcmp(row3, prev3, LCD_COLS) != 0) {
            lcd_write_row(3, row3);
            memcpy(prev3, row3, LCD_COLS);
        }


        // LED 밝기/색 페이드 한 스텝 진행
        panel_led_tick(now);

        usleep(5000);
    }

    return 0;
}
