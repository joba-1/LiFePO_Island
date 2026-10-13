# Monitor for JbdBms Battery Management System and eSmart3/4 MPPT solar charger

ESP32 Arduino firmware to monitor Jbd Battery Management Systems and eSmart3 or eSmart4 chargers. 
Sends status to an Influx db with the [Joba_JbdBms](https://github.com/joba-1/Joba_JbdBms) 
and [Joba_ESmart3](https://github.com/joba-1/Joba_ESmart3) libraries.
Status is also sent to an mqtt broker.
Switching eSmart3/4 load and JbdBms mosfets on and off via web page is also possible.
For now, hardcoded battery parameters for LiFePO are always set at startup by setup_LiFePO().

![lifepo-island-test-rig-IMG-2929](https://user-images.githubusercontent.com/32450554/199218951-2d38cff8-8812-4187-9072-7caadacad0b7.jpg)

Running the monitor on an ESP8266 might work but is not tested.

# Installation
There are many options to compile and install an ESP32 Arduino firmware. I use this one on linux:
* Install MS Code
 * Install PlatformIO as MS Code extension
* Add this folder to the MS Code workspace
* Edit platformio.ini in this folder so the usb device name for monitor and upload matches your environment
* Select build and upload the firmware

# Connection
See library readmes for wiring details.

Required hardware:
* A MAX485 board (or similar) to convert RS485 signals to serial.
* A level shifter from 5V to 3.3V (at least 3 channels) if the MAX485 board does not already have one
* An ESP32 (with USB connection or an additional serial-to-USB adapter for flashing)
* Cable with one RJ45 plug and just wires on the other end. 1-8: A-, B+, nc, nc, Gnd, Gnd, 5V, 5V
* eSmart3 or eSmart4 MPPT charge controller
* JBD BMS like SP04S010A
* LiFePO battery, PV (or >16V power supply for testing)
``` 
 RJ45 plug
for eSmart3
     __
  __/__\__
  |front |
  ||||||||
  1 ...  8
  ABxx--++
```

# InfluxDB
Relevant connection data (Influx host, database, ...) is configured in platformio.ini
Create necessary database like this on the influx server: `influx --execute 'create database LiFePO_Island'` 

* checks eSmart3 Information every 10 minutes
* checks eSmart3 ChgSts every half second
* checks eSmart3 BatParam, LoadParam, ProParam every minute
* checks eSmart Log(wStartCnt, wFaultCnt, dwTotalEng, dwLoadTotalEng, wBacklightTime, bSwitchEnable) every minute
* checks JbdBms Hardware every 10 minutes
* checks JbdBms Cells every 10 seconds
* checks JbdBms Status every 10 seconds 
* updates database at startup and on changes

```
job4:~ > influx -precision rfc3339 --database LiFePO_Island --execute 'show measurements'
name: measurements
name
----
BatParam
Cells
ChgSts
Hardware
Information
LoadParam
Log
Parameters
ProParam
Status

job4:~ > influx -precision rfc3339 --database LiFePO_Island --execute 'select * from Cells limit 5'
name: Cells
time                 Host            Id                      Version voltage1 voltage2 voltage3 voltage4
----                 ----            --                      ------- -------- -------- -------- --------
2022-11-01T09:49:39Z lifepo_island-1 JBD-SP04S010A-L4S-35A-B 1.0     3360     3356     3356     3355
2022-11-01T09:49:45Z lifepo_island-1 JBD-SP04S010A-L4S-35A-B 1.0     3360     3357     3356     3356
2022-11-01T09:49:52Z lifepo_island-1 JBD-SP04S010A-L4S-35A-B 1.0     3360     3356     3356     3356
2022-11-01T09:49:58Z lifepo_island-1 JBD-SP04S010A-L4S-35A-B 1.0     3361     3357     3356     3356
2022-11-01T09:50:05Z lifepo_island-1 JBD-SP04S010A-L4S-35A-B 1.0     3361     3356     3356     3356
```

# Networking
since WiFi is needed for Influx anyways, it is used for other stuff as well:
* Webserver 
    * display links for JSON of all eSmart3 item categories and JbdBms commands
    * enables OTA firmware update
    * later: display and change some values of BatParam, LoadParam, ProParam and Log
* Syslog and mqtt publish of status on changes
    * mqtt topic LiFePO_Island/{instance}/json/# for publishing eSmart3/4 or JBD infos in json format 
    * mqtt topic LiFePO_Island/{instance}/status/# for publishing esmart3/4 or jbd fault status 
    * mqtt topic LiFePO_Island/{instance}/cmd for receiving commands:
        * "load on": switch eSmart3/4 load on
        * "load off": switch eSmart3/4 load off
* NTP to set eSmart3/4 time at startup once
* RSSI and BSSID monitoring to find a place with good WLAN signal reception for the ESP32

## Web interface
![LiFePO_Island](https://user-images.githubusercontent.com/32450554/199284797-8ae049ff-4aa1-495b-8e3d-3111fb9d40d6.png)

## Grafana Panels using LiFePO_Island data
![grafik](https://user-images.githubusercontent.com/32450554/217285351-9d3be438-0db6-45f2-9e24-08eabd34858e.png)

## Live web page, batching, test bench (since 4.2)

* The start page shows the key values live (SoC, V/A/W, PV, load, cells,
  temperatures, faults, link state) and their age; data comes from
  `GET /api/live`. Load and MOSFETs are switched with `POST /api/load` and
  `POST /api/mosfets`; the old form endpoints still work.
* InfluxDB writes are batched (one POST every 5 s, retried with backoff, ms
  timestamps) instead of one POST per change.
* `pio test -e native` runs the unit tests; env `atom_echo` builds a
  simulator firmware for an M5Stack Atom Echo test bench, and
  `test/system/check_device.py` tests a running device.
* JSON now carries `"Version"` as a string.
* Home Assistant MQTT discovery: device `LiFePO_Island <instance>` with 26
  entities; migration of the former hand-written YAML entities with
  `tools/ha_migrate.py` (docs/admin.md).

Documentation: [docs/spec.md](docs/spec.md) (architecture and decisions),
[docs/spec-webui.md](docs/spec-webui.md), [docs/spec-telemetry.md](docs/spec-telemetry.md),
[docs/spec-homeassistant.md](docs/spec-homeassistant.md),
[docs/user.md](docs/user.md), [docs/admin.md](docs/admin.md), [docs/test.md](docs/test.md).

Comments welcome

Joachim Banzhaf

## Server hosts at runtime and admin login (since 4.1)

The `[syslog]`, `[mqtt]`, `[influx]` and `[ntp]` servers in `platformio.ini` are only defaults,
now the service aliases `syslog`, `mqtt` and `influx`. A change needs no rebuild: it is validated,
stored in NVS (ESP32; separate from the EEPROM sector with the static IP config) and active after
the reboot that follows 1 s later (`reboot=0` suppresses it). ESP8266 builds keep the defaults.

    curl -u admin:<pw> -X POST http://lifepo_island-1/api/netconfig -d mqttHost=mqtt -d influxHost=influx
    curl -u admin:<pw> -X POST http://lifepo_island-1/api/netconfig -d defaults=1
    curl http://lifepo_island-1/api/netconfig     # active values, source, defaults
    curl http://lifepo_island-1/api/netstatus     # firmware, WiFi, syslog/MQTT/InfluxDB/NTP with last result

Fields: `syslogHost`, `syslogPort`, `mqttHost`, `mqttPort`, `influxHost`, `influxPort`, `ntp`.
Host names only (letters, digits, `-`, `.`; empty = off), ports 1..65535; a bad value gives
`400 {"error":"invalid value","field":...}` and nothing is stored.

`/update`, `/reset`, `/ip` and `POST /api/netconfig` need the user `admin` and the password
from `secrets.ini` (untracked, mode 600; see `secrets.ini.template`). `pio_secrets.py` compiles it
in, and `upload_script.py` uses it for OTA. Load and mosfet switching stay open. Without
`secrets.ini` there is no lock (`"adminLock": false`).

Note: the ESP8266 env `d1_mini_ota` does not build (it already did not before 4.1): `ip_config()`
uses the ESP32-only EEPROM API (`writeBytes`, `readULong`, bool `begin`).
