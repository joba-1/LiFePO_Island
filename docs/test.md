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
| `test_hadiscovery` | `HaDiscovery.h` | message count, exact payloads, **every message ≤ 480 bytes** (worst-case names, 32 cells, 8 NTCs), unique keys, migration targets exist |

Also build every firmware env that is in use:

```bash
pio run -e atom_echo -e mhetesp32minikit_ota
```

## 2. Component and integration test (every minor release)

On the **test bench**: M5Stack Atom Echo on job6
(`/dev/serial/by-id/usb-Hades2001_M5stack_71527E0C75-if00-port0`), env
`atom_echo`. It runs the same firmware with `-DSIMULATE`: charger and BMS are
replaced by `src/sim/SimDevices.h` (same methods as the libraries, a 20-minute
"day" with PV, charge, load and SoC), so polling, publishing, switching and
the UI run their real code paths. Instance 9, host `lifepo_island-9`, MQTT
`LiFePO_Island/9`, InfluxDB database `LiFePO_Island_test` (30-day retention):
nothing mixes with the island's data.

```bash
pio run -e atom_echo -t upload           # needs bench_wifi_* in secrets.ini (erased board)
python3 test/system/check_device.py --host <echo-ip>
```

`check_device.py` checks:

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

## Results

| Date | Version | Unit | Bench | Island | Notes |
|---|---|---|---|---|---|
| 2026-10-13 | 4.2.0 | 44/44 | 63/63 (`check_device.py`, Atom Echo) | not reachable (nat2 without power) | HA migration: dry run ok, not executed |
