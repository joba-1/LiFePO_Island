#!/usr/bin/env python3
"""One-time migration of the hand-written LiFePO_Island YAML entities in Home
Assistant to the island's MQTT discovery (docs/spec-homeassistant.md).

Runs on job6; reads and writes the HA config on the QNAP over `ssh qnap`.

    tools/ha_migrate.py plan                 # read-only: what would change (default)
    tools/ha_migrate.py run                  # backup, stop HA, migrate, start HA
    tools/ha_migrate.py run --rescale-recorder   # ... and cell history mV -> V (recorder)
    tools/ha_migrate.py rescale-influx       # cell history mV -> V in InfluxDB (HA may run)
    tools/ha_migrate.py rollback STAMP       # restore the backups of a run, restart HA

What `run` does, with HA stopped:
 1. entity registry: the 12 YAML entities (platform mqtt) get the discovery
    unique IDs, so the discovered entities adopt them: same entity IDs,
    history, statistics, energy dashboard, automations. Registry entries a
    discovery run may already have created for the new IDs are removed first.
 2. configuration.yaml: the LiFePO_Island items under mqtt: switch/sensor are
    removed (nothing else; checked by comparing the parsed YAML).
 3. optional: recorder states of the cell sensors > 100 (millivolts with
    unit V) are divided by 1000.
Backups: <file>.bak-ha-migrate-<STAMP> next to each changed file.
"""
import argparse
import datetime
import json
import ssl
import shlex
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

import yaml

SSH = "qnap"
CONFIG = "/share/Docker/homeassistant/config"
DOCKER = "/share/CACHEDEV1_DATA/.qpkg/container-station/bin/docker"
CONTAINER = "home-assistant-2026.10.0"
IMAGE = "homeassistant/home-assistant:2026.10.0"
HA_URL = "https://qnap:8123/manifest.json"   # HA serves TLS on 8123 (own certificate)
INFLUX = "http://influx:8086"

REGISTRY = CONFIG + "/.storage/core.entity_registry"
YAML_FILE = CONFIG + "/configuration.yaml"
RECORDER_DB = CONFIG + "/home-assistant_v2.db"

# old YAML unique id -> discovery unique id (instance 1)
MAPPING = {
    "lifepo_island_load_switch": "lifepo_island_1_load",
    "lifepo_island_cell_1": "lifepo_island_1_cell_1",
    "lifepo_island_cell_2": "lifepo_island_1_cell_2",
    "lifepo_island_cell_3": "lifepo_island_1_cell_3",
    "lifepo_island_cell_4": "lifepo_island_1_cell_4",
    "lifepo_island_capacity": "lifepo_island_1_soc",
    "lifepo_island_load_power": "lifepo_island_1_load_power",
    "lifepo_island_charge_power": "lifepo_island_1_charge_power",
    "lifepo_island_converter_temp": "lifepo_island_1_charger_temp",
    "lifepo_island_battery_temp": "lifepo_island_1_charger_battery_temp",
    "lifepo_island_production": "lifepo_island_1_energy_production",
    "lifepo_island_consumption": "lifepo_island_1_energy_consumption",
}
CELL_ENTITIES = [f"sensor.lifepo_island_cell_{i}" for i in range(1, 5)]


# ---------------------------------------------------------------- ssh helpers

def ssh(cmd, data=None, check=True):
    r = subprocess.run(["ssh", SSH, cmd], input=data, capture_output=True, timeout=600)
    if check and r.returncode != 0:
        sys.exit(f"ssh {cmd!r} failed: {r.stderr.decode(errors='replace').strip()}")
    return r.stdout


def read_remote(path):
    return ssh("cat " + shlex.quote(path)).decode()


def write_remote(path, text):
    tmp = path + ".ha-migrate-tmp"
    ssh("cat > " + shlex.quote(tmp), data=text.encode())
    ssh(f"mv {shlex.quote(tmp)} {shlex.quote(path)}")


def ha_running():
    out = ssh(f"{DOCKER} ps --format '{{{{.Names}}}}'").decode().split()
    return CONTAINER in out


# ---------------------------------------------------------------- registry

def plan_registry(reg):
    """Return (changes, removals, problems) for the registry document."""
    entities = reg["data"]["entities"]
    by_uid = {(e["platform"], e["unique_id"]): e for e in entities}
    changes, removals, problems = [], [], []
    for old, new in MAPPING.items():
        e = by_uid.get(("mqtt", old))
        fresh = by_uid.get(("mqtt", new))
        if e is None:
            if fresh is not None:
                problems.append(f"{old}: already migrated ({fresh['entity_id']})")
            else:
                problems.append(f"{old}: not in the registry")
            continue
        if fresh is not None:
            removals.append(fresh)
        changes.append((e, new))
    return changes, removals, problems


def apply_registry(reg, changes, removals):
    gone = {id(e) for e in removals}
    reg["data"]["entities"] = [e for e in reg["data"]["entities"] if id(e) not in gone]
    for e, new in changes:
        e["unique_id"] = new
    return reg


# ---------------------------------------------------------------- YAML

class AnyTagLoader(yaml.SafeLoader):
    """Loads !secret, !include ... as plain strings (only for comparing)."""


AnyTagLoader.add_multi_constructor("!", lambda loader, suffix, node: f"!{suffix} {getattr(node, 'value', '')}")


def is_lifepo_item(item):
    return isinstance(item, dict) and "lifepo" in str(item.get("unique_id", "")).lower() \
        and str(item.get("unique_id")) in MAPPING


def strip_yaml(text):
    """Remove the LiFePO items from mqtt: switch/sensor as text, keeping all
    formatting. Returns (new_text, removed_item_count)."""
    lines = text.split("\n")
    out, removed, i = [], 0, 0
    while i < len(lines):
        line = lines[i]
        if line.startswith("    - name:"):
            # an item at indent 4: collect it (deeper lines and blank lines inside)
            j = i + 1
            while j < len(lines) and (lines[j].startswith("      ") or lines[j].strip() == ""):
                j += 1
            block = lines[i:j]
            # trailing blank lines belong to the separation, not the item
            k = len(block)
            while k > 1 and block[k - 1].strip() == "":
                k -= 1
            item_text = "\n".join(block[:k])
            item = yaml.load(item_text.replace("    - ", "- ", 1).replace("\n      ", "\n  "), Loader=AnyTagLoader)
            if isinstance(item, list) and item and is_lifepo_item(item[0]):
                removed += 1
                # drop the item; keep at most one blank line between neighbours
                trailing = block[k:]
                if out and (out[-1].strip() == "" or out[-1].rstrip().endswith(":")):
                    trailing = []   # no blank line after a header or a blank line
                out.extend(trailing[:1])
                i = j
                continue
            out.extend(block)
            i = j
            continue
        out.append(line)
        i += 1
    return "\n".join(out), removed


def check_yaml(before, after):
    """The parsed configs must be equal except for the removed LiFePO items."""
    b = yaml.load(before, Loader=AnyTagLoader)
    a = yaml.load(after, Loader=AnyTagLoader)
    for section in ("switch", "sensor"):
        b["mqtt"][section] = [x for x in b["mqtt"].get(section, []) if not is_lifepo_item(x)]
    if a != b:
        sys.exit("YAML check failed: the edited configuration differs in more than the LiFePO items")
    if "lifepo" in json.dumps(a["mqtt"]).lower():
        sys.exit("YAML check failed: LiFePO entries left under mqtt:")


# ---------------------------------------------------------------- recorder

RECORDER_PY = r'''
import sqlite3, sys
ids = %r
db = sqlite3.connect("/config/home-assistant_v2.db")
meta = [r[0] for r in db.execute(
    "SELECT metadata_id FROM states_meta WHERE entity_id IN (%%s)" %% ",".join("?" * len(ids)), ids)]
if not meta:
    print("no recorder history for the cells"); sys.exit(0)
q = "FROM states WHERE metadata_id IN (%%s) AND state GLOB '[0-9]*' AND CAST(state AS REAL) > 100" %% ",".join("?" * len(meta))
n = db.execute("SELECT count(*) " + q, meta).fetchone()[0]
if %r:
    db.execute("UPDATE states SET state = printf('%%.3f', CAST(state AS REAL) / 1000.0) "
               "WHERE state_id IN (SELECT state_id " + q + ")", meta)
    db.commit()
    print(f"recorder: {n} cell states rescaled mV -> V")
else:
    print(f"recorder: {n} cell states would be rescaled mV -> V")
'''


def recorder(apply):
    script = RECORDER_PY % (CELL_ENTITIES, apply)
    out = ssh(f"{DOCKER} run --rm -i --entrypoint python3 -v {CONFIG}:/config {IMAGE} -", data=script.encode())
    print(out.decode().strip())


# ---------------------------------------------------------------- influx

def influx_query(q, db="homeassistant"):
    url = f"{INFLUX}/query?" + urllib.parse.urlencode({"db": db, "q": q})
    req = urllib.request.Request(url, data=b"", method="POST")
    with urllib.request.urlopen(req, timeout=600) as r:
        return json.load(r)["results"][0]


def rescale_influx(apply):
    """Server side: SELECT value/1000 INTO the same series, month by month."""
    for ent in CELL_ENTITIES:
        eid = ent.split(".", 1)[1]
        first = influx_query(f"SELECT first(value) FROM \"V\" WHERE entity_id='{eid}' AND value > 100")
        last = influx_query(f"SELECT last(value) FROM \"V\" WHERE entity_id='{eid}' AND value > 100")
        if "series" not in first:
            print(f"influx {eid}: nothing in millivolts")
            continue
        start = datetime.datetime.fromisoformat(first["series"][0]["values"][0][0].replace("Z", "+00:00"))
        end = datetime.datetime.fromisoformat(last["series"][0]["values"][0][0].replace("Z", "+00:00"))
        total = 0
        t = start.replace(day=1, hour=0, minute=0, second=0, microsecond=0)
        while t <= end:
            nxt = (t + datetime.timedelta(days=32)).replace(day=1)
            where = (f"entity_id='{eid}' AND value > 100 AND time >= '{t.isoformat()}' "
                     f"AND time < '{nxt.isoformat()}'")
            if apply:
                r = influx_query(f"SELECT value / 1000 AS value INTO \"V\" FROM \"V\" WHERE {where} GROUP BY *")
                n = r["series"][0]["values"][0][1] if "series" in r else 0
            else:
                r = influx_query(f"SELECT count(value) FROM \"V\" WHERE {where}")
                n = r["series"][0]["values"][0][1] if "series" in r else 0
            total += n
            t = nxt
        print(f"influx {eid}: {total} points {'rescaled' if apply else 'to rescale'} mV -> V "
              f"({start:%Y-%m-%d} .. {end:%Y-%m-%d})")


# ---------------------------------------------------------------- HA control

def stop_ha():
    print("stopping Home Assistant ...")
    ssh(f"{DOCKER} stop -t 120 {CONTAINER}")


def start_ha():
    print("starting Home Assistant ...")
    ssh(f"{DOCKER} start {CONTAINER}")
    for _ in range(90):
        try:
            ctx = ssl.create_default_context()
            ctx.check_hostname = False          # certificate is for the public name
            ctx.verify_mode = ssl.CERT_NONE
            with urllib.request.urlopen(HA_URL, timeout=5, context=ctx) as r:
                if r.status == 200:
                    print("Home Assistant answers again")
                    return
        except (urllib.error.URLError, OSError):
            pass
        time.sleep(5)
    print("WARNING: Home Assistant did not answer within 7.5 min - check the container log")


# ---------------------------------------------------------------- commands

def load_state():
    reg = json.loads(read_remote(REGISTRY))
    text = read_remote(YAML_FILE)
    changes, removals, problems = plan_registry(reg)
    new_text, removed = strip_yaml(text)
    return reg, text, changes, removals, problems, new_text, removed


def cmd_plan(args):
    reg, text, changes, removals, problems, new_text, removed = load_state()
    print(f"Home Assistant container {CONTAINER}: {'running' if ha_running() else 'STOPPED'}\n")
    print("Entity registry (entity ID stays, unique ID changes):")
    for e, new in changes:
        print(f"  {e['entity_id']:45s} {e['unique_id']:30s} -> {new}")
    for e in removals:
        print(f"  remove fresh discovery entry {e['entity_id']} ({e['unique_id']})")
    for p in problems:
        print(f"  PROBLEM: {p}")
    print(f"\nconfiguration.yaml: {removed} LiFePO_Island items to remove (expected {len(MAPPING)})")
    check_yaml(text, new_text)
    print("  parsed-YAML check: only the LiFePO items differ - ok")
    print()
    recorder(apply=False) if args.rescale_recorder else None
    if problems or removed != len(MAPPING):
        print("\nNOT ready to run.")
        return 1
    print("\nReady. `run` stops HA for about a minute.")
    return 0


def cmd_run(args):
    reg, text, changes, removals, problems, new_text, removed = load_state()
    if problems or removed != len(MAPPING):
        sys.exit("preconditions not met - see `plan`")
    check_yaml(text, new_text)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    was_running = ha_running()
    if was_running:
        stop_ha()
    try:
        # re-read: HA writes the registry on shutdown
        reg, text, changes, removals, problems, new_text, removed = load_state()
        if problems or removed != len(MAPPING):
            raise RuntimeError("preconditions changed while stopping HA")
        for path in (REGISTRY, YAML_FILE) + ((RECORDER_DB,) if args.rescale_recorder else ()):
            ssh(f"cp -p {shlex.quote(path)} {shlex.quote(path)}.bak-ha-migrate-{stamp}")
        print(f"backups: *.bak-ha-migrate-{stamp}")
        write_remote(REGISTRY, json.dumps(apply_registry(reg, changes, removals), indent=4, ensure_ascii=False))
        write_remote(YAML_FILE, new_text)
        print(f"registry: {len(changes)} unique IDs changed, {len(removals)} fresh entries removed")
        print(f"configuration.yaml: {removed} items removed")
        if args.rescale_recorder:
            recorder(apply=True)
    finally:
        if was_running:
            start_ha()
    print(f"\nDone. Rollback: tools/ha_migrate.py rollback {stamp}")
    return 0


def cmd_rollback(args):
    stamp = args.stamp
    files = [REGISTRY, YAML_FILE, RECORDER_DB]
    present = [f for f in files
               if subprocess.run(["ssh", SSH, f"test -e {shlex.quote(f)}.bak-ha-migrate-{stamp}"]).returncode == 0]
    if not present:
        sys.exit(f"no backups with stamp {stamp}")
    was_running = ha_running()
    if was_running:
        stop_ha()
    try:
        for f in present:
            ssh(f"cp -p {shlex.quote(f)}.bak-ha-migrate-{stamp} {shlex.quote(f)}")
            print(f"restored {f}")
    finally:
        if was_running:
            start_ha()
    return 0


def cmd_rescale_influx(args):
    rescale_influx(apply=not args.dry_run)
    return 0


def main():
    sys.stdout.reconfigure(line_buffering=True)   # progress visible when piped
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd")
    p = sub.add_parser("plan")
    p.add_argument("--rescale-recorder", action="store_true")
    p = sub.add_parser("run")
    p.add_argument("--rescale-recorder", action="store_true")
    p = sub.add_parser("rescale-influx")
    p.add_argument("--dry-run", action="store_true")
    p = sub.add_parser("rollback")
    p.add_argument("stamp")
    args = ap.parse_args()
    if args.cmd in (None, "plan"):
        args.rescale_recorder = getattr(args, "rescale_recorder", False)
        return cmd_plan(args)
    return {"run": cmd_run, "rollback": cmd_rollback, "rescale-influx": cmd_rescale_influx}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
