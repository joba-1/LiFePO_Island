# Administration

## Where things are

| What | Where |
|---|---|
| Device | ESP32 `lifepo_island-1`, 192.168.5.222 in the island network |
| Access | only through the range extender `nat2` (192.168.1.239): `http://nat2:8080/` |
| nat2 power | **today: a USB port of job4** (power only). Planned: its own USB supply. |
| Data | InfluxDB `influx:8086`, database `LiFePO_Island`; MQTT `mqtt`, `LiFePO_Island/1/#`; syslog `syslog`, host `lifepo_island-1`, app `Joba1` |
| Dashboard | Grafana `LiFePO_Island` (uid `mYiP89N4z`) |
| Test bench | M5Stack Atom Echo on job6 USB, env `atom_echo`, instance 9 |
| Docs | Trac `LiFePoIsland`; this directory |

## Build and flash

Prerequisite: PlatformIO, `secrets.ini` from `secrets.ini.template`
(`chmod 600`; never committed). Keep a copy of the admin password in the
password manager: without it neither OTA nor the admin pages work.

```bash
pio test -e native                         # unit tests, always first
pio run -e mhetesp32minikit_ota -t upload  # island via nat2:8080 (OTA, admin login)
pio run -e mhetesp32minikit_ser -t upload  # island via USB (on site)
pio run -e atom_echo -t upload             # test bench
```

The island was built with `mhetesp32minikit_ota` for 4.1 (`default_envs` in
`platformio.ini` still names `esp32cam_ota`; pass `-e` explicitly). The OTA
upload goes to `[ota] host` (`nat2:8080`); 4.1 and older used the host name,
which never resolved from the house network.

## Runtime configuration

Server hosts (syslog, MQTT, InfluxDB, NTP) are changed without a rebuild, see
`Readme.md` "Server hosts at runtime". Example after a server move:

```bash
curl -u admin:$PW -X POST http://nat2:8080/api/netconfig -d influxHost=influx
```

## Monitoring and troubleshooting

### No data in Grafana

1. `ping nat2` / `curl http://nat2:8080/api/live`.
   * nat2 does not answer: **check its power first** (job4 USB, see above).
     On 2026-10-10 job4 was shut down and the island was dark for days. Then
     check that nat2 is associated with ax3 (`job.fritz.ssid`, 2.4 GHz).
   * nat2 answers on port 80 but 8080 does not: the island is down or has
     lost the nat2 WLAN. It restarts itself after ~10 min without Wi-Fi.
2. `/api/live`: `charger.ageS` / `bms.ageS` large or `null` → RS485 problem
   (wiring, 5 V supply of the MAX485, device off). Syslog shows
   `charger ChgSts error` once at the start of the problem and
   `... ok again after N failed reads` at the end.
3. `link.influx.code` not 2xx, `queued` growing → InfluxDB unreachable. Up
   to ~8 KiB (~25 s of data) is kept and re-sent; beyond that the oldest lines
   are dropped (`dropped`).
4. Syslog: `{host="lifepo_island-1"}` in Loki (Grafana Explore).

### After a long WLAN outage

Nothing to do: the island reconnects, restarts after ~10 min of failed
reconnects, and on boot only writes the charger parameters if they differ
(`BatParam already set` in syslog).

### Config portal

If the stored WLAN is not reachable at boot, WiFiManager opens an access
point `lifepo_island-1` for 180 s. Its password is the admin password (4.2+,
if ≥ 8 characters), otherwise the host name.

## Home Assistant migration (YAML → MQTT discovery)

Once only, when the island first runs firmware ≥ 4.2. Background and the
entity table: [spec-homeassistant.md](spec-homeassistant.md). The tool is
`tools/ha_migrate.py` (job6, uses `ssh qnap`).

**Behaviour change:** afterwards the cell sensors are in real volts, so the
automation "LiFePO_Island aus wenn Akku leer" (< 2.8 V for 5 min) works for
the first time, and the "an wenn Akku ok" automations only switch on when
every cell is above 3.25 V. Before, they compared millivolts with volts.

1. Preconditions: nat2 has power, the island answers on `http://nat2:8080/`.
2. Dry run, read-only:

   ```bash
   tools/ha_migrate.py plan --rescale-recorder
   ```

   Must list the 12 entities, "12 LiFePO_Island items to remove" and "Ready".
3. Migrate. HA is stopped for about one to two minutes:

   ```bash
   tools/ha_migrate.py run --rescale-recorder
   ```

   Backs up `core.entity_registry`, `configuration.yaml` and the recorder
   DB (`*.bak-ha-migrate-<stamp>`, 2 GB), gives the 12 registry entries the
   discovery unique IDs, removes the 12 YAML items, rescales the last 10 days
   of cell history in the recorder (mV → V) and starts HA again. Until step 4
   the 12 entities show "unavailable".
4. Flash the island: `pio run -e mhetesp32minikit_ota -t upload`. Within
   seconds HA shows the device **LiFePO_Island 1** with 26 entities; the 12
   old ones keep their entity IDs, history and statistics.
5. Check in HA: `sensor.lifepo_island_cell_1` shows ~3.3 V, the energy
   dashboard still has the island's history,
   `switch.lifepo_island_load` switches, and the read-only system test passes
   (docs/test.md, section 3).
6. Optional, any time later, HA may keep running: rescale the long-term cell
   history in the `homeassistant` InfluxDB (≈ 24.5 million points since
   2024-05, server-side `SELECT … INTO`, month by month):

   ```bash
   tools/ha_migrate.py rescale-influx --dry-run   # counts only
   tools/ha_migrate.py rescale-influx
   ```

   Only points above 100 are touched, so a second run changes nothing.

If 4.2 was flashed *before* step 3, discovery has already created duplicate
entities (`sensor.lifepo_island_1_*`); `run` removes them before the rename
(`plan` lists them).

Rollback: `tools/ha_migrate.py rollback <stamp>` restores the three files and
restarts HA. Then flash 4.1 again, or let 4.2 run; its entities then appear
next to the YAML ones under new IDs. An InfluxDB rescale is not undone by the
rollback (that would be `value * 1000` for the cell points before the
migration time).

## Backup and restore

Nothing on the device needs a backup: firmware from git, settings are
compile-time defaults plus NVS host overrides (documented by
`GET /api/netconfig`). Data lives in InfluxDB (backed up with the server).

Restore on new hardware:

1. Flash via USB: `pio run -e mhetesp32minikit_ser -t upload`.
2. Join `job.nat2.ssid` in the WiFiManager portal.
3. If hosts were overridden: replay them with `POST /api/netconfig`.
4. Verify with the read-only system test (docs/test.md, section 3).
