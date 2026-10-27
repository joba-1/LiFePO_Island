/**
 * @file LiveJson.h
 * @brief The document behind GET /api/live (see docs/spec-webui.md).
 *
 * Header-only, no Arduino dependency (native unit tests). main.cpp copies the
 * raw device structs into Live (raw units), LiveJson::write() converts to
 * V/A/W/°C/Ah and formats. A block whose device never answered is written
 * with null values, so the page can show "no data" instead of zeros.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "Faults.h"
#include "JsonWriter.h"
#include "NameList.h"
#include "Protection.h"

namespace LiveJson {

/// LiFePO4 nominal cell voltage for Ah -> Wh (energy = Ah * cells * 3.2 V)
static constexpr uint32_t NOMINAL_CELL_MV = 3200;

struct Charger {
    bool     seen = false;      ///< answered at least once since boot
    uint32_t ageS = 0;          ///< seconds since the last good read
    char     model[17] = "";
    char     serial[9] = "";
    uint16_t mode = 0;          ///< ESmart3::chgMode_t
    uint16_t pvDeciV = 0, batDeciV = 0, chgW = 0, loadW = 0;
    int8_t   load = -1;         ///< -1 unknown, 0 off, 1 on
    uint16_t maxLoadDeciA = 0;  ///< BatParam.wMaxDisChgCurr: load output limit, 0 = unknown
    int16_t  batTempC = 0, innerTempC = 0;
    uint16_t capPct = 0;
    uint16_t faults = 0;
};

struct Bms {
    bool     seen = false;
    uint32_t ageS = 0;
    char     id[33] = "";
    uint16_t centiV = 0;        ///< 10 mV
    int16_t  centiA = 0;        ///< 10 mA, + charge, - discharge
    uint16_t remainCentiAh = 0, fullCentiAh = 0, cycles = 0;
    uint8_t  socPct = 0;
    uint8_t  mosfets = 0;       ///< bit 0 charge, bit 1 discharge
    uint8_t  cells = 0;
    uint16_t cellsMv[32] = {0};
    uint32_t balance = 0;       ///< bit n: cell n+1 balancing
    uint8_t  ntcs = 0;
    int16_t  tempsDeciC[8] = {0};
    const char* ntcNames = nullptr;  ///< "BMS board,Battery box" (build flag NTC_NAMES)
    uint16_t faults = 0;
};

/// Battery reserve policy (Protection.h): level, what is enforced, last event
struct Reserve {
    uint8_t  level = 0;             ///< Protection::Level
    bool     loadInhibited = false;
    uint8_t  socOffset = 0;         ///< points the BMS counter is known to read high
    bool     enforceLoad = false, enforceSleep = false, enforceFet = false;
    uint16_t idleThresholdW = 0;    ///< IdleOff: load off after idleMinutes below this mean power
    uint16_t idleMinutes = 0;       ///< 0: idle switch-off disabled
    uint32_t idleWindowS = 0;       ///< current window
    uint32_t idleMeanDeciW = 0;     ///< mean load power of the window, 0.1 W
    int32_t  eventAgeS = -1;        ///< -1: no event since boot
    char     event[200] = "";
};

struct Link {
    int      rssi = 0;
    char     ip[16] = "";
    int      influxCode = 0;
    int32_t  influxOkAgeS = -1; ///< -1 never
    uint32_t queued = 0, dropped = 0;
    bool     mqtt = false, ntp = false;
};

struct Live {
    char     host[33] = "";
    char     version[16] = "";
    char     commit[24] = "";   ///< git commit of the build
    uint32_t uptimeS = 0;
    char     time[20] = "";     ///< local "YYYY-MM-DDTHH:MM:SS"
    bool     timeValid = false;
    char     msg[256] = "";     ///< one-shot message, raw text (escaped here)
    Charger  charger;
    Bms      bms;
    Reserve  reserve;
    Link     link;
};

/// Name of an eSmart charge mode.
inline const char* modeName(uint16_t mode) {
    static const char* const names[] = { "wait", "MPPT", "bulk", "float", "pre-charge" };
    return mode < sizeof(names) / sizeof(*names) ? names[mode] : "unknown";
}

inline void faultList(JsonWriter& w, const char* const* table, uint8_t n, uint16_t bits) {
    w.open('[');
    for (uint8_t b = 0; b < n; b++) {
        if (bits & (1u << b)) w.string(table[b]);
    }
    w.close(']');
}

inline void writeCharger(JsonWriter& w, const Charger& c) {
    w.open('{');
    w.key("ageS");
    if (!c.seen) {
        w.null();
        static const char* const keys[] = { "model", "serial", "mode", "pvV", "batV", "chgW",
            "loadW", "load", "loadMaxW", "batTempC", "innerTempC", "capPct", "faults" };
        for (const char* k : keys) { w.key(k); w.null(); }
        w.close('}');
        return;
    }
    w.integer((long)c.ageS);
    w.key("model");      w.string(c.model);
    w.key("serial");     w.string(c.serial);
    w.key("mode");       w.string(modeName(c.mode));
    w.key("pvV");        w.fixed(c.pvDeciV, 1);
    w.key("batV");       w.fixed(c.batDeciV, 1);
    w.key("chgW");       w.integer(c.chgW);
    w.key("loadW");      w.integer(c.loadW);
    w.key("load");       if (c.load < 0) w.null(); else w.boolean(c.load);
    // load output limit in W at the present battery voltage (0.1 A * 0.1 V = 0.01 W)
    w.key("loadMaxW");
    if (c.maxLoadDeciA && c.batDeciV) w.integer((long)((uint32_t)c.maxLoadDeciA * c.batDeciV / 100));
    else w.null();
    w.key("batTempC");   w.integer(c.batTempC);
    w.key("innerTempC"); w.integer(c.innerTempC);
    w.key("capPct");     w.integer(c.capPct);
    w.key("faults");     faultList(w, Faults::CHARGER, Faults::CHARGER_BITS, c.faults);
    w.close('}');
}

inline void writeBms(JsonWriter& w, const Bms& b) {
    w.open('{');
    w.key("ageS");
    if (!b.seen) {
        w.null();
        static const char* const keys[] = { "id", "V", "A", "W", "socPct", "remainAh", "fullAh", "remainWh", "fullWh",
            "cycles", "chargeFet", "dischargeFet", "cellsMv", "balancing", "tempsC", "tempNames", "faults" };
        for (const char* k : keys) { w.key(k); w.null(); }
        w.close('}');
        return;
    }
    w.integer((long)b.ageS);
    w.key("id");           w.string(b.id);
    w.key("V");            w.fixed(b.centiV, 2);
    w.key("A");            w.fixed(b.centiA, 2);
    w.key("W");            w.fixed((long long)b.centiV * b.centiA / 1000, 1);  // 10mV*10mA = 0.1mW
    w.key("socPct");       w.integer(b.socPct);
    w.key("remainAh");     w.fixed(b.remainCentiAh, 2);
    w.key("fullAh");       w.fixed(b.fullCentiAh, 2);
    // 10 mAh * mV = 10 uWh; / 100000 -> Wh
    w.key("remainWh");     w.integer((long)((uint64_t)b.remainCentiAh * b.cells * NOMINAL_CELL_MV / 100000));
    w.key("fullWh");       w.integer((long)((uint64_t)b.fullCentiAh * b.cells * NOMINAL_CELL_MV / 100000));
    w.key("cycles");       w.integer(b.cycles);
    w.key("chargeFet");    w.boolean(b.mosfets & 1);
    w.key("dischargeFet"); w.boolean(b.mosfets & 2);
    uint8_t cells = b.cells <= 32 ? b.cells : 32;
    w.key("cellsMv");   w.open('[');
    for (uint8_t i = 0; i < cells; i++) w.integer(b.cellsMv[i]);
    w.close(']');
    w.key("balancing"); w.open('[');
    for (uint8_t i = 0; i < cells; i++) w.boolean((b.balance >> i) & 1);
    w.close(']');
    uint8_t ntcs = b.ntcs <= 8 ? b.ntcs : 8;
    w.key("tempsC");    w.open('[');
    for (uint8_t i = 0; i < ntcs; i++) w.fixed(b.tempsDeciC[i], 1);
    w.close(']');
    w.key("tempNames"); w.open('[');
    for (uint8_t i = 0; i < ntcs; i++) {
        char name[24];
        if (!NameList::nth(b.ntcNames, i, name, sizeof(name))) snprintf(name, sizeof(name), "BMS probe %u", i + 1);
        w.string(name);
    }
    w.close(']');
    w.key("faults");    faultList(w, Faults::BMS, Faults::BMS_BITS, b.faults);
    w.close('}');
}

/**
 * @brief Format @p d into @p out.
 * @return true if the document fit completely (otherwise it is truncated
 *         and not valid JSON; the caller should answer 500).
 */
inline bool write(char* out, size_t cap, const Live& d) {
    JsonWriter w(out, cap);
    w.open('{');
    w.key("host");      w.string(d.host);
    w.key("version");   w.string(d.version);
    w.key("commit");    w.string(d.commit);
    w.key("uptimeS");   w.integer((long)d.uptimeS);
    w.key("time");      if (d.timeValid) w.string(d.time); else w.null();
    w.key("timeValid"); w.boolean(d.timeValid);
    w.key("msg");       w.string(d.msg);
    w.key("charger");   writeCharger(w, d.charger);
    w.key("bms");       writeBms(w, d.bms);
    w.key("protection");
    w.open('{');
    w.key("level");         w.string(Protection::levelName(d.reserve.level));
    w.key("loadInhibited"); w.boolean(d.reserve.loadInhibited);
    w.key("socOffset");     w.integer(d.reserve.socOffset);
    w.key("enforce");
    w.open('{');
    w.key("load");  w.boolean(d.reserve.enforceLoad);
    w.key("sleep"); w.boolean(d.reserve.enforceSleep);
    w.key("fet");   w.boolean(d.reserve.enforceFet);
    w.close('}');
    w.key("idleOff");
    w.open('{');
    w.key("thresholdW"); w.integer(d.reserve.idleThresholdW);
    w.key("minutes");    w.integer(d.reserve.idleMinutes);
    w.key("windowS");    w.integer((long)d.reserve.idleWindowS);
    w.key("meanW");      w.fixed((long)d.reserve.idleMeanDeciW, 1);
    w.close('}');
    w.key("lastEvent");
    if (d.reserve.eventAgeS < 0) {
        w.null();
    }
    else {
        w.open('{');
        w.key("ageS"); w.integer(d.reserve.eventAgeS);
        w.key("text"); w.string(d.reserve.event);
        w.close('}');
    }
    w.close('}');
    w.key("link");
    w.open('{');
    w.key("rssi");   w.integer(d.link.rssi);
    w.key("ip");     w.string(d.link.ip);
    w.key("influx");
    w.open('{');
    w.key("code");    w.integer(d.link.influxCode);
    w.key("okAgeS");  if (d.link.influxOkAgeS < 0) w.null(); else w.integer(d.link.influxOkAgeS);
    w.key("queued");  w.integer((long)d.link.queued);
    w.key("dropped"); w.integer((long)d.link.dropped);
    w.close('}');
    w.key("mqtt");   w.boolean(d.link.mqtt);
    w.key("ntp");    w.boolean(d.link.ntp);
    w.close('}');
    w.close('}');
    return w.ok();
}

}  // namespace LiveJson
