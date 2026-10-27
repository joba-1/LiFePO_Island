/**
 * @file Protection.h
 * @brief Battery reserve policy: when to cut the load, when to let the ESP sleep, when to protect the cells.
 *
 * Header-only, no Arduino dependency (native unit tests); docs/spec-protection.md has the reasoning.
 *
 * Levels (state of charge = the counted value of the BMS, the lowest cell voltage as a second opinion):
 *
 *   NORMAL    everything allowed
 *   LOAD_CUT  the charger's load output must be off; nobody may switch it on. The charger and the BMS are
 *             meant to cut at loadOffSoc; this software follows softMarginSoc points lower, as a backstop.
 *   SLEEP     additionally the ESP sleeps most of the time (a bit after the load was cut, not at once)
 *   CRITICAL  cell damage is near: discharge FET off (if enabled), ESP sleeps with a long interval
 *
 * Leaving a level needs hysteresis (loadOnSoc for the load, +hysteresisSoc/Mv for sleep and critical).
 * Every change of level needs the same level to be wanted debounce evaluations in a row, so one bad
 * reading never cuts or releases anything.
 *
 * Counter offset: when the cell voltage cuts while the BMS counter still says more than the cut level, the
 * counter reads high (it does not see the idle draw). The difference is remembered and subtracted from the
 * counter until it reaches fullSoc (a full charge re-syncs it), so the release waits for real charge and the
 * load does not flap around the knee.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace Protection {

enum Level : uint8_t { NORMAL = 0, LOAD_CUT = 1, SLEEP = 2, CRITICAL = 3 };

inline const char* levelName(uint8_t level) {
    static const char* const names[] = { "normal", "load-cut", "sleep", "critical" };
    return level < 4 ? names[level] : "unknown";
}

struct Config {
    uint8_t  loadOffSoc = 15;       ///< % at which charger / BMS should cut the load
    uint8_t  loadOnSoc = 30;        ///< % needed to release the cut
    uint8_t  softMarginSoc = 2;     ///< software cuts this many points below loadOffSoc
    uint16_t loadCutCellMv = 3190;  ///< ... or when the lowest cell is below this (independent of the BMS counter)
    uint16_t resumeCellMv = 3200;   ///< lowest cell must also be above this to release the cut
    uint8_t  sleepSoc = 10;         ///< ESP sleep schedule from here (after sleepDelayS in LOAD_CUT)
    uint16_t sleepCellMv = 3150;
    uint32_t sleepDelayS = 1800;    ///< the sleep schedule starts this long after the load was cut
    uint8_t  criticalSoc = 3;       ///< cell damage near
    uint16_t criticalCellMv = 2950;
    uint8_t  hysteresisSoc = 5;     ///< to leave SLEEP / CRITICAL
    uint16_t hysteresisMv = 100;
    uint16_t sagMicroOhm = 1000;    ///< per cell: the cell voltage under discharge is lifted by I x this (empirical, below the p25 of the measured 1.1-1.6 mOhm: errs towards cutting early)
    uint16_t maxSagMv = 100;        ///< cap of that lift
    uint8_t  fullSoc = 100;         ///< the BMS re-syncs its counter here: the counter offset is cleared
    uint8_t  debounce = 6;          ///< evaluations in a row (6 x 10 s = 1 min)
    uint32_t sleepS = 1800;         ///< length of one sleep in SLEEP
    uint32_t criticalSleepS = 21600;///< ... in CRITICAL
};

struct Inputs {
    bool     socValid = false;
    uint8_t  soc = 100;             ///< % counted by the BMS
    uint16_t minCellMv = 0;         ///< 0 = unknown
    bool     loadKnown = false;
    bool     loadOn = false;
    int16_t  centiA = 0;            ///< BMS current in 10 mA (+ charge, - discharge): sag compensation of the cell voltage
};

/// Persistent state: copy it to RTC memory so it survives deep sleep.
struct State {
    uint8_t  level = NORMAL;
    uint8_t  wantLevel = NORMAL;
    uint8_t  wantCount = 0;
    uint32_t sinceCutS = 0;         ///< seconds since the load was cut (while level >= LOAD_CUT)
    uint8_t  socOffset = 0;         ///< points the BMS counter reads high (learned at a voltage cut, cleared at fullSoc)
};

/// State plus what the firmware keeps besides it, as an image for memory that survives software resets and
/// deep sleep (RTC_NOINIT_ATTR on the ESP32). After a power-on that memory holds garbage: load() checks magic,
/// ranges and a checksum. Plain data without initializers, so the startup code never touches it.
struct Persist {
    uint32_t magic;
    uint8_t  level, wantLevel, wantCount, socOffset, flags;
    uint32_t sinceCutS;
    uint32_t sleepS;                ///< length of the deep sleep just started (0: awake)
    uint32_t check;
};

static const uint32_t PERSIST_MAGIC = 0x4c694651u;

inline uint32_t persistCheck(const Persist& p) {
    uint32_t h = 2166136261u;                                              // FNV-1a over the fields, not the padding
    auto mix = [&h](uint32_t v, int bytes) {
        for (int i = 0; i < bytes; i++) { h ^= (v >> (8 * i)) & 0xff; h *= 16777619u; }
    };
    mix(p.magic, 4);
    mix(p.level, 1); mix(p.wantLevel, 1); mix(p.wantCount, 1); mix(p.socOffset, 1); mix(p.flags, 1);
    mix(p.sinceCutS, 4);
    mix(p.sleepS, 4);
    return h;
}

inline void save(Persist& p, const State& s, uint8_t flags, uint32_t sleepS) {
    p.magic = PERSIST_MAGIC;
    p.level = s.level; p.wantLevel = s.wantLevel; p.wantCount = s.wantCount; p.socOffset = s.socOffset;
    p.flags = flags;
    p.sinceCutS = s.sinceCutS;
    p.sleepS = sleepS;
    p.check = persistCheck(p);
}

/// false (nothing changed) if @p p does not hold a saved state
inline bool load(const Persist& p, State& s, uint8_t& flags, uint32_t& sleepS) {
    if (p.magic != PERSIST_MAGIC || p.check != persistCheck(p)) return false;
    if (p.level > CRITICAL || p.wantLevel > CRITICAL || p.socOffset > 100) return false;
    s.level = p.level; s.wantLevel = p.wantLevel; s.wantCount = p.wantCount; s.socOffset = p.socOffset;
    s.sinceCutS = p.sinceCutS;
    flags = p.flags;
    sleepS = p.sleepS;
    return true;
}

struct Output {
    Level    level = NORMAL;
    bool     changed = false;
    Level    previous = NORMAL;
    bool     loadInhibited = false; ///< level >= LOAD_CUT: nobody may switch the load on
    bool     forceLoadOff = false;  ///< inhibited and the load is on: switch it off
    bool     resumeLoad = false;    ///< protection ended: the load may be switched on again
    bool     sleepWanted = false;
    uint32_t sleepS = 0;
    bool     fetOff = false;        ///< level CRITICAL: discharge FET off
    uint8_t  socOffset = 0;         ///< State::socOffset after this evaluation
    char     reason[160] = "";      ///< why the level changed (empty if it did not)
};

class Engine {
public:
    explicit Engine(const Config& cfg = Config()) : _cfg(cfg) {}

    State& state() { return _st; }
    const State& state() const { return _st; }
    const Config& config() const { return _cfg; }

    /// Evaluate. @p dtS: seconds since the previous call (add the time asleep after a wake-up).
    Output update(const Inputs& in, uint32_t dtS) {
        Output out;
        if (_st.level >= LOAD_CUT) _st.sinceCutS += dtS;
        if (in.socValid && in.soc >= _cfg.fullSoc) _st.socOffset = 0;   // full: the BMS counter is right again

        Level want = wanted(in);
        out.previous = (Level)_st.level;
        if (want == _st.level) {
            _st.wantLevel = want;
            _st.wantCount = 0;
        }
        else {
            if (want == _st.wantLevel) {
                if (_st.wantCount < 255) _st.wantCount++;
            }
            else {
                _st.wantLevel = want;
                _st.wantCount = 1;
            }
            if (_st.wantCount >= _cfg.debounce) {
                uint8_t offset = want > _st.level ? learnedOffset(in, want) : 0;
                describe(out.reason, sizeof(out.reason), in, (Level)_st.level, want, offset);
                if (offset > _st.socOffset) _st.socOffset = offset;
                if (_st.level == NORMAL) _st.sinceCutS = 0;
                _st.level = want;
                _st.wantCount = 0;
                out.changed = true;
            }
        }

        out.level = (Level)_st.level;
        out.loadInhibited = _st.level >= LOAD_CUT;
        out.forceLoadOff = out.loadInhibited && in.loadKnown && in.loadOn;
        out.resumeLoad = out.changed && out.previous >= LOAD_CUT && out.level == NORMAL;
        out.sleepWanted = _st.level >= SLEEP;
        out.sleepS = _st.level == CRITICAL ? _cfg.criticalSleepS : (_st.level == SLEEP ? _cfg.sleepS : 0);
        out.fetOff = _st.level == CRITICAL;
        out.socOffset = _st.socOffset;
        return out;
    }

private:
    Level wanted(const Inputs& in) const {
        const uint8_t cur = _st.level;
        if (!in.socValid && !in.minCellMv) return (Level)cur;              // no data: hold, never release
        const uint8_t soc = in.socValid ? correctedSoc(in) : 100;           // unknown: the cells decide
        const uint16_t cell = in.minCellMv ? in.minCellMv : 65535;
        const uint8_t cutSoc = socThreshold(LOAD_CUT);

        // The cell voltage is informative on the knee below ~15-20 %. Under discharge the cell sags by I x R
        // (measured 1.1-2.0 mOhm per cell, 40-70 mV at 35 A): look at the voltage it would have at rest.
        // While charging it reads high, so a low value is low for real: no correction. The critical level uses
        // the raw voltage at any current (damage prevention errs towards early).
        const uint32_t eff = cell == 65535 ? 65535u : cell + sagMv(in);   // effective (rest) voltage
        bool enterCut = soc <= cutSoc || eff < _cfg.loadCutCellMv;
        bool enterSleep = soc <= _cfg.sleepSoc || eff < _cfg.sleepCellMv;
        bool enterCrit = soc <= _cfg.criticalSoc || cell < _cfg.criticalCellMv;
        // releasing needs a known state of charge: cell voltages alone are too flat to tell 15 % from 30 %
        bool stayCut = cur >= LOAD_CUT && (!in.socValid || !(soc >= _cfg.loadOnSoc && eff >= _cfg.resumeCellMv));
        bool staySleep = cur >= SLEEP && (!in.socValid || !(soc >= _cfg.sleepSoc + _cfg.hysteresisSoc && eff >= _cfg.sleepCellMv + _cfg.hysteresisMv));
        bool stayCrit = cur >= CRITICAL && (!in.socValid || !(soc >= _cfg.criticalSoc + _cfg.hysteresisSoc && cell >= _cfg.criticalCellMv + _cfg.hysteresisMv));

        Level want = NORMAL;
        if (enterCut || stayCut) want = LOAD_CUT;
        if (enterSleep || staySleep) want = (cur >= SLEEP || _st.sinceCutS >= _cfg.sleepDelayS) && want >= LOAD_CUT ? SLEEP : LOAD_CUT;
        if (enterSleep && cur == NORMAL) want = LOAD_CUT;                  // never straight to sleep
        if (enterCrit || stayCrit) want = CRITICAL;                        // damage prevention does not wait
        return want;
    }

    /// Lift of the cell voltage under discharge (mV): what the cell would read more at rest
    uint32_t sagMv(const Inputs& in) const {
        uint32_t mv = in.centiA < 0 ? ((uint32_t)(-(int32_t)in.centiA) * _cfg.sagMicroOhm) / 100000u : 0;
        return mv > _cfg.maxSagMv ? _cfg.maxSagMv : mv;
    }

    /// The BMS counter minus what it is known to read high
    uint8_t correctedSoc(const Inputs& in) const {
        return in.soc > _st.socOffset ? in.soc - _st.socOffset : 0;
    }

    uint8_t socThreshold(Level to) const {
        if (to == CRITICAL) return _cfg.criticalSoc;
        if (to == SLEEP) return _cfg.sleepSoc;
        return _cfg.loadOffSoc > _cfg.softMarginSoc ? _cfg.loadOffSoc - _cfg.softMarginSoc : 0;
    }

    uint16_t mvThreshold(Level to) const {
        return to == CRITICAL ? _cfg.criticalCellMv : (to == SLEEP ? _cfg.sleepCellMv : _cfg.loadCutCellMv);
    }

    /// The cell voltage alone asks for @p to (critical: raw, otherwise lifted by its sag)
    bool cellTriggers(const Inputs& in, Level to) const {
        if (!in.minCellMv) return false;
        return to == CRITICAL ? in.minCellMv < mvThreshold(to) : (uint32_t)in.minCellMv + sagMv(in) < mvThreshold(to);
    }

    /// Going down to @p to because of the cell voltage while the counter says more: the offset that makes the
    /// counter agree with the cells (0: nothing learned)
    uint8_t learnedOffset(const Inputs& in, Level to) const {
        if (!in.socValid) return 0;
        uint8_t soc = correctedSoc(in), thr = socThreshold(to);
        if (soc <= thr || !cellTriggers(in, to)) return 0;
        uint32_t offset = (uint32_t)_st.socOffset + (soc - thr);
        return offset > 100 ? 100 : (uint8_t)offset;
    }

    void describe(char* out, size_t cap, const Inputs& in, Level from, Level to, uint8_t learned) const {
        char trig[120] = "";
        if (to > from) {                                                    // going down: name the trigger
            uint8_t socThr = socThreshold(to);
            uint16_t mvThr = mvThreshold(to);
            if (in.socValid && correctedSoc(in) <= socThr) {
                if (_st.socOffset) {
                    snprintf(trig, sizeof(trig), "soc %u %% (counter %u %% - %u) <= %u %%", correctedSoc(in), in.soc,
                        _st.socOffset, socThr);
                }
                else {
                    snprintf(trig, sizeof(trig), "soc %u %% <= %u %%", in.soc, socThr);
                }
            }
            else if (cellTriggers(in, to)) {
                uint32_t sag = to == CRITICAL ? 0 : sagMv(in);
                int n;
                if (sag) {
                    n = snprintf(trig, sizeof(trig), "lowest cell %u mV + %u mV sag at %u.%u A < %u mV", in.minCellMv,
                        (unsigned)sag, (unsigned)(-in.centiA / 100), (unsigned)((-in.centiA % 100) / 10), mvThr);
                }
                else {
                    n = snprintf(trig, sizeof(trig), "lowest cell %u mV < %u mV", in.minCellMv, mvThr);
                }
                if (learned > _st.socOffset && n > 0 && (size_t)n < sizeof(trig)) {
                    snprintf(trig + n, sizeof(trig) - n, "; counter %u %% reads %u points high until full", in.soc, learned);
                }
            }
            else if (to == SLEEP || from >= SLEEP) {
                snprintf(trig, sizeof(trig), "load cut for %lu s", (unsigned long)_st.sinceCutS);
            }
        }
        else {                                                              // going up: the release that was passed
            uint8_t socThr = to == NORMAL ? _cfg.loadOnSoc
                : (to == LOAD_CUT ? (uint8_t)(_cfg.sleepSoc + _cfg.hysteresisSoc) : (uint8_t)(_cfg.criticalSoc + _cfg.hysteresisSoc));
            if (_st.socOffset) {
                snprintf(trig, sizeof(trig), "soc %u %% (counter %u %% - %u) >= %u %%, lowest cell %u mV",
                    in.socValid ? correctedSoc(in) : 0, in.soc, _st.socOffset, socThr, in.minCellMv);
            }
            else {
                snprintf(trig, sizeof(trig), "soc %u %% >= %u %%, lowest cell %u mV", in.socValid ? in.soc : 0, socThr,
                    in.minCellMv);
            }
        }
        snprintf(out, cap, "%s -> %s: %s", levelName(from), levelName(to), trig);
    }

    Config _cfg;
    State _st;
};

}  // namespace Protection
