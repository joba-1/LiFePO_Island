#include <unity.h>
#include <string.h>

#include "MqttCmd.h"

using MqttCmd::parse;

static MqttCmd::Cmd p(const char* s) { return parse(s, strlen(s)); }

void setUp() {}
void tearDown() {}

void test_exact_commands() {
    TEST_ASSERT_EQUAL(MqttCmd::LOAD_ON, p("load on"));
    TEST_ASSERT_EQUAL(MqttCmd::LOAD_OFF, p("load off"));
}

void test_case_and_whitespace() {
    TEST_ASSERT_EQUAL(MqttCmd::LOAD_ON, p("LOAD ON"));
    TEST_ASSERT_EQUAL(MqttCmd::LOAD_OFF, p("  Load Off\r\n"));
}

// 4.1 matched prefixes: an empty payload or "l" switched the load on
void test_prefix_and_empty_rejected() {
    TEST_ASSERT_EQUAL(MqttCmd::NONE, p(""));
    TEST_ASSERT_EQUAL(MqttCmd::NONE, p("   "));
    TEST_ASSERT_EQUAL(MqttCmd::NONE, p("l"));
    TEST_ASSERT_EQUAL(MqttCmd::NONE, p("load"));
    TEST_ASSERT_EQUAL(MqttCmd::NONE, p("load o"));
    TEST_ASSERT_EQUAL(MqttCmd::NONE, parse(nullptr, 0));
}

void test_longer_rejected() {
    TEST_ASSERT_EQUAL(MqttCmd::NONE, p("load onx"));
    TEST_ASSERT_EQUAL(MqttCmd::NONE, p("load off now"));
    TEST_ASSERT_EQUAL(MqttCmd::NONE, p("load  on"));
}

void test_not_nul_terminated() {
    const char buf[] = { 'l', 'o', 'a', 'd', ' ', 'o', 'n', 'X' };
    TEST_ASSERT_EQUAL(MqttCmd::LOAD_ON, parse(buf, 7));
    TEST_ASSERT_EQUAL(MqttCmd::NONE, parse(buf, 8));
}

void test_names() {
    TEST_ASSERT_EQUAL_STRING("load on", MqttCmd::name(MqttCmd::LOAD_ON));
    TEST_ASSERT_EQUAL_STRING("", MqttCmd::name(MqttCmd::NONE));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_exact_commands);
    RUN_TEST(test_case_and_whitespace);
    RUN_TEST(test_prefix_and_empty_rejected);
    RUN_TEST(test_longer_rejected);
    RUN_TEST(test_not_nul_terminated);
    RUN_TEST(test_names);
    return UNITY_END();
}
