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
| Battery | state of charge counted by the BMS (the reliable one), voltage measured by the BMS at the cells, current and power (+ charging, − discharging), energy left/full in Wh (Ah × 3.2 V × cells), cycles |
| Solar | charge power, PV voltage, the battery voltage as the charger measures it at its terminals (0.1 V steps, a bit lower than the BMS value because of the cables), charge mode, the charger's SoC estimate from the voltage (rough: LiFePO4 voltage hardly changes between 20 and 90 %) |
| Load | power drawn by the load output (the inverter), with a bar up to the output's limit ("max … W" = the charger's 30 A at the current battery voltage); "off" under the bar when the output is off; **Load ON / OFF** |
| Cells | voltage of every cell with a bar (2.8–3.6 V). Bar colour = deviation from the average: blue below, white at average, orange/red above. While balancing (normal when full): **▼ drain** = the BMS bleeds this fuller cell through a resistor (it then reads a few mV lower), **▲ low** = below average, keeps its charge while the others come down. Hover a cell for its deviation in mV. Min/avg/max, spread |
| Temperatures | the two BMS thermistor probes, "Charger: battery" (the charger's battery temperature input) and "Charger: internal" (heatsink) |
| Protection | the BMS MOSFETs (switching the discharge MOSFET off asks first: it cuts the load from the battery), the active faults as text, the reserve policy level, the idle switch-off (settings, and while the load is on: minutes so far and their mean power) and the last event (who switched what and why) |
| Link | Wi-Fi signal, IP, InfluxDB result and queue, MQTT, time, uptime |

## When the load switches itself off

Only the **old 600 W inverter** hangs on the charger's load output. The new, larger inverter is wired to the pack
before the DC switch: nothing here switches it off, so switch it off by hand after use (its idle draw empties the
battery in winter just the same).

The load output (the old inverter) goes off by itself in two cases. The last event under the Protection tile says
which one.

* **Nothing uses it** (`[idle] load off: nothing in use ...`): the mean load power stayed below 15 W for 30 minutes.
  The inverter alone draws about 6-11 W; in winter that is more than the panel yields, so an inverter left on
  empties the battery. A charger (e-bike, scooter, tool battery) keeps it on; the load goes off about 30 minutes
  after it has finished. So the inverter's own switch can stay on: switch the load on (page, button, Home
  Assistant, MQTT) when you need it, and forget it. It does not come back on by itself.
* **Battery low** (`[software] load off ...`, level `load cut`): below 13 % (or a cell below 3.19 V). Switching it
  on is refused until the battery is back at 30 %; then it comes back by itself if you had wanted it on.

Change the idle settings (admin login), for example in summer: `curl -u admin -X POST
http://nat2:8080/api/idleoff -d thresholdW=15 -d minutes=30`; `minutes=0` switches the idle switch-off off.

## Details and administration

The collapsed section at the bottom has the raw JSON of every item, the
network status, firmware upload, IP change, the breathing-LED toggle and the
reset. Upload, IP change and reset ask for the admin login.

## Without the page

* MQTT: publish `load on` / `load off` to `LiFePO_Island/1/cmd`.
* Home Assistant: `switch.lifepo_island_load`.
* `curl -X POST http://nat2:8080/api/load -d state=on`
