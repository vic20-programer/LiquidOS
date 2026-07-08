// strutil.h — tiny freestanding string helpers. No libc <string.h> exists
// in this environment, so the handful of functions we need (compare,
// length, copy) are implemented directly.

#pragma once
#include <stddef.h>

namespace strutil {

inline size_t length(const char* s) {
    size_t n = 0;
    while (s[n] != '\0') n++;
    return n;
}

inline bool equals(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return false;
        a++;
        b++;
    }
    return *a == *b; // both must hit '\0' at the same time
}

// Compares only the first n characters - used for matching identifiers
// of known length against keyword literals.
inline bool equals_n(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

inline char* copy(char* dest, const char* src, size_t max_len) {
    size_t i = 0;
    while (src[i] != '\0' && i < max_len - 1) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
    return dest;
}

inline bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

inline bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

inline bool is_alnum(char c) {
    return is_alpha(c) || is_digit(c);
}

inline bool is_space(char c) {
    return c == ' ' || c == '\t';
}

} // namespace strutil
