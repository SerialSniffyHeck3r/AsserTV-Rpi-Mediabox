#pragma once
#include <stdint.h>

int kodi_rpc_init(const char *host, int port,
                  const char *user, const char *pass);
void kodi_rpc_shutdown(void);

// volume: 0~100, muted: 0 또는 1
int kodi_get_volume(int *out_volume, int *out_muted);
int kodi_set_volume(int volume);
int kodi_set_mute(int muted);

// 현재 재생 상태/메타데이터용 구조체
typedef struct {
    int active;        // 0 = 재생 없음, 1 = active player 있음
    int playerid;      // Kodi player id
    char type[8];      // "audio", "video", "picture" 등

    char title[128];
    char artist[128];
    char album[128];
    char file[128];

    int time_sec;      // 현재 위치 (초)
    int total_sec;     // 전체 길이 (초)
    int percentage;    // 진행률 0~100
    int playing;       // 1 = 재생중(speed!=0), 0 = 일시정지/정지

    // 재생 상태
    int play_speed;    // Player.GetProperties.speed 원본 값 (0, 1, 2, -1, ...)

    // FILE: 표시용 (플레이리스트 포지션). 1-base, 0이면 unknown
    int playlist_pos;

    // 오디오 트랙 번호 (CD-DA 포함). 0이면 unknown
    int track_number;

    // ↓ 오디오 메타데이터
    int audio_bitrate_kbps;     // 오디오 비트레이트 (kbps)
    int audio_channels;         // 채널 수 (2, 6, 8 등). 0이면 unknown
    int audio_samplerate_hz;    // 샘플레이트 (Hz). 0이면 unknown

    // ↓ 비디오/이미지 메타데이터
    int    video_bitrate_kbps;  // 비디오 비트레이트 (kbps) (없으면 0)
    int    video_width;         // 비디오/이미지 가로 해상도
    int    video_height;        // 비디오/이미지 세로 해상도
    double video_fps;           // 비디오 프레임레이트 (예: 23.976)

    // ↓ 기타
    unsigned long long file_size_bytes;  // 사진용: Slideshow.Filesize 파싱해서 bytes로 저장
} kodi_now_playing_t;



// Player.GetActivePlayers + Player.GetItem + Player.GetProperties 래핑
int kodi_get_now_playing(kodi_now_playing_t *np);

// 날씨 정보 구조체
typedef struct {
    char location[64];          // 도시 이름
    char conditions[64];        // "Partly cloudy" 같은 상태
    char temp_current[16];      // 현재 기온 (예: "21°C")
    char temp_high_today[16];   // 오늘 최고기온
    char temp_low_today[16];    // 오늘 최저기온
    char temp_high_tom[16];     // 내일 최고기온
    char temp_low_tom[16];      // 내일 최저기온
} kodi_weather_info_t;

// XBMC.GetInfoLabels를 이용해 날씨 정보 가져오기
int kodi_get_weather(kodi_weather_info_t *out);


// --- Player control ---

int kodi_player_play_pause(int playerid);           // 재생/일시정지 토글
int kodi_player_stop(int playerid);                // 정지
int kodi_player_goto(int playerid, const char *to); // "next" / "previous"

// direction > 0 -> smallforward, direction < 0 -> smallbackward
int kodi_player_seek_small(int playerid, int direction);

// --- GUI / Input (메뉴/상태 표시용 추가) ---

typedef struct {
    char window_label[64];   // 현재 윈도우 라벨
    char control_label[64];  // 현재 포커스 컨트롤 라벨
} kodi_gui_info_t;

// GUI.GetProperties(currentwindow,currentcontrol)
int kodi_get_gui_info(kodi_gui_info_t *info);

// Input.* (GUI 내비게이션용)
int kodi_input_up(void);
int kodi_input_down(void);
int kodi_input_left(void);
int kodi_input_right(void);
int kodi_input_select(void);
int kodi_input_back(void);
int kodi_input_context_menu(void);   // ★ 추가: 컨텍스트 메뉴 (C 키)
// ★ 새로 추가: OSD / Info
int kodi_input_show_osd(void);
int kodi_input_info(void);
