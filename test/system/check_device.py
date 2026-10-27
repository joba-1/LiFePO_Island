#!/usr/bin/env python3
"""System test against a running LiFePO_Island firmware (see docs/test.md).

Default target is the test bench (Atom Echo, instance 9, simulated devices):

    python3 test/system/check_device.py --host lifepo_island-9

For the real island add --no-sim (skips the simulator-only checks) and
--read-only (no load or MOSFET switching):

    python3 test/system/check_device.py --host nat2:8080 --instance 1 \
        --db LiFePO_Island --ha-prefix homeassistant --no-sim --read-only

Needs only the Python standard library plus mosquitto_pub/mosquitto_sub for
the MQTT checks. Exit code 0 = all checks passed.
"""
import argparse
import base64
import configparser
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append((name, bool(ok)))
    print(("PASS " if ok else "FAIL ") + name + (f"  ({detail})" if detail and not ok else ""))
    return ok


def http(base, path, data=None, timeout=10, auth=None):
    """Return (status, body bytes, content type). auth: "user:password" for basic auth."""
    url = base + path
    body = urllib.parse.urlencode(data).encode() if data is not None else None
    req = urllib.request.Request(url, data=body, method="POST" if data is not None else "GET")
    if auth:
        req.add_header("Authorization", "Basic " + base64.b64encode(auth.encode()).decode())
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read(), r.headers.get("Content-Type", "")
    except urllib.error.HTTPError as e:
        return e.code, e.read(), e.headers.get("Content-Type", "")


def get_json(base, path):
    status, body, _ = http(base, path)
    return status, json.loads(body)


def live(base):
    return get_json(base, "/api/live")[1]


def age(v, default=99):
    """ageS value, with None (never seen) mapped to @default (0 is a valid age!)"""
    return default if v is None else v


def wait_for(fn, timeout=15, step=1.0):
    end = time.time() + timeout
    while True:
        try:
            if fn():
                return True
        except Exception:
            pass
        if time.time() > end:
            return False
        time.sleep(step)


def project_version():
    cp = configparser.ConfigParser(interpolation=None, strict=False)
    cp.read(os.path.join(os.path.dirname(__file__), "..", "..", "platformio.ini"))
    return cp.get("program", "version")


def test_http(base, version):
    status, body, ctype = http(base, "/")
    check("GET / serves the page", status == 200 and b"LiFePO_Island" in body and "html" in ctype)
    status, body, ctype = http(base, "/logo.svg")
    check("GET /logo.svg", status == 200 and body.lstrip().startswith(b"<svg") and "svg" in ctype)
    status, _, _ = http(base, "/does-not-exist")
    check("unknown path gives 404", status == 404)

    status, d = get_json(base, "/api/live")
    check("GET /api/live is JSON", status == 200)
    for key in ("host", "version", "uptimeS", "charger", "bms", "link", "msg", "timeValid"):
        check(f"/api/live has '{key}'", key in d)
    check("/api/live version matches platformio.ini", d.get("version") == version, d.get("version"))
    for key in ("code", "okAgeS", "queued", "dropped"):
        check(f"/api/live link.influx has '{key}'", key in d["link"]["influx"])

    status, ns = get_json(base, "/api/netstatus")
    check("GET /api/netstatus is JSON", status == 200)
    check("netstatus influx.queued/droppedLines",
          "queued" in ns["resources"]["influx"] and "droppedLines" in ns["resources"]["influx"])
    check("netstatus firmware version", ns["firmware"]["version"] == version)

    for item in ("Information", "ChgSts", "BatParam", "Log", "Parameters", "LoadParam",
                 "ProParam", "Status", "Cells", "Wifi"):
        status, body, _ = http(base, "/json/" + item)
        try:
            doc = json.loads(body)
            ok = status == 200 and doc.get("Version") == version
        except ValueError:
            ok = False
        check(f"/json/{item} valid JSON with quoted Version", ok, body[:80])


def test_data_fresh(base):
    check("charger data is live (ageS < 10)",
          wait_for(lambda: age(live(base)["charger"]["ageS"]) < 10, 30))
    check("BMS data is live (ageS < 15)",
          wait_for(lambda: age(live(base)["bms"]["ageS"]) < 15, 30))
    d = live(base)
    b = d["bms"]
    check("BMS cells reported", isinstance(b["cellsMv"], list) and len(b["cellsMv"]) >= 4)
    check("BMS power = V * A (±0.2 W)", b["W"] is not None and abs(b["V"] * b["A"] - b["W"]) <= 0.2,
          f'{b["V"]} * {b["A"]} vs {b["W"]}')
    check("time synced", wait_for(lambda: live(base)["timeValid"], 30))


def test_controls(base):
    status, body, _ = http(base, "/api/load", {"state": "bogus"})
    check("POST /api/load bad state -> 400", status == 400)
    status, body, _ = http(base, "/api/mosfets", {"charge": "2", "discharge": "1"})
    check("POST /api/mosfets bad value -> 400", status == 400)

    status, body, _ = http(base, "/api/load", {"state": "off"})
    r = json.loads(body)
    check("POST /api/load off", status == 200 and r["ok"] and r["load"] is False, body)
    check("live shows load off", wait_for(lambda: live(base)["charger"]["load"] is False, 5))
    status, body, _ = http(base, "/api/load", {"state": "toggle"})
    r = json.loads(body)
    check("POST /api/load toggle -> on", status == 200 and r["load"] is True, body)
    check("live shows load on", wait_for(lambda: live(base)["charger"]["load"] is True, 5))

    status, body, _ = http(base, "/api/mosfets", {"charge": "1", "discharge": "1"})
    r = json.loads(body)
    check("POST /api/mosfets both on", status == 200 and r["chargeFet"] and r["dischargeFet"], body)

    # one-shot message: delivered once, then empty
    http(base, "/api/load", {"state": "on"})
    d1 = live(base)
    d2 = live(base)
    # the poll of an open browser may consume it first; only check it is cleared
    check("msg is one-shot", d2["msg"] == "" or d1["msg"] == "")


def test_sim(base):
    status, s = get_json(base, "/api/sim?chargerFaults=1&bmsFaults=0")
    check("GET /api/sim (simulator present)", status == 200)
    check("charger fault shown as text",
          wait_for(lambda: "Battery over voltage" in (live(base)["charger"]["faults"] or []), 5))
    get_json(base, "/api/sim?chargerFaults=0")
    check("charger fault cleared",
          wait_for(lambda: "Battery over voltage" not in (live(base)["charger"]["faults"] or []), 5))

    get_json(base, "/api/sim?bms=0")
    check("BMS offline: data age grows", wait_for(lambda: age(live(base)["bms"]["ageS"], 0) >= 10, 25))
    get_json(base, "/api/sim?bms=1")
    check("BMS back: data fresh again", wait_for(lambda: age(live(base)["bms"]["ageS"]) < 10, 25))
    status, s = get_json(base, "/api/sim")
    check("setup_LiFePO wrote parameters at most once per boot", s["paramWrites"] <= 2, s)


def test_bmsparam(base, broker, topic, influx, db, admin_password, read_only):
    status, body, _ = http(base, "/api/bmsparam")
    try:
        doc = json.loads(body)
    except ValueError:
        doc = {}
    check("GET /api/bmsparam is JSON with 'read'", status == 200 and "read" in doc, body[:80])
    status, body, _ = http(base, "/json/BmsParam")
    check("GET /json/BmsParam is the same document", status == 200 and "read" in json.loads(body or "{}"))
    if read_only:
        return   # reading puts the BMS into factory mode: never on a real island by script

    locked = get_json(base, "/api/netstatus")[1]["device"].get("adminLock", False)
    if locked:
        status, body, _ = http(base, "/api/bmsparam", {})
        check("POST /api/bmsparam without login -> 401", status == 401, status)
    if locked and not admin_password:
        print("SKIP BMS settings read (device is locked: pass --admin-password)")
        return
    auth = ("admin:" + admin_password) if locked else None

    t0 = time.time()
    status, body, _ = http(base, "/api/bmsparam", {}, timeout=30, auth=auth)
    r = json.loads(body) if body[:1] == b"{" else {}
    p = r.get("params", {})
    check("POST /api/bmsparam reads the settings", status == 200 and r.get("ok") and p.get("read"), body[:120])
    check("the read takes seconds (factory mode), not milliseconds", time.time() - t0 > 1, round(time.time() - t0, 1))
    if not p.get("read"):
        return
    check("cell under voltage level and release are sane",
          2.0 <= p["cellUvp"]["V"] < p["cellUvp"]["releaseV"] <= 3.4, p["cellUvp"])
    check("pack levels agree with the cell count",
          p["packUvp"]["V"] <= p["cells"] * p["cellUvp"]["releaseV"] and p["packOvp"]["V"] >= p["cells"] * 3.0, p)
    check("discharge limit is positive amps", 0 < p["dischargeOcpA"] < 500, p["dischargeOcpA"])
    check("temperature pairs [limit, release]", all(len(v) == 2 for v in p["tempC"].values()), p["tempC"])
    check("SoC table rises", p["socTableMv"] == sorted(p["socTableMv"]), p["socTableMv"])

    status, body, _ = http(base, "/api/bmsparam")
    d = json.loads(body)
    check("GET after the read shows it (ageS small)", d.get("read") and d.get("ageS", 99) < 10, body[:80])
    status, body, _ = http(base, "/api/bmsparam", {}, timeout=30, auth=auth)
    check("a second read within 30 s is refused -> 503", status == 503 and "30 s" in body.decode(), body[:80])

    check("MQTT json/BmsParam retained", len(mqtt_sub(broker, topic + "/json/BmsParam", 1, 5)) == 1)
    q = "SELECT COUNT(CellUvp) FROM BmsParam WHERE time > now() - 5m"
    n = 0
    for _ in range(8):
        try:
            series = influx_query(influx, db, q)
            n = series[0]["values"][0][1] if series else 0
        except (KeyError, IndexError, TypeError, ValueError, OSError):
            n = 0
        if n:
            break
        time.sleep(3)
    check("BmsParam point arrives in InfluxDB", n >= 1, n)


def test_reserve(base, admin_password=""):
    """Battery reserve policy on the simulator: hold the state of charge, watch the levels (bench timers)."""
    def level():
        return live(base)["protection"]["level"]

    def sim(soc):
        http(base, "/api/sim?soc=" + str(soc))

    def load_on():
        return live(base)["charger"]["load"]

    check("/api/live has the protection block", "protection" in live(base) and "enforce" in live(base)["protection"])
    http(base, "/api/sim?nosleep=1")          # the sleep schedule itself is checked by hand: it drops the connection
    http(base, "/api/load", {"state": "on"})
    sim(50)
    check("level normal at 50 %", wait_for(lambda: level() == "normal", 40))
    check("load on at 50 %", wait_for(lambda: load_on() is True, 10))

    sim(14)                      # hardware's 15 % is not ours: 14 % does nothing
    time.sleep(25)
    check("14 %: software does not cut yet (margin)", level() == "normal" and load_on() is True)

    sim(12)
    check("12 %: level load-cut", wait_for(lambda: level() == "load-cut", 60))
    check("12 %: load forced off", wait_for(lambda: load_on() is False, 45))
    ev = live(base)["protection"]["lastEvent"]
    check("event says who and why", ev and "[software]" in ev["text"] and "load" in ev["text"], ev)
    status, body, _ = http(base, "/api/load", {"state": "on"})
    check("switching on is refused while the reserve is low", status == 503 and "reserve" in body.decode().lower(), body[:100])
    check("load stays off", load_on() is False)

    sim(8)
    check("8 %: sleep level after the delay", wait_for(lambda: level() == "sleep", 90))
    sim(2)
    check("2 %: level critical", wait_for(lambda: level() == "critical", 80))
    check("critical: discharge FET off", wait_for(lambda: live(base)["bms"]["dischargeFet"] is False, 30))
    check("critical: charge FET stays on", live(base)["bms"]["chargeFet"] is True)

    # A restart (OTA, watchdog, crash) must keep the level and the "FET cut by us" flag (RTC_NOINIT memory):
    # otherwise the device boots at normal and nobody switches the load or the FET on again.
    locked = get_json(base, "/api/netstatus")[1]["device"].get("adminLock", False)
    if locked and not admin_password:
        print("SKIP restart in critical (device is locked: pass --admin-password)")
    else:
        try:
            http(base, "/reset", {}, auth=("admin:" + admin_password) if locked else None)
        except OSError:
            pass                                  # the connection may drop with the restart
        time.sleep(3)
        check("restart: device is back", wait_for(lambda: live(base)["uptimeS"] < 60, 60))
        http(base, "/api/sim?nosleep=1")          # the simulator forgot its settings
        sim(2)
        p = live(base)["protection"]
        check("restart: level critical at once, not found again after a debounce", p["level"] == "critical", p)
        ev = p["lastEvent"]
        # the simulated BMS comes back with the discharge FET on, so the re-cut event may already follow the restore
        check("restart: event says the level was restored (or the FET was cut again)",
              ev and ("restored level critical" in ev["text"] or "discharge FET off" in ev["text"]), ev)
        check("restart: discharge FET (still or again) off", wait_for(lambda: live(base)["bms"]["dischargeFet"] is False, 45))

    sim(40)
    check("40 %: back to normal", wait_for(lambda: level() == "normal", 150))
    check("40 %: discharge FET on again", wait_for(lambda: live(base)["bms"]["dischargeFet"] is True, 40))
    check("40 %: load back on (the user wanted it on)", wait_for(lambda: load_on() is True, 60))
    http(base, "/api/sim?soc=free&nosleep=0")


def mqtt_sub(broker, topic, count=1, timeout=10):
    try:
        out = subprocess.run(["mosquitto_sub", "-h", broker, "-t", topic, "-C", str(count), "-W", str(timeout)],
                             capture_output=True, text=True, timeout=timeout + 5)
        return out.stdout.strip().splitlines()
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return []


def mqtt_pub(broker, topic, payload):
    subprocess.run(["mosquitto_pub", "-h", broker, "-t", topic, "-m", payload], check=False)


def test_mqtt(base, broker, topic, read_only):
    check("MQTT LWT retained 'Online'", mqtt_sub(broker, topic + "/status/LWT") == ["Online"])
    # items are published on change only; at night the charger can sit still for minutes
    check("MQTT json/* published", len(mqtt_sub(broker, topic + "/json/#", 1, 20)) == 1)
    if read_only:
        return
    http(base, "/api/load", {"state": "off"})
    for payload in ("load", "l", "load o"):   # 4.1 switched the load ON on a prefix
        mqtt_pub(broker, topic + "/cmd", payload)
    time.sleep(3)
    check("MQTT prefix commands ignored", live(base)["charger"]["load"] is False)
    mqtt_pub(broker, topic + "/cmd", " LOAD ON ")
    check("MQTT 'load on' switches", wait_for(lambda: live(base)["charger"]["load"] is True, 8))


def test_ha_discovery(broker, prefix, instance, topic):
    node = f"lifepo_island_{instance}"
    try:
        out = subprocess.run(["mosquitto_sub", "-h", broker, "-t", f"{prefix}/+/{node}/+/config", "-v", "-W", "4"],
                             capture_output=True, text=True, timeout=10).stdout
    except (FileNotFoundError, subprocess.TimeoutExpired):
        out = ""
    msgs = {}
    for line in out.splitlines():
        t, payload = line.split(" ", 1)
        msgs[t] = payload
    check("HA discovery: >= 26 retained config messages", len(msgs) >= 26, len(msgs))
    sizes, ok = [], True
    for t, payload in msgs.items():
        sizes.append(len(t) + len(payload.encode()))
        try:
            d = json.loads(payload)
            ok &= d["uniq_id"].startswith(node + "_") and d["~"] == topic and "avty_t" in d
        except (ValueError, KeyError):
            ok = False
    check("HA discovery: valid JSON, unique ids, availability", ok and msgs)
    check("HA discovery: every message <= 480 bytes", sizes and max(sizes) <= 480, max(sizes or [0]))
    for t in ("status/Load", "status/Charger", "status/BMS", "json/Log"):
        check(f"retained {t}", len(mqtt_sub(broker, f"{topic}/{t}", 1, 3)) == 1)


def influx_query(server, db, q):
    url = f"http://{server}/query?" + urllib.parse.urlencode({"db": db, "q": q, "epoch": "ms"})
    with urllib.request.urlopen(url, timeout=10) as r:
        return json.load(r)["results"][0].get("series", [])


def test_influx(base, server, db):
    check("influx batch posted ok", wait_for(lambda: 200 <= live(base)["link"]["influx"]["code"] < 300, 20))
    # on change only: count all items (Wifi alone comes every ~10 s)
    n = 0
    for meas, field in (("ChgSts", "BatVolt"), ("Status", "voltage"), ("Cells", "voltage1"), ("Wifi", "RSSI")):
        s = influx_query(server, db, f"SELECT count({field}) FROM {meas} WHERE time > now() - 2m")
        n += s[0]["values"][0][1] if s else 0
    check("points arrive in InfluxDB", n > 10, f"{n} points in 2 min")
    s = influx_query(server, db, "SELECT RSSI FROM Wifi WHERE time > now() - 2m")
    times = [v[0] for v in s[0]["values"]] if s else []
    check("points carry ms timestamps", any(t % 1000 for t in times), times[:3])


def test_idle_off(base, admin_password=""):
    """Idle switch-off on the simulator: the load goes off when its mean power stays below the threshold."""
    status, cfg = get_json(base, "/api/idleoff")
    check("GET /api/idleoff has threshold and minutes", status == 200 and "thresholdW" in cfg and "minutes" in cfg, cfg)
    check("/api/live protection.idleOff", "idleOff" in live(base)["protection"])
    locked = get_json(base, "/api/netstatus")[1]["device"].get("adminLock", False)
    if locked and not admin_password:
        print("SKIP idle switch-off (device is locked: pass --admin-password)")
        return
    auth = ("admin:" + admin_password) if locked else None
    status, _, _ = http(base, "/api/idleoff", {"thresholdW": 0}, auth=auth)
    check("POST /api/idleoff thresholdW=0 -> 400", status == 400)

    def load_on():
        return live(base)["charger"]["load"]

    http(base, "/api/sim?soc=50&loadw=22")    # reserve normal, something in use
    http(base, "/api/load", {"state": "on"})
    status, _, _ = http(base, "/api/idleoff", {"thresholdW": 15, "minutes": 1}, auth=auth)
    check("POST /api/idleoff thresholdW=15&minutes=1", status == 200)
    time.sleep(100)
    check("22 W in use: the load stays on", load_on() is True)
    http(base, "/api/sim?loadw=5")            # the e-bike is full, the inverter idles
    check("5 W idle: load off after about a minute", wait_for(lambda: load_on() is False, 150, 5))
    ev = live(base)["protection"]["lastEvent"]
    check("event says idle and why", ev and ev["text"].startswith("[idle] load off: nothing in use"), ev)

    http(base, "/api/idleoff", {"thresholdW": cfg["thresholdW"], "minutes": cfg["minutes"]}, auth=auth)
    http(base, "/api/sim?soc=free&loadw=-1")
    http(base, "/api/load", {"state": "on"})
    check("settings restored", get_json(base, "/api/idleoff")[1]["minutes"] == cfg["minutes"])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="lifepo_island-9", help="device host[:port]")
    ap.add_argument("--instance", default="9")
    ap.add_argument("--db", default="LiFePO_Island_test")
    ap.add_argument("--influx", default="influx:8086")
    ap.add_argument("--mqtt", default="mqtt")
    ap.add_argument("--ha-prefix", default="homeassistant_bench", help="HA discovery prefix")
    ap.add_argument("--no-sim", action="store_true", help="device has real charger/BMS")
    ap.add_argument("--read-only", action="store_true", help="no switching, no BMS settings read")
    ap.add_argument("--admin-password", default=os.environ.get("ADMIN_PASSWORD", ""),
                    help="admin password of the device (default: $ADMIN_PASSWORD)")
    a = ap.parse_args()
    base = "http://" + a.host
    topic = "LiFePO_Island/" + a.instance

    test_http(base, project_version())
    test_data_fresh(base)
    if not a.read_only:
        test_controls(base)
    if not a.no_sim:
        test_sim(base)
    test_mqtt(base, a.mqtt, topic, a.read_only)
    test_ha_discovery(a.mqtt, a.ha_prefix, a.instance, topic)
    test_influx(base, a.influx, a.db)
    test_bmsparam(base, a.mqtt, topic, a.influx, a.db, a.admin_password, a.read_only or a.no_sim)
    if not a.no_sim and not a.read_only:
        test_reserve(base, a.admin_password)
        test_idle_off(base, a.admin_password)

    failed = [n for n, ok in RESULTS if not ok]
    print(f"\n{len(RESULTS) - len(failed)}/{len(RESULTS)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
