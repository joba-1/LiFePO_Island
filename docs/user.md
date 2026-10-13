# Using the web page

Open `http://nat2:8080/` (from the house network). The page updates itself
every two seconds; there is no need to reload.

## The dot in the header

* **green**: charger and battery data are current, InfluxDB writes work, no
  faults.
* **amber**: some data is older than 30 s, a fault is active, or InfluxDB
  writes fail. ("PV low voltage" at night is normal and shown greyed.)
* **red**: no current data at all, or the page cannot reach the device (the
  header then says since when).

Each tile shows how old its data is ("live", "45 s old", "no data").

## Tiles

| Tile | Shows |
|---|---|
| Battery | state of charge from the BMS, voltage, current and power (+ charging, − discharging), remaining/full Ah, cycles |
| Solar | charge power, PV voltage, battery voltage as the charger sees it, charge mode, the charger's own (rough) SoC estimate |
| Load | power drawn by the load output (the inverter) and whether it is on; **Load ON / OFF** |
| Cells | voltage of every cell with a bar (2.8–3.6 V), min/max/spread; ⇄ marks a cell being balanced |
| Temperatures | BMS sensors, charger battery and internal sensor |
| Protection | the BMS MOSFETs (switching the discharge MOSFET off asks first: it cuts the load from the battery) and the active faults as text |
| Link | Wi-Fi signal, IP, InfluxDB result and queue, MQTT, time, uptime |

## Details and administration

The collapsed section at the bottom has the raw JSON of every item, the
network status, firmware upload, IP change, the breathing-LED toggle and the
reset. Upload, IP change and reset ask for the admin login.

## Without the page

* MQTT: publish `load on` / `load off` to `LiFePO_Island/1/cmd`.
* Home Assistant: `switch.lifepo_island_load`.
* `curl -X POST http://nat2:8080/api/load -d state=on`
