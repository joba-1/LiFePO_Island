# Web UI and HTTP API

## Goals

* The key values are visible **all the time** without reloading: state of
  charge, battery voltage/current/power, PV and load power, cell voltages,
  temperatures, faults, load and MOSFET state.
* Stale data is obvious: every block shows the age of its data, and turns
  amber after 30 s and red after 120 s, or when there has never been data.
* Connectivity is visible: Wi-Fi RSSI, InfluxDB and MQTT state.
* Works on a phone, without internet access (no CDN, everything inline), and
  in light and dark mode.
* The device stays cheap: the page is a fixed asset; the ESP only formats
  small JSON documents.

## Page (`web/index.html`, `web/logo.svg`)

`embed_web.py` (PlatformIO pre-script) turns the files in `web/` into C string
literals in the build directory (`web_assets.h`), so the page is edited as
plain HTML and is never stale in the firmware.

Layout, top to bottom:

1. Header: logo, name, host, version; a status dot (green = all data fresh
   and InfluxDB ok, amber = some stale or a write failing, red = no data).
2. Message line for results of actions and for faults.
3. Tiles:
   * **Battery** (BMS): SoC % with bar, V, A, W (+ charging, − discharging),
     energy left/full in Wh (Ah × 3.2 V nominal per cell × cells), cycles.
   * **Solar** (charger): PV V, charge W, charge mode, charger SoC estimate.
   * **Load**: load W with a bar from 0 to the charger's load output limit
     (`loadMaxW` = max. discharge current × battery voltage, ~400 W at 30 A;
     a gauge: fixed gradient green → yellow → red over the full width,
     uncovered up to the present load), "off" under the bar when the output
     is off, buttons ON / OFF.
   * **Cells**: one bar per cell in V (3 decimals); the bar colour shows the
     deviation from the average of all cells (blue below, white at average,
     orange to red above; full colour at ±25 mV), with a legend. While the BMS
     balances: **▼ drain** on cells it drains, **▲ low** on cells below
     average (no explanation text on the page; see user.md). The JBD BMS
     balances passively (it can only bleed charge off), so ▲ means "keeps its
     charge while the others come down". Voltages outside
     3.0–3.6 V are printed red. Min/avg/max in V, spread in mV, per-cell
     tooltip with the deviation in mV.
   * **Temperatures**: BMS NTCs, charger battery and internal sensor.
   * **Protection**: BMS charge/discharge MOSFET switches (with confirm),
     active faults as text, or "none"; the charger's nightly "PV low voltage" bit is
     shown greyed as "(night)" and does not turn the status dot amber.
   * **Link**: RSSI, IP, InfluxDB (last code, age, queued lines), MQTT, NTP.
4. Details (collapsed): JSON links, `/api/netstatus`, `/api/netconfig`,
   firmware update, IP change, breathing LED toggle, reset.

The page polls `GET /api/live` every 2 s while visible (paused in background
tabs via `visibilitychange`). Failed polls show "device not reachable" and the
time of the last good poll.

## API

### `GET /api/live`

```json
{
  "host": "lifepo_island-1", "version": "4.2.0", "uptimeS": 1234,
  "time": "2026-10-13T18:00:00", "timeValid": true,
  "msg": "Load on",
  "charger": {
    "ageS": 0, "model": "eSmart4-60A", "serial": "62060003",
    "mode": "MPPT", "pvV": 23.2, "batV": 13.4, "chgW": 41, "loadW": 1,
    "load": true, "loadMaxW": 402, "batTempC": 13, "innerTempC": 13, "capPct": 97,
    "faults": ["PV low voltage"]
  },
  "bms": {
    "ageS": 2, "id": "JBD-SP04S010A-L4S-35A-B",
    "V": 13.35, "A": 3.04, "W": 40.6, "socPct": 81,
    "remainAh": 220.46, "fullAh": 272.0, "remainWh": 2821, "fullWh": 3481, "cycles": 123,
    "chargeFet": true, "dischargeFet": true,
    "cellsMv": [3342, 3338, 3339, 3338], "balancing": [false, false, false, false],
    "tempsC": [14.5, 12.4], "faults": []
  },
  "link": {
    "rssi": -83, "ip": "192.168.5.222",
    "influx": {"code": 204, "okAgeS": 3, "queued": 12, "dropped": 0},
    "mqtt": true, "ntp": true
  }
}
```

* `ageS` is `null` when the device never answered since boot; the remaining
  values of that block are then `null` too.
* `msg` is a one-shot message (result of the last action, as the old page
  showed it); it is cleared once delivered.
* Labels carry tooltips (`title`) that say where a value comes from: BMS
  vs. charger voltage, counted SoC vs. the charger's voltage-based estimate,
  which temperature probe.
* Units are converted on the device (V, A, W, °C, Ah) so consumers need no
  knowledge of the raw scaling (charger 0.1 V; BMS 10 mV, 10 mA, 10 mAh,
  0.1 K).

### `POST /api/load` (no login)

`state=on|off|toggle` → `{"ok":true,"load":true,"msg":"Load on"}`;
400 on a bad `state`, 503 if the charger did not answer.

### `POST /api/mosfets` (no login)

`charge=0|1&discharge=0|1` → `{"ok":true,"chargeFet":..,"dischargeFet":..,"msg":..}`;
400 on missing/bad values, 503 if the BMS did not answer.

### Unchanged

`/json/<Item>` (`Version` is now a JSON string), `/api/netstatus` (gains
`influx.queued` and `influx.droppedLines`), `/api/netconfig`, `/update`, and the form endpoints
`/on`, `/off`, `/toggle`, `/mosfets`, `/switch`, `/switchon`, `/switchoff`,
`/ip`, `/breathe`, `/reset` keep their behaviour; Home Assistant and
bookmarks keep working.

## Security

* Admin functions (`/update`, `/reset`, `/ip`, `POST /api/netconfig`) need
  HTTP basic auth as before.
* Load and MOSFET control are deliberately open (see spec.md, Decisions).
* The page builds the DOM with `textContent`, never `innerHTML` with data, so
  request text in `msg` (e.g. a rejected `ip` form value) cannot inject
  markup. 4.1 printed `web_msg` raw into the HTML page (reflected XSS behind
  the admin login). `msg` is JSON-escaped in `/api/live`.
* `GET /api/sim` exists only in `SIMULATE` builds.
* Responses carry `Cache-Control: no-store` for the API.
