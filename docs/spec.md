# LiFePO_Island – specification

## Purpose

ESP32 firmware that monitors a small off-grid 12 V solar store ("island"):
an eSmart3/eSmart4 MPPT charge controller and a JBD BMS, both on one RS485
bus. It publishes the readings to InfluxDB, MQTT and syslog, serves a web UI
with live values, and switches the charger's load output (an 800 W inverter)
and the BMS MOSFETs.

## Deployment context

```
 PV ─ eSmart3/4 ─┐ RS485 ┌─ JBD BMS ─ 4S LiFePO4 272 Ah
                 └───┬───┘
              MAX485 + level shifter
                     │ UART2 (RX 14, TX 13)
              ESP32 "lifepo_island-1"  192.168.5.222
                     │ WLAN job.nat2.ssid
              nat2: Tasmota32 range extender, NAPT 192.168.5.0/24
                     │ WLAN job.fritz.ssid (ax3)        192.168.1.239
              home LAN: influx, mqtt, syslog (job6), ntp (fritz.box)
```

The island is out of normal WLAN range, so the extender `nat2` is a hard
dependency: if nat2 is down, the island is unreachable and writes nothing.
`http://nat2:8080/` is NAPT-forwarded to the island's port 80.

## Components

| Component | Where | Spec |
|---|---|---|
| Device polling (charger, BMS) | `src/main.cpp` `handle_es3*`, `handle_jbd*` | this file |
| Telemetry out (InfluxDB, MQTT, syslog) | `src/main.cpp`, `include/InfluxBatch.h` | [spec-telemetry.md](spec-telemetry.md) |
| Web UI and HTTP API | `web/`, `src/main.cpp` `setup_webserver` | [spec-webui.md](spec-webui.md) |
| Runtime host config | `src/NetConfig.*`, `include/NetConfigValidate.h` | Readme "Server hosts at runtime" |
| Home Assistant discovery | `include/HaDiscovery.h`, `tools/ha_migrate.py` | [spec-homeassistant.md](spec-homeassistant.md) |
| Simulator (test builds only) | `src/sim/SimDevices.h` | [test.md](test.md) |

Pure logic that does not need Arduino lives header-only in `include/` so the
native unit tests (`pio test -e native`) cover it: validation, MQTT command
parsing, fault texts, InfluxDB batching, live JSON, JSON escaping, HA
discovery messages.

## Polling

| Item | Interval | Published |
|---|---|---|
| Charger Information, BMS Hardware | 60 s | on change |
| Charger ChgSts | 550 ms | on change |
| Charger BatParam, Log, Parameters, LoadParam, ProParam | 10 s | on change |
| BMS Status, Cells | 6 s | on change |
| Charger load state (LED/button) | 500 ms | log on change |

Every successful read stamps a per-device "last OK" time; the web UI shows
the data age, so stale values are visible instead of silently frozen. Read
errors are logged on the first failure and on recovery (with the count), not
on every attempt: a disconnected bus used to log an error every 550 ms.

## Startup battery parameters

`setup_LiFePO()` holds the battery parameters for 4S LiFePO4 272 Ah (bulk
14.4 V, no float/equalize, charge ≤ 60 A, protection limits derived from
3.0/3.6 V per cell). Since 4.2 it reads BatParam and ProParam first and
writes only when the device differs.

*Why:* the ESP reboots on every WLAN loss longer than ~10 min (see below), and
each boot used to rewrite both parameter sets. With nat2 down that is a write
every ~13 min into the charger's configuration memory, for no benefit.

## Network resilience

* Wi-Fi lost: `WiFi.reconnect()` every 10 s; after 60 failed tries (~10 min)
  the ESP restarts. On boot WiFiManager tries the stored network and opens its
  config portal for 180 s if that fails, then restarts again. So the island
  recovers by itself once nat2 is back, without anyone touching it.
* The config portal is protected with the admin password when one of ≥ 8
  characters is compiled in (WPA2 minimum); otherwise with the host name, as
  before 4.2.
* IP: DHCP unless set explicitly via `/ip` (stored in EEPROM with a new
  magic). Up to 4.1 the first DHCP answer was stored and used as static config
  forever; nat2 forwards ports by MAC via its DHCP leases, so after a nat2
  power loss the island could not be reached (see admin.md, "nat2 after a
  reboot"). Old entries are ignored after the update.
* Hosts are resolved on every use (HTTP POST, MQTT connect, each syslog
  packet), so a moved alias is followed without a reboot.

## Time

NTP via `configTzTime()` with the POSIX rule `CET-1CEST,M3.5.0,M10.5.0/3`
(build flag `TIMEZONE`). Before 4.2 `configTime(3600, 3600)` applied summer
time all year, so in winter local times (web page, charger clock) were one
hour ahead.

## Versioning

Semantic versioning `MAJOR.MINOR.PATCH` in `platformio.ini` `[program]
version`. JSON carries the version as a string since 4.2 (`"Version":"4.2.0"`);
before it was an unquoted number (`4.1`), which a three-part version would
have turned into invalid JSON.

### Rule: every deployed build gets its own version

A change that reaches a device bumps `[program] version` (patch: fix, minor:
feature) and is committed **before** the build; the release commit is tagged
`v<version>`. `git_version.py` compiles the commit (`GIT_COMMIT`, with
`-dirty` for uncommitted changes) into the firmware; `/api/netstatus`
(`firmware.commit`), `/api/live` (`commit`) and the page header show it.

| Version | Date | Content |
|---|---|---|
| 5.0.0 | 2026-10-23 | **Major release: the ESP firmware now has battery safety features of its own**, backing up what charger and BMS should do themselves ([spec-protection.md](spec-protection.md)): software load cut with margin (13 %) and back on (30 %), ESP deep sleep schedule, critical level protecting the cells; one `load_command()` path; every action logged with source and reason (syslog, MQTT, InfluxDB, `/api/live`). Not deployed yet |
| 4.6.0 | 2026-10-23 | BMS protection limits (EEPROM) on request: `BmsParam` (page, `/api/bmsparam`, `/json/BmsParam`, MQTT, InfluxDB); JBD library 2.2 (31+ byte replies, result buffer size); silent bench envs for the MH-ET and the XIAO C6 |
| 4.5.2 | 2026-10-13 | load bar back to one solid colour in steps like the SoC bar (green, amber > 75 %, red > 90 %) |
| 4.5.1 | 2026-10-13 | load bar as a gauge: fixed gradient green → yellow → red over the full width, uncovered up to the load |
| 4.5.0 | 2026-10-13 | load bar 0 … load output limit (`loadMaxW`) instead of the "Output ON" row; header time without "updated" |
| 4.4.3 | 2026-10-13 | cells: "Min / max" and "Average" on separate rows (fits the desktop card) |
| 4.4.2 | 2026-10-13 | no balancing explanation text, more space around the legend, "Charger: battery" |
| 4.4.1 | 2026-10-13 | outline for near-white cell bars (light mode), shorter balancing text |
| 4.4.0 | 2026-10-13 | cell bars coloured by deviation from the average, ▼ drain / ▲ low while balancing |
| 4.3.0 | 2026-10-13 | probe names (`custom_ntc_names`), energy in Wh, cells in V, labels/tooltips, git commit in the firmware |
| 4.2.0 | 2026-10-13 | live page, batched InfluxDB, HA discovery, fixes from the review; later builds `1f7bb06` (DHCP, OTA Origin) and `df405f1` (page details) were deployed **still as 4.2.0** — the reason for this rule |
| 4.1 | 2026-09-23 | runtime host config, `/api/netstatus`, admin login |

## Build environments

| Env | Board | Use |
|---|---|---|
| `mhetesp32minikit_ota` / `_ser` | MH-ET ESP32 MiniKit | the island (last build of 4.1 used this env) |
| `esp32cam_ota` / `_ser` | ESP32-CAM | alternative board |
| `mhetesp32minikit_bench` | MH-ET ESP32 MiniKit | test bench, instance 8, **simulated** charger/BMS, own database, bench Wi-Fi |
| `xiao_esp32c6_bench` | Seeed XIAO ESP32-C6 | test bench, instance 7, simulated, same silent settings |
| `atom_echo` | M5Stack Atom Echo | instance 9, simulated; the Echos are used for speech tests now |
| `native` | host | unit tests |
| `d1_mini_*` | ESP8266 | does not build (EEPROM API), kept for reference |

The instance number (`custom_instance`) sets host name, MQTT topic and
nothing else; test builds additionally use their own InfluxDB database so
simulated data never mixes with the island's history.

## Incident 2026-10-10: no data since 04:31 UTC

* Last InfluxDB point 04:31:55 UTC, last syslog line 04:29:33 UTC. Values just
  before were normal (13.35 V, 81 %, no faults, RSSI −83 dBm).
* `nat2` (192.168.1.239) stopped answering ARP at the same time and is not
  associated with ax3 any more.
* Cause: nat2 is powered only from a USB port of job4 (see Trac
  `Job4Retirement`, "nat2 ESP32 (USB ttyACM0) – power only"); job4 was powered
  off on 2026-10-10 as part of its retirement. Not a hardware defect of the
  island and not the WLAN changes. ax3's 2.4 GHz radio is now on channel 4
  (nat2 was seen on channel 13 in July) and has a new IoT SSID; nat2 should
  follow the channel by itself, but check that it associates once powered.
* Wake-on-LAN for job4 did not work (both MACs, ports 9 and 7).
* Fix: power nat2 from its own USB supply (planned); short-term: power job4 on.

## Decisions

* **Batch InfluxDB writes** (4.2) instead of one HTTP POST per changed item:
  ~3 POSTs/s with up to 2 s timeout each blocked the main loop (web UI, button,
  MQTT) whenever the long WLAN path was slow; syslog shows frequent `-11` read
  timeouts. See [spec-telemetry.md](spec-telemetry.md).
* **Static page + JSON API** for the web UI (4.2) instead of a printf-formatted
  page: the page is a fixed asset, the device only serves small JSON, and the
  browser renders. See [spec-webui.md](spec-webui.md).
* **Simulator** for test builds rather than a second RS485 rig: it exercises
  the real code paths (polling, publishing, load and MOSFET switching) on any
  ESP32.
* **Battery reserve policy in software** (5.0): charger and BMS cut first, the software follows with a margin and
  owns the release; every action is an event with source and reason. See [spec-protection.md](spec-protection.md).
* **BMS limits only on request** (4.6): the JBD BMS answers reads of its EEPROM
  settings only in "factory mode", and what the BMS does meanwhile (does it keep
  protecting the pack?) is not documented. So the island never polls them: an
  admin presses *Read from BMS* (or `POST /api/bmsparam`), the library enters
  factory mode, reads ~40 registers (a few seconds, the island does not answer
  meanwhile) and leaves it again without saving. If the BMS does not confirm
  leaving, the main loop keeps asking every 5 s. There is no write path.
* Load and MOSFET switching stay **without login** (operational controls, also
  used by Home Assistant via MQTT); admin functions need the admin password.
