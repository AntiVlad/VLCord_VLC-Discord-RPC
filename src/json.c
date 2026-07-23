#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool json_extract_string(const char *json, const char *key, char *out_val, size_t out_max)
{
    if (!json || !key || !out_val || out_max == 0) return false;
    out_val[0] = '\0';

    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *pos = strstr(json, pattern);
    if (!pos) return false;

    pos += strlen(pattern);
    while (*pos == ' ' || *pos == '\t' || *pos == '\r' || *pos == '\n' || *pos == ':') {
        pos++;
    }

    if (*pos != '"') return false;
    pos++; // skip opening quote

    size_t i = 0;
    while (*pos != '\0' && *pos != '"' && i + 1 < out_max) {
        if (*pos == '\\' && *(pos + 1) != '\0') {
            pos++; // skip escape slash
        }
        out_val[i++] = *pos++;
    }
    out_val[i] = '\0';
    return i > 0;
}

bool json_extract_int(const char *json, const char *key, int *out_val)
{
    if (!json || !key || !out_val) return false;

    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *pos = strstr(json, pattern);
    if (!pos) return false;

    pos += strlen(pattern);
    while (*pos == ' ' || *pos == '\t' || *pos == '\r' || *pos == '\n' || *pos == ':') {
        pos++;
    }

    if (*pos == '\0') return false;
    *out_val = atoi(pos);
    return true;
}
