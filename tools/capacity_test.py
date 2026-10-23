#!/usr/bin/env python3
"""Capacity of the pack from the BMS current logged in InfluxDB (docs/capacity-test.md).

Capacity is a charge: the current integrated over time. The BMS measures the current, so no known load is
needed. Two modes, both between two anchors:

  charge     (default) low rest anchor -> BMS counter reads 100 %: the Ah that went in. No discharge needed.
  discharge  BMS counter 100 % -> low rest anchor: the Ah that came out.

Low rest anchor: average cell voltage <= --anchor (3.08 V) with |I| < 0.1 A for at least 15 minutes. Below about
12 % the voltage is steep, so this point is well defined (about 1 % per 15 mV).

The BMS does not see the charger and the ESP (about 0.09 A, shown as 0.00 A): minutes with |I| < 0.1 A are
corrected with --idle-a. Gaps in the data are reported, and a result with gaps is marked as unreliable.

    capacity_test.py                      latest finished episode (or the state of a running one)
    capacity_test.py --since 2026-10-23   only look from there
    capacity_test.py --write              also write the result as measurement CapacityTest
"""
import argparse
import datetime
import json
import sys
import urllib.parse
import urllib.request

BIN_S = 60
REST_CENTI_A = 10          # |I| < 0.10 A counts as rest (10 mA units)
GAP_S = 300                # more than 5 minutes without data: gap
MIN_ANCHOR_MIN = 15


def fetch_rows(server, db, since, until=None):
    """[(epoch, soc %, average cell V, mean current in 10 mA, box temperature degC or None)] per minute."""
    where = f"time >= '{since}'" + (f" AND time < '{until}'" if until else "")
    sql = (f"SELECT LAST(currentCapacity) AS soc, MEAN(voltage) AS v, MEAN(current) AS a, MEAN(temperature2) AS t "
           f"FROM Status WHERE {where} GROUP BY time({BIN_S}s) fill(none)")
    url = f"http://{server}/query?" + urllib.parse.urlencode({"db": db, "q": sql, "epoch": "s"})
    with urllib.request.urlopen(url, timeout=120) as r:
        series = json.load(r)["results"][0].get("series", [])
    rows = []
    for t, soc, v, a, temp in (series[0]["values"] if series else []):
        if None in (soc, v, a):
            continue
        rows.append((t, soc, v / 4 / 100.0, a, temp / 10.0 if temp is not None else None))
    return rows


def low_anchors(rows, anchor_v):
    """Indexes of the last rest sample of every low phase: cell <= anchor_v and |I| < 0.1 A for >= 15 minutes
    without a gap in the data."""
    found = []
    run = 0
    for i, (t, soc, cell, a, temp) in enumerate(rows):
        ok = abs(a) < REST_CENTI_A and cell <= anchor_v
        if ok and run and t - rows[i - 1][0] > GAP_S:
            run = 0                                   # a gap ends the run
        if ok:
            run += 1
        else:
            if run >= MIN_ANCHOR_MIN:
                found.append(i - 1)
            run = 0
    if run >= MIN_ANCHOR_MIN:
        found.append(len(rows) - 1)
    return found


def integrate(rows, i0, i1, idle_a):
    """Ah between rows[i0] and rows[i1] (net, + in), idle correction, gaps, mean temperature."""
    ah = 0.0
    idle_min = 0.0
    gaps = []
    temps = []
    for k in range(i0 + 1, i1 + 1):
        dt = rows[k][0] - rows[k - 1][0]
        if dt > GAP_S:
            gaps.append((rows[k - 1][0], rows[k][0]))   # the current of the last sample is assumed to go on
        ah += rows[k][3] / 100.0 * dt / 3600.0
        if abs(rows[k][3]) < REST_CENTI_A:
            idle_min += dt / 60.0
        if rows[k][4] is not None:
            temps.append(rows[k][4])
    return {
        "counted_ah": ah, "idle_ah": idle_a * idle_min / 60.0, "gaps": gaps,
        "temp_c": sum(temps) / len(temps) if temps else None,
        "hours": (rows[i1][0] - rows[i0][0]) / 3600.0,
    }


def episodes(rows, anchor_v, mode, idle_a):
    """All episodes of @p mode, oldest first: dicts with status "done" or (the last one only) "running"."""
    anchors = low_anchors(rows, anchor_v)
    out = []
    if mode == "charge":
        by_end = {}
        for ai in anchors:
            end = next((k for k in range(ai + 1, len(rows)) if rows[k][1] >= 100), None)
            if end is None:
                continue
            by_end[end] = ai                          # the last low anchor before the same full charge
        for end, ai in sorted(by_end.items()):
            res = integrate(rows, ai, end, idle_a)
            res.update(status="done", mode=mode, start=rows[ai][0], end=rows[end][0], anchor_v=rows[ai][2],
                       capacity_ah=res["counted_ah"] - res["idle_ah"])
            out.append(res)
        if anchors and not any(rows[k][1] >= 100 for k in range(anchors[-1] + 1, len(rows))):
            ai = anchors[-1]
            res = integrate(rows, ai, len(rows) - 1, idle_a)
            res.update(status="running", mode=mode, start=rows[ai][0], anchor_v=rows[ai][2], soc=rows[-1][1],
                       capacity_ah=res["counted_ah"] - res["idle_ah"])
            out.append(res)
        return out
    by_start = {}
    for ai in anchors:                                # discharge: the last 100 % before each low anchor
        full = next((k for k in range(ai, -1, -1) if rows[k][1] >= 100), None)
        if full is None:
            continue
        by_start[full] = ai                           # the first anchor after that full charge would be shallower:
    for full, ai in sorted(by_start.items()):         # keep the last one, the pack went down to it
        res = integrate(rows, full, ai, idle_a)
        res.update(status="done", mode=mode, start=rows[full][0], end=rows[ai][0], anchor_v=rows[ai][2],
                   capacity_ah=-res["counted_ah"] + res["idle_ah"])
        out.append(res)
    return out


def find_episode(rows, anchor_v, mode, idle_a):
    """The latest episode (or running state), or a dict with only a status text."""
    if len(rows) < 2:
        return {"status": "no data"}
    eps = episodes(rows, anchor_v, mode, idle_a)
    if eps:
        return eps[-1]
    if mode == "charge":
        return {"status": "no low anchor yet (cell average <= %.2f V at rest for 15 minutes)" % anchor_v}
    return {"status": "no discharge from 100 %% to a low anchor in the data"}


def describe(res):
    if res.get("status") not in ("done", "running"):
        return "No result: " + res.get("status", "?")
    fmt = lambda t: datetime.datetime.fromtimestamp(t, datetime.UTC).strftime("%Y-%m-%d %H:%M")
    lines = []
    if res["status"] == "running":
        lines.append(f"Running {res['mode']} test: low anchor {fmt(res['start'])} (cell {res['anchor_v']:.3f} V), counter now {res['soc']} %.")
        lines.append(f"  so far {res['capacity_ah']:.1f} Ah in {res['hours']:.1f} h; wait for the counter to read 100 % (see docs/capacity-test.md).")
    else:
        lines.append(f"Capacity ({res['mode']}): {res['capacity_ah']:.1f} Ah  ({res['capacity_ah'] / 272.0 * 100:.0f} % of the nominal 272 Ah)")
        lines.append(f"  {fmt(res['start'])} -> {fmt(res['end'])} = {res['hours']:.1f} h, anchor cell {res['anchor_v']:.3f} V")
        lines.append(f"  counted by the BMS {res['counted_ah']:.1f} Ah, unseen idle {res['idle_ah']:.1f} Ah (correction)")
    if res["temp_c"] is not None:
        lines.append(f"  mean battery box temperature {res['temp_c']:.1f} degC (capacity is lower when cold: compare tests at 15-25 degC)")
    if res["gaps"]:
        lost = sum(b - a for a, b in res["gaps"]) / 3600.0
        lines.append(f"  WARNING: {len(res['gaps'])} gap(s) in the data, {lost:.1f} h without samples: the result is unreliable")
    if res["hours"] > 72:
        lines.append(f"  WARNING: the episode took {res['hours'] / 24:.1f} days: the idle correction (+/- 0.03 A) is worth {0.03 * res['hours']:.1f} Ah of uncertainty")
    return "\n".join(lines)


def influx_line(res):
    return (f"CapacityTest,Mode={res['mode']} CapacityAh={res['capacity_ah']:.2f},CountedAh={res['counted_ah']:.2f},"
            f"IdleAh={res['idle_ah']:.2f},Hours={res['hours']:.2f},AnchorV={res['anchor_v']:.4f},"
            f"TempC={res['temp_c'] if res['temp_c'] is not None else 0:.1f},Gaps={len(res['gaps'])}i {int(res['end'])}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--influx", default="influx:8086")
    ap.add_argument("--db", default="LiFePO_Island")
    ap.add_argument("--since", default=None, help="start of the search, e.g. 2026-10-20 (default: 40 days ago)")
    ap.add_argument("--mode", choices=("charge", "discharge"), default="charge")
    ap.add_argument("--anchor", type=float, default=3.08, help="low anchor: average cell V at rest (default 3.08)")
    ap.add_argument("--idle-a", type=float, default=0.09, help="current of charger + ESP that the BMS does not see (A)")
    ap.add_argument("--all", action="store_true", help="list every finished episode in the period")
    ap.add_argument("--write", action="store_true", help="write the finished result to InfluxDB (CapacityTest)")
    a = ap.parse_args()
    since = a.since or (datetime.datetime.now(datetime.UTC) - datetime.timedelta(days=40)).strftime("%Y-%m-%dT%H:%M:%SZ")
    rows = fetch_rows(a.influx, a.db, since)
    if a.all:
        for e in episodes(rows, a.anchor, a.mode, a.idle_a):
            print(describe(e))
            print()
    res = find_episode(rows, a.anchor, a.mode, a.idle_a)
    print(describe(res))
    if a.write and res.get("status") == "done":
        req = urllib.request.Request(f"http://{a.influx}/write?db={a.db}&precision=s", data=influx_line(res).encode(), method="POST")
        urllib.request.urlopen(req, timeout=20).read()
        print("written to InfluxDB (CapacityTest)")
    return 0 if res.get("status") in ("done", "running") else 1


if __name__ == "__main__":
    sys.exit(main())
