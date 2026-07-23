#include "artwork.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <windows.h>
#include <winhttp.h>

#define CACHE_SIZE 32
typedef struct {
    char key[256];
    char url[512];
    DWORD timestamp;
} cache_entry_t;

static cache_entry_t g_cache[CACHE_SIZE];
static CRITICAL_SECTION g_cache_cs;

static char g_tvdb_token[1024] = {0};
static DWORD g_tvdb_token_time = 0;

void artwork_init(void)
{
    InitializeCriticalSection(&g_cache_cs);
    memset(g_cache, 0, sizeof(g_cache));
}

void artwork_cleanup(void)
{
    DeleteCriticalSection(&g_cache_cs);
}

// Perform HTTPS GET/POST request using WinHTTP
static bool http_request(const wchar_t *host, const wchar_t *path, const char *method,
                         const char *headers_extra, const char *post_data, char *out_buf, size_t out_max)
{
    if (!host || !path || !out_buf || out_max == 0) return false;
    out_buf[0] = '\0';

    HINTERNET hSession = WinHttpOpen(L"VLC-Discord-RPC/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    HINTERNET hConnect = WinHttpConnect(hSession, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    wchar_t verb[16];
    mbstowcs(verb, method ? method : "GET", 16);

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, verb, path, NULL, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    wchar_t headers[1024] = L"Content-Type: application/json\r\nUser-Agent: VLC-Discord-RPC/1.0\r\n";
    if (headers_extra) {
        wchar_t w_extra[512];
        mbstowcs(w_extra, headers_extra, 512);
        wcsncat(headers, w_extra, 1024 - wcslen(headers) - 1);
    }

    BOOL res = WinHttpSendRequest(hRequest, headers, (DWORD)wcslen(headers),
                                 post_data ? (LPVOID)post_data : NULL,
                                 post_data ? (DWORD)strlen(post_data) : 0,
                                 post_data ? (DWORD)strlen(post_data) : 0, 0);

    if (res) {
        res = WinHttpReceiveResponse(hRequest, NULL);
    }

    if (res) {
        DWORD bytes_read = 0;
        size_t total_read = 0;
        char buffer[4096];

        while (WinHttpReadData(hRequest, buffer, sizeof(buffer) - 1, &bytes_read) && bytes_read > 0) {
            buffer[bytes_read] = '\0';
            if (total_read + bytes_read < out_max) {
                memcpy(out_buf + total_read, buffer, bytes_read);
                total_read += bytes_read;
            } else {
                break;
            }
        }
        out_buf[total_read] = '\0';
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return res && strlen(out_buf) > 0;
}

static bool is_year_str(const char *str)
{
    if (!str || strlen(str) != 4) return false;
    if (str[0] != '1' && str[0] != '2') return false;
    return isdigit((unsigned char)str[0]) && isdigit((unsigned char)str[1]) &&
           isdigit((unsigned char)str[2]) && isdigit((unsigned char)str[3]);
}

static bool is_quality_keyword(const char *str)
{
    static const char *tags[] = {
        "1080p", "720p", "480p", "2160p", "4k", "hdr", "web-dl", "webrip", "web",
        "bluray", "bdrip", "dvdrip", "hdrip", "x264", "x265", "hevc", "h264",
        "aac", "dts", "ddp5", "5.1", "6ch", "repack", "yts", "pahe", "remux", NULL
    };

    char lower[64];
    size_t i = 0;
    for (; str[i] && i + 1 < sizeof(lower) && !isspace((unsigned char)str[i]); i++) {
        lower[i] = tolower((unsigned char)str[i]);
    }
    lower[i] = '\0';

    for (int k = 0; tags[k]; k++) {
        if (strcmp(lower, tags[k]) == 0) return true;
    }
    return false;
}

static bool is_anime_group(const char *bracket_text)
{
    static const char *anime_tags[] = {
        "anidl", "subsplease", "horriblesubs", "erai-raws", "erai", "judas",
        "nyaa", "commie", "coalgirls", "david", "vivid", "golumpa", NULL
    };

    char lower[64];
    size_t i = 0;
    for (; bracket_text[i] && i + 1 < sizeof(lower); i++) {
        lower[i] = tolower((unsigned char)bracket_text[i]);
    }
    lower[i] = '\0';

    for (int k = 0; anime_tags[k]; k++) {
        if (strstr(lower, anime_tags[k])) return true;
    }
    return false;
}

// Clean filename into printable search title
static void clean_filename_title_ex(const char *in, char *out, size_t out_max, bool *out_is_anime)
{
    if (!in || !out || out_max == 0) return;
    out[0] = '\0';
    if (out_is_anime) *out_is_anime = false;

    // 1. Strip directory path
    const char *p = strrchr(in, '/');
    if (!p) p = strrchr(in, '\\');
    p = p ? p + 1 : in;

    char buf[512];
    size_t len = strlen(p);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    strncpy(buf, p, len);
    buf[len] = '\0';

    // 2. Strip trailing file extension
    char *dot = strrchr(buf, '.');
    if (dot && (stricmp(dot, ".mkv") == 0 || stricmp(dot, ".mp4") == 0 ||
                stricmp(dot, ".avi") == 0 || stricmp(dot, ".webm") == 0 ||
                stricmp(dot, ".mp3") == 0 || stricmp(dot, ".flac") == 0)) {
        *dot = '\0';
    }

    // 3. Check leading bracket for anime uploader
    const char *start = buf;
    while (*start == ' ' || *start == '\t') start++;
    if (*start == '[' || *start == '(') {
        const char *close_b = strchr(start, *start == '[' ? ']' : ')');
        if (close_b) {
            char tag[64];
            size_t tlen = close_b - (start + 1);
            if (tlen >= sizeof(tag)) tlen = sizeof(tag) - 1;
            strncpy(tag, start + 1, tlen);
            tag[tlen] = '\0';

            if (is_anime_group(tag) && out_is_anime) {
                *out_is_anime = true;
            }

            start = close_b + 1;
            while (*start == ' ' || *start == '\t' || *start == '-' || *start == '_') start++;
        }
    }

    // 4. Strip trailing brackets
    char clean_buf[512];
    size_t cb_len = 0;
    bool in_bracket = false;
    for (size_t i = 0; start[i] != '\0' && cb_len + 1 < sizeof(clean_buf); i++) {
        char c = start[i];
        if (c == '[' || c == '(') in_bracket = true;
        if (c == ']' || c == ')') { in_bracket = false; continue; }
        if (in_bracket) continue;
        clean_buf[cb_len++] = c;
    }
    clean_buf[cb_len] = '\0';

    // 5. Replace dots and underscores with spaces
    for (size_t i = 0; i < cb_len; i++) {
        if (clean_buf[i] == '.' || clean_buf[i] == '_') {
            clean_buf[i] = ' ';
        }
    }

    // 6. Check for Anime episode separator
    char *ep_sep = strstr(clean_buf, " - ");
    if (ep_sep) {
        const char *after_dash = ep_sep + 3;
        if (isdigit((unsigned char)*after_dash)) {
            *ep_sep = '\0';
            if (out_is_anime) *out_is_anime = true; 
        }
    }

    // 7. Tokenize by spaces and check for Year
    char temp_tokens[512];
    strncpy(temp_tokens, clean_buf, sizeof(temp_tokens) - 1);
    temp_tokens[sizeof(temp_tokens) - 1] = '\0';

    size_t out_len = 0;
    char *token = strtok(temp_tokens, " ");
    while (token && out_len + 1 < out_max) {
        if (is_year_str(token)) {
            size_t tlen = strlen(token);
            if (out_len + tlen + 1 < out_max) {
                if (out_len > 0) out[out_len++] = ' ';
                strcpy(out + out_len, token);
                out_len += tlen;
            }
            break; 
        }

        if (is_quality_keyword(token)) {
            break; 
        }

        size_t tlen = strlen(token);
        if (out_len + tlen + 1 < out_max) {
            if (out_len > 0) out[out_len++] = ' ';
            strcpy(out + out_len, token);
            out_len += tlen;
        }
        token = strtok(NULL, " ");
    }
    out[out_len] = '\0';
}

static bool g_last_detected_is_anime = false;

void artwork_detect_media(const char *title_meta, const char *artist_meta,
                          const char *album_meta, const char *filename,
                          media_info_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    // 1. Music Detection
    if (artist_meta && strlen(artist_meta) > 0 && album_meta && strlen(album_meta) > 0) {
        out->type = MEDIA_TYPE_MUSIC;
        snprintf(out->artist, sizeof(out->artist), "%s", artist_meta);
        snprintf(out->album, sizeof(out->album), "%s", album_meta);
        snprintf(out->title, sizeof(out->title), "%s", (title_meta && strlen(title_meta) > 0) ? title_meta : "Track");
        return;
    }

    const char *raw_name = (filename && strlen(filename) > 0) ? filename : (title_meta ? title_meta : "");
    clean_filename_title_ex(raw_name, out->clean_name, sizeof(out->clean_name), &g_last_detected_is_anime);

    // 2. TV Show Pattern Detection
    int s = 0, e = 0;
    char show_buf[256] = {0};

    const char *s_match = NULL;
    for (const char *p = raw_name; *p; p++) {
        if ((p[0] == 'S' || p[0] == 's') && isdigit((unsigned char)p[1]) && isdigit((unsigned char)p[2]) &&
            (p[3] == 'E' || p[3] == 'e') && isdigit((unsigned char)p[4]) && isdigit((unsigned char)p[5])) {
            s_match = p;
            sscanf(p + 1, "%d", &s);
            sscanf(p + 4, "%d", &e);
            break;
        }
    }

    if (s_match) {
        out->type = MEDIA_TYPE_TV_SHOW;
        out->season = s;
        out->episode = e;

        size_t len = s_match - raw_name;
        if (len >= sizeof(show_buf)) len = sizeof(show_buf) - 1;
        strncpy(show_buf, raw_name, len);
        show_buf[len] = '\0';

        bool dummy_anime = false;
        clean_filename_title_ex(show_buf, out->show_name, sizeof(out->show_name), &dummy_anime);
        snprintf(out->title, sizeof(out->title), "S%02dE%02d", s, e);
        return;
    }

    // 3. Fallback: Movie
    out->type = MEDIA_TYPE_MOVIE;
    if (title_meta && strlen(title_meta) > 0) {
        snprintf(out->title, sizeof(out->title), "%s", title_meta);
    } else {
        snprintf(out->title, sizeof(out->title), "%s", out->clean_name);
    }
}

static void url_encode(const char *in, char *out, size_t out_max)
{
    size_t j = 0;
    for (size_t i = 0; in[i] != '\0' && j + 4 < out_max; i++) {
        if (isalnum((unsigned char)in[i])) {
            out[j++] = in[i];
        } else {
            snprintf(out + j, 4, "%%%02X", (unsigned char)in[i]);
            j += 3;
        }
    }
    out[j] = '\0';
}

// Fetch Anime Artwork from Kitsu API
static bool fetch_kitsu_artwork(const char *query, char *out_url, size_t out_max)
{
    if (!query || strlen(query) == 0) return false;

    char enc_query[256];
    url_encode(query, enc_query, sizeof(enc_query));

    wchar_t path[512];
    swprintf(path, 512, L"/api/edge/anime?filter[text]=%hs&page[limit]=1", enc_query);

    char resp[32768];
    if (!http_request(L"kitsu.io", path, "GET", NULL, NULL, resp, sizeof(resp))) {
        return false;
    }

    return json_extract_string(resp, "original", out_url, out_max) ||
           json_extract_string(resp, "large", out_url, out_max);
}

// Fetch Artwork from iTunes API
static bool fetch_itunes_artwork(const char *query, char *out_url, size_t out_max)
{
    if (!query || strlen(query) == 0) return false;

    char enc_query[256];
    url_encode(query, enc_query, sizeof(enc_query));

    wchar_t path[512];
    swprintf(path, 512, L"/search?term=%hs&limit=1", enc_query);

    char resp[16384];
    if (!http_request(L"itunes.apple.com", path, "GET", NULL, NULL, resp, sizeof(resp))) {
        return false;
    }

    char img[512];
    if (json_extract_string(resp, "artworkUrl100", img, sizeof(img)) ||
        json_extract_string(resp, "artworkUrl60", img, sizeof(img)) ||
        json_extract_string(resp, "artworkUrl30", img, sizeof(img))) {
        
        char *p = strstr(img, "100x100bb");
        if (!p) p = strstr(img, "60x60bb");
        if (!p) p = strstr(img, "30x30bb");
        if (p) {
            size_t prefix_len = p - img;
            snprintf(out_url, out_max, "%.*s1000x1000bb.jpg", (int)prefix_len, img);
        } else {
            snprintf(out_url, out_max, "%s", img);
        }
        return true;
    }
    return false;
}

// Fetch TVDB Auth Token
static bool tvdb_login(const char *api_key)
{
    DWORD now = GetTickCount();
    if (g_tvdb_token[0] != '\0' && (now - g_tvdb_token_time < 3600000)) {
        return true;
    }

    char body[256];
    snprintf(body, sizeof(body), "{\"apikey\":\"%s\"}", api_key);

    char resp[2048];
    if (!http_request(L"api4.thetvdb.com", L"/v4/login", "POST", NULL, body, resp, sizeof(resp))) {
        return false;
    }

    if (json_extract_string(resp, "token", g_tvdb_token, sizeof(g_tvdb_token))) {
        g_tvdb_token_time = now;
        return true;
    }
    return false;
}

// Fetch Artwork from TVDB
static bool fetch_tvdb_artwork(const char *query, const char *type_str, const char *api_key, char *out_url, size_t out_max)
{
    if (!tvdb_login(api_key)) return false;

    char enc_query[256];
    url_encode(query, enc_query, sizeof(enc_query));

    wchar_t path[512];
    swprintf(path, 512, L"/v4/search?query=%hs&type=%hs", enc_query, type_str);

    char auth_header[1200];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s\r\n", g_tvdb_token);

    char resp[16384];
    if (!http_request(L"api4.thetvdb.com", path, "GET", auth_header, NULL, resp, sizeof(resp))) {
        return false;
    }

    return json_extract_string(resp, "image_url", out_url, out_max) ||
           json_extract_string(resp, "image", out_url, out_max);
}

// Fetch Artwork from MusicBrainz
static bool fetch_musicbrainz_artwork(const char *artist, const char *album, char *out_url, size_t out_max)
{
    char enc_artist[256], enc_album[256];
    url_encode(artist, enc_artist, sizeof(enc_artist));
    url_encode(album, enc_album, sizeof(enc_album));

    wchar_t path[512];
    swprintf(path, 512, L"/ws/2/release/?query=artist:\"%hs\"%%20AND%%20release:\"%hs\"&fmt=json", enc_artist, enc_album);

    char resp[16384];
    if (!http_request(L"musicbrainz.org", path, "GET", NULL, NULL, resp, sizeof(resp))) {
        return false;
    }

    char mbid[128];
    if (json_extract_string(resp, "id", mbid, sizeof(mbid))) {
        snprintf(out_url, out_max, "https://coverartarchive.org/release/%s/front-500", mbid);
        return true;
    }
    return false;
}

bool artwork_get_url(const media_info_t *info, const char *tvdb_api_key, char *out_url, size_t out_max)
{
    if (!info || !out_url || out_max == 0) return false;
    out_url[0] = '\0';

    // Cache Key
    char cache_key[256];
    if (info->type == MEDIA_TYPE_MUSIC) {
        snprintf(cache_key, sizeof(cache_key), "music:%s-%s", info->artist, info->album);
    } else if (info->type == MEDIA_TYPE_TV_SHOW) {
        snprintf(cache_key, sizeof(cache_key), "tv:%s", info->show_name);
    } else {
        snprintf(cache_key, sizeof(cache_key), "movie:%s", info->title);
    }

    // Check Cache
    EnterCriticalSection(&g_cache_cs);
    DWORD now = GetTickCount();
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (strcmp(g_cache[i].key, cache_key) == 0 && (now - g_cache[i].timestamp < 3600000)) {
            snprintf(out_url, out_max, "%s", g_cache[i].url);
            LeaveCriticalSection(&g_cache_cs);
            return true;
        }
    }
    LeaveCriticalSection(&g_cache_cs);

    bool found = false;

    // 1. Try TVDB if API key provided
    if (info->type == MEDIA_TYPE_TV_SHOW && tvdb_api_key && strlen(tvdb_api_key) > 0) {
        found = fetch_tvdb_artwork(info->show_name, "series", tvdb_api_key, out_url, out_max);
    } else if (info->type == MEDIA_TYPE_MOVIE && tvdb_api_key && strlen(tvdb_api_key) > 0) {
        found = fetch_tvdb_artwork(info->title, "movie", tvdb_api_key, out_url, out_max);
    }

    // 2. Try MusicBrainz for Music
    if (!found && info->type == MEDIA_TYPE_MUSIC) {
        found = fetch_musicbrainz_artwork(info->artist, info->album, out_url, out_max);
    }

    const char *search_q = (info->type == MEDIA_TYPE_TV_SHOW && strlen(info->show_name) > 0) ? info->show_name :
                           (info->type == MEDIA_TYPE_MUSIC && strlen(info->album) > 0) ? info->album :
                           (strlen(info->clean_name) > 0) ? info->clean_name : info->title;

    // 3. Smart Ordering: Live Action -> iTunes FIRST, Anime -> Kitsu FIRST
    if (!found && search_q && strlen(search_q) > 0) {
        if (g_last_detected_is_anime) {
            // Anime: Try Kitsu first, fallback to iTunes
            found = fetch_kitsu_artwork(search_q, out_url, out_max) ||
                    fetch_itunes_artwork(search_q, out_url, out_max);
        } else {
            // Live-Action: Try iTunes first, fallback to Kitsu if Romaji title
            found = fetch_itunes_artwork(search_q, out_url, out_max) ||
                    fetch_kitsu_artwork(search_q, out_url, out_max);
        }
    }

    // Cache Result
    if (found && strlen(out_url) > 0) {
        EnterCriticalSection(&g_cache_cs);
        int slot = rand() % CACHE_SIZE;
        snprintf(g_cache[slot].key, sizeof(g_cache[slot].key), "%s", cache_key);
        snprintf(g_cache[slot].url, sizeof(g_cache[slot].url), "%s", out_url);
        g_cache[slot].timestamp = now;
        LeaveCriticalSection(&g_cache_cs);
    }

    return found;
}
