#include <unity.h>
#include <string.h>

#include "Protection.h"

using namespace Protection;

void setUp() {}
void tearDown() {}

static Inputs in(uint8_t soc, uint16_t cellMv = 3300, bool loadOn = true) {
    Inputs i;
    i.socValid = true; i.soc = soc; i.minCellMv = cellMv; i.loadKnown = true; i.loadOn = loadOn;
    return i;
}

// n evaluations of 10 s; returns the last output, @p changedAt the 1-based evaluation with the first change (0: none)
static Output run(Engine& e, const Inputs& i, int n, int* changedAt = nullptr, uint32_t dt = 10) {
    Output o;
    if (changedAt) *changedAt = 0;
    for (int k = 1; k <= n; k++) {
        o = e.update(i, dt);
        if (o.changed && changedAt && !*changedAt) *changedAt = k;
    }
    return o;
}

void test_normal_stays_normal() {
    Engine e;
    int at;
    Output o = run(e, in(50), 100, &at);
    TEST_ASSERT_EQUAL(0, at);
    TEST_ASSERT_EQUAL(NORMAL, o.level);
    TEST_ASSERT_FALSE(o.loadInhibited);
    TEST_ASSERT_FALSE(o.forceLoadOff);
}

void test_software_follows_hardware_with_margin() {
    Engine e;
    int at;
    run(e, in(14), 50, &at);                      // 15 % is the hardware's job; 14 % is still fine
    TEST_ASSERT_EQUAL(0, at);
    Output o = run(e, in(13), 20, &at);           // 13 % = 15 - margin 2
    TEST_ASSERT_EQUAL(6, at);                     // debounced: the 6th evaluation
    TEST_ASSERT_EQUAL(LOAD_CUT, o.level);
    TEST_ASSERT_TRUE(o.loadInhibited);
    TEST_ASSERT_TRUE(o.forceLoadOff);             // load still on: the charger did not cut
    TEST_ASSERT_FALSE(o.sleepWanted);
}

void test_reason_names_the_trigger() {
    Engine e;
    Output o;
    for (int k = 0; k < 6; k++) o = e.update(in(13), 10);
    TEST_ASSERT_TRUE(o.changed);
    TEST_ASSERT_EQUAL_STRING("normal -> load-cut: soc 13 % <= 13 %", o.reason);
    Engine c;
    for (int k = 0; k < 6; k++) o = c.update(in(50, 3080), 10);
    TEST_ASSERT_EQUAL_STRING("normal -> load-cut: lowest cell 3080 mV < 3190 mV", o.reason);
}

void test_cell_voltage_is_an_independent_witness() {
    // the BMS counter claims 60 %, but the weakest cell is on the knee: cut anyway (counter drift)
    Engine e;
    Inputs i = in(60, 3180);
    Output o = run(e, i, 10);
    TEST_ASSERT_EQUAL(LOAD_CUT, o.level);
}

void test_cell_voltage_trigger_needs_rest() {
    Engine e;
    Inputs i = in(60, 3150);
    i.centiA = -3500;                              // 35 A discharge: the cell sags, no conclusion
    Output o = run(e, i, 30);
    TEST_ASSERT_EQUAL(NORMAL, o.level);
    i.centiA = -200;                               // 2 A: at rest for this purpose
    o = run(e, i, 10);
    TEST_ASSERT_EQUAL(LOAD_CUT, o.level);
    Engine c;                                      // charging: the cell rises, never a low reading
    i.centiA = 1500;
    TEST_ASSERT_EQUAL(NORMAL, run(c, i, 30).level);
}

void test_critical_cell_voltage_ignores_current() {
    Engine e;
    Inputs i = in(60, 2900);
    i.centiA = -3500;
    TEST_ASSERT_EQUAL(CRITICAL, run(e, i, 10).level);
}

void test_load_already_off_is_not_forced() {
    Engine e;
    Output o = run(e, in(12, 3300, false), 10);
    TEST_ASSERT_EQUAL(LOAD_CUT, o.level);
    TEST_ASSERT_TRUE(o.loadInhibited);
    TEST_ASSERT_FALSE(o.forceLoadOff);             // the charger or BMS did its job
}

void test_single_bad_reading_ignored() {
    Engine e;
    int at;
    e.update(in(5), 10);
    Output o = run(e, in(50), 50, &at);
    TEST_ASSERT_EQUAL(0, at);
    TEST_ASSERT_EQUAL(NORMAL, o.level);
}

void test_unknown_readings_do_nothing() {
    Engine e;
    Inputs i;                                      // soc and cells unknown
    int at;
    run(e, i, 100, &at);
    TEST_ASSERT_EQUAL(0, at);
    i.minCellMv = 3000;                            // an unknown SoC does not hide a low cell
    Output o = run(e, i, 10);
    TEST_ASSERT_EQUAL(LOAD_CUT, o.level);
}

void test_no_data_holds_the_level() {
    Engine e;
    run(e, in(12), 10);
    TEST_ASSERT_EQUAL(LOAD_CUT, e.state().level);
    Inputs none;                                   // BMS lost: no SoC, no cells
    int at;
    Output o = run(e, none, 200, &at);             // a long time without data
    TEST_ASSERT_EQUAL(0, at);
    TEST_ASSERT_EQUAL(LOAD_CUT, o.level);
    TEST_ASSERT_TRUE(o.loadInhibited);
    Inputs cellsOnly;                              // SoC unknown, cells look fine: still no release
    cellsOnly.minCellMv = 3350;
    o = run(e, cellsOnly, 200, &at);
    TEST_ASSERT_EQUAL(0, at);
    TEST_ASSERT_EQUAL(LOAD_CUT, o.level);
}

void test_resume_needs_soc_and_cell() {
    Engine e;
    run(e, in(12), 10);
    TEST_ASSERT_EQUAL(LOAD_CUT, e.state().level);
    int at;
    run(e, in(20), 20, &at);                       // charging, still low
    TEST_ASSERT_EQUAL(0, at);
    run(e, in(29, 3300), 20, &at);
    TEST_ASSERT_EQUAL(0, at);
    run(e, in(30, 3190), 20, &at);                 // cell not yet above 3.20 V
    TEST_ASSERT_EQUAL(0, at);
    Output o = run(e, in(30, 3250, false), 10, &at);
    TEST_ASSERT_EQUAL(6, at);
    TEST_ASSERT_EQUAL(NORMAL, o.level);
    TEST_ASSERT_FALSE(o.loadInhibited);
}

void test_resume_signals_load_back_on() {
    Engine e;
    run(e, in(12), 10);
    Output o;
    bool resumed = false;
    for (int k = 0; k < 6; k++) { o = e.update(in(35, 3300, false), 10); resumed |= o.resumeLoad; }
    TEST_ASSERT_TRUE(resumed);
    resumed = false;
    for (int k = 0; k < 20; k++) { o = e.update(in(35, 3300, true), 10); resumed |= o.resumeLoad; }
    TEST_ASSERT_FALSE(resumed);                    // only once
}

void test_never_straight_to_sleep() {
    Engine e;
    Output o = run(e, in(8), 10);                  // below the sleep level at once
    TEST_ASSERT_EQUAL(LOAD_CUT, o.level);
    TEST_ASSERT_FALSE(o.sleepWanted);
}

void test_sleep_after_delay() {
    Engine e;
    run(e, in(12), 10);                            // load cut
    Output o = run(e, in(9), 60);                  // 10 min later still only load cut
    TEST_ASSERT_EQUAL(LOAD_CUT, o.level);
    int at;
    o = run(e, in(9), 20, &at, 100);               // time passes in 100 s steps: after 1800 s since the cut
    TEST_ASSERT_TRUE(at > 0);
    TEST_ASSERT_EQUAL(SLEEP, o.level);
    TEST_ASSERT_TRUE(o.sleepWanted);
    TEST_ASSERT_EQUAL_UINT32(1800, o.sleepS);
    TEST_ASSERT_FALSE(o.fetOff);
}

void test_critical_does_not_wait() {
    Engine e;
    int at;
    Output o = run(e, in(2), 20, &at);
    TEST_ASSERT_EQUAL(6, at);
    TEST_ASSERT_EQUAL(CRITICAL, o.level);
    TEST_ASSERT_TRUE(o.fetOff);
    TEST_ASSERT_TRUE(o.sleepWanted);
    TEST_ASSERT_EQUAL_UINT32(21600, o.sleepS);
    TEST_ASSERT_TRUE(o.loadInhibited);
}

void test_critical_by_cell_voltage() {
    Engine e;
    Output o = run(e, in(20, 2900), 10);
    TEST_ASSERT_EQUAL(CRITICAL, o.level);
}

void test_leaving_critical_steps_down() {
    Engine e;
    run(e, in(2), 10);
    Output o = run(e, in(5, 3000), 20);            // above the enter level, inside the hysteresis
    TEST_ASSERT_EQUAL(CRITICAL, o.level);
    o = run(e, in(9, 3100), 20);                   // out of critical: sleep schedule (still low)
    TEST_ASSERT_EQUAL(SLEEP, o.level);
    TEST_ASSERT_FALSE(o.fetOff);
    o = run(e, in(12, 3200), 20);
    TEST_ASSERT_EQUAL(SLEEP, o.level);             // 15 % needed
    o = run(e, in(16, 3250), 20);
    TEST_ASSERT_EQUAL(LOAD_CUT, o.level);
    o = run(e, in(31, 3300), 20);
    TEST_ASSERT_EQUAL(NORMAL, o.level);
}

void test_state_survives_deep_sleep() {
    Engine a;
    run(a, in(12), 10);
    run(a, in(9), 10, nullptr, 100);               // 1000 s since the cut, pending sleep
    State saved = a.state();                       // what main keeps in RTC memory
    Engine b;
    b.state() = saved;
    Output o = run(b, in(9), 20, nullptr, 100);    // woke up, the time asleep is added
    TEST_ASSERT_EQUAL(SLEEP, o.level);
    TEST_ASSERT_TRUE(o.loadInhibited);
}

void test_custom_thresholds() {
    Config c;
    c.loadOffSoc = 20; c.softMarginSoc = 0; c.debounce = 2;
    Engine e(c);
    int at;
    run(e, in(21), 5, &at);
    TEST_ASSERT_EQUAL(0, at);
    run(e, in(20), 5, &at);
    TEST_ASSERT_EQUAL(2, at);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_normal_stays_normal);
    RUN_TEST(test_software_follows_hardware_with_margin);
    RUN_TEST(test_reason_names_the_trigger);
    RUN_TEST(test_cell_voltage_is_an_independent_witness);
    RUN_TEST(test_cell_voltage_trigger_needs_rest);
    RUN_TEST(test_critical_cell_voltage_ignores_current);
    RUN_TEST(test_load_already_off_is_not_forced);
    RUN_TEST(test_single_bad_reading_ignored);
    RUN_TEST(test_unknown_readings_do_nothing);
    RUN_TEST(test_no_data_holds_the_level);
    RUN_TEST(test_resume_needs_soc_and_cell);
    RUN_TEST(test_resume_signals_load_back_on);
    RUN_TEST(test_never_straight_to_sleep);
    RUN_TEST(test_sleep_after_delay);
    RUN_TEST(test_critical_does_not_wait);
    RUN_TEST(test_critical_by_cell_voltage);
    RUN_TEST(test_leaving_critical_steps_down);
    RUN_TEST(test_state_survives_deep_sleep);
    RUN_TEST(test_custom_thresholds);
    return UNITY_END();
}
