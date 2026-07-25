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

/* ─────────────────────────────────────────────────────────────
 * HTTP Request Engine (WinHTTP)
 * ───────────────────────────────────────────────────────────── */
static bool http_request(const wchar_t *host, const wchar_t *path, const char *method,
                         const char *headers_extra, const char *post_data,
                         char *out_buf, size_t out_max)
{
    if (!host || !path || !out_buf || out_max == 0) return false;
    out_buf[0] = '\0';

    HINTERNET hSession = WinHttpOpen(L"VLC-Discord-RPC/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    HINTERNET hConnect = WinHttpConnect(hSession, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    wchar_t verb[16];
    mbstowcs(verb, method ? method : "GET", 16);

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, verb, path, NULL,
                                            WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            WINHTTP_FLAG_SECURE);
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
    if (res) res = WinHttpReceiveResponse(hRequest, NULL);

    if (res) {
        DWORD bytes_read = 0;
        size_t total_read = 0;
        char buffer[4096];
        while (WinHttpReadData(hRequest, buffer, sizeof(buffer) - 1, &bytes_read) && bytes_read > 0) {
            buffer[bytes_read] = '\0';
            if (total_read + bytes_read < out_max) {
                memcpy(out_buf + total_read, buffer, bytes_read);
                total_read += bytes_read;
            } else break;
        }
        out_buf[total_read] = '\0';
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return res && strlen(out_buf) > 0;
}

/* ─────────────────────────────────────────────────────────────
 * Parsing and Keyword Utilities
 * ───────────────────────────────────────────────────────────── */
static bool is_year(const char *str)
{
    if (!str || strlen(str) != 4) return false;
    int y = atoi(str);
    return (y >= 1900 && y <= 2099);
}

static bool is_anime_uploader(const char *tag)
{
    static const char *list[] = {
        "anidl", "subsplease", "horriblesubs", "erai-raws", "erai", "judas",
        "nyaa", "commie", "coalgirls", "david", "vivid", "golumpa", "animetime",
        "animetosho", "sakurato", "attkc", "ember", NULL
    };
    char lower[64];
    size_t i = 0;
    for (; tag[i] && i + 1 < sizeof(lower); i++)
        lower[i] = tolower((unsigned char)tag[i]);
    lower[i] = '\0';

    for (int k = 0; list[k]; k++) {
        if (strstr(lower, list[k])) return true;
    }
    return false;
}

static bool is_noise_keyword(const char *str)
{
    static const char *tags[] = {
        "1080p", "720p", "480p", "2160p", "4k", "hdr", "web-dl", "webrip", "web",
        "bluray", "bdrip", "dvdrip", "hdrip", "x264", "x265", "hevc", "h264",
        "aac", "dts", "ddp5", "5.1", "6ch", "repack", "yts", "pahe", "remux", NULL
    };
    char lower[64];
    size_t i = 0;
    for (; str[i] && i + 1 < sizeof(lower) && !isspace((unsigned char)str[i]); i++)
        lower[i] = tolower((unsigned char)str[i]);
    lower[i] = '\0';

    for (int k = 0; tags[k]; k++) {
        if (strcmp(lower, tags[k]) == 0) return true;
    }
    return false;
}

/* Parse raw filename into clean title, year, season, episode, and anime/TV hints */
static void parse_filename_ex(const char *in, char *out_title, size_t title_max,
                              int *out_year, int *out_season, int *out_episode,
                              bool *out_is_anime, bool *out_is_tv)
{
    if (!in || !out_title || title_max == 0) return;
    out_title[0] = '\0';
    if (out_year) *out_year = 0;
    if (out_season) *out_season = 0;
    if (out_episode) *out_episode = 0;
    if (out_is_anime) *out_is_anime = false;
    if (out_is_tv) *out_is_tv = false;

    /* 1. Strip directory path */
    const char *p = strrchr(in, '/');
    if (!p) p = strrchr(in, '\\');
    p = p ? p + 1 : in;

    char buf[512];
    size_t len = strlen(p);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    strncpy(buf, p, len);
    buf[len] = '\0';

    /* 2. Strip file extension */
    char *dot = strrchr(buf, '.');
    if (dot && (stricmp(dot, ".mkv") == 0 || stricmp(dot, ".mp4") == 0 ||
                stricmp(dot, ".avi") == 0 || stricmp(dot, ".webm") == 0 ||
                stricmp(dot, ".mp3") == 0 || stricmp(dot, ".flac") == 0)) {
        *dot = '\0';
    }

    /* 3. Strip leading uploader bracket e.g. [AniDL] or [SubsPlease] */
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
            if (is_anime_uploader(tag) && out_is_anime) *out_is_anime = true;
            start = close_b + 1;
            while (*start == ' ' || *start == '\t' || *start == '-' || *start == '_') start++;
        }
    }

    /* 4. Strip inline bracket contents */
    char clean_buf[512];
    size_t cb_len = 0;
    bool in_b = false;
    for (size_t i = 0; start[i] && cb_len + 1 < sizeof(clean_buf); i++) {
        char c = start[i];
        if (c == '[' || c == '(') { in_b = true; continue; }
        if (c == ']' || c == ')') { in_b = false; continue; }
        if (in_b) continue;
        clean_buf[cb_len++] = c;
    }
    clean_buf[cb_len] = '\0';

    /* Replace dots and underscores with space */
    for (size_t i = 0; i < cb_len; i++) {
        if (clean_buf[i] == '.' || clean_buf[i] == '_') clean_buf[i] = ' ';
    }

    /* Check SxxExx pattern */
    for (char *sp = clean_buf; *sp; sp++) {
        if ((sp[0] == 'S' || sp[0] == 's') && isdigit((unsigned char)sp[1]) && isdigit((unsigned char)sp[2]) &&
            (sp[3] == 'E' || sp[3] == 'e') && isdigit((unsigned char)sp[4]) && isdigit((unsigned char)sp[5])) {
            if (out_season) sscanf(sp + 1, "%d", out_season);
            if (out_episode) sscanf(sp + 4, "%d", out_episode);
            if (out_is_tv) *out_is_tv = true;
            *sp = '\0'; /* Truncate title before SxxExx */
            break;
        }
    }

    /* Check " - 01" anime episode dash */
    char *ep_dash = strstr(clean_buf, " - ");
    if (ep_dash) {
        const char *after = ep_dash + 3;
        if (isdigit((unsigned char)*after)) {
            *ep_dash = '\0';
            if (out_is_anime) *out_is_anime = true;
        }
    }

    /* 5. Tokenize words, extract Year, stop at year or quality noise */
    char temp[512];
    strncpy(temp, clean_buf, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';

    size_t out_len = 0;
    char *tok = strtok(temp, " ");
    while (tok && out_len + 1 < title_max) {
        if (is_year(tok)) {
            if (out_year) *out_year = atoi(tok);
            break; /* Stop title BEFORE the year */
        }
        if (is_noise_keyword(tok)) break;

        size_t tlen = strlen(tok);
        if (out_len + tlen + 1 < title_max) {
            if (out_len > 0) out_title[out_len++] = ' ';
            strcpy(out_title + out_len, tok);
            out_len += tlen;
        }
        tok = strtok(NULL, " ");
    }
    out_title[out_len] = '\0';
}

/* ─────────────────────────────────────────────────────────────
 * Artwork Provider API Resolvers
 * ───────────────────────────────────────────────────────────── */

/* Provider 1: AniList GraphQL API (Anime) */
static bool fetch_anilist_artwork(const char *query, char *out_url, size_t out_max)
{
    if (!query || strlen(query) == 0) return false;

    char json_body[512];
    snprintf(json_body, sizeof(json_body),
             "{\"query\":\"{ Media(search: \\\"%s\\\", type: ANIME) { coverImage { extraLarge large } } }\"}",
             query);

    char resp[16384];
    if (!http_request(L"graphql.anilist.co", L"/", "POST", "Content-Type: application/json\r\n", json_body, resp, sizeof(resp)))
        return false;

    return json_extract_string(resp, "extraLarge", out_url, out_max) ||
           json_extract_string(resp, "large", out_url, out_max);
}

/* Provider 2: OMDb API (Live-Action Movies & TV Series) */
static bool fetch_omdb_artwork(const char *query, int year, char *out_url, size_t out_max)
{
    if (!query || strlen(query) == 0) return false;

    char enc[256];
    size_t j = 0;
    for (size_t i = 0; query[i] && j + 4 < sizeof(enc); i++) {
        if (isalnum((unsigned char)query[i])) enc[j++] = query[i];
        else enc[j++] = '+';
    }
    enc[j] = '\0';

    wchar_t path[512];
    char resp[16384];
    char poster[512];

    /* 1. Try search `s=` with year */
    if (year > 0) {
        swprintf(path, 512, L"/?s=%hs&y=%d&apikey=trilogy", enc, year);
        if (http_request(L"www.omdbapi.com", path, "GET", NULL, NULL, resp, sizeof(resp))) {
            if (json_extract_string(resp, "Poster", poster, sizeof(poster)) && strcmp(poster, "N/A") != 0) {
                snprintf(out_url, out_max, "%s", poster);
                return true;
            }
        }
    }

    /* 2. Try search `s=` without year */
    swprintf(path, 512, L"/?s=%hs&apikey=trilogy", enc);
    if (http_request(L"www.omdbapi.com", path, "GET", NULL, NULL, resp, sizeof(resp))) {
        if (json_extract_string(resp, "Poster", poster, sizeof(poster)) && strcmp(poster, "N/A") != 0) {
            snprintf(out_url, out_max, "%s", poster);
            return true;
        }
    }

    /* 3. Try exact title lookup `t=` with year */
    if (year > 0) {
        swprintf(path, 512, L"/?t=%hs&y=%d&apikey=trilogy", enc, year);
        if (http_request(L"www.omdbapi.com", path, "GET", NULL, NULL, resp, sizeof(resp))) {
            if (json_extract_string(resp, "Poster", poster, sizeof(poster)) && strcmp(poster, "N/A") != 0) {
                snprintf(out_url, out_max, "%s", poster);
                return true;
            }
        }
    }

    /* 4. Try exact title lookup `t=` without year */
    swprintf(path, 512, L"/?t=%hs&apikey=trilogy", enc);
    if (http_request(L"www.omdbapi.com", path, "GET", NULL, NULL, resp, sizeof(resp))) {
        if (json_extract_string(resp, "Poster", poster, sizeof(poster)) && strcmp(poster, "N/A") != 0) {
            snprintf(out_url, out_max, "%s", poster);
            return true;
        }
    }

    return false;
}

/* Provider 3: TVmaze API (TV Shows Fallback) */
static bool fetch_tvmaze_artwork(const char *query, char *out_url, size_t out_max)
{
    if (!query || strlen(query) == 0) return false;

    char enc[256];
    size_t j = 0;
    for (size_t i = 0; query[i] && j + 4 < sizeof(enc); i++) {
        if (isalnum((unsigned char)query[i])) enc[j++] = query[i];
        else enc[j++] = '+';
    }
    enc[j] = '\0';

    wchar_t path[512];
    swprintf(path, 512, L"/singlesearch/shows?q=%hs", enc);

    char resp[16384];
    if (!http_request(L"api.tvmaze.com", path, "GET", NULL, NULL, resp, sizeof(resp)))
        return false;

    const char *img_block = strstr(resp, "\"image\"");
    if (img_block) {
        return json_extract_string(img_block, "original", out_url, out_max) ||
               json_extract_string(img_block, "medium", out_url, out_max);
    }
    return false;
}

/* Provider 4: iTunes Store API */
static bool fetch_itunes_artwork(const char *query, const char *entity, char *out_url, size_t out_max)
{
    if (!query || strlen(query) == 0) return false;

    char enc[256];
    size_t j = 0;
    for (size_t i = 0; query[i] && j + 4 < sizeof(enc); i++) {
        if (isalnum((unsigned char)query[i])) enc[j++] = query[i];
        else enc[j++] = '+';
    }
    enc[j] = '\0';

    wchar_t path[512];
    if (entity && strlen(entity) > 0)
        swprintf(path, 512, L"/search?term=%hs&entity=%hs&limit=1", enc, entity);
    else
        swprintf(path, 512, L"/search?term=%hs&limit=1", enc);

    char resp[16384];
    if (!http_request(L"itunes.apple.com", path, "GET", NULL, NULL, resp, sizeof(resp)))
        return false;

    char img[512];
    if (json_extract_string(resp, "artworkUrl100", img, sizeof(img)) ||
        json_extract_string(resp, "artworkUrl60",  img, sizeof(img)) ||
        json_extract_string(resp, "artworkUrl30",  img, sizeof(img))) {
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

/* Provider 5: MusicBrainz + Cover Art Archive */
static bool fetch_musicbrainz_artwork(const char *artist, const char *album, char *out_url, size_t out_max)
{
    if (!artist || !album) return false;

    char enc_artist[256], enc_album[256];
    size_t j = 0;
    for (size_t i = 0; artist[i] && j + 4 < sizeof(enc_artist); i++) {
        if (isalnum((unsigned char)artist[i])) enc_artist[j++] = artist[i];
        else enc_artist[j++] = '+';
    }
    enc_artist[j] = '\0';

    j = 0;
    for (size_t i = 0; album[i] && j + 4 < sizeof(enc_album); i++) {
        if (isalnum((unsigned char)album[i])) enc_album[j++] = album[i];
        else enc_album[j++] = '+';
    }
    enc_album[j] = '\0';

    wchar_t path[512];
    swprintf(path, 512, L"/ws/2/release/?query=artist:\"%hs\"%%20AND%%20release:\"%hs\"&fmt=json",
             enc_artist, enc_album);

    char resp[16384];
    if (!http_request(L"musicbrainz.org", path, "GET", NULL, NULL, resp, sizeof(resp)))
        return false;

    char mbid[128];
    if (json_extract_string(resp, "id", mbid, sizeof(mbid))) {
        snprintf(out_url, out_max, "https://coverartarchive.org/release/%s/front-500", mbid);
        return true;
    }
    return false;
}

/* Provider 6: TVDB API (Optional user key) */
static bool tvdb_login(const char *api_key)
{
    DWORD now = GetTickCount();
    if (g_tvdb_token[0] != '\0' && (now - g_tvdb_token_time < 3600000))
        return true;

    char body[256];
    snprintf(body, sizeof(body), "{\"apikey\":\"%s\"}", api_key);

    char resp[2048];
    if (!http_request(L"api4.thetvdb.com", L"/v4/login", "POST", NULL, body, resp, sizeof(resp)))
        return false;

    if (json_extract_string(resp, "token", g_tvdb_token, sizeof(g_tvdb_token))) {
        g_tvdb_token_time = now;
        return true;
    }
    return false;
}

static bool fetch_tvdb_artwork(const char *query, const char *type_str,
                               const char *api_key, char *out_url, size_t out_max)
{
    if (!tvdb_login(api_key)) return false;

    char enc_query[256];
    size_t j = 0;
    for (size_t i = 0; query[i] && j + 4 < sizeof(enc_query); i++) {
        if (isalnum((unsigned char)query[i])) enc_query[j++] = query[i];
        else enc_query[j++] = '+';
    }
    enc_query[j] = '\0';

    wchar_t path[512];
    swprintf(path, 512, L"/v4/search?query=%hs&type=%hs", enc_query, type_str);

    char auth_header[1200];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s\r\n", g_tvdb_token);

    char resp[16384];
    if (!http_request(L"api4.thetvdb.com", path, "GET", auth_header, NULL, resp, sizeof(resp)))
        return false;

    return json_extract_string(resp, "image_url", out_url, out_max) ||
           json_extract_string(resp, "image",     out_url, out_max);
}

/* ─────────────────────────────────────────────────────────────
 * Detection & Parsing Entry Point
 * ───────────────────────────────────────────────────────────── */
void artwork_detect_media(const char *title_meta, const char *artist_meta,
                          const char *album_meta, const char *filename,
                          media_info_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    /* 1. Music (Artist + Album present in ID3 tags) */
    if (artist_meta && strlen(artist_meta) > 0 &&
        album_meta  && strlen(album_meta)  > 0) {
        out->type = MEDIA_TYPE_MUSIC;
        snprintf(out->artist, sizeof(out->artist), "%s", artist_meta);
        snprintf(out->album,  sizeof(out->album),  "%s", album_meta);
        snprintf(out->title,  sizeof(out->title),  "%s",
                 (title_meta && strlen(title_meta) > 0) ? title_meta : "Track");
        return;
    }

    const char *raw_name = (filename && strlen(filename) > 0)
                           ? filename
                           : (title_meta ? title_meta : "");

    bool is_anime_hint = false, is_tv_hint = false;
    parse_filename_ex(raw_name, out->clean_name, sizeof(out->clean_name),
                      &out->year, &out->season, &out->episode,
                      &is_anime_hint, &is_tv_hint);

    if (is_anime_hint) {
        out->type = MEDIA_TYPE_ANIME;
        snprintf(out->show_name, sizeof(out->show_name), "%s", out->clean_name);
        snprintf(out->title,     sizeof(out->title),     "%s", out->clean_name);
        return;
    }

    if (is_tv_hint || out->season > 0) {
        out->type = MEDIA_TYPE_TV_SHOW;
        snprintf(out->show_name, sizeof(out->show_name), "%s", out->clean_name);
        snprintf(out->title,     sizeof(out->title),     "S%02dE%02d", out->season, out->episode);
        return;
    }

    /* Movie Default */
    out->type = MEDIA_TYPE_MOVIE;
    if (title_meta && strlen(title_meta) > 0 && !strstr(title_meta, ".mkv") && !strstr(title_meta, ".mp4"))
        snprintf(out->title, sizeof(out->title), "%s", title_meta);
    else
        snprintf(out->title, sizeof(out->title), "%s", out->clean_name);
}

/* ─────────────────────────────────────────────────────────────
 * Artwork Resolution with Cache
 * ───────────────────────────────────────────────────────────── */
bool artwork_get_url(const media_info_t *info, const char *tvdb_api_key,
                     char *out_url, size_t out_max)
{
    if (!info || !out_url || out_max == 0) return false;
    out_url[0] = '\0';

    char cache_key[256];
    switch (info->type) {
        case MEDIA_TYPE_MUSIC:
            snprintf(cache_key, sizeof(cache_key), "music:%s-%s", info->artist, info->album);
            break;
        case MEDIA_TYPE_TV_SHOW:
            snprintf(cache_key, sizeof(cache_key), "tv:%s", info->show_name);
            break;
        case MEDIA_TYPE_ANIME:
            snprintf(cache_key, sizeof(cache_key), "anime:%s", info->show_name[0] ? info->show_name : info->clean_name);
            break;
        default:
            snprintf(cache_key, sizeof(cache_key), "movie:%s-%d", info->title, info->year);
            break;
    }

    /* Check Cache */
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
    const char *q = (info->type == MEDIA_TYPE_TV_SHOW || info->type == MEDIA_TYPE_ANIME)
                    ? (info->show_name[0] ? info->show_name : info->clean_name)
                    : (info->clean_name[0] ? info->clean_name : info->title);

    switch (info->type) {
        case MEDIA_TYPE_ANIME:
            found = fetch_anilist_artwork(q, out_url, out_max);
            if (!found) found = fetch_tvmaze_artwork(q, out_url, out_max);
            if (!found) found = fetch_itunes_artwork(q, "tvShow", out_url, out_max);
            break;

        case MEDIA_TYPE_TV_SHOW:
            found = fetch_omdb_artwork(q, info->year, out_url, out_max);
            if (!found) found = fetch_tvmaze_artwork(q, out_url, out_max);
            if (!found && tvdb_api_key && strlen(tvdb_api_key) > 0)
                found = fetch_tvdb_artwork(q, "series", tvdb_api_key, out_url, out_max);
            if (!found) found = fetch_itunes_artwork(q, "tvShow", out_url, out_max);
            break;

        case MEDIA_TYPE_MOVIE:
            found = fetch_omdb_artwork(q, info->year, out_url, out_max);
            if (!found && tvdb_api_key && strlen(tvdb_api_key) > 0)
                found = fetch_tvdb_artwork(q, "movie", tvdb_api_key, out_url, out_max);
            if (!found) found = fetch_itunes_artwork(q, "movie", out_url, out_max);
            if (!found) found = fetch_itunes_artwork(q, NULL, out_url, out_max);
            break;

        case MEDIA_TYPE_MUSIC:
            found = fetch_musicbrainz_artwork(info->artist, info->album, out_url, out_max);
            if (!found) found = fetch_itunes_artwork(info->album, "album", out_url, out_max);
            break;

        default:
            found = fetch_omdb_artwork(q, info->year, out_url, out_max);
            if (!found) found = fetch_itunes_artwork(q, NULL, out_url, out_max);
            break;
    }

    /* Cache Result */
    if (found && strlen(out_url) > 0) {
        EnterCriticalSection(&g_cache_cs);
        int slot = rand() % CACHE_SIZE;
        snprintf(g_cache[slot].key,   sizeof(g_cache[slot].key),   "%s", cache_key);
        snprintf(g_cache[slot].url,   sizeof(g_cache[slot].url),   "%s", out_url);
        g_cache[slot].timestamp = now;
        LeaveCriticalSection(&g_cache_cs);
    }

    return found;
}
