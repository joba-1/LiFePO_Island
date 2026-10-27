/**
 * @file IdleOff.h
 * @brief Switch the load off when nothing uses it: the inverter's own idle draw (about 10 W) is more than the
 *        panel yields on a winter day, so an inverter left on with nothing plugged in empties the battery.
 *
 * Header-only, no Arduino dependency (native unit tests); docs/spec-protection.md has the reasoning.
 *
 * The decision uses the mean load power since the window started, not single readings: an idle inverter can
 * pulse (0 / 11 W every few seconds). While the mean is at or above the threshold something is in use and the
 * window starts again (checked once the window is long enough to average the pulses), so the load goes off
 * about @c minutes after the last use. Readings without fresh data are not counted; the window waits.
 */
#pragma once

#include <stdint.h>

namespace IdleOff {

struct Config {
    uint16_t thresholdW = 15;       ///< mean below this = nothing in use (idle inverter: 6-11 W measured)
    uint16_t minutes = 30;          ///< that long below the threshold switches the load off; 0 = never
    uint16_t settleS = 120;         ///< the mean is trusted from this window length on (averages idle pulses)
};

class Watch {
public:
    explicit Watch(const Config& c = Config()) : _c(c) {}

    void configure(const Config& c) { _c = c; reset(); }
    const Config& config() const { return _c; }

    /**
     * @param loadOn   the load output is on
     * @param valid    @p loadW is fresh charger data
     * @param loadW    load power in W
     * @param dtS      seconds since the last call
     * @return true once: switch the load off now (the window starts again)
     */
    bool update(bool loadOn, bool valid, uint16_t loadW, uint32_t dtS) {
        if (!loadOn || !_c.minutes) { reset(); return false; }
        if (!valid || !dtS) return false;
        _windowS += dtS;
        _energyWs += (uint64_t)loadW * dtS;
        if (_windowS >= _c.settleS && meanDeciW() >= (uint32_t)_c.thresholdW * 10) {
            reset();                                    // in use
            return false;
        }
        if (_windowS >= (uint32_t)_c.minutes * 60) {
            _lastMeanDeciW = meanDeciW();
            reset();
            return true;
        }
        return false;
    }

    void reset() { _windowS = 0; _energyWs = 0; }

    uint32_t windowS() const { return _windowS; }   ///< seconds of the current window
    /// mean load power of the current window in 0.1 W (0 while the window is empty)
    uint32_t meanDeciW() const { return _windowS ? (uint32_t)(_energyWs * 10 / _windowS) : 0; }
    /// mean of the window that switched the load off, in 0.1 W
    uint32_t lastMeanDeciW() const { return _lastMeanDeciW; }

private:
    Config   _c;
    uint32_t _windowS = 0;
    uint64_t _energyWs = 0;
    uint32_t _lastMeanDeciW = 0;
};

/// Sane settings for the API: threshold 1..500 W, 0 (off) or 1..1440 minutes
inline bool valid(const Config& c) {
    return c.thresholdW >= 1 && c.thresholdW <= 500 && c.minutes <= 1440;
}

}  // namespace IdleOff
