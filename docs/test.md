# Test plan

Per the coding standards: unit tests before every commit, component and
integration tests on every minor release, full system test on a major release.

## 1. Unit tests (every commit)

```bash
pio test -e native
```

Runs on the host, no hardware. Covers the header-only logic in `include/`:

| Suite | Header | What |
|---|---|---|
| `test_mqttcmd` | `MqttCmd.h` | exact command match, case, white space; prefixes and empty payloads rejected (4.1 bug: they switched the load on) |
| `test_faults` | `Faults.h` | bit → text tables, describe(), safe truncation |
| `test_escape` | `TextEscape.h` | JSON string escaping, never a split escape |
| `test_influxbatch` | `InfluxBatch.h` | flush interval, 3/4-full trigger, retry backoff 10/20/40/60 s, drop-oldest, millis() wrap |
| `test_livejson` | `LiveJson.h` | exact `/api/live` document, unit conversion, null blocks, truncation |
| `test_netconfigvalidate` | `NetConfigValidate.h` | host names, ports, field copy |
| `test_protection` | `Protection.h` | reserve ladder: margin, debounce, hysteresis, sleep delay, critical, unknown data holds the level, cell voltage witness gated by current, state across deep sleep |
| `test_bmsparam` | `BmsParam.h` | BMS settings JSON (volts, amps, °C, `[limit, release]` pairs, balancer bits, unread document), Influx line, truncation |
| `test_hadiscovery` | `HaDiscovery.h` | message count, exact payloads, **every message ≤ 480 bytes** (worst-case names, 32 cells, 8 NTCs), unique keys, migration targets exist |

Also build every firmware env that is in use:

```bash
pio run -e atom_echo -e mhetesp32minikit_ota
```

## 2. Component and integration test (every minor release)

On the **test bench**: an MH-ET MiniKit (env `mhetesp32minikit_bench`, instance 8) or a
Seeed XIAO ESP32-C6 (env `xiao_esp32c6_bench`, instance 7) on job6. The M5Stack Atom Echos
(env `atom_echo`, instance 9) are used for speech tests now and are not available for this.
The boards run the same firmware with `-DSIMULATE`: charger and BMS are
replaced by `src/sim/SimDevices.h` (same methods as the libraries, a 20-minute
"day" with PV, charge, load and SoC), so polling, publishing, switching and
the UI run their real code paths. Instance 8 (7): host `lifepo_island-8`, MQTT
`LiFePO_Island/8`, InfluxDB database `LiFePO_Island_test` (30-day retention), HA prefix
`homeassistant_bench`: nothing mixes with the island's data, and the boards join the bench
Wi-Fi from `secrets.ini`, never the nat2 network.

```bash
pio run -e mhetesp32minikit_bench -t upload --upload-port /dev/serial/by-id/<the board>   # needs bench_wifi_* in secrets.ini (erased board)
ADMIN_PASSWORD=... python3 test/system/check_device.py --host lifepo_island-8 --instance 8
```

`check_device.py` checks (with `ADMIN_PASSWORD` or `--admin-password` also the BMS settings read; `--read-only`
and `--no-sim` skip it, because reading puts a real BMS into factory mode):

* HTTP: page, logo, 404, `/api/live` keys and version, `/api/netstatus`
  queue fields, every `/json/<Item>` is valid JSON with a quoted version.
* Data: charger and BMS fresh, cells present, W = V × A, NTP synced.
* Controls: 400 on bad input; load off/toggle, MOSFETs; one-shot message.
* Simulator (`/api/sim`): a charger fault appears as text and clears; BMS
  offline → data age grows → back to fresh; `setup_LiFePO()` writes the
  charger parameters at most once per boot.
* MQTT: LWT `Online` retained, `json/ChgSts` published; prefix payloads
  (`load`, `l`, `load o`) do **not** switch; ` LOAD ON ` does.
* InfluxDB: batch posted with 2xx, ChgSts points arrive, timestamps in ms.
* HA discovery (prefix `homeassistant_bench`, which HA ignores): ≥ 26
  retained config messages, valid JSON, unique IDs, availability, ≤ 480
  bytes each; `status/Load`, `status/Charger`, `status/BMS`, `json/Log`
  retained.

Discovery templates were also rendered with Jinja2 against live bench
payloads (all 26 produce values; cells in V, the night-time PV bit is no
charger problem).

Manual checks (browser, phone width and desktop, light and dark mode):

1. Tiles update every 2 s; switching to another tab pauses polling.
2. `/api/sim?bms=0`: battery and cell tiles turn amber after 30 s, red after
   120 s, the header dot turns amber/red; `bms=1` recovers.
3. `/api/sim?chargerFaults=64&bmsFaults=4096`: both faults listed in the
   Protection tile, dot amber.
4. Unplug the bench from Wi-Fi (or stop InfluxDB access) for a minute:
   "Influx queue" grows, then drains after reconnect without gaps in
   `LiFePO_Island_test`.
5. Discharge MOSFET off asks for confirmation.

## 3. Release to the island

1. All of the above passed.
2. `pio run -e mhetesp32minikit_ota -t upload` (OTA via `nat2:8080`, admin
   password from `secrets.ini`).
3. Read-only system test against the island:

   ```bash
   python3 test/system/check_device.py --host nat2:8080 --instance 1 \
       --db LiFePO_Island --no-sim --read-only
   ```

4. Grafana dashboard `LiFePO_Island` shows continuous data; syslog shows
   `BatParam already set` / `ProParam already set` after the boot.

## 4. Full system test (major release)

Section 2 and 3, plus on the island: switch the load from the page, from MQTT
(`LiFePO_Island/1/cmd`) and from Home Assistant (`switch.lifepo_island_load`);
power-cycle nat2 and confirm the island reconnects by itself within ~15 min.

### Test boards: silence a board before it joins the network

A board that was a range extender (the old nat2, an ESP32-D0 with Tasmota, MAC `34:86:5D:3A:53:24`, or a spare
C6) opens `job.nat2.ssid`, takes the topic `nat2` and writes `Nat2Link` to the island's database as soon as it
has power. So:

1. Plug it in and find **its** port: `ls -l /dev/serial/by-id/` before and after. Several USB serial adapters
   look alike (`1a86:55d4` is also the Zigbee2MQTT coordinator on `/dev/ttyACM0`): never aim esptool at a port you
   did not just see appear, and check `sudo lsof <port>` is empty.
2. Keep it from booting: `esptool --port <port> --after no-reset chip-id` leaves it in the ROM bootloader.
   (A board that is still running its old firmware is silent from then on.)
3. `esptool --port <port> --after no-reset erase-flash`, then
   `pio run -e <bench env> -t upload --upload-port <port>`: after the first boot it is a bench device.
4. Run `check_device.py` against it.

## Results

| Date | Version | Unit | Bench | Island | Notes |
|---|---|---|---|---|---|
| 2026-10-23 | 4.6.0 | 56/56 | 77/77 (`check_device.py`, MH-ET `mhetesp32minikit_bench`, includes BMS settings read, MQTT, InfluxDB) | not deployed | BmsParam; JBD library 2.2 (host tests with a fake BMS) |
| 2026-10-13 | 4.2.0 | 44/44 | 63/63 (`check_device.py`, Atom Echo) | 47/47 read-only, OTA via nat2:8080 | HA migration done: 12 entities adopted, 134,813 recorder states rescaled |
