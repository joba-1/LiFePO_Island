/*
Runtime-configurable external hosts (syslog, MQTT, InfluxDB, NTP).

The build flags (platformio.ini) are only the defaults. POST /api/netconfig
stores a validated replacement in NVS (ESP32, namespace "netcfg"), which wins
at the next boot, so moving a server needs no rebuild. NVS is separate from
the EEPROM sector that holds the static IP config (ip_config) and from any
filesystem. On ESP8266 builds there is no NVS: the defaults stay in effect
and save() refuses. Same pattern as Joba_Modbus / IntertechnoGW / Radieschen.
*/

#pragma once

#include "NetConfigValidate.h"

class NetConfig {
public:
    struct Values {
        char     syslogHost[NetConfigValidate::MAX_HOST_LEN + 1];
        uint16_t syslogPort;
        char     mqttHost[NetConfigValidate::MAX_HOST_LEN + 1];
        uint16_t mqttPort;
        char     influxHost[NetConfigValidate::MAX_HOST_LEN + 1];
        uint16_t influxPort;
        char     ntp[NetConfigValidate::MAX_HOST_LEN + 1];
    };

    NetConfig(const char *syslogHost, uint16_t syslogPort,
              const char *mqttHost, uint16_t mqttPort,
              const char *influxHost, uint16_t influxPort, const char *ntp);

    void load();                                        // stored set over defaults
    bool save(const Values &v, const char **badField);  // validate + store; next boot
    bool clear();                                       // defaults at next boot

    static bool validate(const Values &v, const char **badField);

    const Values &active() const { return _active; }
    const Values &defaults() const { return _defaults; }
    bool fromFlash() const { return _fromFlash; }

private:
    Values _defaults;
    Values _active;
    bool _fromFlash = false;
};
