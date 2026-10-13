/**
 * @file Faults.h
 * @brief Fault bit texts of the eSmart3/4 charger and the JBD BMS.
 *
 * Header-only, no Arduino dependency (native unit tests). One table per
 * device; used by the web page (HTML list), /api/live (JSON array) and logs.
 * Bit numbers as in the libraries' isXxx() helpers.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace Faults {

static const char* const CHARGER[] = {
    "Battery over voltage",                // 0x001
    "PV over voltage",                     // 0x002
    "Charge over current",                 // 0x004
    "Discharge over current",              // 0x008
    "Battery temperature alarm",           // 0x010
    "Internal temperature alarm",          // 0x020
    "PV low voltage",                      // 0x040
    "Battery low voltage",                 // 0x080
    "Trip zero protection trigger",        // 0x100
    "In the control of manual switchgear", // 0x200
};
static constexpr uint8_t CHARGER_BITS = sizeof(CHARGER) / sizeof(*CHARGER);

static const char* const BMS[] = {
    "Cell block over voltage",       // 0x0001
    "Cell block under voltage",      // 0x0002
    "Battery over voltage",          // 0x0004
    "Battery under voltage",         // 0x0008
    "Charging over temperature",     // 0x0010
    "Charging low temperature",      // 0x0020
    "Discharging over temperature",  // 0x0040
    "Discharging low temperature",   // 0x0080
    "Charging over current",         // 0x0100
    "Discharging over current",      // 0x0200
    "Short circuit",                 // 0x0400
    "Frontend IC error",             // 0x0800
    "MOS software lockout",          // 0x1000
};
static constexpr uint8_t BMS_BITS = sizeof(BMS) / sizeof(*BMS);

/// Text of bit @p bit in @p table of @p n entries, nullptr if out of range.
inline const char* text(const char* const* table, uint8_t n, uint8_t bit) {
    return bit < n ? table[bit] : nullptr;
}

/**
 * @brief Append the active faults of @p bits as lines "<prefix>: <text><sep>".
 * @return length written (excluding NUL). On truncation the output ends in
 *         " ..." and still fits into @p cap including the NUL.
 */
inline size_t describe(char* out, size_t cap, const char* prefix,
                       const char* const* table, uint8_t n, uint16_t bits,
                       const char* sep) {
    if (!out || cap == 0) return 0;
    size_t len = 0;
    out[0] = '\0';
    for (uint8_t b = 0; b < n; b++) {
        if (!(bits & (1u << b))) continue;
        const char* parts[] = { prefix, ": ", table[b], sep };
        for (const char* p : parts) {
            for (; *p; p++) {
                if (len + 1 >= cap) {
                    static const char more[] = " ...";
                    size_t at = cap > sizeof(more) ? cap - sizeof(more) : 0;
                    for (size_t i = 0; more[i] && at + 1 < cap; i++) out[at++] = more[i];
                    out[at] = '\0';
                    return at;
                }
                out[len++] = *p;
            }
        }
    }
    out[len] = '\0';
    return len;
}

}  // namespace Faults
