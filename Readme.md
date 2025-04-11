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

Comments welcome

Joachim Banzhaf
