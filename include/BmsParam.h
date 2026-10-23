/**
 * @file BmsParam.h
 * @brief The BMS protection settings (EEPROM) as JSON and as an InfluxDB line.
 *
 * Header-only, no Arduino dependency (native unit tests). The functions are
 * templates over the settings struct, so they take JbdBms::Settings_t on the
 * device and a plain struct with the same member names in the tests.
 *
 * Raw units of the BMS: pack voltages 10 mV, cell voltages mV, currents 10 mA,
 * temperatures 0.1 K, shunt 0.1 mOhm, capacity 10 mAh, delays seconds.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "JsonWriter.h"

namespace BmsParam {

/// Delay of the second cell under voltage level, from bits 7:6 of register 0x39 (seconds)
inline uint8_t cellUvpHighDelayS(uint8_t highDelays) {
    static const uint8_t s[] = { 1, 4, 8, 16 };
    return s[(highDelays >> 6) & 3];
}

/// Delay of the second cell over voltage level, from bits 5:4 of register 0x39 (seconds)
inline uint8_t cellOvpHighDelayS(uint8_t highDelays) {
    static const uint8_t s[] = { 1, 2, 4, 8 };
    return s[(highDelays >> 4) & 3];
}

/// 0.1 K -> 0.1 degC
inline long deciCelsius(uint16_t deciKelvin) { return (long)deciKelvin - 2731; }

namespace detail {

/// {"V":..,"releaseV":..,"delayS":..} with @p decimals for the voltage (2: 10 mV units, 3: mV)
inline void level(JsonWriter& w, const char* name, long raw, long rawRelease, int decimals, int delayS, long highRaw = -1, int highDelayS = 0) {
    w.key(name);
    w.open('{');
    w.key("V");        w.fixed(raw, decimals);
    w.key("releaseV"); w.fixed(rawRelease, decimals);
    w.key("delayS");   w.integer(delayS);
    if (highRaw >= 0) {
        w.key("highV");       w.fixed(highRaw, 3);
        w.key("highDelayS");  w.integer(highDelayS);
    }
    w.close('}');
}

inline void temp(JsonWriter& w, const char* name, uint16_t limit, uint16_t release) {
    w.key(name);
    w.open('[');
    w.fixed(deciCelsius(limit), 1);
    w.fixed(deciCelsius(release), 1);
    w.close(']');
}

}  // namespace detail

/**
 * @brief Format the settings into @p out.
 * @param have  false: nothing read yet, writes {"read":false}
 * @param ageS  seconds since the settings were read
 * @param full  false: only the protection levels and current limits (fits an MQTT
 *              message: PubSubClient's buffer is 512 bytes), true: everything
 * @return true if the document fit completely
 */
template <class S>
bool writeJson(char* out, size_t cap, bool have, uint32_t ageS, const S& s, bool full = true) {
    JsonWriter w(out, cap);
    w.open('{');
    w.key("read"); w.boolean(have);
    if (!have) {
        w.close('}');
        return w.ok();
    }
    w.key("ageS");  w.integer((long)ageS);
    w.key("cells"); w.integer(s.cells);
    detail::level(w, "cellUvp", s.cellUvp, s.cellUvpRelease, 3, s.cellUvpDelay,
        full ? s.cellUvpHigh : -1, cellUvpHighDelayS(s.highDelays));
    detail::level(w, "cellOvp", s.cellOvp, s.cellOvpRelease, 3, s.cellOvpDelay,
        full ? s.cellOvpHigh : -1, cellOvpHighDelayS(s.highDelays));
    detail::level(w, "packUvp", s.packUvp, s.packUvpRelease, 2, s.packUvpDelay);
    detail::level(w, "packOvp", s.packOvp, s.packOvpRelease, 2, s.packOvpDelay);
    w.key("dischargeOcpA"); w.fixed(abs((long)s.dischargeOcp), 2);
    w.key("dischargeOcpDelayS"); w.integer(s.dischargeOcpDelay);
    w.key("chargeOcpA");    w.fixed(s.chargeOcp, 2);
    w.key("chargeOcpDelayS"); w.integer(s.chargeOcpDelay);
    if (!full) {
        w.close('}');
        return w.ok();
    }
    w.key("tempC");   // [limit, release] in degC
    w.open('{');
    detail::temp(w, "chargeOver", s.chargeOvertemp, s.chargeOvertempRelease);
    detail::temp(w, "chargeUnder", s.chargeUndertemp, s.chargeUndertempRelease);
    detail::temp(w, "dischargeOver", s.dischargeOvertemp, s.dischargeOvertempRelease);
    detail::temp(w, "dischargeUnder", s.dischargeUndertemp, s.dischargeUndertempRelease);
    w.close('}');
    w.key("balancer");
    w.open('{');
    w.key("on");            w.boolean(s.functionConfig & 0x04);
    w.key("whileCharging"); w.boolean(s.functionConfig & 0x08);
    w.key("startV");        w.fixed(s.balanceStart, 3);
    w.key("windowMv");      w.integer(s.balanceWindow);
    w.close('}');
    w.key("socTableMv");    // cell voltage the BMS assigns to 0, 20, 40, 60, 80, 100 %
    w.open('[');
    w.integer(s.cellVoltage0);  w.integer(s.cellVoltage20); w.integer(s.cellVoltage40);
    w.integer(s.cellVoltage60); w.integer(s.cellVoltage80); w.integer(s.cellVoltage100);
    w.close(']');
    w.key("designAh");        w.fixed(s.designCapacity, 2);
    w.key("selfDischargePct"); w.fixed(s.selfDischarge, 1);
    w.key("shuntMOhm");       w.fixed(s.shuntResistance, 1);
    w.key("ntcMask");         w.integer(s.ntcConfig);
    w.close('}');
    return w.ok();
}

/**
 * @brief InfluxDB line (raw device units, like the other device measurements).
 * @return length, or 0 if it did not fit
 */
template <class S>
size_t influxLine(char* out, size_t cap, const char* id, const char* version, const char* host, const S& s) {
    int n = snprintf(out, cap,
        "BmsParam,Id=%.32s,Version=%s Host=\"%s\","
        "Cells=%u,CellOvp=%u,CellOvpRelease=%u,CellOvpDelay=%u,CellUvp=%u,CellUvpRelease=%u,CellUvpDelay=%u,"
        "CellOvpHigh=%u,CellUvpHigh=%u,HighDelays=%u,"
        "PackOvp=%u,PackOvpRelease=%u,PackOvpDelay=%u,PackUvp=%u,PackUvpRelease=%u,PackUvpDelay=%u,"
        "ChargeOcp=%d,ChargeOcpDelay=%u,DischargeOcp=%d,DischargeOcpDelay=%u,"
        "ChargeOvertemp=%u,ChargeUndertemp=%u,DischargeOvertemp=%u,DischargeUndertemp=%u,"
        "BalanceStart=%u,BalanceWindow=%u,FunctionConfig=%u,NtcConfig=%u,"
        "ShuntResistance=%u,DesignCapacity=%u,SelfDischarge=%u,"
        "CellVoltage0=%u,CellVoltage20=%u,CellVoltage40=%u,CellVoltage60=%u,CellVoltage80=%u,CellVoltage100=%u",
        id, version, host,
        (unsigned)s.cells, (unsigned)s.cellOvp, (unsigned)s.cellOvpRelease, (unsigned)s.cellOvpDelay,
        (unsigned)s.cellUvp, (unsigned)s.cellUvpRelease, (unsigned)s.cellUvpDelay,
        (unsigned)s.cellOvpHigh, (unsigned)s.cellUvpHigh, (unsigned)s.highDelays,
        (unsigned)s.packOvp, (unsigned)s.packOvpRelease, (unsigned)s.packOvpDelay,
        (unsigned)s.packUvp, (unsigned)s.packUvpRelease, (unsigned)s.packUvpDelay,
        (int)s.chargeOcp, (unsigned)s.chargeOcpDelay, (int)s.dischargeOcp, (unsigned)s.dischargeOcpDelay,
        (unsigned)s.chargeOvertemp, (unsigned)s.chargeUndertemp, (unsigned)s.dischargeOvertemp, (unsigned)s.dischargeUndertemp,
        (unsigned)s.balanceStart, (unsigned)s.balanceWindow, (unsigned)s.functionConfig, (unsigned)s.ntcConfig,
        (unsigned)s.shuntResistance, (unsigned)s.designCapacity, (unsigned)s.selfDischarge,
        (unsigned)s.cellVoltage0, (unsigned)s.cellVoltage20, (unsigned)s.cellVoltage40,
        (unsigned)s.cellVoltage60, (unsigned)s.cellVoltage80, (unsigned)s.cellVoltage100);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

}  // namespace BmsParam
