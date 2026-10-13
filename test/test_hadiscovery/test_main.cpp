#include <unity.h>
#include <string.h>

#include "HaDiscovery.h"

using namespace HaDiscovery;

void setUp() {}
void tearDown() {}

static const Context CTX = { "homeassistant", "LiFePO_Island/1", "lifepo_island_1", "LiFePO_Island 1", "4.2.0" };

// worst case for the size check: longest plausible names everywhere
static const Context BIG = { "homeassistant_bench", "LiFePO_Island/99", "lifepo_island_99",
                             "LiFePO_Island 99", "10.20.300-rc1" };

static bool find(size_t i, uint8_t cells, uint8_t ntcs, char* topic, char* payload) {
    return build(CTX, i, cells, ntcs, topic, 128, payload, 600);
}

void test_count() {
    TEST_ASSERT_EQUAL(FIXED_COUNT + 4 + 2, count(4, 2));
    TEST_ASSERT_EQUAL(FIXED_COUNT, count(0, 0));
    TEST_ASSERT_EQUAL(FIXED_COUNT + MAX_CELLS + MAX_NTCS, count(200, 200));
}

void test_out_of_range() {
    char t[128], p[600];
    TEST_ASSERT_FALSE(find(count(4, 2), 4, 2, t, p));
}

// every message fits PubSubClient's 512 byte packet with margin
void test_all_messages_fit() {
    char t[128], p[600];
    size_t worst = 0;
    for (size_t i = 0; i < count(MAX_CELLS, MAX_NTCS); i++) {
        TEST_ASSERT_TRUE(build(BIG, i, MAX_CELLS, MAX_NTCS, t, sizeof(t), p, sizeof(p)));
        size_t total = strlen(t) + strlen(p);
        if (total > worst) worst = total;
        TEST_ASSERT_TRUE_MESSAGE(total <= MAX_MESSAGE, t);
    }
    printf("largest discovery message: %u bytes\n", (unsigned)worst);
}

void test_build_fails_on_small_buffer() {
    char t[128], p[100];
    TEST_ASSERT_FALSE(build(CTX, 0, 4, 2, t, sizeof(t), p, sizeof(p)));
}

void test_soc_message() {
    char t[128], p[600];
    TEST_ASSERT_TRUE(find(0, 4, 2, t, p));
    TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/lifepo_island_1/soc/config", t);
    TEST_ASSERT_EQUAL_STRING(
        "{\"~\":\"LiFePO_Island/1\",\"name\":\"Battery SoC\",\"uniq_id\":\"lifepo_island_1_soc\","
        "\"stat_t\":\"~/json/Status\",\"val_tpl\":\"{{value_json.Status.currentCapacity}}\","
        "\"unit_of_meas\":\"%\",\"dev_cla\":\"battery\",\"stat_cla\":\"measurement\","
        "\"avty_t\":\"~/status/LWT\",\"pl_avail\":\"Online\",\"pl_not_avail\":\"Offline\","
        "\"dev\":{\"ids\":\"lifepo_island_1\",\"name\":\"LiFePO_Island 1\",\"mf\":\"joba-1\","
        "\"mdl\":\"eSmart3/4 + JBD BMS\",\"sw\":\"4.2.0\"}}", p);
}

void test_cells_and_ntcs_indexed() {
    char t[128], p[600];
    TEST_ASSERT_TRUE(find(FIXED_COUNT + 0, 4, 2, t, p));
    TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/lifepo_island_1/cell_1/config", t);
    TEST_ASSERT_NOT_NULL(strstr(p, "\"name\":\"Cell 1\""));
    TEST_ASSERT_NOT_NULL(strstr(p, "\"val_tpl\":\"{{value_json.Cells[0]/1000}}\""));
    TEST_ASSERT_NOT_NULL(strstr(p, "\"unit_of_meas\":\"V\""));
    TEST_ASSERT_NOT_NULL(strstr(p, "\"sug_dsp_prc\":3"));
    TEST_ASSERT_TRUE(find(FIXED_COUNT + 3, 4, 2, t, p));
    TEST_ASSERT_NOT_NULL(strstr(p, "\"uniq_id\":\"lifepo_island_1_cell_4\""));
    TEST_ASSERT_NOT_NULL(strstr(p, "Cells[3]"));
    TEST_ASSERT_TRUE(find(FIXED_COUNT + 4, 4, 2, t, p));
    TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/lifepo_island_1/bms_temp_1/config", t);
    TEST_ASSERT_NOT_NULL(strstr(p, "temperatures[0]/10"));
    TEST_ASSERT_TRUE(find(FIXED_COUNT + 5, 4, 2, t, p));
    TEST_ASSERT_NOT_NULL(strstr(p, "\"name\":\"BMS temperature 2\""));
}

void test_device_info_only_in_first() {
    char t[128], p[600];
    TEST_ASSERT_TRUE(find(1, 4, 2, t, p));
    TEST_ASSERT_NOT_NULL(strstr(p, "\"dev\":{\"ids\":\"lifepo_island_1\"}}"));
}

void test_switch_message() {
    char t[128], p[600];
    size_t i = 0;
    while (strcmp(FIXED[i].key, "load")) i++;
    TEST_ASSERT_TRUE(find(i, 4, 2, t, p));
    TEST_ASSERT_EQUAL_STRING("homeassistant/switch/lifepo_island_1/load/config", t);
    TEST_ASSERT_NOT_NULL(strstr(p, "\"stat_t\":\"~/status/Load\""));
    TEST_ASSERT_NOT_NULL(strstr(p, "\"cmd_t\":\"~/cmd\",\"pl_on\":\"load on\",\"pl_off\":\"load off\""));
    TEST_ASSERT_NOT_NULL(strstr(p, "\"stat_on\":\"on\",\"stat_off\":\"off\""));
    TEST_ASSERT_NULL(strstr(p, "val_tpl"));
}

// the migration maps the old YAML entities onto these unique ids
void test_migration_targets_exist() {
    static const char* const keys[] = { "soc", "load_power", "charge_power", "charger_temp",
        "charger_battery_temp", "energy_production", "energy_consumption", "load" };
    for (const char* k : keys) {
        bool found = false;
        for (size_t i = 0; i < FIXED_COUNT; i++) found |= !strcmp(FIXED[i].key, k);
        TEST_ASSERT_TRUE_MESSAGE(found, k);
    }
}

void test_unique_keys() {
    for (size_t i = 0; i < FIXED_COUNT; i++)
        for (size_t j = i + 1; j < FIXED_COUNT; j++)
            TEST_ASSERT_TRUE_MESSAGE(strcmp(FIXED[i].key, FIXED[j].key), FIXED[i].key);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_count);
    RUN_TEST(test_out_of_range);
    RUN_TEST(test_all_messages_fit);
    RUN_TEST(test_build_fails_on_small_buffer);
    RUN_TEST(test_soc_message);
    RUN_TEST(test_cells_and_ntcs_indexed);
    RUN_TEST(test_device_info_only_in_first);
    RUN_TEST(test_switch_message);
    RUN_TEST(test_migration_targets_exist);
    RUN_TEST(test_unique_keys);
    return UNITY_END();
}
