/**
 * @file TextEscape.h
 * @brief Escape untrusted text for JSON string values.
 *
 * Header-only, no Arduino dependency (native unit tests). Always
 * NUL-terminates and never splits an escape sequence: if the next
 * character (or its escape) does not fit, output stops before it.
 */
#pragma once

#include <stddef.h>
#include <string.h>

namespace TextEscape {

namespace detail {
inline bool put(char* out, size_t cap, size_t& len, const char* s) {
    size_t n = strlen(s);
    if (len + n >= cap) return false;
    memcpy(out + len, s, n);
    len += n;
    return true;
}
}  // namespace detail

/**
 * @brief Escape @p src as the inside of a JSON string (no quotes added).
 * Quote and backslash are escaped, \n and \t too; other control characters
 * are dropped. Bytes >= 0x80 pass unchanged (UTF-8).
 */
inline size_t json(char* out, size_t cap, const char* src) {
    if (!out || cap == 0) return 0;
    size_t len = 0;
    for (const char* p = src ? src : ""; *p; p++) {
        unsigned char c = (unsigned char)*p;
        char one[2] = { *p, '\0' };
        const char* rep = one;
        if (c == '"') rep = "\\\"";
        else if (c == '\\') rep = "\\\\";
        else if (c == '\n') rep = "\\n";
        else if (c == '\t') rep = "\\t";
        else if (c < 0x20) continue;
        if (!detail::put(out, cap, len, rep)) break;
    }
    out[len] = '\0';
    return len;
}

}  // namespace TextEscape
