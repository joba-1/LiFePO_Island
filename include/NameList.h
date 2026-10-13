/**
 * @file NameList.h
 * @brief Pick the n-th name from a comma separated list (e.g. the BMS
 *        temperature probe names from the build flag NTC_NAMES).
 *
 * Header-only, no Arduino dependency (native unit tests).
 */
#pragma once

#include <stddef.h>
#include <string.h>

namespace NameList {

/**
 * @brief Copy item @p index (0-based) of @p csv into @p out, trimmed.
 * @return false (and out = "") if there is no such non-empty item.
 */
inline bool nth(const char* csv, unsigned index, char* out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!csv) return false;
    const char* p = csv;
    for (unsigned i = 0; i < index; i++) {
        p = strchr(p, ',');
        if (!p) return false;
        p++;
    }
    const char* end = strchr(p, ',');
    if (!end) end = p + strlen(p);
    while (p < end && *p == ' ') p++;
    while (end > p && end[-1] == ' ') end--;
    size_t n = (size_t)(end - p);
    if (n == 0) return false;
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

}  // namespace NameList
