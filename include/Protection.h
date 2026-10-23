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
};

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
    char     reason[112] = "";      ///< why the level changed (empty if it did not)
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
                describe(out.reason, sizeof(out.reason), in, (Level)_st.level, want);
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
        return out;
    }

private:
    Level wanted(const Inputs& in) const {
        const uint8_t cur = _st.level;
        if (!in.socValid && !in.minCellMv) return (Level)cur;              // no data: hold, never release
        const uint8_t soc = in.socValid ? in.soc : 100;                    // unknown: the cells decide
        const uint16_t cell = in.minCellMv ? in.minCellMv : 65535;
        const uint8_t cutSoc = _cfg.loadOffSoc > _cfg.softMarginSoc ? _cfg.loadOffSoc - _cfg.softMarginSoc : 0;

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

    void describe(char* out, size_t cap, const Inputs& in, Level from, Level to) const {
        const uint8_t cutSoc = _cfg.loadOffSoc > _cfg.softMarginSoc ? _cfg.loadOffSoc - _cfg.softMarginSoc : 0;
        char trig[72] = "";
        if (to > from) {                                                    // going down: name the trigger
            uint8_t soc = in.socValid ? in.soc : 100;
            uint16_t cell = in.minCellMv ? in.minCellMv : 65535;
            uint8_t socThr = to == CRITICAL ? _cfg.criticalSoc : (to == SLEEP ? _cfg.sleepSoc : cutSoc);
            uint16_t mvThr = to == CRITICAL ? _cfg.criticalCellMv : (to == SLEEP ? _cfg.sleepCellMv : _cfg.loadCutCellMv);
            if (in.socValid && soc <= socThr) {
                snprintf(trig, sizeof(trig), "soc %u %% <= %u %%", soc, socThr);
            }
            else if (to == CRITICAL ? cell < mvThr : (uint32_t)cell + sagMv(in) < mvThr) {
                uint32_t sag = to == CRITICAL ? 0 : sagMv(in);
                if (sag) {
                    snprintf(trig, sizeof(trig), "lowest cell %u mV + %u mV sag at %u.%u A < %u mV", cell, (unsigned)sag,
                        (unsigned)(-in.centiA / 100), (unsigned)((-in.centiA % 100) / 10), mvThr);
                }
                else {
                    snprintf(trig, sizeof(trig), "lowest cell %u mV < %u mV", cell, mvThr);
                }
            }
            else if (to == SLEEP || from >= SLEEP) {
                snprintf(trig, sizeof(trig), "load cut for %lu s", (unsigned long)_st.sinceCutS);
            }
        }
        else {                                                              // going up
            snprintf(trig, sizeof(trig), "soc %u %% >= %u %%, lowest cell %u mV", in.socValid ? in.soc : 0,
                to == NORMAL ? _cfg.loadOnSoc : (to == LOAD_CUT ? (uint8_t)(_cfg.sleepSoc + _cfg.hysteresisSoc) : (uint8_t)(_cfg.criticalSoc + _cfg.hysteresisSoc)),
                in.minCellMv);
        }
        snprintf(out, cap, "%s -> %s: %s", levelName(from), levelName(to), trig);
    }

    Config _cfg;
    State _st;
};

}  // namespace Protection
