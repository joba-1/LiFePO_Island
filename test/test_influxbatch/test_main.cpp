#include <unity.h>
#include <string.h>

#include "InfluxBatch.h"

void setUp() {}
void tearDown() {}

void test_empty_not_due() {
    char buf[64];
    InfluxBatch b(buf, sizeof(buf));
    TEST_ASSERT_FALSE(b.due(0));
    TEST_ASSERT_FALSE(b.due(100000));
    TEST_ASSERT_EQUAL(0, b.size());
}

void test_add_with_and_without_timestamp() {
    char buf[128];
    InfluxBatch b(buf, sizeof(buf));
    TEST_ASSERT_TRUE(b.add("m,t=1 f=1", 0, 0));
    TEST_ASSERT_TRUE(b.add("m,t=1 f=2", 1791606573123ULL, 0));
    TEST_ASSERT_EQUAL_STRING("m,t=1 f=1\nm,t=1 f=2 1791606573123\n", b.data());
    TEST_ASSERT_EQUAL(2, b.lines());
}

void test_rejects_bad_lines() {
    char buf[32];
    InfluxBatch b(buf, sizeof(buf));
    TEST_ASSERT_FALSE(b.add("", 0, 0));
    TEST_ASSERT_FALSE(b.add(nullptr, 0, 0));
    TEST_ASSERT_FALSE(b.add("a\nb", 0, 0));
    TEST_ASSERT_FALSE(b.add("this line is far too long for 32 bytes", 0, 0));
    TEST_ASSERT_EQUAL(0, b.lines());
}

void test_due_after_flush_interval() {
    char buf[256];
    InfluxBatch b(buf, sizeof(buf));
    b.add("m f=1", 0, 1000);
    TEST_ASSERT_FALSE(b.due(1000 + InfluxBatch::FLUSH_MS - 1));
    TEST_ASSERT_TRUE(b.due(1000 + InfluxBatch::FLUSH_MS));
}

void test_due_when_three_quarters_full() {
    char buf[40];
    InfluxBatch b(buf, sizeof(buf));
    b.add("0123456789", 0, 0);          // 11
    b.add("0123456789", 0, 0);          // 22
    TEST_ASSERT_FALSE(b.due(1));
    b.add("01234567", 0, 0);            // 31 >= 30
    TEST_ASSERT_TRUE(b.due(1));
}

void test_success_clears() {
    char buf[64];
    InfluxBatch b(buf, sizeof(buf));
    b.add("m f=1", 0, 0);
    b.sent(true, 6000);
    TEST_ASSERT_EQUAL(0, b.size());
    TEST_ASSERT_EQUAL_STRING("", b.data());
    TEST_ASSERT_FALSE(b.due(20000));
}

void test_failure_keeps_data_and_backs_off() {
    char buf[64];
    InfluxBatch b(buf, sizeof(buf));
    b.add("m f=1", 0, 0);
    b.sent(false, 5000);
    TEST_ASSERT_EQUAL(1, b.lines());
    TEST_ASSERT_EQUAL(1, b.failures());
    TEST_ASSERT_FALSE(b.due(5000 + 9999));
    TEST_ASSERT_TRUE(b.due(5000 + 10000));
    b.sent(false, 15000);   // 20 s
    TEST_ASSERT_FALSE(b.due(15000 + 19999));
    TEST_ASSERT_TRUE(b.due(15000 + 20000));
    b.sent(false, 35000);   // 40 s
    b.sent(false, 75000);   // 60 s cap
    TEST_ASSERT_FALSE(b.due(75000 + 59999));
    TEST_ASSERT_TRUE(b.due(75000 + 60000));
    b.sent(false, 135000);  // still 60 s
    TEST_ASSERT_TRUE(b.due(135000 + 60000));
    b.sent(true, 200000);
    TEST_ASSERT_EQUAL(0, b.failures());
}

void test_overflow_drops_oldest() {
    char buf[24];
    InfluxBatch b(buf, sizeof(buf));
    TEST_ASSERT_TRUE(b.add("aaaaaa", 0, 0));    // 7
    TEST_ASSERT_TRUE(b.add("bbbbbb", 0, 0));    // 14
    TEST_ASSERT_TRUE(b.add("cccccc", 0, 0));    // 21 (+NUL = 22)
    TEST_ASSERT_TRUE(b.add("dddddd", 0, 0));    // needs room: drop a
    TEST_ASSERT_EQUAL_STRING("bbbbbb\ncccccc\ndddddd\n", b.data());
    TEST_ASSERT_EQUAL(1, b.dropped());
    TEST_ASSERT_EQUAL(3, b.lines());
}

void test_wraparound_millis() {
    char buf[64];
    InfluxBatch b(buf, sizeof(buf));
    uint32_t t = 0xffffff00u;
    b.add("m f=1", 0, t);
    TEST_ASSERT_FALSE(b.due(t + 100));
    TEST_ASSERT_TRUE(b.due(t + InfluxBatch::FLUSH_MS));   // wrapped past 0
    b.sent(false, t + InfluxBatch::FLUSH_MS);
    TEST_ASSERT_FALSE(b.due(t + InfluxBatch::FLUSH_MS + 100));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_empty_not_due);
    RUN_TEST(test_add_with_and_without_timestamp);
    RUN_TEST(test_rejects_bad_lines);
    RUN_TEST(test_due_after_flush_interval);
    RUN_TEST(test_due_when_three_quarters_full);
    RUN_TEST(test_success_clears);
    RUN_TEST(test_failure_keeps_data_and_backs_off);
    RUN_TEST(test_overflow_drops_oldest);
    RUN_TEST(test_wraparound_millis);
    return UNITY_END();
}
