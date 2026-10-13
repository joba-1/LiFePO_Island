# Home Assistant: MQTT discovery

## Goal

The island announces its entities to Home Assistant itself (MQTT discovery)
instead of hand-written YAML in HA's `configuration.yaml`. The 12 existing
YAML entities are taken over **with their entity IDs, history, long-term
statistics, energy-dashboard entries and automations unchanged**.

## Messages

* Topic: `<prefix>/<component>/lifepo_island_<instance>/<key>/config`,
  prefix `homeassistant` (build flag `HA_DISCOVERY_PREFIX`; the test bench uses
  `homeassistant_bench`, which HA ignores).
* Retained. Published after every MQTT connect and whenever HA announces
  itself (`homeassistant/status` = `online`), one message per loop pass so the
  loop never stalls.
* Abbreviated keys (`~`, `stat_t`, `val_tpl`, `uniq_id`, …). The full device
  block (name, maker, model, version) is only in the first message (`soc`);
  all others carry just the device identifier. HA merges device information
  from all messages, and the identifier is enough to link an entity. With the
  full block in every message the largest would have been 498 bytes for
  instance 1, too close to the limit. **Size limit:** PubSubClient's buffer is `MQTT_MAX_PACKET_SIZE` = 512
  bytes for header + topic + payload. A unit test builds every message
  (including 8 cells, 8 NTCs and the longest host name/version) and fails if
  one exceeds 480 bytes, leaving margin.
* Cells and BMS temperatures are announced once the first BMS status tells how
  many there are; a changed count re-announces them.
* Availability: `~/status/LWT` (`Online`/`Offline`, retained, last will).

## Entities

`~` = `LiFePO_Island/<instance>`; unique ID `lifepo_island_<instance>_<key>`;
device `LiFePO_Island <instance>` (identifier `lifepo_island_<instance>`).

| Key | Component | Name | Source | Unit | Class | Takes over (instance 1) |
|---|---|---|---|---|---|---|
| `soc` | sensor | Battery SoC | `json/Status` currentCapacity | % | battery | `sensor.lifepo_island_capacity` |
| `voltage` | sensor | Battery voltage | Status.voltage / 100 | V | voltage | |
| `current` | sensor | Battery current | Status.current / 100 | A | current | |
| `power` | sensor | Battery power | V × A | W | power | |
| `remaining` | sensor | Remaining capacity | remainingCapacity / 100 | Ah | | |
| `cycles` | sensor | Cycles | Status.cycles | | total_increasing, diagnostic | |
| `bms_temp_N` | sensor | BMS temperature N | temperatures[N-1] / 10 | °C | temperature | |
| `cell_N` | sensor | Cell N | Cells[N-1] / 1000 | **V** | voltage | `sensor.lifepo_island_cell_N` |
| `pv_voltage` | sensor | PV voltage | ChgSts.PvVolt / 10 | V | voltage | |
| `charge_power` | sensor | Charge power | ChgSts.ChgPower | W | power | `sensor.lifepo_island_charge_power` |
| `load_power` | sensor | Load power | ChgSts.LoadPower | W | power | `sensor.lifepo_island_load_power` |
| `charger_battery_temp` | sensor | Charger battery temperature | ChgSts.BatTemp | °C | temperature | `sensor.lifepo_island_battery_temperature` |
| `charger_temp` | sensor | Charger temperature | ChgSts.InnerTemp | °C | temperature | `sensor.lifepo_island_converter_temperature` |
| `energy_production` | sensor | Energy production | Log.TotalEng | Wh | energy, total_increasing | `sensor.lifepo_island_battery_production` |
| `energy_consumption` | sensor | Energy consumption | Log.LoadTotalEng | Wh | energy, total_increasing | `sensor.lifepo_island_battery_consumption` |
| `charge_mode` | sensor | Charge mode | ChgSts.ChgMode as text | | | |
| `rssi` | sensor | Wi-Fi signal | `json/Wifi` RSSI | dBm | signal_strength, diagnostic | |
| `charger_problem` | binary_sensor | Charger problem | `status/Charger` | | problem | |
| `bms_problem` | binary_sensor | BMS problem | `status/BMS` | | problem | |
| `charge_fet` | binary_sensor | Charge MOSFET | Status.mosfetStatus bit 0 | | diagnostic | |
| `discharge_fet` | binary_sensor | Discharge MOSFET | Status.mosfetStatus bit 1 | | diagnostic | |
| `load` | switch | Load | `status/Load` on/off; command `cmd` | | | `switch.lifepo_island_load` |

Measurements carry `state_class: measurement`. `charger_problem` ignores the
"PV low voltage" bit (normal every night).

### State topics that must be retained

HA restarts more often than the island changes state, so these are published
**retained** (new or changed in 4.2):

* `status/Load` `on`/`off` (new): on every load change and on MQTT connect.
  Before, the switch read `json/LoadParam`, which is only sent on change, so
  after an HA restart the switch was unknown until the next change.
* `status/Charger`, `status/BMS` fault bit strings (were not retained).
* `json/Log` (energy totals; changes only every few minutes).

These are also re-sent on every MQTT connect: a value read before the connect,
or one that never changes, would otherwise never reach the broker.

The other `json/*` topics stay non-retained; they refresh every few seconds.

### Cell voltage scale (behaviour change!)

The old YAML sensors passed the firmware's millivolts with unit `V`
(history: 3279–3670 "V"). The automations compare against 2.8 V and 3.25 V,
so

* "LiFePO_Island aus wenn Akku leer" (cells below 2.8 for 5 min) **never
  fired**,
* the two "… an wenn Akku ok" automations (all cells above 3.25) **always
  passed**.

Discovery sends volts (`Cells[i] / 1000`). From the migration on, the
automations work as written: the load really goes off below 2.8 V per cell,
and only comes on when every cell is above 3.25 V.

## Migration of the existing entities

HA identifies an entity by (domain, platform, unique ID). The YAML entities
are registry entries of the MQTT integration (platform `mqtt`, same config
entry as discovery). Giving such an entry the discovery unique ID makes the
discovered entity **adopt** it: same entity ID, same recorder history and
statistics (keyed by entity ID), energy dashboard and Lovelace untouched,
InfluxDB series (tagged by entity ID) continue.

| Old unique ID (YAML) | New unique ID |
|---|---|
| `lifepo_island_load_switch` | `lifepo_island_1_load` |
| `lifepo_island_cell_1` … `_4` | `lifepo_island_1_cell_1` … `_4` |
| `lifepo_island_capacity` | `lifepo_island_1_soc` |
| `lifepo_island_load_power` | `lifepo_island_1_load_power` |
| `lifepo_island_charge_power` | `lifepo_island_1_charge_power` |
| `lifepo_island_converter_temp` | `lifepo_island_1_charger_temp` |
| `lifepo_island_battery_temp` | `lifepo_island_1_charger_battery_temp` |
| `lifepo_island_production` | `lifepo_island_1_energy_production` |
| `lifepo_island_consumption` | `lifepo_island_1_energy_consumption` |

*Why rename in the registry instead of reusing the old IDs in the firmware:*
the firmware keeps one uniform ID scheme for every instance; the old names
live only in the one-time migration script.

Executed 2026-10-13: all 12 entities adopted (unchanged entity IDs, one device with 26 entities).

Procedure, tooling and rollback: [admin.md](admin.md), section "Home
Assistant migration"; script `tools/ha_migrate.py`.
