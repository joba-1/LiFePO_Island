#include <unity.h>

#include "NetConfigValidate.h"

using namespace NetConfigValidate;

void setUp() {}
void tearDown() {}

void test_hosts() {
    TEST_ASSERT_TRUE(isValidHost(""));            // empty = disabled
    TEST_ASSERT_TRUE(isValidHost("influx"));
    TEST_ASSERT_TRUE(isValidHost("fritz.box"));
    TEST_ASSERT_TRUE(isValidHost("192.168.1.6"));
    TEST_ASSERT_FALSE(isValidHost(nullptr));
    TEST_ASSERT_FALSE(isValidHost("-a"));
    TEST_ASSERT_FALSE(isValidHost("a."));
    TEST_ASSERT_FALSE(isValidHost("a..b"));
    TEST_ASSERT_FALSE(isValidHost("a b"));
    TEST_ASSERT_FALSE(isValidHost("a_b"));
    TEST_ASSERT_FALSE(isValidHost("a/b"));
}

void test_host_length() {
    char s[MAX_HOST_LEN + 2];
    memset(s, 'a', sizeof(s));
    s[MAX_HOST_LEN] = '\0';
    TEST_ASSERT_TRUE(isValidHost(s));
    s[MAX_HOST_LEN] = 'a';
    s[MAX_HOST_LEN + 1] = '\0';
    TEST_ASSERT_FALSE(isValidHost(s));
}

void test_ports() {
    uint16_t p = 7;
    TEST_ASSERT_TRUE(parsePort("1", p));
    TEST_ASSERT_EQUAL(1, p);
    TEST_ASSERT_TRUE(parsePort("65535", p));
    TEST_ASSERT_EQUAL(65535, p);
    TEST_ASSERT_FALSE(parsePort("0", p));
    TEST_ASSERT_FALSE(parsePort("65536", p));
    TEST_ASSERT_FALSE(parsePort("", p));
    TEST_ASSERT_FALSE(parsePort("-1", p));
    TEST_ASSERT_FALSE(parsePort("80 ", p));
    TEST_ASSERT_FALSE(parsePort("99999999999", p));
    TEST_ASSERT_EQUAL(65535, p);    // untouched on failure
}

void test_copy_field() {
    char dst[4] = "xyz";
    TEST_ASSERT_TRUE(copyField(dst, sizeof(dst), "ab"));
    TEST_ASSERT_EQUAL_STRING("ab", dst);
    TEST_ASSERT_FALSE(copyField(dst, sizeof(dst), "abcd"));
    TEST_ASSERT_EQUAL_STRING("ab", dst);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_hosts);
    RUN_TEST(test_host_length);
    RUN_TEST(test_ports);
    RUN_TEST(test_copy_field);
    return UNITY_END();
}
