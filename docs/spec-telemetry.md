# Telemetry: InfluxDB, MQTT, syslog

## InfluxDB

### Line format (unchanged)

One measurement per item group, as before 4.2 (Grafana and Home Assistant
depend on it):

| Measurement | Tags | Fields |
|---|---|---|
| `ChgSts`, `BatParam`, `Log`, `Parameters`, `LoadParam`, `ProParam`, `Information` | `Serial`, `Version` | item fields + `Host` |
| `Status`, `Cells`, `Hardware` | `Id`, `Version` | item fields + `Host` |
| `Protection` (5.0, one line per event of the battery reserve policy, [spec-protection.md](spec-protection.md)) | `Source`, `Version` | `Host`, `Action`, `Why` (strings), `Level`, `Soc`, `CellMin`, `PackV` |
| `BmsParam` (4.6, only after a read on request) | `Id`, `Version` | the BMS EEPROM settings in raw device units + `Host`: `Cells`, `CellOvp`/`CellOvpRelease`/`CellOvpDelay`, `CellUvp`/`CellUvpRelease`/`CellUvpDelay`, `CellOvpHigh`, `CellUvpHigh`, `HighDelays`, `PackOvp…`, `PackUvp…` (10 mV), `ChargeOcp`, `DischargeOcp` (10 mA), temperatures (0.1 K), balancer, `ShuntResistance`, `DesignCapacity`, `CellVoltage0…100` (mV) |
| `Wifi` | `Host`, `Version` | `BSSID`, `IP`, `RSSI` |

New in 4.2: each line carries its own timestamp in milliseconds
(`precision=ms`) once NTP time is valid. Lines produced before the first NTP
sync have no timestamp, so the server stamps them on arrival.

### Batching (`include/InfluxBatch.h`)

*Problem:* 4.1 sent one HTTP POST per changed item, about 3 per second, each
blocking the main loop up to the 2 s timeout (5 s connect timeout if the host
is down). Over the weak WLAN path through nat2 `-11` (read timeout) errors were
frequent, and the loop stalled the web UI, the button and MQTT.

*Design:*

* Lines are appended to one fixed 8 KiB buffer (no heap churn).
* A flush POSTs the whole buffer when it is older than 5 s or more than 75 %
  full.
* Success (2xx) empties the buffer. Failure keeps it, and retries back off:
  10 s, 20 s, 40 s, max 60 s.
* When a line does not fit, the oldest complete lines are dropped until it
  does; dropped lines are counted (`/api/netstatus` `influx.droppedLines`).
  A line larger than the buffer is rejected.
* Retrying is safe because timestamped points are idempotent in InfluxDB (same
  measurement, tags, time → overwritten, not duplicated).
* HTTP connect timeout 2 s, read timeout 3 s.

*Reasoning:* one POST per 5 s cuts the request rate ~15×, survives outages of
a few minutes without loss, and keeps the worst-case loop stall to one POST
per backoff interval. 8 KiB holds ~25 s of typical traffic.

## MQTT

Topic tree under `LiFePO_Island/<instance>`:

```
status/LWT          Online / Offline (retained, last will)
status/Hostname     status/Version   status/StartTime
status/DBServer     status/DBPort    status/DBName    status/DBResponse
status/Charger      10 fault bits     status/BMS   13 fault bits   (retained)
status/Load         on / off (retained, new in 4.2)
status/Protection   last event of the battery reserve policy (retained JSON, 5.0)
json/<Item>         the item JSON (same as GET /json/<Item>); json/Log and json/BmsParam retained
                    (json/BmsParam: only the protection levels and current limits, the 512 byte
                    MQTT buffer does not hold more; everything: GET /api/bmsparam)
cmd                 commands, see below
```

Home Assistant discovery topics: [spec-homeassistant.md](spec-homeassistant.md).

### Commands on `<topic>/cmd`

| Payload | Action |
|---|---|
| `load on` | switch charger load output on |
| `load off` | switch charger load output off |

Matching is case-insensitive, surrounding white space is ignored, and the
payload must be the **whole** command. Before 4.2 a prefix matched
(`strncasecmp` over the payload length), so an empty payload or `l` switched
the load **on**. (`include/MqttCmd.h`)

## Syslog

* Host `syslog`, UDP 514, IETF format, app name `Joba1`, device host name.
* INFO messages only for the first 10 minutes after boot, NOTICE and above
  always.
* Device read errors are logged once on the first failure and once on
  recovery, with the number of failed attempts.
* InfluxDB failures are logged per failed flush with HTTP code, batch size
  and a short response excerpt. 4.1 built that message in the same buffer
  that held the line being reported (overlapping `snprintf`), which produced
  the garbled `Post …status=-11 line='Post …` messages.
