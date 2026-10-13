/*
Monitor an eSmart3 and a JDB BMS via RS485 communication

Influx DB can be created with command influx -execute "create database LiFePO_Island"

Monitors GPIO 0 pulled to ground as key press to toggle load
Use builtin led to represent health status
Use defined pin LED to represent load status
Use pin 22 to toggle RS485 read/write

Specification: docs/spec.md (and docs/spec-*.md)
*/


#include <Arduino.h>

// Pure logic, unit tested natively (pio test -e native). Included before the
// device libraries: esmart3.h/jbdbms.h leave #pragma pack(2) active.
#include "Faults.h"
#include "InfluxBatch.h"
#include "LiveJson.h"
#include "HaDiscovery.h"
#include "MqttCmd.h"
#include "TextEscape.h"
#include "web_assets.h"   // generated from web/ by embed_web.py

// Config for ESP8266 or ESP32
#if defined(ESP8266)
    #include <SoftwareSerial.h>
    SoftwareSerial rs485;  // Use SoftwareSerial port
    #define RS485_DIR_PIN -1  // != -1: Use pin for explicit DE/!RE
    #define RS485_RX_PIN -1
    #define RS485_TX_PIN -1

    #define HEALTH_LED_ON LOW
    #define HEALTH_LED_OFF HIGH
    #define HEALTH_LED_PIN LED_BUILTIN
    #define LOAD_LED_ON HIGH
    #define LOAD_LED_OFF LOW
    #define LOAD_LED_PIN D5
    #define LOAD_BUTTON_PIN 0

    // Web Updater
    #include <ESP8266HTTPUpdateServer.h>
    #include <ESP8266WebServer.h>
    #include <ESP8266WiFi.h>
    #include <ESP8266mDNS.h>
    #include <WiFiClient.h>
    #define WebServer ESP8266WebServer
    #define HTTPUpdateServer ESP8266HTTPUpdateServer

    // Post to InfluxDB
    #include <ESP8266HTTPClient.h>

    // Time sync
    #include <NTPClient.h>
    #include <WiFiUdp.h>
    WiFiUDP ntpUDP;
    NTPClient ntp(ntpUDP, NTP_SERVER);
#elif defined(ESP32)
    HardwareSerial &rs485 = Serial2;
    #define RS485_DIR_PIN -1  // != -1: Use pin for explicit DE/!RE
    #define RS485_RX_PIN  14  // != -1: Use non-default pin for Rx
    #define RS485_TX_PIN  13  // != -1: Use non-default pin for Tx

    // Pins can be overridden per environment (custom_flags); -1 = not present
    #define HEALTH_LED_ON HIGH
    #define HEALTH_LED_OFF LOW
    #ifndef HEALTH_LED_PIN
        #ifdef LED_BUILTIN
            #define HEALTH_LED_PIN LED_BUILTIN
        #else
            #define HEALTH_LED_PIN 16
        #endif
    #endif
    #define LOAD_LED_ON LOW
    #define LOAD_LED_OFF HIGH
    #ifndef LOAD_LED_PIN
        #define LOAD_LED_PIN 33
    #endif
    #ifndef LOAD_BUTTON_PIN
        #define LOAD_BUTTON_PIN 0
    #endif

    // Web Updater
    #include <HTTPUpdateServer.h>
    #include <WebServer.h>
    #include <WiFi.h>
    #include <ESPmDNS.h>
    #include <WiFiClient.h>

    // Post to InfluxDB
    #include <HTTPClient.h>
    
    // Time sync
    #include <time.h>
    #include <esp_sntp.h>

    // Reset reason
    #include "rom/rtc.h"
#else
    #error "No ESP8266 or ESP32, define your rs485 stream, pins and includes here!"
#endif

// Infrastructure
// #include <LittleFS.h>
#include <EEPROM.h>
#include <Syslog.h>
#include <WiFiManager.h>

// Web status page and OTA updater
#define WEBSERVER_PORT 80

WebServer web_server(WEBSERVER_PORT);
HTTPUpdateServer esp_updater;

// Post to InfluxDB
int influx_status = 0;
time_t post_time = 0;

// publish to mqtt broker
#include <PubSubClient.h>

WiFiClient wifiMqtt;
PubSubClient mqtt(wifiMqtt);

// Breathing status LED
const uint32_t ok_interval = 5000;
const uint32_t err_interval = 1000;

uint32_t breathe_interval = ok_interval; // ms for one led breathe cycle
bool enabledBreathing = true;  // global flag to switch breathing animation on or off

#ifndef PWMRANGE
#define PWMRANGE 1023
#define PWMBITS 10
#endif

// Syslog
WiFiUDP logUDP;
Syslog syslog(logUDP, SYSLOG_PROTO_IETF);
char msg[512];  // one buffer for all syslog and json messages
char start_time[30];

// eSmart3 device
#include <esmart3.h>
// JbdBms device
#include <jbdbms.h>

#ifdef SIMULATE
    // Test builds: simulated devices, same interface (docs/test.md)
    #include "sim/SimDevices.h"
    SimESmart3 esmart3;
    SimJbdBms jbdbms;
#else
    uint32_t rs485_access_ms = 0;              // rs485 access timestamp for esmart3 and jbdbms
    ESmart3 esmart3(rs485, &rs485_access_ms);  // Serial port to communicate with RS485 adapter
    JbdBms jbdbms(rs485, &rs485_access_ms);    // Same serial port as esmart3 is ok, if parameters are the same
#endif

// Device health for the web UI: millis() of the last good read (valid if seen)
struct DeviceHealth {
    const char *name;
    bool seen = false;          // answered at least once since boot
    uint32_t ok_ms = 0;
    uint32_t failures = 0;      // consecutive failed reads
};
DeviceHealth chargerHealth{"charger"}, bmsHealth{"BMS"};


// Runtime-configurable hosts (build flags are only the defaults)
#include "NetConfig.h"
NetConfig netConfig(SYSLOG_SERVER, SYSLOG_PORT, MQTT_SERVER, MQTT_PORT,
                    INFLUX_SERVER, INFLUX_PORT, NTP_SERVER);

// Status of the external resources for /api/netstatus (millis() stamps, 0 = never)
uint32_t syslog_sent = 0, syslog_failed = 0, syslog_last_ok_ms = 0;
uint32_t mqtt_last_connect_ms = 0, mqtt_connect_failures = 0;
int mqtt_last_rc = -99;
uint32_t mqtt_last_publish_ms = 0, mqtt_publish_failures = 0;
uint32_t influx_last_ok_ms = 0, influx_failures = 0;
volatile uint32_t ntp_last_reply_ms = 0;

// Admin login for /update, /reset, /ip and POST /api/netconfig. Switching the
// load and the mosfets stays open (operational use). The password comes from
// the untracked secrets.ini (pio_secrets.py); without it there is no lock.
#ifndef ADMIN_PASSWORD
#define ADMIN_PASSWORD ""
#endif
static const char admin_user[] = "admin";

void slog(const char *message, uint16_t pri = LOG_INFO) {
    static bool log_infos = true;
    
    if (pri < LOG_INFO || log_infos) {
        Serial.println(message);
        if (syslog.log(pri, message)) {
            syslog_sent++;
            syslog_last_ok_ms = millis();
        }
        else {
            syslog_failed++;
        }
    }

    if (log_infos && millis() > 10 * 60 * 1000) {
        log_infos = false;  // log infos only for first 10 minutes
        slog("Switch off info level messages", LOG_NOTICE);
    }
}


void publish( const char *topic, const char *payload, bool retained = false );

// State topics HA needs after its own restart (docs/spec-homeassistant.md)
void publish_retained( const char *topic, const char *payload ) {
    publish(topic, payload, true);
}

void publish( const char *topic, const char *payload, bool retained ) {
    if (!mqtt.connected()) {
        return;
    }
    if (mqtt.publish(topic, payload, retained)) {
        mqtt_last_publish_ms = millis();
    }
    else {
        mqtt_publish_failures++;
        slog("Mqtt publish failed");
    }
}


// Log a device read result: the first failure and the recovery, not every
// attempt (a disconnected bus used to log an error every 550 ms).
// Returns ok for use in conditions.
bool report_read(DeviceHealth &dev, const char *what, bool ok) {
    char buf[96];
    if (ok) {
        if (dev.failures) {
            snprintf(buf, sizeof(buf), "%s %s ok again after %lu failed reads",
                dev.name, what, (unsigned long)dev.failures);
            slog(buf, LOG_NOTICE);
        }
        dev.failures = 0;
        dev.seen = true;
        dev.ok_ms = millis();
    }
    else if (dev.failures++ == 0) {
        snprintf(buf, sizeof(buf), "%s %s error", dev.name, what);
        slog(buf, LOG_ERR);
    }
    return ok;
}


// InfluxDB: lines are batched and posted every few seconds (docs/spec-telemetry.md)
static char influx_buf[8192];
InfluxBatch influx(influx_buf, sizeof(influx_buf));

bool time_valid() { return time(nullptr) > 1582230020; }

// Queue one line protocol line; timestamped in ms once NTP time is valid
void postInflux(const char *line) {
    uint64_t ts_ms = 0;
    if (time_valid()) {
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        ts_ms = (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    }
    if (!influx.add(line, ts_ms, millis())) {
        char buf[80];
        snprintf(buf, sizeof(buf), "Influx line rejected (%u bytes)", (unsigned)strlen(line));
        slog(buf, LOG_ERR);
    }
}

// Post the batch if due; blocks at most connect + read timeout
void handle_influx() {
    static const char uri[] = "/write?db=" INFLUX_DB "&precision=ms";

    uint32_t now = millis();
    if (!influx.due(now)) {
        return;
    }
    const NetConfig::Values &net = netConfig.active();
    if (!net.influxHost[0]) {
        influx.sent(true, now);   // InfluxDB disabled: discard
        return;
    }
    if (!WiFi.isConnected()) {
        influx.sent(false, now);  // keep the data, back off
        return;
    }

    WiFiClient wifiHttp;
    HTTPClient http;
    http.setConnectTimeout(2000);
    http.setTimeout(3000);
    http.begin(wifiHttp, net.influxHost, net.influxPort, uri);
    http.setUserAgent(PROGNAME);
    int prev = influx_status;
    influx_status = http.POST((uint8_t *)influx.data(), influx.size());
    String payload;
    if (influx_status > 0 && http.getSize() > 0) { // workaround for bug in getString()
        payload = http.getString();
    }
    http.end();

    bool ok = influx_status >= 200 && influx_status < 300;
    if (influx_status != prev) {
        char code[12];
        snprintf(code, sizeof(code), "%d", influx_status);
        publish(MQTT_TOPIC "/status/DBResponse", code);
    }
    if (ok) {
        influx_last_ok_ms = millis();
        post_time = time(NULL);
    }
    // A client error (bad line, 400 "partial write", 404 no database) does not
    // get better by retrying: drop that batch instead of blocking all later data.
    bool hopeless = influx_status >= 400 && influx_status < 500
                 && influx_status != 408 && influx_status != 429;
    if (!ok) {
        influx_failures++;
        char buf[256];   // own buffer: the global msg may hold a line being built
        snprintf(buf, sizeof(buf), "Post %s:%u%s status=%d lines=%u bytes=%u %s response='%.120s'",
            net.influxHost, net.influxPort, uri, influx_status, (unsigned)influx.lines(),
            (unsigned)influx.size(), hopeless ? "dropped" : "kept", payload.c_str());
        slog(buf, LOG_ERR);
    }
    influx.sent(ok || hopeless, millis());
}


// Wifi status as JSON
bool json_Wifi(char *json, size_t maxlen, const char *bssid, int8_t rssi) {
    static const char jsonFmt[] =
        "{\"Version\":\"" VERSION "\",\"Hostname\":\"%s\",\"Wifi\":{"
        "\"BSSID\":\"%s\","
        "\"IP\":\"%s\","
        "\"Subnet\":\"%s\","
        "\"Gateway\":\"%s\","
        "\"DNS0\":\"%s\","
        "\"DNS1\":\"%s\","
        "\"RSSI\":%d}}";

    int len = snprintf(json, maxlen, jsonFmt, WiFi.getHostname(), bssid, 
        WiFi.localIP().toString().c_str(), 
        WiFi.subnetMask().toString().c_str(), 
        WiFi.gatewayIP().toString().c_str(), 
        WiFi.dnsIP(0).toString().c_str(), 
        WiFi.dnsIP(1).toString().c_str(), 
        rssi);

    return len < maxlen;
}


char lastBssid[] = "00:00:00:00:00:00";  // last known connected AP (for web page) 
int8_t lastRssi = 0;                     // last RSSI (for web page)

// Report a change of RSSI or BSSID
void report_wifi( int8_t rssi, const byte *bssid ) {
    static const char digits[] = "0123456789abcdef";
    static const char lineFmt[] =
        "Wifi,Host=%s,Version=" VERSION " "
        "BSSID=\"%s\","
        "IP=\"%s\","
        "RSSI=%d";
    static const uint32_t interval = 10000;
    static const int8_t min_diff = 5;
    static uint32_t prev = 0;
    static int8_t reportedRssi = 0;

    // Update for web page
    lastRssi = rssi;
    for (size_t i=0; i<sizeof(lastBssid); i+=3) {
        lastBssid[i] = digits[bssid[i/3] >> 4];
        lastBssid[i+1] = digits[bssid[i/3] & 0xf];
    }

    // RSSI rate limit for log and db
    int8_t diff = reportedRssi - lastRssi;
    if (diff < 0 ) {
        diff = -diff;
    }
    uint32_t now = millis();
    if (diff >= min_diff || (now - prev > interval) ) {
        json_Wifi(msg, sizeof(msg), lastBssid, lastRssi);
        slog(msg);
        publish(MQTT_TOPIC "/json/Wifi", msg);

        snprintf(msg, sizeof(msg), lineFmt, WiFi.getHostname(), lastBssid, WiFi.localIP().toString().c_str(), lastRssi);
        postInflux(msg);

        reportedRssi = lastRssi;
        prev = now;
    }
}


// check and report RSSI and BSSID changes
void handle_wifi() {
    static byte prevBssid[6] = {0};
    static int8_t prevRssi = 0;
    static bool prevConnected = false;

    static const uint32_t reconnectInterval = 10000;  // try reconnect every 10s
    static const uint32_t reconnectLimit = 60;        // try restart after 10min
    static uint32_t reconnectPrev = 0;
    static uint32_t reconnectCount = 0;

    bool currConnected = WiFi.isConnected();
    int8_t currRssi = 0;
    byte *currBssid = prevBssid;

    if (currConnected) {
        currRssi = WiFi.RSSI();
        currBssid = WiFi.BSSID();

        if (!prevConnected) {
            report_wifi(prevRssi, prevBssid);
        }

        if (currRssi != prevRssi || memcmp(currBssid, prevBssid, sizeof(prevBssid))) {
            report_wifi(currRssi, currBssid);
        }

        memcpy(prevBssid, currBssid, sizeof(prevBssid));
        reconnectCount = 0;
    }
    else {
        uint32_t now = millis();
        if (reconnectCount == 0 || now - reconnectPrev > reconnectInterval) {
            WiFi.reconnect();
            reconnectCount++;
            if (reconnectCount > reconnectLimit) {
                Serial.println("Failed to reconnect WLAN, about to reset");
                for (int i = 0; i < 20; i++) {
                    if (HEALTH_LED_PIN >= 0) digitalWrite(HEALTH_LED_PIN, (i & 1) ? HEALTH_LED_ON : HEALTH_LED_OFF);
                    delay(100);
                }
                ESP.restart();
                while (true)
                    ;
            }
            reconnectPrev = now;
        }
    }

    prevRssi = currRssi;
    prevConnected = currConnected;
}


bool json_Information(char *json, size_t maxlen, ESmart3::Information_t data) {
    static const char jsonFmt[] =
        "{\"Version\":\"" VERSION "\",\"Serial\":\"%8.8s\",\"Information\":{"
        "\"Model\":\"%16.16s\","
        "\"Date\":\"%8.8s\","
        "\"FirmWare\":\"%4.4s\"}}";

    int len = snprintf(json, maxlen, jsonFmt, (char *)data.wSerial, 
        (char *)data.wModel, (char *)data.wDate, (char *)data.wFirmWare);

    return len < maxlen;
}


ESmart3::Information_t es3Information = {0};

// get device info once every minute
void handle_es3Information() {
    static const uint32_t interval = 60000;
    static uint32_t prev = 0 - interval + 0;  // check at start first

    uint32_t now = millis();
    if( now - prev >= interval ) {
        prev += interval;
        ESmart3::Information_t data = {0};
        if (report_read(chargerHealth, "Information", esmart3.getInformation(data))) {
            if (strncmp((const char *)data.wSerialID, (const char *)es3Information.wSerialID, sizeof(data.wSerialID))) {
                // found a new/different eSmart3
                static const char lineFmt[] =
                    "Information,Serial=%8.8s,Version=" VERSION " "
                    "Host=\"%s\","
                    "Model=\"%16.16s\","
                    "Date=\"%8.8s\","
                    "FirmWare=\"%4.4s\"";

                es3Information = data;
                json_Information(msg, sizeof(msg), data);
                slog(msg);
                publish(MQTT_TOPIC "/json/Information", msg);

                snprintf(msg, sizeof(msg), lineFmt, (char *)data.wSerial,
                    WiFi.getHostname(), (char *)data.wModel,
                    (char *)data.wDate, (char *)data.wFirmWare);
                postInflux(msg);
            }
        }
    }
}


static const char *bits( uint16_t faults, uint8_t num_bits ) {
    static char str[17];

    if (num_bits >= sizeof(str)) {
        *str = '\0';
    }
    else {
        char *ptr = &str[num_bits];
        *ptr = '\0';
        uint16_t mask = 1;
        while (num_bits--) {
            *(--ptr) = (faults & mask) ? '1' : '0';
            mask <<= 1;
        }
    }

    return str;
}


// Log the active faults of a device as text whenever they change
void log_faults(const char *device, const char *const *table, uint8_t n, uint16_t bits) {
    char buf[300];
    size_t len = Faults::describe(buf, sizeof(buf), device, table, n, bits, "; ");
    if (!len) snprintf(buf, sizeof(buf), "%s: no faults", device);
    slog(buf, bits ? LOG_WARNING : LOG_NOTICE);
}


bool json_ChgSts(char *json, size_t maxlen, ESmart3::ChgSts_t data) {
    static const char jsonFmt[] =
        "{\"Version\":\"" VERSION "\",\"Serial\":\"%8.8s\",\"ChgSts\":{"
        "\"ChgMode\":%u,"
        "\"PvVolt\":%u,"
        "\"BatVolt\":%u,"
        "\"ChgCurr\":%u,"
        "\"OutVolt\":%u,"
        "\"LoadVolt\":%u,"
        "\"LoadCurr\":%u,"
        "\"ChgPower\":%u,"
        "\"LoadPower\":%u,"
        "\"BatTemp\":%d,"
        "\"InnerTemp\":%d,"
        "\"BatCap\":%u,"
        "\"CO2\":%u,"
        "\"Fault\":\"%s\","
        "\"SystemReminder\":%u}}";

    int len = snprintf(json, maxlen, jsonFmt, (char *)es3Information.wSerial,
        data.wChgMode, data.wPvVolt, data.wBatVolt, data.wChgCurr, data.wOutVolt,
        data.wLoadVolt, data.wLoadCurr, data.wChgPower, data.wLoadPower, data.wBatTemp, 
        data.wInnerTemp, data.wBatCap, data.dwCO2, bits(data.wFault, 10), data.wSystemReminder);

    return len < maxlen;
}


ESmart3::ChgSts_t es3ChgSts = {0};

// get device status once every 1/2 second
void handle_es3ChgSts() {
    static const uint32_t interval = 500 + 50;
    static uint32_t prev = 0 - interval;  // check at start + delay

    uint32_t now = millis();
    if( now - prev >= interval ) {
        prev += interval;
        ESmart3::ChgSts_t data = {0};
        if( report_read(chargerHealth, "ChgSts", esmart3.getChgSts(data)) ) {
            if( memcmp(&data, &es3ChgSts, sizeof(data) ) ) {
                // values have changed: publish
                static const char lineFmt[] =
                    "ChgSts,Serial=%8.8s,Version=" VERSION " "
                    "Host=\"%s\","
                    "ChgMode=%u,"
                    "PvVolt=%u,"
                    "BatVolt=%u,"
                    "ChgCurr=%u,"
                    "OutVolt=%u,"
                    "LoadVolt=%u,"
                    "LoadCurr=%u,"
                    "ChgPower=%u,"
                    "LoadPower=%u,"
                    "BatTemp=%d,"
                    "InnerTemp=%d,"
                    "BatCap=%u,"
                    "CO2=%u,"
                    "Fault=\"%s\","
                    "SystemReminder=%u";
                
                
                json_ChgSts(msg, sizeof(msg), data);
                slog(msg);
                publish(MQTT_TOPIC "/json/ChgSts", msg);

                const char *faults = bits(data.wFault, 10);
                if (es3ChgSts.wFault != data.wFault) {
                    publish_retained(MQTT_TOPIC "/status/Charger", faults);
                    log_faults("Charger", Faults::CHARGER, Faults::CHARGER_BITS, data.wFault);
                }

                es3ChgSts = data;

                snprintf(msg, sizeof(msg), lineFmt, (char *)es3Information.wSerial, WiFi.getHostname(), 
                    data.wChgMode, data.wPvVolt, data.wBatVolt, data.wChgCurr, data.wOutVolt,
                    data.wLoadVolt, data.wLoadCurr, data.wChgPower, data.wLoadPower, data.wBatTemp, 
                    data.wInnerTemp, data.wBatCap, data.dwCO2, faults, data.wSystemReminder);
                postInflux(msg);
            }
        }
    }
}


bool json_BatParam(char *json, size_t maxlen, ESmart3::BatParam_t data) {
    static const char jsonFmt[] =
        "{\"Version\":\"" VERSION "\",\"Serial\":\"%8.8s\",\"BatParam\":{"
        "\"BatType\":%u,"
        "\"BatSysType\":%u,"
        "\"BulkVolt\":%u,"
        "\"FloatVolt\":%u,"
        "\"MaxChgCurr\":%u,"
        "\"MaxDisChgCurr\":%u,"
        "\"EqualizeChgVolt\":%u,"
        "\"EqualizeChgTime\":%u,"
        "\"LoadUseSel\":%u}}";

    int len = snprintf(json, maxlen, jsonFmt, (char *)es3Information.wSerial,
        data.wBatType, data.wBatSysType, data.wBulkVolt, data.wFloatVolt, data.wMaxChgCurr,
        data.wMaxDisChgCurr, data.wEqualizeChgVolt, data.wEqualizeChgTime, data.bLoadUseSel);

    return len < maxlen;
}


ESmart3::BatParam_t es3BatParam = {0};

// get battery parameters once every 10s
void handle_es3BatParam() {
    static const uint32_t interval = 10000;
    static uint32_t prev = 0 - interval + 100;  // check at start + delay

    uint32_t now = millis();
    if( now - prev >= interval ) {
        prev += interval;
        ESmart3::BatParam_t data = {0};
        if( report_read(chargerHealth, "BatParam", esmart3.getBatParam(data)) ) {
            if( memcmp(&data, &es3BatParam, sizeof(data) ) ) {
                // values have changed: publish
                static const char lineFmt[] =
                    "BatParam,Serial=%8.8s,Version=" VERSION " "
                    "Host=\"%s\","
                    "BatType=%u,"
                    "BatSysType=%u,"
                    "BulkVolt=%u,"
                    "FloatVolt=%u,"
                    "MaxChgCurr=%u,"
                    "MaxDisChgCurr=%u,"
                    "EqualizeChgVolt=%u,"
                    "EqualizeChgTime=%u,"
                    "LoadUseSel=%u";
                
                es3BatParam = data;
                json_BatParam(msg, sizeof(msg), data);
                slog(msg);
                publish(MQTT_TOPIC "/json/BatParam", msg);

                snprintf(msg, sizeof(msg), lineFmt, (char *)es3Information.wSerial, WiFi.getHostname(), 
                    data.wBatType, data.wBatSysType, data.wBulkVolt, data.wFloatVolt, data.wMaxChgCurr,
                    data.wMaxDisChgCurr, data.wEqualizeChgVolt, data.wEqualizeChgTime, data.bLoadUseSel);
                postInflux(msg);
            }
        }
    }
}


bool json_Log(char *json, size_t maxlen, ESmart3::Log_t data) {
    static const char jsonFmt[] =
        "{\"Version\":\"" VERSION "\",\"Serial\":\"%8.8s\",\"Log\":{"
        "\"RunTime\":%u,"
        "\"StartCnt\":%u,"
        "\"LastFaultInfo\":%u,"
        "\"FaultCnt\":%u,"
        "\"TodayEng\":%u,"
        "\"TodayEngDate\":\"%d:%d\","
        "\"MonthEng\":%u,"
        "\"MonthEngDate\":\"%d:%d\","
        "\"TotalEng\":%u,"
        "\"LoadTodayEng\":%u,"
        "\"LoadMonthEng\":%u,"
        "\"LoadTotalEng\":%u,"
        "\"BacklightTime\":%u,"
        "\"SwitchEnable\":%u}}";

    int len = snprintf(json, maxlen, jsonFmt, (char *)es3Information.wSerial,
        data.dwRunTime, data.wStartCnt, data.wLastFaultInfo, data.wFaultCnt, 
        data.dwTodayEng, data.wTodayEngDate.month, data.wTodayEngDate.day, data.dwMonthEng, 
        data.wMonthEngDate.month, data.wMonthEngDate.day, data.dwTotalEng, data.dwLoadTodayEng, 
        data.dwLoadMonthEng, data.dwLoadTotalEng, data.wBacklightTime, data.bSwitchEnable);

    return len < maxlen;
}


ESmart3::Log_t es3Log = {0};

// get status log once every 10s
void handle_es3Log() {
    static const uint32_t interval = 10000;
    static uint32_t prev = 0 - interval + 150;  // check at start + delay

    uint32_t now = millis();
    if( now - prev >= interval ) {
        prev += interval;
        ESmart3::Log_t data = {0};
        if( report_read(chargerHealth, "Log", esmart3.getLog(data)) ) {
            if( memcmp(&data.wStartCnt, &es3Log.wStartCnt, sizeof(data) - offsetof(ESmart3::Log_t, wStartCnt) ) ) {
                // values have changed: publish
                static const char lineFmt[] =
                    "Log,Serial=%8.8s,Version=" VERSION " "
                    "Host=\"%s\","
                    "RunTime=%u,"
                    "StartCnt=%u,"
                    "LastFaultInfo=%u,"
                    "FaultCnt=%u,"
                    "TodayEng=%u,"
                    "TodayEngDate=\"%d:%d\","
                    "MonthEng=%u,"
                    "MonthEngDate=\"%d:%d\","
                    "TotalEng=%u,"
                    "LoadTodayEng=%u,"
                    "LoadMonthEng=%u,"
                    "LoadTotalEng=%u,"
                    "BacklightTime=%u,"
                    "SwitchEnable=%u";
                
                es3Log = data;
                json_Log(msg, sizeof(msg), data);
                slog(msg);
                publish_retained(MQTT_TOPIC "/json/Log", msg);   // energy totals change rarely

                snprintf(msg, sizeof(msg), lineFmt, (char *)es3Information.wSerial, WiFi.getHostname(), 
                    data.dwRunTime, data.wStartCnt, data.wLastFaultInfo, data.wFaultCnt, 
                    data.dwTodayEng, data.wTodayEngDate.month, data.wTodayEngDate.day, data.dwMonthEng, 
                    data.wMonthEngDate.month, data.wMonthEngDate.day, data.dwTotalEng, data.dwLoadTodayEng, 
                    data.dwLoadMonthEng, data.dwLoadTotalEng, data.wBacklightTime, data.bSwitchEnable);
                postInflux(msg);
            }
        }
    }
}


bool json_Parameters(char *json, size_t maxlen, ESmart3::Parameters_t data) {
    static const char jsonFmt[] =
        "{\"Version\":\"" VERSION "\",\"Serial\":\"%8.8s\",\"Parameters\":{"
        "\"PvVoltRatio\":%u,"
        "\"PvVoltOffset\":%u,"
        "\"BatVoltRatio\":%u,"
        "\"BatVoltOffset\":%u,"
        "\"ChgCurrRatio\":%u,"
        "\"ChgCurrOffset\":%u,"
        "\"LoadCurrRatio\":%u,"
        "\"LoadCurrOffset\":%u,"
        "\"LoadVoltRatio\":%u,"
        "\"LoadVoltOffset\":%u,"
        "\"OutVoltRatio\":%u,"
        "\"OutVoltOffset\":%u}}";

    int len = snprintf(json, maxlen, jsonFmt, (char *)es3Information.wSerial,
        data.wPvVoltRatio, data.wPvVoltOffset, data.wBatVoltRatio, data.wBatVoltOffset, 
        data.wChgCurrRatio, data.wChgCurrOffset, data.wLoadCurrRatio, data.wLoadCurrOffset, 
        data.wLoadVoltRatio, data.wLoadVoltOffset, data.wOutVoltRatio, data.wOutVoltOffset);

    return len < maxlen;
}


ESmart3::Parameters_t es3Parameters = {0};

// get calibration parameters once every 10s
void handle_es3Parameters() {
    static const uint32_t interval = 10000;
    static uint32_t prev = 0 - interval + 200;  // check at start + delay

    uint32_t now = millis();
    if( now - prev >= interval ) {
        prev += interval;
        ESmart3::Parameters_t data = {0};
        if( report_read(chargerHealth, "Parameters", esmart3.getParameters(data)) ) {
            if( memcmp(&data, &es3Parameters, sizeof(data)) ) {
                // values have changed: publish
                static const char lineFmt[] =
                    "Parameters,Serial=%8.8s,Version=" VERSION " "
                    "Host=\"%s\","
                    "PvVoltRatio=%u,"
                    "PvVoltOffset=%u,"
                    "BatVoltRatio=%u,"
                    "BatVoltOffset=%u,"
                    "ChgCurrRatio=%u,"
                    "ChgCurrOffset=%u,"
                    "LoadCurrRatio=%u,"
                    "LoadCurrOffset=%u,"
                    "LoadVoltRatio=%u,"
                    "LoadVoltOffset=%u,"
                    "OutVoltRatio=%u,"
                    "OutVoltOffset=%u";
                
                es3Parameters = data;
                json_Parameters(msg, sizeof(msg), data);
                // slog(msg);
                publish(MQTT_TOPIC "/json/Parameters", msg);

                snprintf(msg, sizeof(msg), lineFmt, (char *)es3Information.wSerial, WiFi.getHostname(), 
                    data.wPvVoltRatio, data.wPvVoltOffset, data.wBatVoltRatio, data.wBatVoltOffset, 
                    data.wChgCurrRatio, data.wChgCurrOffset, data.wLoadCurrRatio, data.wLoadCurrOffset, 
                    data.wLoadVoltRatio, data.wLoadVoltOffset, data.wOutVoltRatio, data.wOutVoltOffset);
                postInflux(msg);
            }
        }
    }
}


bool json_LoadParam(char *json, size_t maxlen, ESmart3::LoadParam_t data) {
    static const char jsonFmt[] =
        "{\"Version\":\"" VERSION "\",\"Serial\":\"%8.8s\",\"LoadParam\":{"
        "\"LoadModuleSelect1\":%u,"
        "\"LoadModuleSelect2\":%u,"
        "\"LoadOnPvVolt\":%u,"
        "\"LoadOffPvVolt\":%u,"
        "\"PvContrlTurnOnDelay\":%u,"
        "\"PvContrlTurnOffDelay\":%u,"
        "\"AftLoadOnTime\":\"%d:%d\","
        "\"AftLoadOffTime\":\"%d:%d\","
        "\"MonLoadOnTime\":\"%d:%d\","
        "\"MonLoadOffTime\":\"%d:%d\","
        "\"LoadSts\":%u,"
        "\"Time2Enable\":%u}}";

    int len = snprintf(json, maxlen, jsonFmt, (char *)es3Information.wSerial,
        data.wLoadModuleSelect1, data.wLoadModuleSelect2, data.wLoadOnPvVolt, data.wLoadOffPvVolt, 
        data.wPvContrlTurnOnDelay, data.wPvContrlTurnOffDelay, data.AftLoadOnTime.hour, data.AftLoadOnTime.minute, 
        data.AftLoadOffTime.hour, data.AftLoadOffTime.minute, data.MonLoadOnTime.hour, data.MonLoadOnTime.minute, 
        data.MonLoadOffTime.hour, data.MonLoadOffTime.minute, data.wLoadSts, data.wTime2Enable);

    return len < maxlen;
}


ESmart3::LoadParam_t es3LoadParam = {0};

// get load parameters once every 10s
void handle_es3LoadParam() {
    static const uint32_t interval = 10000;
    static uint32_t prev = 0 - interval + 250;  // check at start + delay

    uint32_t now = millis();
    if( now - prev >= interval ) {
        prev += interval;
        ESmart3::LoadParam_t data = {0};
        if( report_read(chargerHealth, "LoadParam", esmart3.getLoadParam(data)) ) {
            if( memcmp(&data, &es3LoadParam, sizeof(data) ) ) {
                // values have changed: publish
                static const char lineFmt[] =
                    "LoadParam,Serial=%8.8s,Version=" VERSION " "
                    "Host=\"%s\","
                    "LoadModuleSelect1=%u,"
                    "LoadModuleSelect2=%u,"
                    "LoadOnPvVolt=%u,"
                    "LoadOffPvVolt=%u,"
                    "PvContrlTurnOnDelay=%u,"
                    "PvContrlTurnOffDelay=%u,"
                    "AftLoadOnTime=\"%d:%d\","
                    "AftLoadOffTime=\"%d:%d\","
                    "MonLoadOnTime=\"%d:%d\","
                    "MonLoadOffTime=\"%d:%d\","
                    "LoadSts=%u,"
                    "Time2Enable=%u";
                
                es3LoadParam = data;
                json_LoadParam(msg, sizeof(msg), data);
                slog(msg);
                publish(MQTT_TOPIC "/json/LoadParam", msg);

                snprintf(msg, sizeof(msg), lineFmt, (char *)es3Information.wSerial, WiFi.getHostname(), 
                    data.wLoadModuleSelect1, data.wLoadModuleSelect2, data.wLoadOnPvVolt, data.wLoadOffPvVolt, 
                    data.wPvContrlTurnOnDelay, data.wPvContrlTurnOffDelay, data.AftLoadOnTime.hour, data.AftLoadOnTime.minute, 
                    data.AftLoadOffTime.hour, data.AftLoadOffTime.minute, data.MonLoadOnTime.hour, data.MonLoadOnTime.minute, 
                    data.MonLoadOffTime.hour, data.MonLoadOffTime.minute, data.wLoadSts, data.wTime2Enable);
                postInflux(msg);
            }
        }
    }
}


bool json_ProParam(char *json, size_t maxlen, ESmart3::ProParam_t data) {
    static const char jsonFmt[] =
        "{\"Version\":\"" VERSION "\",\"Serial\":\"%8.8s\",\"ProParam\":{"
        "\"LoadOvp\":%u,"
        "\"LoadUvp\":%u,"
        "\"BatOvp\":%u,"
        "\"BatOvB\":%u,"
        "\"BatUvp\":%u,"
        "\"BatUvB\":%u}}";

    int len = snprintf(json, maxlen, jsonFmt, (char *)es3Information.wSerial,
        data.wLoadOvp, data.wLoadUvp, data.wBatOvp, data.wBatOvB, data.wBatUvp, data.wBatUvB);

    return len < maxlen;
}


ESmart3::ProParam_t es3ProParam = {0};

// get protection parameters once every 10s
void handle_es3ProParam() {
    static const uint32_t interval = 10000;
    static uint32_t prev = 0 - interval + 300;  // check at start + delay

    uint32_t now = millis();
    if( now - prev >= interval ) {
        prev += interval;
        ESmart3::ProParam_t data = {0};
        if( report_read(chargerHealth, "ProParam", esmart3.getProParam(data)) ) {
            if( memcmp(&data, &es3ProParam, sizeof(data) ) ) {
                // values have changed: publish
                static const char lineFmt[] =
                    "ProParam,Serial=%8.8s,Version=" VERSION " "
                    "Host=\"%s\","
                    "LoadOvp=%u,"
                    "LoadUvp=%u,"
                    "BatOvp=%u,"
                    "BatOvB=%u,"
                    "BatUvp=%u,"
                    "BatUvB=%u";
                
                es3ProParam = data;
                json_ProParam(msg, sizeof(msg), data);
                slog(msg);
                publish(MQTT_TOPIC "/json/ProParam", msg);

                snprintf(msg, sizeof(msg), lineFmt, (char *)es3Information.wSerial, WiFi.getHostname(), 
                    data.wLoadOvp, data.wLoadUvp, data.wBatOvp, data.wBatOvB, data.wBatUvp, data.wBatUvB);
                postInflux(msg);
            }
        }
    }
}


JbdBms::Hardware_t jbdHardware = {0};

bool json_Hardware(char *json, size_t maxlen, JbdBms::Hardware_t data) {
    static const char jsonFmt[] = "{\"Version\":\"" VERSION "\",\"Id\":\"%.32s\"}";
    int len = snprintf(json, maxlen, jsonFmt, data.id);

    return len < maxlen;
}


void handle_jbdHardware() {
    static const uint32_t interval = 60000;
    static uint32_t prev = 0 - interval + 0;  // check at start first

    uint32_t now = millis();
    if( now - prev >= interval ) {
        prev += interval;
        JbdBms::Hardware_t data = {0};
        if (report_read(bmsHealth, "Hardware", jbdbms.getHardware(data))) {
            if (strncmp((const char *)data.id, (const char *)jbdHardware.id, sizeof(data.id))) {
                // found a new/different JBD BMS
                static const char lineFmt[] =
                    "Hardware,Id=%.32s,Version=" VERSION " "
                    "Host=\"%s\"";

                jbdHardware = data;
                json_Hardware(msg, sizeof(msg), data);
                slog(msg);
                publish(MQTT_TOPIC "/json/Hardware", msg);

                snprintf(msg, sizeof(msg), lineFmt, (char *)data.id, WiFi.getHostname());
                postInflux(msg);
            }
        }
    }
}


JbdBms::Status_t jbdStatus = {0};

bool json_Status(char *json, size_t maxlen, JbdBms::Status_t data) {
    static const char jsonFmt[] =
        "{\"Version\":\"" VERSION "\",\"Id\":\"%.32s\",\"Status\":{"
        "\"voltage\":%u,"
        "\"current\":%d,"
        "\"remainingCapacity\":%u,"
        "\"nominalCapacity\":%u,"
        "\"cycles\":%u,"
        "\"productionDate\":\"%04u-%02u-%02u\","
        "\"balance\":\"%s\","
        "\"fault\":%u,"
        "\"version\":%u,"
        "\"currentCapacity\":%u,"
        "\"mosfetStatus\":%u,"
        "\"cells\":%u,"
        "\"ntcs\":%u,"
        "\"temperatures\":%s]}}";
    char temps[sizeof(data.temperatures)/sizeof(*data.temperatures) * 6 + 1] = "[";
    int len = 0;

    for (size_t i = 0; i < data.ntcs && i < sizeof(data.temperatures)/sizeof(*data.temperatures) && len < sizeof(temps); i++) {
        char *str = &temps[len];
        len += snprintf(str, sizeof(temps) - len, ",%d", JbdBms::deciCelsius(data.temperatures[i]));
    }
    temps[0] = '[';  // replace first comma

    if (len < sizeof(temps)) {
        len = snprintf(json, maxlen, jsonFmt, jbdHardware.id,
            data.voltage, data.current, data.remainingCapacity, data.nominalCapacity, data.cycles, 
            JbdBms::year(data.productionDate), JbdBms::month(data.productionDate), JbdBms::day(data.productionDate), 
            JbdBms::balance(data), data.fault, data.version, 
            data.currentCapacity, data.mosfetStatus, data.cells, data.ntcs, temps);

        return len < maxlen;
    }
    return false;
}


void handle_jbdStatus() {
    static const uint32_t interval = 6000;
    static uint32_t prev = 0 - interval + 600;

    uint32_t now = millis();
    if( now - prev >= interval ) {
        prev += interval;
        JbdBms::Status_t data = {0};
        if (report_read(bmsHealth, "Status", jbdbms.getStatus(data))) {
            if (memcmp(&data, &jbdStatus, sizeof(data))) {
                // some voltage has changed
                static const char lineFmt[] =
                    "Status,Id=%.32s,Version=" VERSION " "
                    "Host=\"%s\","
                    "voltage=%u,"
                    "current=%d,"
                    "remainingCapacity=%u,"
                    "nominalCapacity=%u,"
                    "cycles=%u,"
                    "productionDate=\"%04u-%02u-%02u\","
                    "balance=\"%s\","
                    "fault=%u,"
                    "version=%u,"
                    "currentCapacity=%u,"
                    "mosfetStatus=%u,"
                    "cells=%u,"
                    "ntcs=%u";

                json_Status(msg, sizeof(msg), data);
                slog(msg);
                publish(MQTT_TOPIC "/json/Status", msg);
                
                if (jbdStatus.fault != data.fault) {
                    publish_retained(MQTT_TOPIC "/status/BMS", bits(data.fault, 13));
                    log_faults("BMS", Faults::BMS, Faults::BMS_BITS, data.fault);
                }

                jbdStatus = data;

                size_t len = snprintf(msg, sizeof(msg), lineFmt, jbdHardware.id, WiFi.getHostname(), 
                    data.voltage, data.current, data.remainingCapacity, data.nominalCapacity, data.cycles,
                    JbdBms::year(data.productionDate), JbdBms::month(data.productionDate), JbdBms::day(data.productionDate), 
                    JbdBms::balance(data), data.fault, data.version,
                    data.currentCapacity, data.mosfetStatus, data.cells, data.ntcs);

                for (size_t i = 0; i < sizeof(data.temperatures)/sizeof(*data.temperatures) && i < data.ntcs && len < sizeof(msg); i++) {
                    char *str = &msg[len];
                    len += snprintf(str, sizeof(msg) - len, ",temperature%u=%d", i+1, JbdBms::deciCelsius(data.temperatures[i]));
                }

                postInflux(msg);
            }
        }
    }
}


JbdBms::Cells_t jbdCells = {0};

bool json_Cells(char *json, size_t maxlen, JbdBms::Cells_t data) {
    static const char jsonFmt[] = "{\"Version\":\"" VERSION "\",\"Id\":\"%.32s\",\"Cells\":%s]}";
    char voltages[sizeof(data.voltages)/sizeof(*data.voltages) * 6 + 1] = "[";
    int len = 0;

    for (size_t i = 0; i < jbdStatus.cells && i < sizeof(data.voltages)/sizeof(*data.voltages) && len < sizeof(voltages); i++) {
        char *str = &voltages[len];
        len += snprintf(str, sizeof(voltages) - len, ",%u", data.voltages[i]);
    }
    voltages[0] = '[';  // replace first comma

    if (len < sizeof(voltages)) {
        len = snprintf(json, maxlen, jsonFmt, jbdHardware.id, voltages);

        return len < maxlen;
    }
    return false;
}


void handle_jbdCells() {
    static const uint32_t interval = 6000;
    static uint32_t prev = 0 - interval + 700;  // after handle_jbdStatus() so we have valid jbdStatus.cells

    uint32_t now = millis();
    if( now - prev >= interval ) {
        prev += interval;
        JbdBms::Cells_t data = {0};
        if (report_read(bmsHealth, "Cells", jbdbms.getCells(data))) {
            if (memcmp(&data, &jbdCells, sizeof(data))) {
                // some voltage has changed
                static const char lineFmt[] =
                    "Cells,Id=%.32s,Version=" VERSION " "
                    "Host=\"%s\"";

                jbdCells = data;
                json_Cells(msg, sizeof(msg), data);
                slog(msg);
                publish(MQTT_TOPIC "/json/Cells", msg);

                size_t len = snprintf(msg, sizeof(msg), lineFmt, jbdHardware.id, WiFi.getHostname());
                for (size_t i=0; i < sizeof(data.voltages)/sizeof(*data.voltages) && len < sizeof(msg) && i < jbdStatus.cells; i++) {
                    char *str = &msg[len];
                    len += snprintf(str, sizeof(msg) - len, ",voltage%u=%u", i+1, data.voltages[i]);
                }
                postInflux(msg);
            }
        }
    }
}


char web_msg[256] = "";  // one-shot message for the web page (/api/live "msg"), raw text
bool changeIp = false;   // if true, ip changes after display of root url
IPAddress ip;            // the ip to change to (use DHCP if 0)
int8_t load_state = -1;  // charger load output: -1 unknown, 0 off, 1 on (handle_load_led)

// Copy a fixed-width, not NUL-terminated device text field; trims trailing
// blanks/NULs and replaces non-printable bytes
void device_text(char *dst, size_t cap, const void *src, size_t len) {
    const char *s = (const char *)src;
    size_t n = 0;
    for (size_t i = 0; i < len && n + 1 < cap && s[i]; i++) {
        dst[n++] = (s[i] >= 0x20 && s[i] < 0x7f) ? s[i] : '?';
    }
    while (n && dst[n - 1] == ' ') n--;
    dst[n] = '\0';
}

// Collect the current values for GET /api/live (docs/spec-webui.md)
void fill_live(LiveJson::Live &d) {
    uint32_t now = millis();
    snprintf(d.host, sizeof(d.host), "%s", WiFi.getHostname());
    snprintf(d.version, sizeof(d.version), "%s", VERSION);
    d.uptimeS = now / 1000;
    d.timeValid = time_valid();
    if (d.timeValid) {
        time_t t = time(nullptr);
        strftime(d.time, sizeof(d.time), "%FT%T", localtime(&t));
    }
    snprintf(d.msg, sizeof(d.msg), "%s", web_msg);

    LiveJson::Charger &c = d.charger;
    c.seen = chargerHealth.seen && es3ChgSts.wBatVolt;   // Information alone is no live data
    c.ageS = (now - chargerHealth.ok_ms) / 1000;
    device_text(c.model, sizeof(c.model), es3Information.wModel, sizeof(es3Information.wModel));
    device_text(c.serial, sizeof(c.serial), es3Information.wSerial, sizeof(es3Information.wSerial));
    c.mode = es3ChgSts.wChgMode;
    c.pvDeciV = es3ChgSts.wPvVolt;
    c.batDeciV = es3ChgSts.wBatVolt;
    c.chgW = es3ChgSts.wChgPower;
    c.loadW = es3ChgSts.wLoadPower;
    c.load = load_state;
    c.batTempC = es3ChgSts.wBatTemp;
    c.innerTempC = es3ChgSts.wInnerTemp;
    c.capPct = es3ChgSts.wBatCap;
    c.faults = es3ChgSts.wFault;

    LiveJson::Bms &b = d.bms;
    b.seen = bmsHealth.seen && jbdStatus.cells;   // cell count comes with the first Status
    b.ageS = (now - bmsHealth.ok_ms) / 1000;
    device_text(b.id, sizeof(b.id), jbdHardware.id, sizeof(jbdHardware.id));
    b.centiV = jbdStatus.voltage;
    b.centiA = jbdStatus.current;
    b.remainCentiAh = jbdStatus.remainingCapacity;
    b.fullCentiAh = jbdStatus.nominalCapacity;
    b.cycles = jbdStatus.cycles;
    b.socPct = jbdStatus.currentCapacity;
    b.mosfets = jbdStatus.mosfetStatus;
    b.cells = jbdStatus.cells;
    for (size_t i = 0; i < sizeof(b.cellsMv) / sizeof(*b.cellsMv); i++) b.cellsMv[i] = jbdCells.voltages[i];
    b.balance = (uint32_t)jbdStatus.balanceHigh << 16 | jbdStatus.balanceLow;
    b.ntcs = jbdStatus.ntcs;
    for (size_t i = 0; i < sizeof(b.tempsDeciC) / sizeof(*b.tempsDeciC); i++) {
        b.tempsDeciC[i] = JbdBms::deciCelsius(jbdStatus.temperatures[i]);
    }
    b.faults = jbdStatus.fault;

    LiveJson::Link &l = d.link;
    l.rssi = WiFi.RSSI();
    snprintf(l.ip, sizeof(l.ip), "%s", WiFi.localIP().toString().c_str());
    l.influxCode = influx_status;
    l.influxOkAgeS = influx_last_ok_ms ? (int32_t)((now - influx_last_ok_ms) / 1000) : -1;
    l.queued = influx.lines();
    l.dropped = influx.dropped();
    l.mqtt = mqtt.connected();
    l.ntp = d.timeValid;
}


// Read and write ip config
bool ip_config(uint32_t *ip, int num_ip, bool write = false) {
    const uint32_t magic = 0xdeadbeef;
    size_t got_bytes = 0;
    size_t want_bytes = sizeof(*ip) * num_ip;
    if (*ip != 0xffffffff && EEPROM.begin(want_bytes + sizeof(uint32_t))) {
        if (write) {
            got_bytes = EEPROM.writeBytes(0, ip, want_bytes);
            EEPROM.writeULong(want_bytes, magic);
            EEPROM.commit();

        }
        else {
            got_bytes = EEPROM.readBytes(0, ip, want_bytes);
            if (EEPROM.readULong(want_bytes) != magic) {
                got_bytes = 0;
            }
        }
        EEPROM.end();
    }
    return got_bytes == want_bytes && *ip != 0xffffffff;
}

// bool ip_config(uint32_t *ip, int num_ip, bool write = false) {
//     size_t got_bytes = 0;
//     size_t want_bytes = sizeof(*ip) * num_ip;
//     if (LittleFS.begin(write)) {
//         File f = LittleFS.open("ip.cfg", write ? "w" : "r", write);
//         if (f) {
//             got_bytes = write ? f.write((uint8_t *)ip, want_bytes) : f.read((uint8_t *)ip, want_bytes);
//             f.close();
//         }
//         LittleFS.end();
//     }
//     return got_bytes == want_bytes;
// }


// true if allowed; otherwise answers with a 401 challenge
bool admin_ok() {
    if (!ADMIN_PASSWORD[0] || web_server.authenticate(admin_user, ADMIN_PASSWORD)) {
        return true;
    }
    web_server.requestAuthentication(BASIC_AUTH, PROGNAME);
    return false;
}

// JSON string with quotes/backslashes/control chars escaped (SSIDs are user data)
String json_str(const char *v) {
    String out("\"");
    for (const char *p = v; *p; p++) {
        if (*p == '"' || *p == '\\') { out += '\\'; out += *p; }
        else if ((unsigned char)*p >= 0x20) { out += *p; }
    }
    out += '"';
    return out;
}

long age_s(uint32_t ms) { return ms ? (long)((millis() - ms) / 1000) : -1L; }

String netvalues_json(const NetConfig::Values &v) {
    char buf[400];
    snprintf(buf, sizeof(buf),   // validated host names need no escaping
        "{\"syslogHost\":\"%s\",\"syslogPort\":%u,\"mqttHost\":\"%s\",\"mqttPort\":%u,"
        "\"influxHost\":\"%s\",\"influxPort\":%u,\"ntp\":\"%s\"}",
        v.syslogHost, v.syslogPort, v.mqttHost, v.mqttPort, v.influxHost, v.influxPort, v.ntp);
    return String(buf);
}

String netconfig_json() {
    return String("{\"source\":\"") + (netConfig.fromFlash() ? "flash" : "defaults")
        + "\",\"active\":" + netvalues_json(netConfig.active())
        + ",\"defaults\":" + netvalues_json(netConfig.defaults()) + "}";
}

String netstatus_json() {
    const NetConfig::Values &v = netConfig.active();
    time_t now = time(nullptr);
    bool synced = now > 1582230020;
    char buf[1200];
    snprintf(buf, sizeof(buf),
        "{\"firmware\":{\"name\":\"" PROGNAME "\",\"version\":\"" VERSION "\",\"built\":\"" __DATE__ " " __TIME__ "\"},"
        "\"device\":{\"hostname\":%s,\"ip\":\"%s\",\"mac\":\"%s\",\"ssid\":%s,\"rssi\":%d,\"uptimeS\":%lu,\"adminLock\":%s},"
        "\"configSource\":\"%s\","
        "\"resources\":{"
        "\"syslog\":{\"host\":\"%s\",\"port\":%u,\"sent\":%lu,\"failed\":%lu,\"lastOkAgeS\":%ld},"
        "\"mqtt\":{\"host\":\"%s\",\"port\":%u,\"connected\":%s,\"lastConnectAgeS\":%ld,\"lastRc\":%d,"
                  "\"connectFailures\":%lu,\"lastPublishAgeS\":%ld,\"publishFailures\":%lu},"
        "\"influx\":{\"host\":\"%s\",\"port\":%u,\"database\":\"" INFLUX_DB "\",\"lastHttpCode\":%d,\"lastOkAgeS\":%ld,\"failures\":%lu,"
                    "\"queued\":%u,\"droppedLines\":%lu},"
        "\"ntp\":{\"server\":\"%s\",\"synced\":%s,\"lastReplyAgeS\":%ld}}}",
        json_str(WiFi.getHostname()).c_str(), WiFi.localIP().toString().c_str(), WiFi.macAddress().c_str(),
        json_str(WiFi.SSID().c_str()).c_str(), (int)WiFi.RSSI(), (unsigned long)(millis() / 1000),
        ADMIN_PASSWORD[0] ? "true" : "false",
        netConfig.fromFlash() ? "flash" : "defaults",
        v.syslogHost, v.syslogPort, (unsigned long)syslog_sent, (unsigned long)syslog_failed, age_s(syslog_last_ok_ms),
        v.mqttHost, v.mqttPort, mqtt.connected() ? "true" : "false", age_s(mqtt_last_connect_ms), mqtt_last_rc,
        (unsigned long)mqtt_connect_failures, age_s(mqtt_last_publish_ms), (unsigned long)mqtt_publish_failures,
        v.influxHost, v.influxPort, influx_status, age_s(influx_last_ok_ms), (unsigned long)influx_failures,
        (unsigned)influx.lines(), (unsigned long)influx.dropped(),
        v.ntp, synced ? "true" : "false", age_s(ntp_last_reply_ms));
    return String(buf);
}

// Apply one form field: false if present but too long / not a port
bool apply_str(const char *name, char *dst, size_t cap) {
    return !web_server.hasArg(name) || NetConfigValidate::copyField(dst, cap, web_server.arg(name).c_str());
}
bool apply_port(const char *name, uint16_t &dst) {
    return !web_server.hasArg(name) || NetConfigValidate::parsePort(web_server.arg(name).c_str(), dst);
}

void netconfig_error(int code, const char *err, const char *field) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\",\"field\":\"%s\"}", err, field ? field : "");
    web_server.send(code, "application/json", buf);
}

bool should_reboot = false;   // handled in loop() after the answer went out
uint32_t reboot_at = 0;

// POST /api/netconfig: syslogHost/Port, mqttHost/Port, influxHost/Port, ntp; defaults=1; reboot=0
void handle_netconfig_post() {
    bool to_defaults = web_server.arg("defaults") == "1";
    bool reboot = web_server.arg("reboot") != "0";
    NetConfig::Values v = netConfig.active();
    const char *bad = nullptr;
    if (to_defaults) {
        if (!netConfig.clear()) return netconfig_error(500, "nvs clear failed", nullptr);
        v = netConfig.defaults();
    }
    else {
        if (!apply_str("syslogHost", v.syslogHost, sizeof(v.syslogHost))) bad = "syslogHost";
        else if (!apply_str("mqttHost", v.mqttHost, sizeof(v.mqttHost))) bad = "mqttHost";
        else if (!apply_str("influxHost", v.influxHost, sizeof(v.influxHost))) bad = "influxHost";
        else if (!apply_str("ntp", v.ntp, sizeof(v.ntp))) bad = "ntp";
        else if (!apply_port("syslogPort", v.syslogPort)) bad = "syslogPort";
        else if (!apply_port("mqttPort", v.mqttPort)) bad = "mqttPort";
        else if (!apply_port("influxPort", v.influxPort)) bad = "influxPort";
        else NetConfig::validate(v, &bad);
        if (bad) return netconfig_error(400, "invalid value", bad);
        if (!netConfig.save(v, &bad)) return netconfig_error(500, "store failed", bad);
    }
    if (reboot) { should_reboot = true; reboot_at = millis() + 1000; }
    web_server.send(200, "application/json",
        String("{\"ok\":true,\"source\":\"") + (to_defaults ? "defaults" : "flash")
        + "\",\"values\":" + netvalues_json(v) + ",\"reboot\":" + (reboot ? "true" : "false") + "}");
}


// Switch the charger load: state "on", "off" or "toggle". Returns false if
// the charger did not answer; msg gets the result text, on the new state.
bool switch_load(const char *state, const char *&msg, bool &on) {
    msg = "Load unknown";
    bool cur = false;
    bool known = esmart3.getLoad(cur);
    if (!strcmp(state, "toggle")) {
        if (!known) return false;
        on = !cur;
    }
    else {
        on = !strcmp(state, "on");
        if (known && cur == on) {      // nothing to do
            msg = on ? "Load on" : "Load off";
            return true;
        }
    }
    if (!esmart3.setLoad(on)) return false;
    msg = on ? "Load on" : "Load off";
    load_state = on;
    slog(on ? "Load switched ON (web)" : "Load switched OFF (web)", LOG_NOTICE);
    return true;
}

// Set the BMS mosfets; false if the BMS did not answer
bool set_mosfets(uint8_t mosfetStatus, const char *&msg) {
    msg = "Mosfet status unchanged";
    if (mosfetStatus == jbdStatus.mosfetStatus) return true;
    if (!jbdbms.setMosfetStatus((JbdBms::mosfet_t)mosfetStatus)) {
        msg = "Set mosfet status failed";
        return false;
    }
    jbdStatus.mosfetStatus = mosfetStatus;
    static const char *const texts[] = { "Charge and discharge OFF", "Charge ON and discharge OFF",
                                         "Charge OFF and discharge ON", "Charge and discharge ON" };
    msg = texts[mosfetStatus & 3];
    slog(msg, LOG_NOTICE);
    return true;
}

// Old form endpoints answer with a redirect to the page, which shows web_msg
void redirect_home(const char *message) {
    snprintf(web_msg, sizeof(web_msg), "%s", message);
    web_server.sendHeader("Location", "/", true);
    web_server.send(302, "text/plain", "");
}

void send_json(int code, const char *json) {
    web_server.sendHeader("Cache-Control", "no-store");
    web_server.send(code, "application/json", json);
}

// {"ok":..,"msg":"..",<extra>}
void send_result(int code, bool ok, const char *message, const char *extra) {
    char esc[128], buf[256];
    TextEscape::json(esc, sizeof(esc), message);
    snprintf(buf, sizeof(buf), "{\"ok\":%s,\"msg\":\"%s\"%s%s}", ok ? "true" : "false", esc,
        extra && *extra ? "," : "", extra ? extra : "");
    send_json(code, buf);
}


// Define web pages for update, reset or for event infos
void setup_webserver() {
    web_server.on("/toggle", HTTP_POST, []() {
        const char *msg;
        bool on;
        switch_load("toggle", msg, on);
        redirect_home(msg);
    });

    web_server.on("/on", HTTP_POST, []() {
        const char *msg;
        bool on;
        switch_load("on", msg, on);
        redirect_home(msg);
    });

    web_server.on("/off", HTTP_POST, []() {
        const char *msg;
        bool on;
        switch_load("off", msg, on);
        redirect_home(msg);
    });

    web_server.on("/mosfets", HTTP_POST, []() {
        uint8_t mosfetStatus = 0;
        if (web_server.hasArg("charge") && web_server.arg("charge") == "Charge") {
            mosfetStatus |= JbdBms::MOSFET_CHARGE;
        }
        if (web_server.hasArg("discharge") && web_server.arg("discharge") == "Discharge") {
            mosfetStatus |= JbdBms::MOSFET_DISCHARGE;
        }
        const char *msg;
        set_mosfets(mosfetStatus, msg);
        redirect_home(msg);
    });

    // JSON API for the page (docs/spec-webui.md); no login, like the forms above
    web_server.on("/api/load", HTTP_POST, []() {
        String state = web_server.arg("state");
        if (state != "on" && state != "off" && state != "toggle") {
            return send_result(400, false, "state must be on, off or toggle", nullptr);
        }
        const char *msg;
        bool on = false;
        bool ok = switch_load(state.c_str(), msg, on);
        send_result(ok ? 200 : 503, ok, msg, ok ? (on ? "\"load\":true" : "\"load\":false") : nullptr);
    });

    web_server.on("/api/mosfets", HTTP_POST, []() {
        String c = web_server.arg("charge"), d = web_server.arg("discharge");
        if ((c != "0" && c != "1") || (d != "0" && d != "1")) {
            return send_result(400, false, "charge and discharge must be 0 or 1", nullptr);
        }
        uint8_t status = (c == "1" ? JbdBms::MOSFET_CHARGE : 0) | (d == "1" ? JbdBms::MOSFET_DISCHARGE : 0);
        const char *msg;
        bool ok = set_mosfets(status, msg);
        char extra[64];
        snprintf(extra, sizeof(extra), "\"chargeFet\":%s,\"dischargeFet\":%s",
            jbdStatus.mosfetStatus & JbdBms::MOSFET_CHARGE ? "true" : "false",
            jbdStatus.mosfetStatus & JbdBms::MOSFET_DISCHARGE ? "true" : "false");
        send_result(ok ? 200 : 503, ok, msg, extra);
    });

    web_server.on("/api/live", HTTP_GET, []() {
        static LiveJson::Live live;      // static: ~600 bytes, keep them off the stack
        static char json[3072];
        live = LiveJson::Live();
        fill_live(live);
        if (LiveJson::write(json, sizeof(json), live)) {
            *web_msg = '\0';             // delivered
            send_json(200, json);
        }
        else {
            send_json(500, "{\"error\":\"live document too large\"}");
        }
    });

    web_server.on("/logo.svg", HTTP_GET, []() {
        web_server.sendHeader("Cache-Control", "max-age=86400");
        web_server.send(200, "image/svg+xml", WEB_LOGO_SVG);
    });

#ifdef SIMULATE
    // Test builds only: GET /api/sim?charger=0|1&bms=0|1&chargerFaults=N&bmsFaults=N
    web_server.on("/api/sim", HTTP_GET, []() {
        if (web_server.hasArg("charger")) simControl.chargerOnline = web_server.arg("charger") != "0";
        if (web_server.hasArg("bms")) simControl.bmsOnline = web_server.arg("bms") != "0";
        if (web_server.hasArg("chargerFaults")) simControl.chargerFaults = web_server.arg("chargerFaults").toInt();
        if (web_server.hasArg("bmsFaults")) simControl.bmsFaults = web_server.arg("bmsFaults").toInt();
        char buf[160];
        snprintf(buf, sizeof(buf), "{\"charger\":%s,\"bms\":%s,\"chargerFaults\":%u,\"bmsFaults\":%u,\"paramWrites\":%lu}",
            simControl.chargerOnline ? "true" : "false", simControl.bmsOnline ? "true" : "false",
            simControl.chargerFaults, simControl.bmsFaults, (unsigned long)esmart3.writes);
        send_json(200, buf);
    });
#endif

    web_server.on("/switch", []() {
        static const char fmt[] =
            "<!doctype html>\n"
            "<html lang=\"en\">\n"
            " <head>\n"
            "  <title>" PROGNAME " v" VERSION "</title>\n"
            "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
            "  <meta charset=\"utf-8\">\n"
            " </head>\n"
            " <body>\n"
            "  <h1>" PROGNAME " v" VERSION "</h1>\n"
            "   <form action=\"%s\" method=\"post\">\n"
            "    <input type=\"submit\" name=\"switch\" value=\"%s\" />\n"
            "   </form>\n"
            " </body>\n"
            "</html>\n";
        static char page[sizeof(fmt) + 20] = "";

        bool on;
        const char *url = "switchoff";
        const char *txt = "Off";
        if (!esmart3.getLoad(on) || !on ) {
            url = "switchon";
            txt = "On";
        }
        snprintf(page, sizeof(page), fmt, url, txt);
        web_server.send(200, "text/html", page);
    });

    web_server.on("/switchon", HTTP_POST, []() {
        esmart3.setLoad(true);
        web_server.sendHeader("Location", "/switch", true);  
        web_server.send(302, "text/plain", "");
    });

    web_server.on("/switchoff", HTTP_POST, []() {
        esmart3.setLoad(false);
        web_server.sendHeader("Location", "/switch", true);  
        web_server.send(302, "text/plain", "");
    });


    web_server.on("/json/Information", []() {
        json_Information(msg, sizeof(msg), es3Information);
        web_server.send(200, "application/json", msg);
    });

    web_server.on("/json/ChgSts", []() {
        json_ChgSts(msg, sizeof(msg), es3ChgSts);
        web_server.send(200, "application/json", msg);
    });

    web_server.on("/json/BatParam", []() {
        json_BatParam(msg, sizeof(msg), es3BatParam);
        web_server.send(200, "application/json", msg);
    });

    web_server.on("/json/Log", []() {
        json_Log(msg, sizeof(msg), es3Log);
        web_server.send(200, "application/json", msg);
    });

    web_server.on("/json/Parameters", []() {
        json_Parameters(msg, sizeof(msg), es3Parameters);
        web_server.send(200, "application/json", msg);
    });

    web_server.on("/json/LoadParam", []() {
        json_LoadParam(msg, sizeof(msg), es3LoadParam);
        web_server.send(200, "application/json", msg);
    });

    web_server.on("/json/ProParam", []() {
        json_ProParam(msg, sizeof(msg), es3ProParam);
        web_server.send(200, "application/json", msg);
    });


    web_server.on("/json/Status", []() {
        json_Status(msg, sizeof(msg), jbdStatus);
        web_server.send(200, "application/json", msg);
    });

    web_server.on("/json/Cells", []() {
        json_Cells(msg, sizeof(msg), jbdCells);
        web_server.send(200, "application/json", msg);
    });

    web_server.on("/json/Wifi", []() {
        json_Wifi(msg, sizeof(msg), lastBssid, lastRssi);
        web_server.send(200, "application/json", msg);
    });


    // Change host part of ip, if ip&subnet == 0 -> dynamic
    web_server.on("/ip", HTTP_POST, []() {
        if (!admin_ok()) return;
        String strIp = web_server.arg("ip");
        uint16_t prio = LOG_ERR;
        if (ip.fromString(strIp)) {
            uint32_t newIp = (uint32_t)ip;
            uint32_t oldIp = (uint32_t)WiFi.localIP();
            uint32_t subMask = (uint32_t)WiFi.subnetMask();
            if (newIp) {
                // make sure new ip is in the same subnet
                uint32_t netIp = oldIp & (uint32_t)WiFi.subnetMask();
                newIp = (newIp & ~subMask) | netIp;
            }
            if (newIp != oldIp) {
                // don't accidentially use broadcast address
                if ((newIp & ~subMask) != ~subMask) {
                    changeIp = true;
                    ip = newIp;
                    snprintf(web_msg, sizeof(web_msg), "Change IP to '%s'", ip.toString().c_str());
                    prio = LOG_WARNING;
                }
                else {
                    snprintf(web_msg, sizeof(web_msg), "Broadcast address '%s' not possible", IPAddress(newIp).toString().c_str());
                }
            }
            else {
                snprintf(web_msg, sizeof(web_msg), "No IP change for '%s'", strIp.c_str());
                prio = LOG_WARNING;
            }
        }
        else {
            snprintf(web_msg, sizeof(web_msg), "Invalid ip '%s'", strIp.c_str());
        }
        slog(web_msg, prio);

        web_server.sendHeader("Location", "/", true);  
        web_server.send(302, "text/plain", "");
    });

    // Call this page to reset the ESP
    web_server.on("/reset", HTTP_POST, []() {
        if (!admin_ok()) return;
        slog("RESET ESP32", LOG_NOTICE);
        web_server.send(200, "text/html",
                        "<html>\n"
                        " <head>\n"
                        "  <title>" PROGNAME " v" VERSION "</title>\n"
                        "  <meta http-equiv=\"refresh\" content=\"7; url=/\"> \n"
                        " </head>\n"
                        " <body>Resetting...</body>\n"
                        "</html>\n");
        delay(200);
        ESP.restart();
    });

    // Index page
    web_server.on("/", []() {
        web_server.send(200, "text/html", WEB_INDEX_HTML);   // static; data via /api/live

        if (changeIp) {
            delay(200);  // let the send finish
            bool ok = false;
            if (ip != INADDR_NONE) {  // static
                ok = WiFi.config(ip, WiFi.gatewayIP(), WiFi.subnetMask(), WiFi.dnsIP(0), WiFi.dnsIP(1));
            }
            else {  // dynamic
                // How to decide if gw and dns was dhcp provided or static?
                // Assuming it is fully dynamic with dhcp, so set to 0, not old values
                ok = WiFi.config(0UL, 0UL, 0UL);
            }

            snprintf(msg, sizeof(msg), "New IP config ip:%s, gw:%s, sn:%s, d0:%s, d1:%s", WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(), 
                WiFi.subnetMask().toString().c_str(), WiFi.dnsIP(0).toString().c_str(), WiFi.dnsIP(1).toString().c_str());
            slog(msg, LOG_NOTICE);
            if (ok) {
                uint32_t ip[5] = { (uint32_t)WiFi.localIP(), (uint32_t)WiFi.gatewayIP(), (uint32_t)WiFi.subnetMask(), (uint32_t)WiFi.dnsIP(0), (uint32_t)WiFi.dnsIP(1) };
                if (ip_config(ip, 5, true)) {
                    slog("Wrote changed IP config");
                }
                else {
                    slog("Write changed IP config failed");
                }
            }
            changeIp = false;
        }
    });

    // Toggle breathing status led if you dont like it or ota does not work
    web_server.on("/breathe", HTTP_POST, []() {
        enabledBreathing = !enabledBreathing; 
        snprintf(web_msg, sizeof(web_msg), "%s", enabledBreathing ? "breathing enabled" : "breathing disabled");
        web_server.sendHeader("Location", "/", true);  
        web_server.send(302, "text/plain", "");
    });

    web_server.on("/breathe", HTTP_GET, []() {
        snprintf(web_msg, sizeof(web_msg), "%s", enabledBreathing ? "breathing enabled" : "breathing disabled");
        web_server.sendHeader("Location", "/", true);  
        web_server.send(302, "text/plain", "");
    });

    // Runtime network configuration and status (see Readme.md)
    web_server.on("/api/netstatus", HTTP_GET, []() {
        web_server.send(200, "application/json", netstatus_json());
    });
    web_server.on("/api/netconfig", HTTP_GET, []() {
        web_server.send(200, "application/json", netconfig_json());
    });
    web_server.on("/api/netconfig", HTTP_POST, []() {
        if (!admin_ok()) return;
        handle_netconfig_post();
    });

    // Catch all page
    web_server.onNotFound( []() {
        web_server.send(404, "text/plain", "Not found. The page is at /\n");
    });

    web_server.begin();

    MDNS.addService("http", "tcp", WEBSERVER_PORT);

    snprintf(msg, sizeof(msg), "Serving HTTP on port %d", WEBSERVER_PORT);
    slog(msg, LOG_NOTICE);
}


// toggle load on key press
// pin is pulled up if released and pulled down if pressed
void handle_load_button( bool loadOn ) {
    if (LOAD_BUTTON_PIN < 0) return;
    static uint32_t prevTime = 0;
    static uint32_t debounceStatus = 1;
    static bool pressed = false;

    uint32_t now = millis();
    if( now - prevTime > 2 ) {  // debounce check every 2 ms, decision after 2ms/bit * 32bit = 64ms
        prevTime = now;

        // shift bits left, set lowest bit if button pressed
        debounceStatus = (debounceStatus << 1) | ((digitalRead(LOAD_BUTTON_PIN) == LOW) ? 1 : 0);

        if( debounceStatus == 0 && pressed ) {
            pressed = false;
        }
        else if( debounceStatus == 0xffffffff && !pressed ) {
            pressed = true;
            if (esmart3.setLoad(!loadOn)) {
                if( !loadOn ) {
                    slog("Load switched ON", LOG_NOTICE);
                }
                else {
                    slog("Load switched OFF", LOG_NOTICE);
                }
            }
            else {
                slog("Load UNKNOWN", LOG_ERR);
            }
        }
        // else if (debounceStatus) {
        //     Serial.println(debounceStatus, HEX);
        // }
    }
}


// check once every 500ms if load status has changed
// return true if load is on (or unknown)
bool handle_load_led() {
    static uint32_t prevTime = 0;
    static bool prevStatus = false;  // status unknown
    static bool prevLoad = true;     // assume load is on

    uint32_t now = millis();
    if( now - prevTime > 500 ) {
        prevTime = now;
        bool loadOn = false;
        if( esmart3.getLoad(loadOn) ) {
            load_state = loadOn;
            if( !prevStatus || loadOn != prevLoad ) {
                if( loadOn ) {
                    if (LOAD_LED_PIN >= 0) digitalWrite(LOAD_LED_PIN, LOAD_LED_ON);
                    slog("Load is ON", LOG_NOTICE);
                }
                else {
                    if (LOAD_LED_PIN >= 0) digitalWrite(LOAD_LED_PIN, LOAD_LED_OFF);
                    slog("Load is OFF", LOG_NOTICE);
                }
                prevStatus = true;
                prevLoad = loadOn;
            }
        }
        else {
            load_state = -1;
            if( prevStatus ) {
                if (LOAD_LED_PIN >= 0) digitalWrite(LOAD_LED_PIN, LOAD_LED_ON);  // assume ON
                slog("Load is UNKNOWN", LOG_ERR);
                prevStatus = false;
                prevLoad = true;
            }
        }
    }

    return prevLoad;
}


// check ntp status
// return true if time is valid
bool check_ntptime() {
    static bool have_time = false;

    #if defined(ESP32)
        bool valid_time = time(0) > 1582230020;
    #else
        ntp.update();
        bool valid_time = ntp.isTimeSet();
    #endif

    if (!have_time && valid_time) {
        have_time = true;
        time_t now = time(NULL);
        strftime(start_time, sizeof(start_time), "%FT%T", localtime(&now));
        snprintf(msg, sizeof(msg), "Got valid time at %s", start_time);
        slog(msg, LOG_NOTICE);
        if (mqtt.connected()) {
            publish(MQTT_TOPIC "/status/StartTime", start_time);
        }
    }

    return have_time;
}


// Status led update
void handle_breathe() {
    if (HEALTH_LED_PIN < 0) return;
    static uint32_t start = 0;  // start of last breath
    static uint32_t min_duty = 1;  // limit min brightness
    static uint32_t max_duty = PWMRANGE / 2;  // limit max brightness
    static uint32_t prev_duty = 0;

    // map elapsed in breathing intervals
    uint32_t now = millis();
    uint32_t elapsed = now - start;
    if (elapsed > breathe_interval) {
        start = now;
        elapsed -= breathe_interval;
    }

    // map min brightness to max brightness twice in one breathe interval
    uint32_t duty = (max_duty - min_duty) * elapsed * 2 / breathe_interval + min_duty;
    if (duty > max_duty) {
        // second range: breathe out aka get darker
        duty = 2 * max_duty - duty;
    }

    duty = duty * duty / max_duty;  // generally reduce lower brightness levels

    if (duty != prev_duty) {
        // adjust pwm duty cycle
        prev_duty = duty;
        if (HEALTH_LED_ON == LOW) {
            // inverted
            duty = PWMRANGE - duty;
        }
        #if defined(ESP32)
            ledcWrite(HEALTH_LED_PIN, duty);  // core 3.x: pin, not channel
        #else
            analogWrite(HEALTH_LED_PIN, duty);
        #endif
    }
}


void handle_es3Time( bool time_valid ) {
    static bool time_set = false;

    if( !time_set && time_valid ) {
        struct tm now;
        getLocalTime(&now);  // TODO: ESP32 only?
        if (esmart3.setTime(now)) {
            time_set = true;
            slog("eSmart3 time set", LOG_NOTICE);
        }
    }
}


// True if the 16 bit words [start, end) of two device structs are equal
bool same_words(const void *a, const void *b, size_t start, size_t end) {
    return !memcmp((const uint16_t *)a + start, (const uint16_t *)b + start, (end - start) * 2);
}

// Set battery parameters for 4S, 272Ah (only where the charger differs)
// TODO set in webpage or mqtt, store preference
void setup_LiFePO() {
    uint16_t s_cells = 4;  // 4, 8, 12, 16
    uint16_t p_cells = 1;  // 1 - 4
    uint16_t maxCellDeciVolt = 36;  // ~95% LiFePO capacity
    uint16_t minCellDeciVolt = 30;  // ~15% LiFePO capacity
    uint16_t capacityAh = 272;  // my LiFePO capacity
    uint16_t maxDeviceCurr = 600;  // max deciAmps of my eSmart3
    
    ESmart3::BatParam_t batParam = {0};
    batParam.wBatType = 0;  // LiFePO: user(0)
    batParam.wBatSysType = s_cells / 4;  // SysType=12V-multiplier and 4 LiFePO cells are ~12V
    batParam.wBulkVolt = maxCellDeciVolt * s_cells;  // CC -> CV limit
    batParam.wFloatVolt = 0;  // no continuous charge for lifepo
    batParam.wEqualizeChgVolt = 0;  // no monthly refresh charge for lifepo
    batParam.wEqualizeChgTime = 0;  // minutes refresh for lifepo
    batParam.wMaxChgCurr = capacityAh * p_cells * 10;  // 1C for LiFePO
    if( batParam.wMaxChgCurr > maxDeviceCurr ) {
        batParam.wMaxChgCurr = maxDeviceCurr;  // eSmart3 40A limit, esmart4 60A limit
    }
    batParam.wMaxDisChgCurr = 300;  // load max 30A for eSmart4 (eSmart3 claimed 40A but broke :()

    // Write only what differs: the ESP reboots on every longer WLAN loss, and
    // 4.1 rewrote the charger's settings on each boot. Compared are the words
    // setBatParam() writes: [1, end-2), i.e. without wFlag, bLoadUseSel, ChkSum.
    ESmart3::BatParam_t curBat = {0};
    if( esmart3.getBatParam(curBat) && same_words(&curBat, &batParam, 1, sizeof(batParam) / 2 - 2) ) {
        slog("BatParam already set");
    }
    else if( esmart3.setBatParam(batParam) ) {
        slog("setBatParam done", LOG_NOTICE);
    }
    else {
        slog("setBatParam error", LOG_ERR);
    }

    ESmart3::ProParam_t proParam = {0};
    proParam.wLoadOvp = 148;  // protect end device from >= 14.8V
    proParam.wLoadUvp = minCellDeciVolt * s_cells;  // protect battery from load if < 15% capa
    proParam.wBatOvB = maxCellDeciVolt * s_cells + 5; // unprotect battery at slightly above max voltage
    proParam.wBatOvp = proParam.wBatOvB + proParam.wBatOvB / 10; // protect battery from > 10% max voltage
    proParam.wBatUvp = proParam.wLoadUvp - proParam.wLoadUvp / 10;  // protect battery 10% below wLoadUvp
    proParam.wBatUvB = proParam.wLoadUvp - 5;  // recovery slightly below wLoadUvp
    ESmart3::ProParam_t curPro = {0};   // setProParam() writes words [1, end-1)
    if( esmart3.getProParam(curPro) && same_words(&curPro, &proParam, 1, sizeof(proParam) / 2 - 1) ) {
        slog("ProParam already set");
    }
    else if( esmart3.setProParam(proParam) ) {
        slog("setProParam done", LOG_NOTICE);
    }
    else {
        slog("setProParam error", LOG_ERR);
    }
}


// Reset reason can be quite useful...
// Messages from arduino core example
void print_reset_reason(int core) {
  switch (rtc_get_reset_reason(core)) {
    case 1  : slog("Vbat power on reset");break;
    case 3  : slog("Software reset digital core");break;
    case 4  : slog("Legacy watch dog reset digital core");break;
    case 5  : slog("Deep Sleep reset digital core");break;
    case 6  : slog("Reset by SLC module, reset digital core");break;
    case 7  : slog("Timer Group0 Watch dog reset digital core");break;
    case 8  : slog("Timer Group1 Watch dog reset digital core");break;
    case 9  : slog("RTC Watch dog Reset digital core");break;
    case 10 : slog("Instrusion tested to reset CPU");break;
    case 11 : slog("Time Group reset CPU");break;
    case 12 : slog("Software reset CPU");break;
    case 13 : slog("RTC Watch dog Reset CPU");break;
    case 14 : slog("for APP CPU, reseted by PRO CPU");break;
    case 15 : slog("Reset when the vdd voltage is not stable");break;
    case 16 : slog("RTC Watch dog reset digital core and rtc module");break;
    default : slog("Reset reason unknown");
  }
}


// Home Assistant MQTT discovery (docs/spec-homeassistant.md): one retained
// message per loop pass, so connecting never blocks the loop for long.
size_t ha_next = SIZE_MAX;             // next message index, SIZE_MAX = all sent
uint8_t ha_cells = 0, ha_ntcs = 0;     // cell / NTC counts announced so far
int8_t published_load = -2;            // load state last sent to status/Load
bool published_faults = false;         // fault bits sent since the last connect

void ha_announce_from(size_t index) {
    if (index < ha_next) ha_next = index;
}

void handle_ha_discovery() {
    if (!mqtt.connected()) return;

    // cells and NTCs are known after the first BMS status; announce on change
    if (jbdStatus.cells != ha_cells || jbdStatus.ntcs != ha_ntcs) {
        ha_cells = jbdStatus.cells;
        ha_ntcs = jbdStatus.ntcs;
        ha_announce_from(HaDiscovery::FIXED_COUNT);
    }

    // retained fault bits: otherwise only sent on change, so a state from before
    // the connect (or one that never changes) would never reach HA
    if (!published_faults && chargerHealth.seen && bmsHealth.seen && jbdStatus.cells) {
        publish_retained(MQTT_TOPIC "/status/Charger", bits(es3ChgSts.wFault, 10));
        publish_retained(MQTT_TOPIC "/status/BMS", bits(jbdStatus.fault, 13));
        if (es3Log.wStartCnt) {
            json_Log(msg, sizeof(msg), es3Log);
            publish_retained(MQTT_TOPIC "/json/Log", msg);
        }
        published_faults = true;
    }

    // retained load state for the HA switch
    if (load_state >= 0 && load_state != published_load) {
        publish_retained(MQTT_TOPIC "/status/Load", load_state ? "on" : "off");
        published_load = load_state;
    }

    if (ha_next >= HaDiscovery::count(ha_cells, ha_ntcs)) {
        ha_next = SIZE_MAX;
        return;
    }
    static char node[33], dev_name[40];
    if (!*node) {
        size_t i = 0;     // "LiFePO_Island-1" -> "lifepo_island_1"
        for (const char *p = HOSTNAME; *p && i + 1 < sizeof(node); p++) {
            node[i++] = *p == '-' ? '_' : tolower((unsigned char)*p);
        }
        node[i] = '\0';
        snprintf(dev_name, sizeof(dev_name), "%s", HOSTNAME);
        char *dash = strrchr(dev_name, '-');
        if (dash) *dash = ' ';
    }
    const HaDiscovery::Context ctx = { HA_DISCOVERY_PREFIX, MQTT_TOPIC, node, dev_name, VERSION };
    static char topic[128], payload[HaDiscovery::MAX_MESSAGE];
    if (HaDiscovery::build(ctx, ha_next, ha_cells, ha_ntcs, topic, sizeof(topic), payload, sizeof(payload))) {
        if (!mqtt.publish(topic, payload, true)) {
            mqtt_publish_failures++;
            snprintf(msg, sizeof(msg), "HA discovery publish failed: %s (%u bytes)", topic, (unsigned)strlen(payload));
            slog(msg, LOG_ERR);
        }
    }
    else {
        snprintf(msg, sizeof(msg), "HA discovery message %u does not fit", (unsigned)ha_next);
        slog(msg, LOG_ERR);
    }
    ha_next++;
}


// Called on incoming mqtt messages (commands: docs/spec-telemetry.md)
void mqtt_callback(char* topic, byte* payload, unsigned int length) {
    if (strcmp(HA_DISCOVERY_PREFIX "/status", topic) == 0) {
        if (length == 6 && strncasecmp((const char *)payload, "online", 6) == 0) {
            ha_announce_from(0);   // HA (re)started: announce everything again
        }
        return;
    }

    if (strcasecmp(MQTT_TOPIC "/cmd", topic) == 0) {
        MqttCmd::Cmd cmd = MqttCmd::parse((const char *)payload, length);
        if (cmd != MqttCmd::NONE) {
            snprintf(msg, sizeof(msg), "Execute mqtt command '%s'", MqttCmd::name(cmd));
            slog(msg, LOG_INFO);
            bool on = cmd == MqttCmd::LOAD_ON;
            if (esmart3.setLoad(on)) {
                load_state = on;
            }
            else {
                slog("Mqtt load command failed", LOG_ERR);
            }
            return;
        }
    }

    snprintf(msg, sizeof(msg), "Ignore mqtt %s: '%.*s'", topic, length > 64 ? 64 : (int)length, (char *)payload);
    slog(msg, LOG_WARNING);
}


void handle_mqtt( bool time_valid ) {
    static const int32_t interval = 5000;  // if disconnected try reconnect this often in ms
    static uint32_t prev = -interval;      // first connect attempt without delay

    if (mqtt.connected()) {
        mqtt.loop();
    }
    else {
        uint32_t now = millis();
        if (now - prev > interval) {
            if (mqtt.connect(HOSTNAME, MQTT_TOPIC "/status/LWT", 0, true, "Offline")
             && mqtt.publish(MQTT_TOPIC "/status/LWT", "Online", true)
             && mqtt.publish(MQTT_TOPIC "/status/Hostname", HOSTNAME)
             && mqtt.publish(MQTT_TOPIC "/status/DBServer", netConfig.active().influxHost)
             && mqtt.publish(MQTT_TOPIC "/status/DBPort", itoa(netConfig.active().influxPort, msg, 10))
             && mqtt.publish(MQTT_TOPIC "/status/DBName", INFLUX_DB)
             && mqtt.publish(MQTT_TOPIC "/status/Version", VERSION)
             && (!time_valid || mqtt.publish(MQTT_TOPIC "/status/StartTime", start_time))
             && mqtt.subscribe(MQTT_TOPIC "/cmd")
             && mqtt.subscribe(HA_DISCOVERY_PREFIX "/status")) {
                ha_announce_from(0);
                published_load = -2;
                published_faults = false;
                mqtt_last_connect_ms = millis();
                mqtt_last_rc = mqtt.state();
                snprintf(msg, sizeof(msg), "Connected to MQTT broker %s:%u using topic %s",
                    netConfig.active().mqttHost, netConfig.active().mqttPort, MQTT_TOPIC);
                slog(msg, LOG_NOTICE);
            }
            else {
                int error = mqtt.state();
                mqtt_last_rc = error;
                mqtt_connect_failures++;
                mqtt.disconnect();
                snprintf(msg, sizeof(msg), "Connect to MQTT broker %s:%u failed with code %d",
                    netConfig.active().mqttHost, netConfig.active().mqttPort, error);
                slog(msg, LOG_ERR);
            }
            prev = now;
        }
    }
 }


// Startup
void setup() {
    WiFi.mode(WIFI_STA);
    String host(HOSTNAME);
    host.toLowerCase();
    WiFi.hostname(host.c_str());

    if (HEALTH_LED_PIN >= 0) {
        pinMode(HEALTH_LED_PIN, OUTPUT);
        digitalWrite(HEALTH_LED_PIN, HEALTH_LED_ON);
    }

    Serial.begin(BAUDRATE);
    Serial.println("\nStarting " PROGNAME " v" VERSION " " __DATE__ " " __TIME__);

    // Hosts: NVS over build-flag defaults; the strings never move
    netConfig.load();
    const NetConfig::Values &net = netConfig.active();

    // Syslog setup (the library resolves the name per packet: moved aliases are followed)
    if (net.syslogHost[0]) {
        syslog.server(net.syslogHost, net.syslogPort);
    }
    syslog.deviceHostname(WiFi.getHostname());
    syslog.appName("Joba1");
    syslog.defaultPriority(LOG_KERN);

    if (HEALTH_LED_PIN >= 0) digitalWrite(HEALTH_LED_PIN, HEALTH_LED_OFF);

    WiFiManager wm;
    // wm.resetSettings();
    wm.setConfigPortalTimeout(180);
    uint32_t ip[5] = {0};
    if (ip_config(ip, 5) && ip[0]) {
        // TODO if dns ip changes, set it here once instead of ip[3]
        wm.setSTAStaticIPConfig(IPAddress(ip[0]), IPAddress(ip[1]), IPAddress(ip[2]), IPAddress(ip[3]));
    }
#ifdef BENCH_WIFI_SSID
    // Test bench: join the bench network directly (stored like portal credentials)
    if (WiFi.SSID() != BENCH_WIFI_SSID) {
        WiFi.begin(BENCH_WIFI_SSID, BENCH_WIFI_PASS);
        for (int i = 0; i < 30 && !WiFi.isConnected(); i++) delay(500);
    }
#endif
    // Portal password: the admin password if it is long enough for WPA2 (>= 8),
    // otherwise the host name as before 4.2 (which anyone nearby can guess)
    const char *portal_pw = strlen(ADMIN_PASSWORD) >= 8 ? ADMIN_PASSWORD : WiFi.getHostname();
    if (!wm.autoConnect(WiFi.getHostname(), portal_pw)) {
        Serial.println("Failed to connect WLAN, about to reset");
        for (int i = 0; i < 20; i++) {
            if (HEALTH_LED_PIN >= 0) digitalWrite(HEALTH_LED_PIN, (i & 1) ? HEALTH_LED_ON : HEALTH_LED_OFF);
            delay(100);
        }
        ESP.restart();
        while (true)
            ;
    }
    uint32_t ip2[5] = { (uint32_t)WiFi.localIP(), (uint32_t)WiFi.gatewayIP(), (uint32_t)WiFi.subnetMask(), (uint32_t)WiFi.dnsIP(0), (uint32_t)WiFi.dnsIP(1) };
    if (memcmp(ip, ip2, sizeof(ip))) {
        if (ip_config(ip2, 5, true)) {
            slog("Wrote IP config");
        }
        else {
            slog("Write IP config failed");
        }
    }

    if (HEALTH_LED_PIN >= 0) digitalWrite(HEALTH_LED_PIN, HEALTH_LED_ON);
    char msg[80];
    snprintf(msg, sizeof(msg), "%s Version %s, WLAN IP is %s", PROGNAME, VERSION,
        WiFi.localIP().toString().c_str());
    slog(msg, LOG_NOTICE);

    {
        char nmsg[400];
        snprintf(nmsg, sizeof(nmsg), "Netconfig (%s): syslog=%s:%u mqtt=%s:%u influx=%s:%u ntp=%s",
            netConfig.fromFlash() ? "flash" : "defaults", net.syslogHost, net.syslogPort,
            net.mqttHost, net.mqttPort, net.influxHost, net.influxPort, net.ntp);
        slog(nmsg, LOG_NOTICE);
    }

    #if defined(ESP8266)
        ntp.setPoolServerName(net.ntp);
        ntp.begin();
    #else
        sntp_set_time_sync_notification_cb([](struct timeval *) { ntp_last_reply_ms = millis(); });
        configTzTime(TIMEZONE, net.ntp);  // CET/CEST with the switch dates (4.1: CEST all year)
    #endif

    MDNS.begin(WiFi.getHostname());

    if (ADMIN_PASSWORD[0]) {
        esp_updater.setup(&web_server, "/update", admin_user, ADMIN_PASSWORD);
    }
    else {
        esp_updater.setup(&web_server);
    }
    setup_webserver();

    mqtt.setServer(net.mqttHost, net.mqttPort);   // resolved again on every connect
    mqtt.setCallback(mqtt_callback);

#if defined(ESP8266)
    rs485.begin(9600, SWSERIAL_8N1, 13, 15);  // Use pins 13 and 15 for RX and TX
    analogWriteRange(PWMRANGE);  // for health led breathing steps
#elif defined(ESP32)
    print_reset_reason(0);
    print_reset_reason(1);  // assume 2nd core (should I ask?)

#ifndef SIMULATE
    rs485.begin(9600, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN, false, 1000);
#endif
    if (HEALTH_LED_PIN >= 0) ledcAttach(HEALTH_LED_PIN, 1000, PWMBITS);
#else
    analogWriteRange(PWMRANGE);  // for health led breathing steps
    // init your non ESP serial port here
#endif

    if (LOAD_BUTTON_PIN >= 0) pinMode(LOAD_BUTTON_PIN, INPUT_PULLUP);  // to toggle load status
    if (LOAD_LED_PIN >= 0) {
        pinMode(LOAD_LED_PIN, OUTPUT);  // to show load status
        digitalWrite(LOAD_LED_PIN, LOAD_LED_OFF);
    }

    esmart3.begin(RS485_DIR_PIN);
    setup_LiFePO();

    jbdbms.begin(RS485_DIR_PIN);  // same pin as esmart3

    slog("Setup done", LOG_NOTICE);
}


// Main loop
void loop() {
    handle_es3Information();
    handle_jbdHardware();
    
    bool have_time = check_ntptime();
    if( es3Information.wSerial[0] ) {  // we have required esmart3 infos
        handle_es3Time(have_time);
        handle_es3ChgSts();
        handle_es3BatParam();
        handle_es3Log();
        handle_es3Parameters();
        handle_es3ProParam();
        handle_es3LoadParam();
        // ignoring TempParam and EngSave (for now?)
    }
    
    if( jbdHardware.id[0] ) {  // we have required bms infos
        handle_jbdStatus();
        handle_jbdCells();
    }

    if (es3Information.wSerial[0] 
     && jbdHardware.id[0]
     && have_time 
     && enabledBreathing) {
        breathe_interval = (influx_status < 200 || influx_status >= 300 || es3ChgSts.wFault || jbdStatus.fault) ? err_interval : ok_interval;
        handle_breathe();  // health indicator
    }

    handle_load_button(handle_load_led());
    web_server.handleClient();
    if (should_reboot && (int32_t)(millis() - reboot_at) >= 0) {
        ESP.restart();
    }
    handle_mqtt(have_time);
    handle_wifi();
    handle_influx();
    handle_ha_discovery();
}
