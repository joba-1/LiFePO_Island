/**
 * @file HaDiscovery.h
 * @brief Home Assistant MQTT discovery messages (docs/spec-homeassistant.md).
 *
 * Header-only, no Arduino dependency (native unit tests). The firmware asks
 * for message i of count() and publishes topic + payload retained. Payloads
 * use HA's abbreviated keys and must fit PubSubClient's MQTT_MAX_PACKET_SIZE
 * together with the topic; test_hadiscovery checks every message.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "JsonWriter.h"
#include "NameList.h"

namespace HaDiscovery {

/// Largest topic + payload a message may have (512 byte buffer minus MQTT
/// header and length fields, with margin).
static constexpr size_t MAX_MESSAGE = 480;

struct Context {
    const char* prefix;    ///< discovery prefix, e.g. "homeassistant"
    const char* base;      ///< state topic base, e.g. "LiFePO_Island/1"
    const char* node;      ///< node / device id, e.g. "lifepo_island_1"
    const char* devName;   ///< device name, e.g. "LiFePO_Island 1"
    const char* version;   ///< firmware version
    const char* ntcNames;  ///< "BMS board,Battery box": names of the BMS probes, or nullptr
};

struct Entity {
    const char* key;        ///< unique id suffix; "%u" for indexed entities
    const char* component;  ///< sensor, binary_sensor, switch
    const char* name;       ///< "%u" for indexed entities
    const char* topic;      ///< state topic below base
    const char* tpl;        ///< value template ("%u" = 0-based index), or nullptr
    const char* unit;
    const char* devClass;
    const char* stateClass;
    int8_t precision;       ///< suggested display precision, -1 = none
    bool diagnostic;
};

static const char M[] = "measurement";

// Fixed entities, in publish order. Templates use single quotes only.
static const Entity FIXED[] = {
    { "soc", "sensor", "Battery SoC", "json/Status",
      "{{value_json.Status.currentCapacity}}", "%", "battery", M, -1, false },
    { "voltage", "sensor", "Battery voltage", "json/Status",
      "{{value_json.Status.voltage/100}}", "V", "voltage", M, 2, false },
    { "current", "sensor", "Battery current", "json/Status",
      "{{value_json.Status.current/100}}", "A", "current", M, 2, false },
    { "power", "sensor", "Battery power", "json/Status",
      "{{(value_json.Status.voltage*value_json.Status.current/10000)|round(1)}}", "W", "power", M, 1, false },
    { "remaining", "sensor", "Remaining capacity", "json/Status",
      "{{value_json.Status.remainingCapacity/100}}", "Ah", nullptr, M, 1, false },
    { "cycles", "sensor", "Cycles", "json/Status",
      "{{value_json.Status.cycles}}", nullptr, nullptr, "total_increasing", -1, true },
    { "pv_voltage", "sensor", "PV voltage", "json/ChgSts",
      "{{value_json.ChgSts.PvVolt/10}}", "V", "voltage", M, 1, false },
    { "charge_power", "sensor", "Charge power", "json/ChgSts",
      "{{value_json.ChgSts.ChgPower}}", "W", "power", M, -1, false },
    { "load_power", "sensor", "Load power", "json/ChgSts",
      "{{value_json.ChgSts.LoadPower}}", "W", "power", M, -1, false },
    { "charger_battery_temp", "sensor", "Charger battery temperature", "json/ChgSts",
      "{{value_json.ChgSts.BatTemp}}", "\xc2\xb0" "C", "temperature", M, -1, false },
    { "charger_temp", "sensor", "Charger temperature", "json/ChgSts",
      "{{value_json.ChgSts.InnerTemp}}", "\xc2\xb0" "C", "temperature", M, -1, false },
    { "energy_production", "sensor", "Energy production", "json/Log",
      "{{value_json.Log.TotalEng}}", "Wh", "energy", "total_increasing", -1, false },
    { "energy_consumption", "sensor", "Energy consumption", "json/Log",
      "{{value_json.Log.LoadTotalEng}}", "Wh", "energy", "total_increasing", -1, false },
    { "charge_mode", "sensor", "Charge mode", "json/ChgSts",
      "{{ {0:'wait',1:'MPPT',2:'bulk',3:'float',4:'pre-charge'}.get(value_json.ChgSts.ChgMode,'unknown') }}",
      nullptr, nullptr, nullptr, -1, false },
    { "rssi", "sensor", "Wi-Fi signal", "json/Wifi",
      "{{value_json.Wifi.RSSI}}", "dBm", "signal_strength", M, -1, true },
    // fault bits, MSB first; charger bit 6 (index 3) "PV low voltage" is normal at night
    { "charger_problem", "binary_sensor", "Charger problem", "status/Charger",
      "{{'ON' if '1' in value[:3]~value[4:] else 'OFF'}}", nullptr, "problem", nullptr, -1, false },
    { "bms_problem", "binary_sensor", "BMS problem", "status/BMS",
      "{{'ON' if '1' in value else 'OFF'}}", nullptr, "problem", nullptr, -1, false },
    { "charge_fet", "binary_sensor", "Charge MOSFET", "json/Status",
      "{{'ON' if value_json.Status.mosfetStatus|bitwise_and(1) else 'OFF'}}", nullptr, nullptr, nullptr, -1, true },
    { "discharge_fet", "binary_sensor", "Discharge MOSFET", "json/Status",
      "{{'ON' if value_json.Status.mosfetStatus|bitwise_and(2) else 'OFF'}}", nullptr, nullptr, nullptr, -1, true },
    { "load", "switch", "Load", "status/Load", nullptr, nullptr, nullptr, nullptr, -1, false },
};
static constexpr size_t FIXED_COUNT = sizeof(FIXED) / sizeof(*FIXED);

static const Entity CELL = { "cell_%u", "sensor", "Cell %u", "json/Cells",
    "{{value_json.Cells[%u]/1000}}", "V", "voltage", M, 3, false };
static const Entity NTC = { "bms_temp_%u", "sensor", "BMS temperature %u", "json/Status",
    "{{value_json.Status.temperatures[%u]/10}}", "\xc2\xb0" "C", "temperature", M, 1, false };

static constexpr uint8_t MAX_CELLS = 32;
static constexpr uint8_t MAX_NTCS = 8;

/// Number of messages for @p cells cells and @p ntcs temperature sensors.
inline size_t count(uint8_t cells, uint8_t ntcs) {
    if (cells > MAX_CELLS) cells = MAX_CELLS;
    if (ntcs > MAX_NTCS) ntcs = MAX_NTCS;
    return FIXED_COUNT + cells + ntcs;
}

/**
 * @brief Build message @p index (0 .. count()-1).
 * @return false if index is out of range or a buffer is too small.
 */
inline bool build(const Context& c, size_t index, uint8_t cells, uint8_t ntcs,
                  char* topic, size_t topicCap, char* payload, size_t payloadCap) {
    if (index >= count(cells, ntcs)) return false;
    const Entity* e;
    unsigned n = 0;                          // 1-based index of a cell / NTC
    if (index < FIXED_COUNT) {
        e = &FIXED[index];
    }
    else if (index < FIXED_COUNT + (cells > MAX_CELLS ? MAX_CELLS : cells)) {
        e = &CELL;
        n = (unsigned)(index - FIXED_COUNT + 1);
    }
    else {
        e = &NTC;
        n = (unsigned)(index - FIXED_COUNT - (cells > MAX_CELLS ? MAX_CELLS : cells) + 1);
    }

    char key[24], name[40], tpl[128], uid[64], stat[32];
    snprintf(key, sizeof(key), e->key, n);
    snprintf(name, sizeof(name), e->name, n);
    char probe[24];
    if (e == &NTC && NameList::nth(c.ntcNames, n - 1, probe, sizeof(probe))) {
        snprintf(name, sizeof(name), "%s temperature", probe);   // "Battery box temperature"
    }
    if (e->tpl) snprintf(tpl, sizeof(tpl), e->tpl, n - 1);
    snprintf(uid, sizeof(uid), "%s_%s", c.node, key);
    snprintf(stat, sizeof(stat), "~/%s", e->topic);

    int t = snprintf(topic, topicCap, "%s/%s/%s/%s/config", c.prefix, e->component, c.node, key);
    if (t < 0 || (size_t)t >= topicCap) return false;

    JsonWriter w(payload, payloadCap);
    w.open('{');
    w.key("~");        w.string(c.base);
    w.key("name");     w.string(name);
    w.key("uniq_id");  w.string(uid);
    w.key("stat_t");   w.string(stat);
    if (e->tpl)        { w.key("val_tpl"); w.string(tpl); }
    if (e->unit)       { w.key("unit_of_meas"); w.string(e->unit); }
    if (e->devClass)   { w.key("dev_cla"); w.string(e->devClass); }
    if (e->stateClass) { w.key("stat_cla"); w.string(e->stateClass); }
    if (e->precision >= 0) { w.key("sug_dsp_prc"); w.integer(e->precision); }
    if (e->diagnostic) { w.key("ent_cat"); w.string("diagnostic"); }
    if (!strcmp(e->component, "switch")) {
        w.key("cmd_t");   w.string("~/cmd");
        w.key("pl_on");   w.string("load on");
        w.key("pl_off");  w.string("load off");
        w.key("stat_on"); w.string("on");
        w.key("stat_off"); w.string("off");
    }
    w.key("avty_t");       w.string("~/status/LWT");
    w.key("pl_avail");     w.string("Online");
    w.key("pl_not_avail"); w.string("Offline");
    // Full device info only in message 0: HA merges device info from all
    // messages, the identifier alone links an entity (saves ~90 bytes each).
    w.key("dev");
    w.open('{');
    w.key("ids");  w.string(c.node);
    if (index == 0) {
        w.key("name"); w.string(c.devName);
        w.key("mf");   w.string("joba-1");
        w.key("mdl");  w.string("eSmart3/4 + JBD BMS");
        w.key("sw");   w.string(c.version);
    }
    w.close('}');
    w.close('}');
    return w.ok();
}

}  // namespace HaDiscovery
