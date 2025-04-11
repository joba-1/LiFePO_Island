/*
Monitor an eSmart3 and a JDB BMS via RS485 communication

Influx DB can be created with command influx -execute "create database LiFePO_Island"

Monitors GPIO 0 pulled to ground as key press to toggle load
Use builtin led to represent health status
Use defined pin LED to represent load status
Use pin 22 to toggle RS485 read/write
*/


#include <Arduino.h>

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

    #define HEALTH_LED_ON HIGH
    #define HEALTH_LED_OFF LOW
    #ifdef LED_BUILTIN
        #define HEALTH_LED_PIN LED_BUILTIN
    #else
        #define HEALTH_LED_PIN 16
    #endif
    #define HEALTH_PWM_CH 0
    #define LOAD_LED_ON LOW
    #define LOAD_LED_OFF HIGH
    #define LOAD_LED_PIN 33
    #define LOAD_BUTTON_PIN 0

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

uint32_t rs485_access_ms = 0;              // rs485 access timestamp for esmart3 and jbdbms
ESmart3 esmart3(rs485, &rs485_access_ms);  // Serial port to communicate with RS485 adapter

// JbdBms device
#include <jbdbms.h>

JbdBms jbdbms(rs485, &rs485_access_ms);  // Same serial port as esmart3 is ok, if parameters are the same


void slog(const char *message, uint16_t pri = LOG_INFO) {
    static bool log_infos = true;
    
    if (pri < LOG_INFO || log_infos) {
        Serial.println(message);
        syslog.log(pri, message);
    }

    if (log_infos && millis() > 10 * 60 * 1000) {
        log_infos = false;  // log infos only for first 10 minutes
        slog("Switch off info level messages", LOG_NOTICE);
    }
}


void publish( const char *topic, const char *payload ) {
    if (mqtt.connected() && !mqtt.publish(topic, payload)) {
        slog("Mqtt publish failed");
    }
}


// Post data to InfluxDB
bool postInflux(const char *line) {
    static const char uri[] = "/write?db=" INFLUX_DB "&precision=s";

    WiFiClient wifiHttp;
    HTTPClient http;

    http.begin(wifiHttp, INFLUX_SERVER, INFLUX_PORT, uri);
    http.setUserAgent(PROGNAME);
    int prev = influx_status;
    influx_status = http.POST(line);
    String payload;
    if (http.getSize() > 0) { // workaround for bug in getString()
        payload = http.getString();
    }
    http.end();

    if (influx_status != prev) {
        snprintf(msg, sizeof(msg), "%d", influx_status);
        publish(MQTT_TOPIC "/status/DBResponse", msg);
    }

    if (influx_status < 200 || influx_status >= 300) {
        snprintf(msg, sizeof(msg), "Post %s:%d%s status=%d line='%s' response='%s'",
            INFLUX_SERVER, INFLUX_PORT, uri, influx_status, line, payload.c_str());
        slog(msg, LOG_ERR);
        return false;
    }

    post_time = time(NULL);
    return true;
}


// Wifi status as JSON
bool json_Wifi(char *json, size_t maxlen, const char *bssid, int8_t rssi) {
    static const char jsonFmt[] =
        "{\"Version\":" VERSION ",\"Hostname\":\"%s\",\"Wifi\":{"
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
                    digitalWrite(HEALTH_LED_PIN, (i & 1) ? HEALTH_LED_ON : HEALTH_LED_OFF);
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
        "{\"Version\":" VERSION ",\"Serial\":\"%8.8s\",\"Information\":{"
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
        if (esmart3.getInformation(data)) {
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
        else {
            slog("getInformation error", LOG_ERR);
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


bool json_ChgSts(char *json, size_t maxlen, ESmart3::ChgSts_t data) {
    static const char jsonFmt[] =
        "{\"Version\":" VERSION ",\"Serial\":\"%8.8s\",\"ChgSts\":{"
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
        if( esmart3.getChgSts(data) ) {
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
                    publish(MQTT_TOPIC "/status/Charger", faults);        
                }

                es3ChgSts = data;

                snprintf(msg, sizeof(msg), lineFmt, (char *)es3Information.wSerial, WiFi.getHostname(), 
                    data.wChgMode, data.wPvVolt, data.wBatVolt, data.wChgCurr, data.wOutVolt,
                    data.wLoadVolt, data.wLoadCurr, data.wChgPower, data.wLoadPower, data.wBatTemp, 
                    data.wInnerTemp, data.wBatCap, data.dwCO2, faults, data.wSystemReminder);
                postInflux(msg);
            }
        }
        else {
            slog("getChgSts error", LOG_ERR);
        }
    }
}


bool json_BatParam(char *json, size_t maxlen, ESmart3::BatParam_t data) {
    static const char jsonFmt[] =
        "{\"Version\":" VERSION ",\"Serial\":\"%8.8s\",\"BatParam\":{"
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
        if( esmart3.getBatParam(data) ) {
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
        else {
            slog("getBatParam error", LOG_ERR);
        }
    }
}


bool json_Log(char *json, size_t maxlen, ESmart3::Log_t data) {
    static const char jsonFmt[] =
        "{\"Version\":" VERSION ",\"Serial\":\"%8.8s\",\"Log\":{"
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
        if( esmart3.getLog(data) ) {
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
                publish(MQTT_TOPIC "/json/Log", msg);

                snprintf(msg, sizeof(msg), lineFmt, (char *)es3Information.wSerial, WiFi.getHostname(), 
                    data.dwRunTime, data.wStartCnt, data.wLastFaultInfo, data.wFaultCnt, 
                    data.dwTodayEng, data.wTodayEngDate.month, data.wTodayEngDate.day, data.dwMonthEng, 
                    data.wMonthEngDate.month, data.wMonthEngDate.day, data.dwTotalEng, data.dwLoadTodayEng, 
                    data.dwLoadMonthEng, data.dwLoadTotalEng, data.wBacklightTime, data.bSwitchEnable);
                postInflux(msg);
            }
        }
        else {
            slog("getLog error", LOG_ERR);
        }
    }
}


bool json_Parameters(char *json, size_t maxlen, ESmart3::Parameters_t data) {
    static const char jsonFmt[] =
        "{\"Version\":" VERSION ",\"Serial\":\"%8.8s\",\"Parameters\":{"
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
        if( esmart3.getParameters(data) ) {
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
        else {
            slog("getParameters error", LOG_ERR);
        }
    }
}


bool json_LoadParam(char *json, size_t maxlen, ESmart3::LoadParam_t data) {
    static const char jsonFmt[] =
        "{\"Version\":" VERSION ",\"Serial\":\"%8.8s\",\"LoadParam\":{"
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
        if( esmart3.getLoadParam(data) ) {
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
        else {
            slog("getLoadParam error", LOG_ERR);
        }
    }
}


bool json_ProParam(char *json, size_t maxlen, ESmart3::ProParam_t data) {
    static const char jsonFmt[] =
        "{\"Version\":" VERSION ",\"Serial\":\"%8.8s\",\"ProParam\":{"
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
        if( esmart3.getProParam(data) ) {
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
        else {
            slog("getProParam error", LOG_ERR);
        }
    }
}


JbdBms::Hardware_t jbdHardware = {0};

bool json_Hardware(char *json, size_t maxlen, JbdBms::Hardware_t data) {
    static const char jsonFmt[] = "{\"Version\":" VERSION ",\"Id\":\"%.32s\"}";
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
        if (jbdbms.getHardware(data)) {
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
        else {
            slog("getHardware error", LOG_ERR);
        }
    }
}


JbdBms::Status_t jbdStatus = {0};

bool json_Status(char *json, size_t maxlen, JbdBms::Status_t data) {
    static const char jsonFmt[] =
        "{\"Version\":" VERSION ",\"Id\":\"%.32s\",\"Status\":{"
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
        if (jbdbms.getStatus(data)) {
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
                    publish(MQTT_TOPIC "/status/BMS", bits(data.fault, 13));        
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
        else {
            slog("getStatus error", LOG_ERR);
        }
    }
}


JbdBms::Cells_t jbdCells = {0};

bool json_Cells(char *json, size_t maxlen, JbdBms::Cells_t data) {
    static const char jsonFmt[] = "{\"Version\":" VERSION ",\"Id\":\"%.32s\",\"Cells\":%s]}";
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
        if (jbdbms.getCells(data)) {
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
        else {
            slog("getCells error", LOG_ERR);
        }
    }
}


// Copy verbose error status string into msg
// Return length of message (ends in ' ...' if cut due to msg_size too small)
size_t decode_error( char *msg, size_t msg_size ) {
    char *endp = msg + msg_size;
    char *cursor = msg;  // cursor position in msg

    if (es3ChgSts.wFault) {
        if (es3ChgSts.wFault &0b0000000001 && endp > cursor) {
            cursor += snprintf(cursor, endp - cursor, "CHG: %s<br/>\n", "Battery over voltage");
        }
        if (es3ChgSts.wFault &0b0000000010 && endp > cursor) {
            cursor += snprintf(cursor, endp - cursor, "CHG: %s<br/>\n", "PV over voltage");
        }
        if (es3ChgSts.wFault &0b0000000100 && endp > cursor) {
            cursor += snprintf(cursor, endp - cursor, "CHG: %s<br/>\n", "Charge over current");
        }
        if (es3ChgSts.wFault &0b0000001000 && endp > cursor) {
            cursor += snprintf(cursor, endp - cursor, "CHG: %s<br/>\n", "Discharge over current");
        }
        if (es3ChgSts.wFault &0b0000010000 && endp > cursor) {
            cursor += snprintf(cursor, endp - cursor, "CHG: %s<br/>\n", "Battery temperature alarm");
        }
        if (es3ChgSts.wFault &0b0000100000 && endp > cursor) {
            cursor += snprintf(cursor, endp - cursor, "CHG: %s<br/>\n", "Internal temperature alarm");
        }
        if (es3ChgSts.wFault &0b0001000000 && endp > cursor) {
            cursor += snprintf(cursor, endp - cursor, "CHG: %s<br/>\n", "PV low voltage");
        }
        if (es3ChgSts.wFault &0b0010000000 && endp > cursor) {
            cursor += snprintf(cursor, endp - cursor, "CHG: %s<br/>\n", "Battery low voltage");
        }
        if (es3ChgSts.wFault &0b0100000000 && endp > cursor) {
            cursor += snprintf(cursor, endp - cursor, "CHG: %s<br/>\n", "Trip zero protection trigger");
        }
        if (es3ChgSts.wFault &0b1000000000 && endp > cursor) {
            cursor += snprintf(cursor, endp - cursor, "CHG: %s<br/>\n", "In the control of manual switchgear");
        }
    }

    if (jbdStatus.fault) {
        if (jbdStatus.fault &0b0000000000001 && endp > cursor) {
            cursor += snprintf(cursor, endp - cursor, "BMS: %s<br/>\n", "Cell block over voltage");
        }
        if (jbdStatus.fault &0b0000000000010 && endp > cursor) {
            cursor += snprintf(cursor, endp - cursor, "BMS: %s<br/>\n", "Cell block under voltage");
        }
        if (jbdStatus.fault &0b0000000000100 && endp > cursor) {
            cursor += snprintf(cursor, endp - cursor, "BMS: %s<br/>\n", "Battery over voltage");
        }
        if (jbdStatus.fault &0b0000000001000 && endp > cursor) {
            cursor += snprintf(cursor, endp - cursor, "BMS: %s<br/>\n", "Battery under voltage");
        }
        if (jbdStatus.fault &0b0000000010000 && endp > cursor) {
            cursor += snprintf(cursor, endp - cursor, "BMS: %s<br/>\n", "Charging over temperature");
        }
        if (jbdStatus.fault &0b0000000100000 && endp > cursor) {
            cursor += snprintf(cursor, endp - cursor, "BMS: %s<br/>\n", "Charging low temperature");
        }
        if (jbdStatus.fault &0b0000001000000 && endp > cursor) {
            cursor += snprintf(cursor, endp - cursor, "BMS: %s<br/>\n", "Discharging over temperature");
        }
        if (jbdStatus.fault &0b0000010000000 && endp > cursor) {
            cursor += snprintf(cursor, endp - cursor, "BMS: %s<br/>\n", "Discharging low temperature");
        }
        if (jbdStatus.fault &0b0000100000000 && endp > cursor) {
            cursor += snprintf(cursor, endp - cursor, "BMS: %s<br/>\n", "Charging over current");
        }
        if (jbdStatus.fault &0b0001000000000 && endp > cursor) {
            cursor += snprintf(cursor, endp - cursor, "BMS: %s<br/>\n", "Discharging over current");
        }
        if (jbdStatus.fault &0b0010000000000 && endp > cursor) {
            cursor += snprintf(cursor, endp - cursor, "BMS: %s<br/>\n", "Short circuit");
        }
        if (jbdStatus.fault &0b0100000000000 && endp > cursor) {
            cursor += snprintf(cursor, endp - cursor, "BMS: %s<br/>\n", "Frontend IC error");
        }
        if (jbdStatus.fault &0b1000000000000 && endp > cursor) {
            cursor += snprintf(cursor, endp - cursor, "BMS: %s<br/>\n", "MOS software lockout");
        }
    }

    if (cursor >= endp) {
        snprintf(endp - 5, 5, " ...");
    }

    return cursor - msg;
}

char web_msg[256] = "";  // main web page displays and then clears this
bool changeIp = false;   // if true, ip changes after display of root url
IPAddress ip;            // the ip to change to (use DHCP if 0)

// Standard web page
const char *main_page() {
    static const char fmt[] =
        "<!doctype html>\n"
        "<html lang=\"en\">\n"
        " <head>\n"
        "  <title>" PROGNAME " %.16s %.32s v" VERSION "</title>\n"
        "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
        "  <meta charset=\"utf-8\">\n"
        "  <meta http-equiv=\"expires\" content=\"5\">\n"
        " </head>\n"
        " <body>\n"
        "  <h1>" PROGNAME " v" VERSION "</h1>\n"
        "  <h2>Charger %.16s</h2>\n"
        "  <table><tr>\n"
        "   <td><form action=\"on\" method=\"post\">\n"
        "    <input type=\"submit\" name=\"on\" value=\"Load ON\" />\n"
        "   </form></td>\n"
        "   <td><form action=\"toggle\" method=\"post\">\n"
        "    <input type=\"submit\" name=\"toggle\" value=\"Toggle Load\" />\n"
        "   </form></td>\n"
        "   <td><form action=\"off\" method=\"post\">\n"
        "    <input type=\"submit\" name=\"off\" value=\"Load OFF\" />\n"
        "   </form></td>\n"
        "  </tr></table>\n"
        "  <h2>BMS %.32s</h2>\n"
        "  <table><form action=\"mosfets\" method=\"post\"><tr>\n"
        "    <td><input type=\"checkbox\" name=\"charge\" id=\"charge\" value=\"Charge\" %s/><label for=\"charge\">Charge</label></td>\n"
        "    <td><input type=\"checkbox\" name=\"discharge\" id=\"discharge\" value=\"Discharge\" %s/><label for=\"discharge\">Discharge</label></td>\n"
        "    <td><input type=\"submit\" name=\"mosfets\" value=\"Set Mosfets\" />\n"
        "  </tr></form></table>\n"
        "  <p><strong>%s</strong></p>\n"
        "  <p><table>\n"
        "   <tr><td>Information</td><td><a href=\"/json/Information\">JSON</a></td></tr>\n"
        "   <tr><td>ChgSts</td><td><a href=\"/json/ChgSts\">JSON</a></td></tr>\n"
        "   <tr><td>BatParam</td><td><a href=\"/json/BatParam\">JSON</a></td></tr>\n"
        "   <tr><td>Log</td><td><a href=\"/json/Log\">JSON</a></td></tr>\n"
        "   <tr><td>Parameters</td><td><a href=\"/json/Parameters\">JSON</a></td></tr>\n"
        "   <tr><td>LoadParam</td><td><a href=\"/json/LoadParam\">JSON</a></td></tr>\n"
        "   <tr><td>ProParam</td><td><a href=\"/json/ProParam\">JSON</a></td></tr>\n"
        "   <tr><td></td></tr>\n"
        "   <tr><td>Status</td><td><a href=\"/json/Status\">JSON</a></td></tr>\n"
        "   <tr><td>Cells</td><td><a href=\"/json/Cells\">JSON</a></td></tr>\n"
        "   <tr><td></td></tr>\n"
        "   <tr><td>Wifi</td><td><a href=\"/json/Wifi\">JSON</a></td></tr>\n"
        "   <tr><td></td></tr>\n"
        "   <tr><td>Post firmware image to</td><td><a href=\"/update\">/update</a></td></tr>\n"
        "   <tr><td>Last start time</td><td>%s</td></tr>\n"
        "   <tr><td>Last web update</td><td>%s</td></tr>\n"
        "   <tr><td>Last influx update</td><td>%s</td></tr>\n"
        "   <tr><td>Influx status</td><td>%d</td></tr>\n"
        "   <tr><td>RSSI %s</td><td>%d</td></tr>\n"
        "   <tr><form action=\"ip\" method=\"post\">\n"
        "    <td>IP <input type=\"text\" id=\"ip\" name=\"ip\" value=\"%s\" /></td>\n"
        "    <td><input type=\"submit\" name=\"change\" value=\"Change IP\" /></td>\n"
        "   </form></tr>\n"
        "  </table></p>\n"
        "  <p><table><tr>\n"
        "   <td><form action=\"/\" method=\"get\">\n"
        "    <input type=\"submit\" name=\"reload\" value=\"Reload\" />\n"
        "   </form></td>\n"
        "   <td><form action=\"breathe\" method=\"post\">\n"
        "    <input type=\"submit\" name=\"breathe\" value=\"Toggle Breathe\" />\n"
        "   </form></td>\n"
        "   <td><form action=\"reset\" method=\"post\">\n"
        "    <input type=\"submit\" name=\"reset\" value=\"Reset ESP\" />\n"
        "   </form></td>\n"
        "  </tr></table></p>\n"
        "  <p><small>... by <a href=\"https://github.com/joba-1/LiFePO_Island\">Joachim Banzhaf</a>, " __DATE__ " " __TIME__ "</small></p>\n"
        " </body>\n"
        "</html>\n";
    static char page[sizeof(fmt) + 700] = "";
    static char curr_time[30], influx_time[30];
    time_t now;
    time(&now);
    strftime(curr_time, sizeof(curr_time), "%FT%T", localtime(&now));
    strftime(influx_time, sizeof(influx_time), "%FT%T", localtime(&post_time));
    if (!*web_msg && (es3ChgSts.wFault || jbdStatus.fault)) {
        decode_error(web_msg, sizeof(web_msg));
    }
    snprintf(page, sizeof(page), fmt, (char *)es3Information.wModel, jbdHardware.id, 
        (char *)es3Information.wModel, jbdHardware.id, 
        jbdStatus.mosfetStatus & JbdBms::MOSFET_CHARGE ? "checked " : "", 
        jbdStatus.mosfetStatus & JbdBms::MOSFET_DISCHARGE ? "checked " : "", 
        web_msg, start_time, curr_time, influx_time, influx_status, lastBssid, lastRssi, WiFi.localIP().toString().c_str());
    *web_msg = '\0';
    return page;
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


// Define web pages for update, reset or for event infos
void setup_webserver() {
    web_server.on("/toggle", HTTP_POST, []() {
        bool on;
        const char *msg = "Load unknown";
        if (esmart3.getLoad(on)) {
            on = !on;
            if (esmart3.setLoad(on)) {
                msg = on ? "Load on" : "Load off";
            }
        }
        snprintf(web_msg, sizeof(web_msg), "%s", msg);
        web_server.sendHeader("Location", "/", true);  
        web_server.send(302, "text/plain", "");
    });

    web_server.on("/on", HTTP_POST, []() {
        bool on;
        const char *msg = "Load on";
        if (!esmart3.getLoad(on) || !on) {
            if (!esmart3.setLoad(true)) {
                msg = "Load unknown";
            }
        }
        snprintf(web_msg, sizeof(web_msg), "%s", msg);
        web_server.sendHeader("Location", "/", true);  
        web_server.send(302, "text/plain", "");
    });

    web_server.on("/off", HTTP_POST, []() {
        bool on;
        const char *msg = "Load off";
        if (!esmart3.getLoad(on) || on) {
            if (!esmart3.setLoad(false)) {
                msg = "Load unknown";
            }
        }
        snprintf(web_msg, sizeof(web_msg), "%s", msg);
        web_server.sendHeader("Location", "/", true);  
        web_server.send(302, "text/plain", "");
    });


    web_server.on("/mosfets", HTTP_POST, []() {
        uint8_t mosfetStatus = 0;
        const char *msg = "Mosfet status unchanged";

        if (web_server.hasArg("charge") && web_server.arg("charge") == "Charge") {
            mosfetStatus |= JbdBms::MOSFET_CHARGE;
        }
        if (web_server.hasArg("discharge") && web_server.arg("discharge") == "Discharge") {
            mosfetStatus |= JbdBms::MOSFET_DISCHARGE;
        }
        if (mosfetStatus != jbdStatus.mosfetStatus) {
            if (jbdbms.setMosfetStatus((JbdBms::mosfet_t)mosfetStatus)) {
                jbdStatus.mosfetStatus = mosfetStatus;
                switch (mosfetStatus) {
                    case JbdBms::MOSFET_NONE:
                        msg = "Charge and discharge OFF";
                        break; 
                    case JbdBms::MOSFET_CHARGE:
                        msg = "Charge ON and discharge OFF";
                        break; 
                    case JbdBms::MOSFET_DISCHARGE:
                        msg = "Charge OFF and discharge ON";
                        break; 
                    case JbdBms::MOSFET_BOTH:
                        msg = "Charge and discharge ON";
                        break; 
                }
            }
            else {
                msg = "Set mosfet status failed";
            }
        }

        snprintf(web_msg, sizeof(web_msg), "%s", msg);
        web_server.sendHeader("Location", "/", true);  
        web_server.send(302, "text/plain", "");
    });

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
        web_server.send(200, "text/html", main_page());

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

    // Catch all page
    web_server.onNotFound( []() { 
        snprintf(web_msg, sizeof(web_msg), "%s", "<h2>page not found</h2>\n");
        web_server.send(404, "text/html", main_page()); 
    });

    web_server.begin();

    MDNS.addService("http", "tcp", WEBSERVER_PORT);

    snprintf(msg, sizeof(msg), "Serving HTTP on port %d", WEBSERVER_PORT);
    slog(msg, LOG_NOTICE);
}


// toggle load on key press
// pin is pulled up if released and pulled down if pressed
void handle_load_button( bool loadOn ) {
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
            if( !prevStatus || loadOn != prevLoad ) {
                if( loadOn ) {
                    digitalWrite(LOAD_LED_PIN, LOAD_LED_ON);
                    slog("Load is ON", LOG_NOTICE);
                }
                else {
                    digitalWrite(LOAD_LED_PIN, LOAD_LED_OFF);
                    slog("Load is OFF", LOG_NOTICE);
                }
                prevStatus = true;
                prevLoad = loadOn;
            }
        }
        else {
            if( prevStatus ) {
                digitalWrite(LOAD_LED_PIN, LOAD_LED_ON);  // assume ON
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
            ledcWrite(HEALTH_PWM_CH, duty);
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


// Set battery parameters for 4S, 272Ah
// TODO set in webpage or mqtt, store preference
// TODO maybe just a warning if parameters differ? 
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

    if( esmart3.setBatParam(batParam) ) {
        slog("setBatParam done");
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
    if( esmart3.setProParam(proParam) ) {
        slog("setProParam done");
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


// Called on incoming mqtt messages
void mqtt_callback(char* topic, byte* payload, unsigned int length) {

    typedef struct cmd { const char *name; void (*action)(void); } cmd_t;
    
    static cmd_t cmds[] = { 
        { "load on", [](){ esmart3.setLoad(true); } },
        { "load off", [](){ esmart3.setLoad(false); } }
    };

    if (strcasecmp(MQTT_TOPIC "/cmd", topic) == 0) {
        for (auto &cmd: cmds) {
            if (strncasecmp(cmd.name, (char *)payload, length) == 0) {
                snprintf(msg, sizeof(msg), "Execute mqtt command '%s'", cmd.name);
                slog(msg, LOG_INFO);
                (*cmd.action)();
                return;
            }
        }
    }

    snprintf(msg, sizeof(msg), "Ignore mqtt %s: '%.*s'", topic, length, (char *)payload);
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
             && mqtt.publish(MQTT_TOPIC "/status/DBServer", INFLUX_SERVER)
             && mqtt.publish(MQTT_TOPIC "/status/DBPort", itoa(INFLUX_PORT, msg, 10))
             && mqtt.publish(MQTT_TOPIC "/status/DBName", INFLUX_DB)
             && mqtt.publish(MQTT_TOPIC "/status/Version", VERSION)
             && (!time_valid || mqtt.publish(MQTT_TOPIC "/status/StartTime", start_time))
             && mqtt.subscribe(MQTT_TOPIC "/cmd")) {
                snprintf(msg, sizeof(msg), "Connected to MQTT broker %s:%d using topic %s", MQTT_SERVER, MQTT_PORT, MQTT_TOPIC);
                slog(msg, LOG_NOTICE);
            }
            else {
                int error = mqtt.state();
                mqtt.disconnect();
                snprintf(msg, sizeof(msg), "Connect to MQTT broker %s:%d failed with code %d", MQTT_SERVER, MQTT_PORT, error);
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

    pinMode(HEALTH_LED_PIN, OUTPUT);
    digitalWrite(HEALTH_LED_PIN, HEALTH_LED_ON);

    Serial.begin(BAUDRATE);
    Serial.println("\nStarting " PROGNAME " v" VERSION " " __DATE__ " " __TIME__);

    // Syslog setup
    syslog.server(SYSLOG_SERVER, SYSLOG_PORT);
    syslog.deviceHostname(WiFi.getHostname());
    syslog.appName("Joba1");
    syslog.defaultPriority(LOG_KERN);

    digitalWrite(HEALTH_LED_PIN, HEALTH_LED_OFF);

    WiFiManager wm;
    // wm.resetSettings();
    wm.setConfigPortalTimeout(180);
    uint32_t ip[5] = {0};
    if (ip_config(ip, 5) && ip[0]) {
        // TODO if dns ip changes, set it here once instead of ip[3]
        wm.setSTAStaticIPConfig(IPAddress(ip[0]), IPAddress(ip[1]), IPAddress(ip[2]), IPAddress(ip[3]));
    }
    if (!wm.autoConnect(WiFi.getHostname(), WiFi.getHostname())) {
        Serial.println("Failed to connect WLAN, about to reset");
        for (int i = 0; i < 20; i++) {
            digitalWrite(HEALTH_LED_PIN, (i & 1) ? HEALTH_LED_ON : HEALTH_LED_OFF);
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

    digitalWrite(HEALTH_LED_PIN, HEALTH_LED_ON);
    char msg[80];
    snprintf(msg, sizeof(msg), "%s Version %s, WLAN IP is %s", PROGNAME, VERSION,
        WiFi.localIP().toString().c_str());
    slog(msg, LOG_NOTICE);

    #if defined(ESP8266)
        ntp.begin();
    #else
        configTime(3600, 3600, NTP_SERVER);  // MEZ/MESZ
    #endif

    MDNS.begin(WiFi.getHostname());

    esp_updater.setup(&web_server);
    setup_webserver();

    mqtt.setServer(MQTT_SERVER, MQTT_PORT);
    mqtt.setCallback(mqtt_callback);

#if defined(ESP8266)
    rs485.begin(9600, SWSERIAL_8N1, 13, 15);  // Use pins 13 and 15 for RX and TX
    analogWriteRange(PWMRANGE);  // for health led breathing steps
#elif defined(ESP32)
    print_reset_reason(0);
    print_reset_reason(1);  // assume 2nd core (should I ask?)

    rs485.begin(9600, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN, false, 1000);
    ledcAttach(HEALTH_LED_PIN, 1000, PWMBITS);
#else
    analogWriteRange(PWMRANGE);  // for health led breathing steps
    // init your non ESP serial port here
#endif

    pinMode(LOAD_BUTTON_PIN, INPUT_PULLUP);  // to toggle load status
    pinMode(LOAD_LED_PIN, OUTPUT);  // to show load status
    digitalWrite(LOAD_LED_PIN, LOAD_LED_OFF);

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
    handle_mqtt(have_time);
    handle_wifi();
}
