#include <unity.h>

#include "IdleOff.h"

using namespace IdleOff;

void setUp() {}
void tearDown() {}

// n evaluations of 10 s with the same reading; returns the 1-based evaluation that said "off" (0: none)
static int run(Watch& w, int n, uint16_t loadW, bool loadOn = true, bool valid = true) {
    for (int k = 1; k <= n; k++) {
        if (w.update(loadOn, valid, loadW, 10)) return k;
    }
    return 0;
}

void test_idle_inverter_goes_off_after_the_time() {
    Watch w;                                         // 15 W, 30 min
    TEST_ASSERT_EQUAL(180, run(w, 400, 10));         // 180 x 10 s = 30 min
    TEST_ASSERT_EQUAL(100, w.lastMeanDeciW());
}

void test_pulsing_idle_counts_as_idle() {
    Watch w;
    int at = 0;
    for (int k = 1; k <= 400 && !at; k++) {          // 0 / 13 W alternating, mean 6.5 W
        if (w.update(true, true, k % 2 ? 13 : 0, 10)) at = k;
    }
    TEST_ASSERT_EQUAL(180, at);
}

void test_use_keeps_it_on() {
    Watch w;
    TEST_ASSERT_EQUAL(0, run(w, 1000, 80));          // e-bike charging
}

void test_off_follows_the_last_use() {
    Watch w;
    TEST_ASSERT_EQUAL(0, run(w, 50, 80));            // charging, then done
    int at = run(w, 400, 8);
    // the window holds a few seconds of the charge; it restarts while the mean is >= 15 W, so off ~30 min later
    TEST_ASSERT_TRUE(at >= 180 && at <= 200);
}

void test_short_use_restarts_the_window() {
    Watch w;
    TEST_ASSERT_EQUAL(0, run(w, 100, 8));            // 1000 s idle
    TEST_ASSERT_EQUAL(0, run(w, 30, 200));           // 5 min of a tool charger
    int at = run(w, 400, 8);                         // a full window from about the end of the use
    TEST_ASSERT_TRUE(at >= 180 && at <= 200);
}

void test_load_off_resets() {
    Watch w;
    TEST_ASSERT_EQUAL(0, run(w, 170, 8));
    TEST_ASSERT_EQUAL(0, run(w, 1, 0, false));       // switched off by hand
    TEST_ASSERT_EQUAL(0u, w.windowS());
    TEST_ASSERT_EQUAL(180, run(w, 400, 8));          // on again: a full window
}

void test_stale_data_waits() {
    Watch w;
    TEST_ASSERT_EQUAL(0, run(w, 100, 8));
    TEST_ASSERT_EQUAL(0, run(w, 500, 0, true, false));   // charger silent: not counted
    TEST_ASSERT_EQUAL(1000u, w.windowS());
    TEST_ASSERT_EQUAL(80, run(w, 400, 8));
}

void test_minutes_zero_disables() {
    Config c;
    c.minutes = 0;
    Watch w(c);
    TEST_ASSERT_EQUAL(0, run(w, 10000, 0));
}

void test_fires_once_then_new_window() {
    Watch w;
    TEST_ASSERT_EQUAL(180, run(w, 400, 5));
    TEST_ASSERT_EQUAL(0u, w.windowS());
    TEST_ASSERT_EQUAL(180, run(w, 400, 5));          // charger did not switch: try again a window later
}

void test_short_settings_for_the_bench() {
    Config c;
    c.minutes = 1;
    c.settleS = 30;
    Watch w(c);
    TEST_ASSERT_EQUAL(6, run(w, 100, 5));
    TEST_ASSERT_EQUAL(0, run(w, 100, 22));
}

void test_valid() {
    Config c;
    TEST_ASSERT_TRUE(valid(c));
    c.minutes = 0;     TEST_ASSERT_TRUE(valid(c));
    c.minutes = 1441;  TEST_ASSERT_FALSE(valid(c));
    c.minutes = 30; c.thresholdW = 0;   TEST_ASSERT_FALSE(valid(c));
    c.thresholdW = 501;                 TEST_ASSERT_FALSE(valid(c));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_idle_inverter_goes_off_after_the_time);
    RUN_TEST(test_pulsing_idle_counts_as_idle);
    RUN_TEST(test_use_keeps_it_on);
    RUN_TEST(test_off_follows_the_last_use);
    RUN_TEST(test_short_use_restarts_the_window);
    RUN_TEST(test_load_off_resets);
    RUN_TEST(test_stale_data_waits);
    RUN_TEST(test_minutes_zero_disables);
    RUN_TEST(test_fires_once_then_new_window);
    RUN_TEST(test_short_settings_for_the_bench);
    RUN_TEST(test_valid);
    return UNITY_END();
}
