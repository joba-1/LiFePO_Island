#include <unity.h>
#include <string.h>

#include "LiveJson.h"

void setUp() {}
void tearDown() {}

static LiveJson::Live sample() {
    LiveJson::Live d;
    strcpy(d.host, "lifepo_island-9");
    strcpy(d.version, "4.2.0");
    strcpy(d.commit, "df405f1");
    d.uptimeS = 77;
    d.timeValid = true;
    strcpy(d.time, "2026-10-13T18:00:00");
    strcpy(d.msg, "Load \"on\"");
    d.charger.seen = true;
    d.charger.ageS = 1;
    strcpy(d.charger.model, "eSmart4");
    strcpy(d.charger.serial, "62060003");
    d.charger.mode = 1;
    d.charger.pvDeciV = 232;
    d.charger.batDeciV = 134;
    d.charger.chgW = 41;
    d.charger.loadW = 0;
    d.charger.load = 1;
    d.charger.maxLoadDeciA = 300;   // 30 A at 13.4 V = 402 W
    d.charger.batTempC = 13;
    d.charger.innerTempC = -2;
    d.charger.capPct = 97;
    d.charger.faults = 0x040;
    d.bms.seen = true;
    d.bms.ageS = 2;
    strcpy(d.bms.id, "JBD");
    d.bms.centiV = 1335;
    d.bms.centiA = -191;
    d.bms.remainCentiAh = 22046;
    d.bms.fullCentiAh = 27200;
    d.bms.cycles = 123;
    d.bms.socPct = 81;
    d.bms.mosfets = 3;
    d.bms.cells = 4;
    d.bms.cellsMv[0] = 3342; d.bms.cellsMv[1] = 3338; d.bms.cellsMv[2] = 3339; d.bms.cellsMv[3] = 3338;
    d.bms.balance = 0x2;
    d.bms.ntcs = 2;
    d.bms.tempsDeciC[0] = 145; d.bms.tempsDeciC[1] = -5;
    d.bms.ntcNames = "BMS board";   // only one name: the second probe gets the default
    d.link.rssi = -83;
    strcpy(d.link.ip, "192.168.5.222");
    d.link.influxCode = 204;
    d.link.influxOkAgeS = 3;
    d.link.queued = 12;
    d.link.mqtt = true;
    d.link.ntp = true;
    d.reserve.level = 1;
    d.reserve.loadInhibited = true;
    d.reserve.enforceLoad = true;
    d.reserve.enforceSleep = true;
    d.reserve.eventAgeS = 30;
    strcpy(d.reserve.event, "[software] load off: no \"cut\"");
    return d;
}

void test_full_document() {
    char out[2048];
    TEST_ASSERT_TRUE(LiveJson::write(out, sizeof(out), sample()));
    TEST_ASSERT_EQUAL_STRING(
        "{\"host\":\"lifepo_island-9\",\"version\":\"4.2.0\",\"commit\":\"df405f1\",\"uptimeS\":77,"
        "\"time\":\"2026-10-13T18:00:00\",\"timeValid\":true,\"msg\":\"Load \\\"on\\\"\","
        "\"charger\":{\"ageS\":1,\"model\":\"eSmart4\",\"serial\":\"62060003\",\"mode\":\"MPPT\","
        "\"pvV\":23.2,\"batV\":13.4,\"chgW\":41,\"loadW\":0,\"load\":true,\"loadMaxW\":402,\"batTempC\":13,"
        "\"innerTempC\":-2,\"capPct\":97,\"faults\":[\"PV low voltage\"]},"
        "\"bms\":{\"ageS\":2,\"id\":\"JBD\",\"V\":13.35,\"A\":-1.91,\"W\":-25.4,\"socPct\":81,"
        "\"remainAh\":220.46,\"fullAh\":272.00,\"remainWh\":2821,\"fullWh\":3481,\"cycles\":123,\"chargeFet\":true,\"dischargeFet\":true,"
        "\"cellsMv\":[3342,3338,3339,3338],\"balancing\":[false,true,false,false],"
        "\"tempsC\":[14.5,-0.5],\"tempNames\":[\"BMS board\",\"BMS probe 2\"],\"faults\":[]},"
        "\"protection\":{\"level\":\"load-cut\",\"loadInhibited\":true,"
        "\"enforce\":{\"load\":true,\"sleep\":true,\"fet\":false},"
        "\"lastEvent\":{\"ageS\":30,\"text\":\"[software] load off: no \\\"cut\\\"\"}},"
        "\"link\":{\"rssi\":-83,\"ip\":\"192.168.5.222\",\"influx\":{\"code\":204,\"okAgeS\":3,"
        "\"queued\":12,\"dropped\":0},\"mqtt\":true,\"ntp\":true}}",
        out);
}

void test_never_seen_devices_are_null() {
    LiveJson::Live d;
    strcpy(d.host, "h");
    char out[2048];
    TEST_ASSERT_TRUE(LiveJson::write(out, sizeof(out), d));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"charger\":{\"ageS\":null,\"model\":null,"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"faults\":null}"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"bms\":{\"ageS\":null,\"id\":null,"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"time\":null"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"okAgeS\":null"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"protection\":{\"level\":\"normal\",\"loadInhibited\":false,"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"lastEvent\":null}"));
}

void test_load_unknown_is_null() {
    LiveJson::Live d = sample();
    d.charger.load = -1;
    char out[2048];
    LiveJson::write(out, sizeof(out), d);
    TEST_ASSERT_NOT_NULL(strstr(out, "\"load\":null"));
}

void test_truncation_reported() {
    char out[100];
    memset(out, 'Z', sizeof(out));
    TEST_ASSERT_FALSE(LiveJson::write(out, sizeof(out), sample()));
    TEST_ASSERT_TRUE(strlen(out) < sizeof(out));
}

void test_load_max_unknown_is_null() {
    LiveJson::Live d = sample();
    d.charger.maxLoadDeciA = 0;     // BatParam not read yet
    char out[2048];
    LiveJson::write(out, sizeof(out), d);
    TEST_ASSERT_NOT_NULL(strstr(out, "\"loadMaxW\":null"));
}

void test_mode_names() {
    TEST_ASSERT_EQUAL_STRING("wait", LiveJson::modeName(0));
    TEST_ASSERT_EQUAL_STRING("float", LiveJson::modeName(3));
    TEST_ASSERT_EQUAL_STRING("unknown", LiveJson::modeName(9));
}

void test_cell_count_clamped() {
    LiveJson::Live d = sample();
    d.bms.cells = 200;
    d.bms.ntcs = 200;
    char out[4096];
    TEST_ASSERT_TRUE(LiveJson::write(out, sizeof(out), d));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_full_document);
    RUN_TEST(test_never_seen_devices_are_null);
    RUN_TEST(test_load_unknown_is_null);
    RUN_TEST(test_truncation_reported);
    RUN_TEST(test_load_max_unknown_is_null);
    RUN_TEST(test_mode_names);
    RUN_TEST(test_cell_count_clamped);
    return UNITY_END();
}
