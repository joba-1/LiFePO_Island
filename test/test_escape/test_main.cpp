#include <unity.h>
#include <string.h>

#include "TextEscape.h"

void setUp() {}
void tearDown() {}

void test_json_never_splits_escape() {
    char out[6];   // ab\" fits (4 + NUL), a second \" (2 more) does not
    TextEscape::json(out, sizeof(out), "ab\"\"");
    TEST_ASSERT_EQUAL_STRING("ab\\\"", out);
}

void test_json_specials() {
    char out[64];
    TextEscape::json(out, sizeof(out), "a\"b\\c\nd\te\x01");
    TEST_ASSERT_EQUAL_STRING("a\\\"b\\\\c\\nd\\te", out);
}

void test_json_utf8_passes() {
    char out[16];
    TextEscape::json(out, sizeof(out), "13 \xc2\xb0" "C");
    TEST_ASSERT_EQUAL_STRING("13 \xc2\xb0" "C", out);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_json_never_splits_escape);
    RUN_TEST(test_json_specials);
    RUN_TEST(test_json_utf8_passes);
    return UNITY_END();
}
