#include <unity.h>

#include "NameList.h"

void setUp() {}
void tearDown() {}

void test_items() {
    char out[24];
    TEST_ASSERT_TRUE(NameList::nth("BMS board,Battery box", 0, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("BMS board", out);
    TEST_ASSERT_TRUE(NameList::nth("BMS board, Battery box ", 1, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Battery box", out);
}

void test_missing_or_empty() {
    char out[24] = "x";
    TEST_ASSERT_FALSE(NameList::nth("a,b", 2, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_FALSE(NameList::nth("a,,b", 1, out, sizeof(out)));
    TEST_ASSERT_FALSE(NameList::nth("", 0, out, sizeof(out)));
    TEST_ASSERT_FALSE(NameList::nth(nullptr, 0, out, sizeof(out)));
}

void test_truncates() {
    char out[5];
    TEST_ASSERT_TRUE(NameList::nth("Battery box", 0, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Batt", out);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_items);
    RUN_TEST(test_missing_or_empty);
    RUN_TEST(test_truncates);
    return UNITY_END();
}
