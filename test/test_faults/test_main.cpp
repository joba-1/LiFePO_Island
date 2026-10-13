#include <unity.h>
#include <string.h>

#include "Faults.h"

void setUp() {}
void tearDown() {}

void test_table_sizes() {
    TEST_ASSERT_EQUAL(10, Faults::CHARGER_BITS);
    TEST_ASSERT_EQUAL(13, Faults::BMS_BITS);
}

void test_text_lookup() {
    TEST_ASSERT_EQUAL_STRING("PV low voltage", Faults::text(Faults::CHARGER, Faults::CHARGER_BITS, 6));
    TEST_ASSERT_EQUAL_STRING("MOS software lockout", Faults::text(Faults::BMS, Faults::BMS_BITS, 12));
    TEST_ASSERT_NULL(Faults::text(Faults::BMS, Faults::BMS_BITS, 13));
}

void test_describe_none() {
    char out[64] = "x";
    TEST_ASSERT_EQUAL(0, Faults::describe(out, sizeof(out), "CHG", Faults::CHARGER, Faults::CHARGER_BITS, 0, "\n"));
    TEST_ASSERT_EQUAL_STRING("", out);
}

void test_describe_two() {
    char out[128];
    // 0x040 PV low voltage, 0x080 Battery low voltage
    size_t n = Faults::describe(out, sizeof(out), "CHG", Faults::CHARGER, Faults::CHARGER_BITS, 0x0c0, "\n");
    TEST_ASSERT_EQUAL_STRING("CHG: PV low voltage\nCHG: Battery low voltage\n", out);
    TEST_ASSERT_EQUAL(strlen(out), n);
}

void test_describe_ignores_unknown_bits() {
    char out[64];
    Faults::describe(out, sizeof(out), "BMS", Faults::BMS, Faults::BMS_BITS, 0xe000, ";");
    TEST_ASSERT_EQUAL_STRING("", out);
}

void test_describe_truncates_safely() {
    char out[20];
    memset(out, 'Z', sizeof(out));
    size_t n = Faults::describe(out, sizeof(out), "BMS", Faults::BMS, Faults::BMS_BITS, 0x1fff, "\n");
    TEST_ASSERT_TRUE(n < sizeof(out));
    TEST_ASSERT_EQUAL(strlen(out), n);
    TEST_ASSERT_EQUAL_STRING(" ...", out + n - 4);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_table_sizes);
    RUN_TEST(test_text_lookup);
    RUN_TEST(test_describe_none);
    RUN_TEST(test_describe_two);
    RUN_TEST(test_describe_ignores_unknown_bits);
    RUN_TEST(test_describe_truncates_safely);
    return UNITY_END();
}
