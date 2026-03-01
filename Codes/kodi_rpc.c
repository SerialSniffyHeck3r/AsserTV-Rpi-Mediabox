#include "kodi_rpc.h"
#include <curl/curl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>


static char g_url[128];
static char g_userpwd[128];
static int g_inited = 0;
static CURL *g_curl = NULL;
static struct curl_slist *g_headers = NULL;


struct resp_buf {
    char *buf;
    size_t size;
    size_t cap;
};

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    size_t total = size * nmemb;
    struct resp_buf *rb = (struct resp_buf *)userdata;

    size_t copy = total;
    if (rb->size + copy >= rb->cap) {
        if (rb->cap <= rb->size + 1) {
            copy = 0;
        } else {
            copy = rb->cap - rb->size - 1;
        }
    }

    if (copy > 0) {
        memcpy(rb->buf + rb->size, ptr, copy);
        rb->size += copy;
        rb->buf[rb->size] = '\0';
    }

    return total;
}

int kodi_rpc_init(const char *host, int port,
                  const char *user, const char *pass)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);

    snprintf(g_url, sizeof(g_url),
             "http://%s:%d/jsonrpc", host, port);

    g_userpwd[0] = '\0';
    if (user && user[0]) {
        snprintf(g_userpwd, sizeof(g_userpwd), "%s:%s", user, pass ? pass : "");
    }

    g_curl = curl_easy_init();
    if (!g_curl) {
        curl_global_cleanup();
        return -1;
    }

    g_headers = NULL;
    g_headers = curl_slist_append(g_headers, "Content-Type: application/json");

    curl_easy_setopt(g_curl, CURLOPT_URL, g_url);
    curl_easy_setopt(g_curl, CURLOPT_HTTPHEADER, g_headers);
    curl_easy_setopt(g_curl, CURLOPT_POST, 1L);
    curl_easy_setopt(g_curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(g_curl, CURLOPT_TIMEOUT, 2L);
    curl_easy_setopt(g_curl, CURLOPT_NOSIGNAL, 1L);

    // HTTP keep-alive (UI 폴링 레이턴시 ↓)
    curl_easy_setopt(g_curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(g_curl, CURLOPT_TCP_KEEPIDLE, 30L);
    curl_easy_setopt(g_curl, CURLOPT_TCP_KEEPINTVL, 30L);

    // Kodi에 인증이 걸려있으면(웹서버 사용자/비번) 여기서 사용
    if (g_userpwd[0]) {
        curl_easy_setopt(g_curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        curl_easy_setopt(g_curl, CURLOPT_USERPWD, g_userpwd);
    }

    g_inited = 1;
    return 0;
}

void kodi_rpc_shutdown(void)
{
    if (g_curl) {
        curl_easy_cleanup(g_curl);
        g_curl = NULL;
    }
    if (g_headers) {
        curl_slist_free_all(g_headers);
        g_headers = NULL;
    }

    if (g_inited) {
        curl_global_cleanup();
        g_inited = 0;
    }
}

static int kodi_rpc_call(const char *payload,
                         char *out_buf, size_t out_cap)
{
    if (!g_inited || !g_curl) return -1;

    struct resp_buf rb;
    rb.buf  = out_buf;
    rb.size = 0;
    rb.cap  = out_cap;

    out_buf[0] = '\0';

    // 요청마다 바뀌는 것만 세팅
    curl_easy_setopt(g_curl, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(g_curl, CURLOPT_POSTFIELDSIZE, (long)strlen(payload));
    curl_easy_setopt(g_curl, CURLOPT_WRITEDATA, &rb);

    CURLcode res = curl_easy_perform(g_curl);
    long http_code = 0;
    if (res == CURLE_OK) {
        curl_easy_getinfo(g_curl, CURLINFO_RESPONSE_CODE, &http_code);
    }

    if (res != CURLE_OK || http_code != 200) {
        fprintf(stderr,
                "kodi_rpc_call error: curl=%d (%s), http=%ld\n",
                (int)res,
                curl_easy_strerror(res),
                http_code);
        return -1;
    }

    return 0;
}

// ---- InfoLabels (video bitrate/fps 등) ----
// Kodi JSON-RPC 스키마상 currentvideostream에는 bitrate/fps가 없다.
// 그래서 XBMC.GetInfoLabels로 보조로 뽑아온다.
static int kodi_get_info_labels(const char **labels, int n,
                                char *out_buf, size_t out_cap)
{
    if (!labels || n <= 0) return -1;

    char payload[1024];
    int off = 0;

    off = snprintf(payload, sizeof(payload),
                   "{\"jsonrpc\":\"2.0\",\"id\":1,"
                   "\"method\":\"XBMC.GetInfoLabels\","
                   "\"params\":{\"labels\":[");
    if (off < 0 || off >= (int)sizeof(payload)) return -1;

    for (int i = 0; i < n; i++) {
        const char *lab = labels[i] ? labels[i] : "";
        int w = snprintf(payload + off, sizeof(payload) - (size_t)off,
                         "%s\"%s\"", (i == 0) ? "" : ",", lab);
        if (w < 0) return -1;
        off += w;
        if (off >= (int)sizeof(payload)) return -1;
    }

    int w = snprintf(payload + off, sizeof(payload) - (size_t)off, "]}}");
    if (w < 0) return -1;
    off += w;
    if (off >= (int)sizeof(payload)) return -1;

    return kodi_rpc_call(payload, out_buf, out_cap);
}


static int parse_bitrate_kbps_str(const char *s)
{
    if (!s || !*s) return 0;

    while (*s == ' ' || *s == '\t') s++;

    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s) return 0;

    while (*end == ' ' || *end == '\t') end++;

    // "Mbps", "Mb/s" 같은 경우 대비
    if (*end == 'M' || *end == 'm')
        v *= 1000.0;

    if (v < 0) v = 0;
    if (v > 99999.0) v = 99999.0;

    return (int)(v + 0.5);
}


// ---- 공용 파서 헬퍼들 ----

int parse_int_after_key_from(const char *start,
                                    const char *key,
                                    int def_val)
{
    const char *p = strstr(start, key);
    if (!p) return def_val;
    p = strchr(p, ':');
    if (!p) return def_val;

    int v;
    if (sscanf(p + 1, "%d", &v) == 1) {
        return v;
    }
    return def_val;
}

static double parse_double_after_key_from(const char *start,
                                          const char *key,
                                          double def_val)
{
    const char *p = strstr(start, key);
    if (!p) return def_val;
    p = strchr(p, ':');
    if (!p) return def_val;

    double v;
    if (sscanf(p + 1, "%lf", &v) == 1) {
        return v;
    }
    return def_val;
}


static void parse_string_field(const char *json,
                               const char *key,
                               char *out, size_t out_sz)
{
    out[0] = '\0';

    const char *p = strstr(json, key);
    if (!p) return;
    p = strchr(p, ':');
    if (!p) return;
    p++;

    // 공백 / [ 스킵 (artist: ["..."] 같은 케이스 처리)
    while (*p == ' ' || *p == '\t' || *p == '[') {
        p++;
    }
    if (*p != '"') return;
    p++;

    const char *q = strchr(p, '"');
    if (!q) return;

    size_t len = (size_t)(q - p);
    if (len >= out_sz) len = out_sz - 1;

    memcpy(out, p, len);
    out[len] = '\0';
}

static void parse_scalar_field(const char *json,
                               const char *key,
                               char *out, size_t out_sz)
{
    out[0] = '\0';
    const char *p = strstr(json, key);
    if (!p) return;

    p = strchr(p, ':');
    if (!p) return;
    p++;

    while (*p == ' ' || *p == '\t') p++;

    // string
    if (*p == '"') {
        p++;
        const char *q = strchr(p, '"');
        if (!q) return;

        size_t len = (size_t)(q - p);
        if (len >= out_sz) len = out_sz - 1;
        memcpy(out, p, len);
        out[len] = '\0';
        return;
    }

    // number / bare token
    const char *q = p;
    while (*q && *q != ',' && *q != '}' && *q != ']') q++;

    // trim right
    while (q > p && (q[-1] == ' ' || q[-1] == '\t' || q[-1] == '\r' || q[-1] == '\n'))
        q--;

    size_t len = (size_t)(q - p);
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, p, len);
    out[len] = '\0';
}

static int parse_kbps_from_text(const char *s)
{
    if (!s) return 0;

    // find first digit or dot
    while (*s && !isdigit((unsigned char)*s) && *s != '.') s++;
    if (!*s) return 0;

    char *end = NULL;
    double v = strtod(s, &end);
    if (!end || end == s) return 0;

    // optional unit handling (e.g. "0.26" Mb/s -> 260 kbps)
    while (*end == ' ' || *end == '\t') end++;
    if (*end == 'M' || *end == 'm') {
        v *= 1000.0;
    }

    if (v < 0) v = 0;
    return (int)(v + 0.5);
}



static int parse_time_object(const char *json, const char *key)
{
    const char *p = strstr(json, key);
    if (!p) return -1;

    int h = parse_int_after_key_from(p, "\"hours\"", 0);
    int m = parse_int_after_key_from(p, "\"minutes\"", 0);
    int s = parse_int_after_key_from(p, "\"seconds\"", 0);

    return h * 3600 + m * 60 + s;
}

static int parse_percentage(const char *json)
{
    const char *p = strstr(json, "\"percentage\"");
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p) return -1;

    double f = 0.0;
    if (sscanf(p + 1, "%lf", &f) == 1) {
        int v = (int)(f + 0.5);
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        return v;
    }
    return -1;
}

#include <ctype.h>
#include <stdlib.h>

static int parse_resolution_wh(const char *s, int *w, int *h)
{
    if (!s || !*s) return 0;

    while (*s && !isdigit((unsigned char)*s)) s++;
    if (!*s) return 0;

    char *end = NULL;
    long ww = strtol(s, &end, 10);
    if (!end || end == s) return 0;

    while (*end && !isdigit((unsigned char)*end)) end++;
    if (!*end) return 0;

    long hh = strtol(end, NULL, 10);
    if (ww <= 0 || hh <= 0) return 0;

    *w = (int)ww;
    *h = (int)hh;
    return 1;
}

static unsigned long long parse_size_to_bytes(const char *s)
{
    if (!s || !*s) return 0;

    while (*s && !isdigit((unsigned char)*s) && *s != '.') s++;
    if (!*s) return 0;

    char *end = NULL;
    double v = strtod(s, &end);
    if (!end || end == s) return 0;

    while (*end && isspace((unsigned char)*end)) end++;

    // "12345"만 오면 bytes로 취급
    if (!*end) {
        if (v < 0) v = 0;
        return (unsigned long long)(v + 0.5);
    }

    // Kodi Slideshow.Filesize가 "12.3 MB" / "800 KB" 같은 문자열로 오는 경우 대응
    if ((end[0] == 'K' || end[0] == 'k') && (end[1] == 'B' || end[1] == 'b'))
        return (unsigned long long)(v * 1024.0 + 0.5);
    if ((end[0] == 'M' || end[0] == 'm') && (end[1] == 'B' || end[1] == 'b'))
        return (unsigned long long)(v * 1024.0 * 1024.0 + 0.5);
    if ((end[0] == 'G' || end[0] == 'g') && (end[1] == 'B' || end[1] == 'b'))
        return (unsigned long long)(v * 1024.0 * 1024.0 * 1024.0 + 0.5);

    // 단위가 애매하면 bytes로 처리
    if (v < 0) v = 0;
    return (unsigned long long)(v + 0.5);
}



// ---- 기존 볼륨 관련 ----

static int parse_volume_response(const char *json,
                                 int *out_volume, int *out_muted)
{
    if (out_volume) *out_volume = -1;
    if (out_muted)  *out_muted  = -1;

    const char *p;

    if (out_volume) {
        p = strstr(json, "\"volume\"");
        if (p) {
            p = strchr(p, ':');
            if (p) {
                int vol;
                if (sscanf(p + 1, "%d", &vol) == 1) {
                    *out_volume = vol;
                }
            }
        }
    }

    if (out_muted) {
        p = strstr(json, "\"muted\"");
        if (p) {
            p = strchr(p, ':');
            if (p) {
                if (strncmp(p + 1, "true", 4) == 0) {
                    *out_muted = 1;
                } else if (strncmp(p + 1, "false", 5) == 0) {
                    *out_muted = 0;
                }
            }
        }
    }

    return 0;
}

static int json_get_value_by_key(const char *json,
                                 const char *key_no_quotes,
                                 char *out, size_t outsz)
{
    if (!json || !key_no_quotes || !out || outsz == 0) return 0;

    char needle[192];
    snprintf(needle, sizeof(needle), "\"%s\"", key_no_quotes);

    const char *p = strstr(json, needle);
    if (!p) return 0;

    p = strchr(p, ':');
    if (!p) return 0;
    p++;

    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;

    size_t n = 0;

    if (*p == '"') {
        p++; // skip opening quote
        while (*p && *p != '"' && n + 1 < outsz) {
            if (*p == '\\' && p[1]) { // minimal escape handling
                p++;
            }
            out[n++] = *p++;
        }
    } else {
        while (*p && *p != ',' && *p != '}' && *p != ']' && n + 1 < outsz) {
            out[n++] = *p++;
        }
        while (n > 0 && isspace((unsigned char)out[n - 1])) n--;
    }

    out[n] = '\0';
    if (strcmp(out, "null") == 0) out[0] = '\0';
    return 1;
}

static int str_empty_or_unknown(const char *s)
{
    if (!s || !s[0]) return 1;
    if (strcmp(s, "unknown") == 0) return 1;
    if (strcmp(s, "0.000") == 0) return 1;
    return 0;
}

// Kodi major version cache (한번만 조회)
static int g_kodi_major_ver = -2; // -2: unknown/unqueried
static int kodi_get_major_version_cached(void)
{
    if (g_kodi_major_ver != -2) return g_kodi_major_ver;

    // Application.GetProperties -> version (major/minor/patch)
    static const char *payload =
        "{\"jsonrpc\":\"2.0\",\"id\":1,"
        "\"method\":\"Application.GetProperties\","
        "\"params\":{\"properties\":[\"version\"]}}";

    char buf[1024];
    if (kodi_rpc_call(payload, buf, sizeof(buf)) != 0) {
        g_kodi_major_ver = -1;
        return g_kodi_major_ver;
    }

    const char *v = strstr(buf, "\"version\"");
    if (!v) { g_kodi_major_ver = -1; return g_kodi_major_ver; }

    int major = parse_int_after_key_from(v, "\"major\"", -1);
    g_kodi_major_ver = major;
    return g_kodi_major_ver;
}



int kodi_get_volume(int *out_volume, int *out_muted)
{
    static const char *payload =
        "{\"jsonrpc\":\"2.0\",\"id\":1,"
        "\"method\":\"Application.GetProperties\","
        "\"params\":{\"properties\":[\"volume\",\"muted\"]}}";

    char buf[1024];
    if (kodi_rpc_call(payload, buf, sizeof(buf)) != 0) {
        return -1;
    }

    parse_volume_response(buf, out_volume, out_muted);
    return 0;
}

int kodi_set_volume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"jsonrpc\":\"2.0\",\"id\":1,"
             "\"method\":\"Application.SetVolume\","
             "\"params\":{\"volume\":%d}}",
             volume);

    char buf[256];
    if (kodi_rpc_call(payload, buf, sizeof(buf)) != 0) {
        return -1;
    }
    return 0;
}

int kodi_set_mute(int muted)
{
    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"jsonrpc\":\"2.0\",\"id\":1,"
             "\"method\":\"Application.SetMute\","
             "\"params\":{\"mute\":%s}}",
             muted ? "true" : "false");

    char buf[256];
    if (kodi_rpc_call(payload, buf, sizeof(buf)) != 0) {
        return -1;
    }
    return 0;
}

// ---- Now Playing ----
int kodi_get_now_playing(kodi_now_playing_t *np)
{
    if (!np) return -1;
    memset(np, 0, sizeof(*np));
    np->playerid = -1;
    np->active   = 0;
    np->type[0]  = '\0';

    // 1) Active player 확인
    static const char *payload_active =
        "{\"jsonrpc\":\"2.0\",\"id\":1,"
        "\"method\":\"Player.GetActivePlayers\"}";

    char buf[2048];
    if (kodi_rpc_call(payload_active, buf, sizeof(buf)) != 0) {
        return -1;
    }

    const char *p = strstr(buf, "\"playerid\"");
    if (!p) {
        np->active = 0;
        return 0; // 재생 없음
    }

    np->active = 1;
    np->playerid = parse_int_after_key_from(p, "\"playerid\"", -1);
    parse_string_field(p, "\"type\"", np->type, sizeof(np->type));
    if (np->playerid < 0) {
        np->active = 0;
        return 0;
    }


    // 2) 현재 아이템(제목/아티스트/앨범/파일)
    char payload_item[512];
    snprintf(payload_item, sizeof(payload_item),
             "{\"jsonrpc\":\"2.0\",\"id\":2,"
             "\"method\":\"Player.GetItem\","
             "\"params\":{\"playerid\":%d,"
             "\"properties\":[\"title\",\"artist\",\"album\",\"file\"]}}",
             np->playerid);

    char buf_item[4096];
    if (kodi_rpc_call(payload_item, buf_item, sizeof(buf_item)) == 0) {
        parse_string_field(buf_item, "\"title\"",  np->title,  sizeof(np->title));
        parse_string_field(buf_item, "\"artist\"", np->artist, sizeof(np->artist));
        parse_string_field(buf_item, "\"album\"",  np->album,  sizeof(np->album));
        parse_string_field(buf_item, "\"file\"",   np->file,   sizeof(np->file));
    }

    // 2-1) 오디오일 때 트랙 번호는 InfoLabel에서 별도로 뽑는다.
    //      (Player.GetItem properties에 "track"을 얹으면 일부 Kodi 버전에서 응답이 깨지는 듯 해서 여기로 분리)
    np->track_number = 0;
    if (strcmp(np->type, "audio") == 0) {
        static const char *labels[] = { "MusicPlayer.TrackNumber" };
        char info_buf[256];

        if (kodi_get_info_labels(labels, 1, info_buf, sizeof(info_buf)) == 0) {
            // 응답 예:
            // {"id":1,"jsonrpc":"2.0","result":{"MusicPlayer.TrackNumber":"7"}}
            char tn_str[16];
            tn_str[0] = '\0';
            parse_string_field(info_buf,
                               "\"MusicPlayer.TrackNumber\"",
                               tn_str, sizeof(tn_str));
            if (tn_str[0]) {
                int tn = atoi(tn_str);
                if (tn > 0)
                    np->track_number = tn;
            }
        }
    }



    // 3) 플레이 상태 + position + current streams (bitrate/width/height)
    char payload_props[1024];
    snprintf(payload_props, sizeof(payload_props),
             "{\"jsonrpc\":\"2.0\",\"id\":3,"
             "\"method\":\"Player.GetProperties\","
             "\"params\":{\"playerid\":%d,"
             "\"properties\":["
             "\"speed\",\"time\",\"totaltime\",\"percentage\","
             "\"playlistid\",\"position\","
             "\"currentaudiostream\",\"currentvideostream\""
             "]}}",
             np->playerid);

    char buf_props[4096];
    if (kodi_rpc_call(payload_props, buf_props, sizeof(buf_props)) != 0) {
        return -1;
    }

    int speed = parse_int_after_key_from(buf_props, "\"speed\"", 0);
    np->play_speed = speed;      
np->playing = (speed != 0);
    


    np->time_sec  = parse_time_object(buf_props, "\"time\"");
    np->total_sec = parse_time_object(buf_props, "\"totaltime\"");
    np->percentage = parse_percentage(buf_props);

    // FILE: position (0-base) -> 1-base
    int pos0 = parse_int_after_key_from(buf_props, "\"position\"", -1);
    if (pos0 >= 0) np->playlist_pos = pos0 + 1;

    // currentaudiostream.bitrate -> kbps
    const char *aobj = strstr(buf_props, "\"currentaudiostream\"");
    if (aobj) {
        int br = parse_int_after_key_from(aobj, "\"bitrate\"", 0);
        // Kodi가 bps(160000)로 주는 경우도, kbps(160)로 주는 경우도 있어서 휴리스틱 처리
        if (br >= 10000) np->audio_bitrate_kbps = (br + 500) / 1000;
        else             np->audio_bitrate_kbps = br;
    }

    // currentvideostream.width/height
// currentaudiostream/currentvideostream (Kodi 스키마에 공식 존재)
const char *pa = strstr(buf_props, "\"currentaudiostream\"");
if (pa) {
    // bitrate: bits/sec -> kbps 로 변환
    int bits = parse_int_after_key_from(pa, "\"bitrate\"", 0); // bits/sec
    if (bits > 0) {
        np->audio_bitrate_kbps = (bits + 500) / 1000;
    }

    // channels: 정수 채널 수 (2, 6, 8 등)
    int ch = parse_int_after_key_from(pa, "\"channels\"", 0);
    if (ch > 0) {
        np->audio_channels = ch;
    }

    // samplerate: Hz
    int sr = parse_int_after_key_from(pa, "\"samplerate\"", 0);
    if (sr > 0) {
        np->audio_samplerate_hz = sr;
    }
}

const char *pv = strstr(buf_props, "\"currentvideostream\"");
if (pv) {
    int w = parse_int_after_key_from(pv, "\"width\"", 0);
    int h = parse_int_after_key_from(pv, "\"height\"", 0);
    if (w > 0) np->video_width  = w;
    if (h > 0) np->video_height = h;
}


    // 4) FPS는 Player.GetProperties에 없어서 InfoLabel로 보강
    //    Player.Process(videofps) 계열

    // ---- v22+ : "현재 재생 중 비디오 bitrate(kbps)"를 라벨에서 가져와서 최종값으로 사용 ----
    // (Kodi v21 이하면 이 라벨이 비어서 --- 나올 수 있음)
        // 4) Extra stream meta via InfoLabels (fps/width/height, and video bitrate on Kodi v22+)
    if (np->active && np->type[0]) {

        // v17+에서 안정적인 값: Player.Process(VideoFPS/VideoWidth/VideoHeight) :contentReference[oaicite:2]{index=2}
        // 비디오 bitrate는 공식적으로 ListItem.Property(Stream.Bitrate) (v22) :contentReference[oaicite:3]{index=3}
        const int major = kodi_get_major_version_cached();

        // 라벨은 "요청한 그대로" 키로 돌아오니까, 대소문자 버전 둘 다 요청해두면 안전함
        char payload_lbl[768];
        snprintf(payload_lbl, sizeof(payload_lbl),
                 "{\"jsonrpc\":\"2.0\",\"id\":1,"
                 "\"method\":\"XBMC.GetInfoLabels\","
                 "\"params\":{\"labels\":["
                 "\"Player.Process(VideoFPS)\",\"Player.Process(videofps)\","
                 "\"Player.Process(VideoWidth)\",\"Player.Process(videowidth)\","
                 "\"Player.Process(VideoHeight)\",\"Player.Process(videoheight)\","
                 "\"ListItem.Property(Stream.FPS)\","
                 "\"ListItem.Property(Stream.Bitrate)\""
                 "]}}");

        char buf_lbl[2048];
        if (kodi_rpc_call(payload_lbl, buf_lbl, sizeof(buf_lbl)) == 0) {

#ifdef KODI_RPC_DEBUG
            fprintf(stderr, "[KODI_RPC_DEBUG] GetInfoLabels=%s\n", buf_lbl);
#endif

            char tmp[96];

            // width
            tmp[0] = '\0';
            if (!json_get_value_by_key(buf_lbl, "Player.Process(VideoWidth)", tmp, sizeof(tmp)) || !tmp[0])
                json_get_value_by_key(buf_lbl, "Player.Process(videowidth)", tmp, sizeof(tmp));
            if (!str_empty_or_unknown(tmp)) np->video_width = atoi(tmp);

            // height
            tmp[0] = '\0';
            if (!json_get_value_by_key(buf_lbl, "Player.Process(VideoHeight)", tmp, sizeof(tmp)) || !tmp[0])
                json_get_value_by_key(buf_lbl, "Player.Process(videoheight)", tmp, sizeof(tmp));
            if (!str_empty_or_unknown(tmp)) np->video_height = atoi(tmp);

            // fps (v22 Stream.FPS 우선, 없으면 Player.Process(VideoFPS))
            tmp[0] = '\0';
            if (!json_get_value_by_key(buf_lbl, "ListItem.Property(Stream.FPS)", tmp, sizeof(tmp)) || !tmp[0]) {
                if (!json_get_value_by_key(buf_lbl, "Player.Process(VideoFPS)", tmp, sizeof(tmp)) || !tmp[0])
                    json_get_value_by_key(buf_lbl, "Player.Process(videofps)", tmp, sizeof(tmp));
            }
            if (!str_empty_or_unknown(tmp)) np->video_fps = atof(tmp);

            // video bitrate (Kodi v22+ only)
            np->video_bitrate_kbps = 0;
            if (major >= 22) {
                tmp[0] = '\0';
                if (json_get_value_by_key(buf_lbl, "ListItem.Property(Stream.Bitrate)", tmp, sizeof(tmp)) && tmp[0]) {
                    if (!str_empty_or_unknown(tmp)) {
                        double kbps = atof(tmp); // already kbps :contentReference[oaicite:4]{index=4}
                        if (kbps > 0.0) np->video_bitrate_kbps = (int)(kbps + 0.5);
                    }
                }
            }
        }
    }

// --- picture meta: resolution + filesize (Kodi v21에서도 동작) ---
if (np->active && strcmp(np->type, "picture") == 0) {

    // Slideshow.Resolution / Slideshow.Filesize는 InfoLabels에 있음 :contentReference[oaicite:8]{index=8}
    const char *payload =
        "{\"jsonrpc\":\"2.0\",\"id\":1,"
        "\"method\":\"XBMC.GetInfoLabels\","
        "\"params\":{\"labels\":["
            "\"Slideshow.Resolution\","
            "\"Slideshow.Filesize\","
            "\"Slideshow.Filename\""
        "]}}";

    char buf[2048];
    if (kodi_rpc_call(payload, buf, sizeof(buf)) == 0) {

        char tmp[128];

        // resolution: "1920 x 1080"
        tmp[0] = '\0';
        parse_scalar_field(buf, "\"Slideshow.Resolution\"", tmp, sizeof(tmp));
        int w = 0, h = 0;
        if (parse_resolution_wh(tmp, &w, &h)) {
            np->video_width  = w;
            np->video_height = h;
        } else {
            np->video_width = np->video_height = 0;
        }

        // filesize
        tmp[0] = '\0';
        parse_scalar_field(buf, "\"Slideshow.Filesize\"", tmp, sizeof(tmp));
        np->file_size_bytes = parse_size_to_bytes(tmp);
    }
}


    // 5) 오디오 메타 (채널 / 샘플레이트 / 비트레이트)를 InfoLabels로 보강
    //    - currentaudiostream에서 못 얻거나 이상한 값이 들어온 경우를 덮어쓴다.
    if (np->active) {
        static const char *alabels[] = {
            "Player.Process(AudioChannels)",
            "Player.Process(audiochannels)",
            "Player.Process(AudioSamplerate)",
            "Player.Process(audiosamplerate)",
            "MusicPlayer.BitRate"
        };
        char buf_a[512];

        if (kodi_get_info_labels(alabels, 5, buf_a, sizeof(buf_a)) == 0) {
            char tmp[64];

            // audio channels (1~16ch 범위만 신뢰)
            tmp[0] = '\0';
            if (!json_get_value_by_key(buf_a,
                                       "Player.Process(AudioChannels)",
                                       tmp, sizeof(tmp)) || !tmp[0]) {
                json_get_value_by_key(buf_a,
                                      "Player.Process(audiochannels)",
                                      tmp, sizeof(tmp));
            }
            if (!str_empty_or_unknown(tmp)) {
                int ch = atoi(tmp);
                if (ch > 0 && ch <= 16) {
                    np->audio_channels = ch;
                }
            }

            // audio samplerate (Hz, 8kHz~384kHz 범위만 신뢰)
            tmp[0] = '\0';
            if (!json_get_value_by_key(buf_a,
                                       "Player.Process(AudioSamplerate)",
                                       tmp, sizeof(tmp)) || !tmp[0]) {
                json_get_value_by_key(buf_a,
                                      "Player.Process(audiosamplerate)",
                                      tmp, sizeof(tmp));
            }
            if (!str_empty_or_unknown(tmp)) {
                int sr = atoi(tmp);
                if (sr >= 8000 && sr <= 384000) {
                    np->audio_samplerate_hz = sr;
                }
            }

            // MusicPlayer.BitRate (kbps) 가 들어오면 오디오 비트레이트를 이걸로 override
            tmp[0] = '\0';
            if (json_get_value_by_key(buf_a,
                                      "MusicPlayer.BitRate",
                                      tmp, sizeof(tmp)) && tmp[0]) {
                if (!str_empty_or_unknown(tmp)) {
                    int kb = atoi(tmp);
                    if (kb > 0 && kb < 100000) {
                        np->audio_bitrate_kbps = kb;
                    }
                }
            }
        }
    }


    return 0;
}


int kodi_get_gui_info(kodi_gui_info_t *info)
{
    if (!info) {
        return -1;
    }

    memset(info, 0, sizeof(*info));

    // GUI.GetProperties: currentwindow + currentcontrol
    static const char *payload =
        "{\"jsonrpc\":\"2.0\",\"id\":1,"
        "\"method\":\"GUI.GetProperties\","
        "\"params\":{\"properties\":[\"currentwindow\",\"currentcontrol\"]}}";

    char buf[1024];
    if (kodi_rpc_call(payload, buf, sizeof(buf)) != 0) {
        return -1;
    }

    // currentwindow.label
    const char *p_win = strstr(buf, "\"currentwindow\"");
    if (p_win) {
        // parse_string_field는 이미 위에서 now playing parsing에 쓰고 있던 그 함수
        parse_string_field(p_win, "\"label\"",
                           info->window_label,
                           sizeof(info->window_label));
    }

    // currentcontrol.label
    const char *p_ctrl = strstr(buf, "\"currentcontrol\"");
    if (p_ctrl) {
        parse_string_field(p_ctrl, "\"label\"",
                           info->control_label,
                           sizeof(info->control_label));
    }

    return 0;
}

int kodi_get_weather(kodi_weather_info_t *out)
{
    if (!out) return -1;

    memset(out, 0, sizeof(*out));

    // XBMC.GetInfoLabels 호출 페이로드
    static const char *payload =
        "{"
          "\"jsonrpc\":\"2.0\","
          "\"id\":1,"
          "\"method\":\"XBMC.GetInfoLabels\","
          "\"params\":{"
            "\"labels\":["
              "\"Weather.Location\","
              "\"Weather.Conditions\","
              "\"Weather.Temperature\","
              "\"Window(Weather).Property(Day[0].HighTemp)\","
              "\"Window(Weather).Property(Day[0].LowTemp)\","
              "\"Window(Weather).Property(Day[1].HighTemp)\","
              "\"Window(Weather).Property(Day[1].LowTemp)\""
            "]"
          "}"
        "}";

    char buf[2048];
    if (kodi_rpc_call(payload, buf, sizeof(buf)) != 0) {
        return -1;
    }

    // result 객체에서 각 필드를 문자열로 파싱
    // parse_string_field(json, "키", out, out_sz)
    // 여기서 키는 JSON 속성 이름 그대로 따옴표까지 넣어서 써야 함

    parse_string_field(buf,
                       "\"Weather.Location\"",
                       out->location,
                       sizeof(out->location));

    parse_string_field(buf,
                       "\"Weather.Conditions\"",
                       out->conditions,
                       sizeof(out->conditions));

    parse_string_field(buf,
                       "\"Weather.Temperature\"",
                       out->temp_current,
                       sizeof(out->temp_current));

    parse_string_field(buf,
                       "\"Window(Weather).Property(Day[0].HighTemp)\"",
                       out->temp_high_today,
                       sizeof(out->temp_high_today));

    parse_string_field(buf,
                       "\"Window(Weather).Property(Day[0].LowTemp)\"",
                       out->temp_low_today,
                       sizeof(out->temp_low_today));

    parse_string_field(buf,
                       "\"Window(Weather).Property(Day[1].HighTemp)\"",
                       out->temp_high_tom,
                       sizeof(out->temp_high_tom));

    parse_string_field(buf,
                       "\"Window(Weather).Property(Day[1].LowTemp)\"",
                       out->temp_low_tom,
                       sizeof(out->temp_low_tom));

    return 0;
}


int kodi_player_play_pause(int playerid)
{
    if (playerid < 0) return -1;

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"jsonrpc\":\"2.0\",\"id\":1,"
             "\"method\":\"Player.PlayPause\","
             "\"params\":{\"playerid\":%d}}",
             playerid);

    char buf[256];
    return kodi_rpc_call(payload, buf, sizeof(buf));
}

int kodi_player_stop(int playerid)
{
    if (playerid < 0) return -1;

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"jsonrpc\":\"2.0\",\"id\":1,"
             "\"method\":\"Player.Stop\","
             "\"params\":{\"playerid\":%d}}",
             playerid);

    char buf[256];
    return kodi_rpc_call(payload, buf, sizeof(buf));
}

int kodi_player_goto(int playerid, const char *to)
{
    if (playerid < 0 || !to) return -1;

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"jsonrpc\":\"2.0\",\"id\":1,"
             "\"method\":\"Player.GoTo\","
             "\"params\":{\"playerid\":%d,\"to\":\"%s\"}}",
             playerid, to);

    char buf[256];
    return kodi_rpc_call(payload, buf, sizeof(buf));
}

int kodi_player_seek_small(int playerid, int direction)
{
    if (playerid < 0) return -1;

    const char *val = (direction >= 0) ? "smallforward" : "smallbackward";

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"jsonrpc\":\"2.0\",\"id\":1,"
             "\"method\":\"Player.Seek\","
             "\"params\":{\"playerid\":%d,\"value\":\"%s\"}}",
             playerid, val);

    char buf[256];
    return kodi_rpc_call(payload, buf, sizeof(buf));
}


// --- GUI / Input control ---

// 공통으로 쓰는 작은 헬퍼: method 이름만 바꿔서 Input.Up/Down 등 호출
static int kodi_simple_input(const char *method)
{
    char payload[128];
    snprintf(payload, sizeof(payload),
             "{\"jsonrpc\":\"2.0\",\"id\":1,"
             "\"method\":\"%s\"}",
             method);

    char buf[128];
    return kodi_rpc_call(payload, buf, sizeof(buf));
}

int kodi_input_up(void)
{
    return kodi_simple_input("Input.Up");
}

int kodi_input_down(void)
{
    return kodi_simple_input("Input.Down");
}

int kodi_input_left(void)
{
    return kodi_simple_input("Input.Left");
}

int kodi_input_right(void)
{
    return kodi_simple_input("Input.Right");
}

int kodi_input_select(void)
{
    return kodi_simple_input("Input.Select");
}

int kodi_input_back(void)
{
    return kodi_simple_input("Input.Back");
}

int kodi_input_context_menu(void)
{
    return kodi_simple_input("Input.ContextMenu");
}

// ★ 새로 추가: OSD / Info
int kodi_input_show_osd(void)
{
    return kodi_simple_input("Input.ShowOSD");
}

int kodi_input_info(void)
{
    return kodi_simple_input("Input.Info");
}
