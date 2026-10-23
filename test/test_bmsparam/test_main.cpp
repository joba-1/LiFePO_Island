#include <unity.h>
#include <string.h>

#include "BmsParam.h"

void setUp() {}
void tearDown() {}

// Same member names as JbdBms::Settings_t
struct Settings {
    uint16_t designCapacity, cycleCapacity, cellVoltage100, cellVoltage80, cellVoltage60, cellVoltage40;
    uint16_t cellVoltage20, cellVoltage0, selfDischarge;
    uint16_t packOvp, packOvpRelease, packUvp, packUvpRelease;
    uint16_t cellOvp, cellOvpRelease, cellUvp, cellUvpRelease, cellOvpHigh, cellUvpHigh;
    uint8_t cellUvpDelay, cellOvpDelay, packUvpDelay, packOvpDelay, highDelays, shortCircuitRelease;
    int16_t chargeOcp, dischargeOcp;
    uint8_t chargeOcpDelay, chargeOcpRelease, dischargeOcpDelay, dischargeOcpRelease;
    uint16_t chargeOvertemp, chargeOvertempRelease, chargeUndertemp, chargeUndertempRelease;
    uint16_t dischargeOvertemp, dischargeOvertempRelease, dischargeUndertemp, dischargeUndertempRelease;
    uint16_t balanceStart, balanceWindow, shuntResistance, functionConfig, ntcConfig, cells;
};

static Settings sample() {
    Settings s;
    memset(&s, 0, sizeof(s));
    s.designCapacity = 27200; s.cycleCapacity = 27200;
    s.cellVoltage100 = 3650; s.cellVoltage80 = 3350; s.cellVoltage60 = 3320; s.cellVoltage40 = 3290;
    s.cellVoltage20 = 3250; s.cellVoltage0 = 2800; s.selfDischarge = 10;
    s.packOvp = 1480; s.packOvpRelease = 1440; s.packUvp = 900; s.packUvpRelease = 1000;
    s.cellOvp = 3700; s.cellOvpRelease = 3500; s.cellUvp = 2500; s.cellUvpRelease = 2700;
    s.cellOvpHigh = 3900; s.cellUvpHigh = 2300;
    s.cellUvpDelay = 2; s.cellOvpDelay = 3; s.packUvpDelay = 4; s.packOvpDelay = 5;
    s.highDelays = 0x80 | 0x10;   // uvp high 8 s, ovp high 2 s
    s.chargeOcp = 3000; s.dischargeOcp = -3500; s.chargeOcpDelay = 2; s.dischargeOcpDelay = 6;
    s.chargeOvertemp = 3331; s.chargeOvertempRelease = 3281; s.chargeUndertemp = 2731; s.chargeUndertempRelease = 2781;
    s.dischargeOvertemp = 3331; s.dischargeOvertempRelease = 3281; s.dischargeUndertemp = 2531; s.dischargeUndertempRelease = 2581;
    s.balanceStart = 3400; s.balanceWindow = 20; s.shuntResistance = 10; s.functionConfig = 0x0c; s.ntcConfig = 3; s.cells = 4;
    return s;
}

void test_not_read() {
    char out[64];
    Settings s = sample();
    TEST_ASSERT_TRUE(BmsParam::writeJson(out, sizeof(out), false, 0, s));
    TEST_ASSERT_EQUAL_STRING("{\"read\":false}", out);
}

void test_levels_in_volts() {
    char out[2048];
    Settings s = sample();
    TEST_ASSERT_TRUE(BmsParam::writeJson(out, sizeof(out), true, 42, s));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"read\":true,\"ageS\":42,\"cells\":4,"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"cellUvp\":{\"V\":2.500,\"releaseV\":2.700,\"delayS\":2,\"highV\":2.300,\"highDelayS\":8}"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"cellOvp\":{\"V\":3.700,\"releaseV\":3.500,\"delayS\":3,\"highV\":3.900,\"highDelayS\":2}"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"packUvp\":{\"V\":9.00,\"releaseV\":10.00,\"delayS\":4}"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"packOvp\":{\"V\":14.80,\"releaseV\":14.40,\"delayS\":5}"));
}

void test_currents_temps_balancer_capacity() {
    char out[2048];
    Settings s = sample();
    TEST_ASSERT_TRUE(BmsParam::writeJson(out, sizeof(out), true, 0, s));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"dischargeOcpA\":35.00,"));     // magnitude of the negative raw value
    TEST_ASSERT_NOT_NULL(strstr(out, "\"chargeOcpA\":30.00,"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"chargeOver\":[60.0,55.0]"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"chargeUnder\":[0.0,5.0]"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"dischargeUnder\":[-20.0,-15.0]"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"balancer\":{\"on\":true,\"whileCharging\":true,\"startV\":3.400,\"windowMv\":20}"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"socTableMv\":[2800,3250,3290,3320,3350,3650]"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"designAh\":272.00"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"shuntMOhm\":1.0"));
}

void test_compact_fits_mqtt() {
    char out[1024];
    Settings s = sample();
    TEST_ASSERT_TRUE(BmsParam::writeJson(out, sizeof(out), true, 0, s, false));
    TEST_ASSERT_TRUE(strlen(out) < 450);                       // PubSubClient: 512 incl. topic and header
    TEST_ASSERT_NOT_NULL(strstr(out, "\"cellUvp\":{\"V\":2.500,\"releaseV\":2.700,\"delayS\":2}"));
    TEST_ASSERT_NULL(strstr(out, "highV"));
    TEST_ASSERT_NULL(strstr(out, "tempC"));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"chargeOcpDelayS\":2}"));
    // worst case widths still fit
    s.cellUvp = s.cellOvp = 65535; s.packUvp = s.packOvp = 65535; s.dischargeOcp = -32768; s.chargeOcp = 32767;
    s.cellUvpDelay = s.cellOvpDelay = s.packUvpDelay = s.packOvpDelay = 255; s.chargeOcpDelay = 255;
    TEST_ASSERT_TRUE(BmsParam::writeJson(out, sizeof(out), true, 4000000000u, s, false));
    TEST_ASSERT_TRUE(strlen(out) < 450);
}

void test_balancer_off() {
    char out[2048];
    Settings s = sample();
    s.functionConfig = 0;
    TEST_ASSERT_TRUE(BmsParam::writeJson(out, sizeof(out), true, 0, s));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"balancer\":{\"on\":false,\"whileCharging\":false,"));
}

void test_truncation_reported() {
    char out[100];
    Settings s = sample();
    TEST_ASSERT_FALSE(BmsParam::writeJson(out, sizeof(out), true, 0, s));
}

void test_high_delay_codes() {
    TEST_ASSERT_EQUAL(1, BmsParam::cellUvpHighDelayS(0x00));
    TEST_ASSERT_EQUAL(4, BmsParam::cellUvpHighDelayS(0x40));
    TEST_ASSERT_EQUAL(8, BmsParam::cellUvpHighDelayS(0x80));
    TEST_ASSERT_EQUAL(16, BmsParam::cellUvpHighDelayS(0xc0));
    TEST_ASSERT_EQUAL(1, BmsParam::cellOvpHighDelayS(0x00));
    TEST_ASSERT_EQUAL(8, BmsParam::cellOvpHighDelayS(0x30));
}

void test_influx_line() {
    char out[1024];
    Settings s = sample();
    size_t n = BmsParam::influxLine(out, sizeof(out), "JBD-SP04S010A-L4S-35A-B", "4.6.0", "lifepo_island-1", s);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL(strlen(out), n);
    TEST_ASSERT_EQUAL_INT(0, strncmp(out, "BmsParam,Id=JBD-SP04S010A-L4S-35A-B,Version=4.6.0 Host=\"lifepo_island-1\",Cells=4,", 80));
    TEST_ASSERT_NOT_NULL(strstr(out, ",CellUvp=2500,CellUvpRelease=2700,CellUvpDelay=2,"));
    TEST_ASSERT_NOT_NULL(strstr(out, ",DischargeOcp=-3500,"));
    TEST_ASSERT_NULL(strchr(out, '\n'));
    char small[64];
    TEST_ASSERT_EQUAL(0, BmsParam::influxLine(small, sizeof(small), "x", "4.6.0", "h", s));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_not_read);
    RUN_TEST(test_levels_in_volts);
    RUN_TEST(test_currents_temps_balancer_capacity);
    RUN_TEST(test_compact_fits_mqtt);
    RUN_TEST(test_balancer_off);
    RUN_TEST(test_truncation_reported);
    RUN_TEST(test_high_delay_codes);
    RUN_TEST(test_influx_line);
    return UNITY_END();
}
