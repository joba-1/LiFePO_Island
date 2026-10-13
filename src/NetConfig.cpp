#include "NetConfig.h"

#include <Arduino.h>
#include <Syslog.h>   // LOG_* priorities
#if defined(ESP32)
#include <Preferences.h>
#endif

void slog(const char *message, uint16_t pri);   // main.cpp

namespace {

const char *NVS_NS = "netcfg";
const uint8_t LAYOUT_VERSION = 1;   // bump when Values changes

void fill(NetConfig::Values &v, const char *sh, uint16_t sp, const char *mh, uint16_t mp,
          const char *ih, uint16_t ip, const char *ntp) {
    using NetConfigValidate::copyField;
    memset(&v, 0, sizeof(v));
    copyField(v.syslogHost, sizeof(v.syslogHost), sh ? sh : "");
    v.syslogPort = sp;
    copyField(v.mqttHost, sizeof(v.mqttHost), mh ? mh : "");
    v.mqttPort = mp;
    copyField(v.influxHost, sizeof(v.influxHost), ih ? ih : "");
    v.influxPort = ip;
    copyField(v.ntp, sizeof(v.ntp), ntp ? ntp : "");
}

}  // namespace

NetConfig::NetConfig(const char *syslogHost, uint16_t syslogPort,
                     const char *mqttHost, uint16_t mqttPort,
                     const char *influxHost, uint16_t influxPort, const char *ntp) {
    fill(_defaults, syslogHost, syslogPort, mqttHost, mqttPort, influxHost, influxPort, ntp);
    _active = _defaults;
}

bool NetConfig::validate(const Values &v, const char **badField) {
    using namespace NetConfigValidate;
    const char *bad = nullptr;
    if (!isValidHost(v.syslogHost))      bad = "syslogHost";
    else if (v.syslogPort == 0)          bad = "syslogPort";
    else if (!isValidHost(v.mqttHost))   bad = "mqttHost";
    else if (v.mqttPort == 0)            bad = "mqttPort";
    else if (!isValidHost(v.influxHost)) bad = "influxHost";
    else if (v.influxPort == 0)          bad = "influxPort";
    else if (!isValidHost(v.ntp))        bad = "ntp";
    if (badField) *badField = bad;
    return bad == nullptr;
}

#if defined(ESP32)

namespace {
// Read one string key into a fixed field; keep dst on absence or overflow.
void readStr(Preferences &p, const char *key, char *dst, size_t cap) {
    if (!p.isKey(key)) return;
    char tmp[NetConfigValidate::MAX_URL_LEN + 1];
    if (p.getString(key, tmp, sizeof(tmp)) == 0) return;   // missing/too long/error
    NetConfigValidate::copyField(dst, cap, tmp);
}
}  // namespace

void NetConfig::load() {
    Preferences p;
    if (!p.begin(NVS_NS, true)) return;
    if (p.getUChar("ver", 0) != LAYOUT_VERSION) { p.end(); return; }
    Values v = _defaults;
    readStr(p, "syslogHost", v.syslogHost, sizeof(v.syslogHost));
    v.syslogPort = p.getUShort("syslogPort", v.syslogPort);
    readStr(p, "mqttHost", v.mqttHost, sizeof(v.mqttHost));
    v.mqttPort = p.getUShort("mqttPort", v.mqttPort);
    readStr(p, "influxHost", v.influxHost, sizeof(v.influxHost));
    v.influxPort = p.getUShort("influxPort", v.influxPort);
    readStr(p, "ntp", v.ntp, sizeof(v.ntp));
    p.end();
    const char *bad = nullptr;
    if (!validate(v, &bad)) {
        char msg[80];
        snprintf(msg, sizeof(msg), "Stored netconfig value '%s' invalid - using defaults", bad);
        slog(msg, LOG_WARNING);
        return;
    }
    _active = v;
    _fromFlash = true;
}

bool NetConfig::save(const Values &v, const char **badField) {
    if (!validate(v, badField)) return false;
    Preferences p;
    if (!p.begin(NVS_NS, false)) { if (badField) *badField = "nvs"; return false; }
    bool ok = p.clear();
    ok = ok && p.putString("syslogHost", v.syslogHost) == strlen(v.syslogHost);
    ok = ok && p.putUShort("syslogPort", v.syslogPort) == sizeof(uint16_t);
    ok = ok && p.putString("mqttHost", v.mqttHost) == strlen(v.mqttHost);
    ok = ok && p.putUShort("mqttPort", v.mqttPort) == sizeof(uint16_t);
    ok = ok && p.putString("influxHost", v.influxHost) == strlen(v.influxHost);
    ok = ok && p.putUShort("influxPort", v.influxPort) == sizeof(uint16_t);
    ok = ok && p.putString("ntp", v.ntp) == strlen(v.ntp);
    ok = ok && p.putUChar("ver", LAYOUT_VERSION) == 1;   // last: no half-written set
    p.end();
    if (!ok && badField) *badField = "nvs";
    char msg[400];
    snprintf(msg, sizeof(msg), "Netconfig saved: syslog=%s:%u mqtt=%s:%u influx=%s:%u ntp=%s (%s)",
             v.syslogHost, v.syslogPort, v.mqttHost, v.mqttPort, v.influxHost, v.influxPort,
             v.ntp, ok ? "ok" : "FAILED");
    slog(msg, LOG_NOTICE);
    return ok;
}

bool NetConfig::clear() {
    Preferences p;
    if (!p.begin(NVS_NS, false)) return false;
    bool ok = p.clear();
    p.end();
    slog("Netconfig cleared, defaults at next boot", LOG_NOTICE);
    return ok;
}

#else   // ESP8266: no NVS, defaults only

void NetConfig::load() {}
bool NetConfig::save(const Values &, const char **badField) {
    if (badField) *badField = "unsupported on this board";
    return false;
}
bool NetConfig::clear() { return true; }

#endif
