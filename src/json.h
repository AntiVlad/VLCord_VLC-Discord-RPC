#ifndef JSON_H
#define JSON_H

#include <stdbool.h>
#include <stddef.h>

// Simple light string helper for JSON fields
bool json_extract_string(const char *json, const char *key, char *out_val, size_t out_max);
bool json_extract_int(const char *json, const char *key, int *out_val);

#endif // JSON_H
