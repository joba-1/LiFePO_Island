# Battery reserve policy

Status: implemented in 5.0.0 (`include/Protection.h`, glue in `src/main.cpp`), tested on the bench only.
Not yet deployed to the island; wiring review pending (see [wiring-check.md](wiring-check.md)).

## Goal

When the sun stays dark (holiday), the battery must keep **charger and ESP alive for at least 2 weeks, better a
month**, and the cells must never get near damaging voltages. Observed: charger + ESP draw about 1 W, which the
BMS does not see (0.00 A); 14 days are 340-540 Wh (10-15 %), 30 days 720-1150 Wh (21-33 %) of 3.48 kWh.

## Ladder

Charger and BMS act first. This software follows with a margin, and it alone decides when things come back.

| Level | Enter when (debounced: 6 evaluations of 10 s in a row) | Does |
|---|---|---|
| `normal` | | everything allowed |
| `load-cut` | BMS SoC <= **13 %** (15 % target of charger/BMS minus 2), **or** the lowest cell, lifted by its sag (below), < **3.19 V** | forces the charger load off if it is still on (the charger or BMS did not cut it), refuses any "load on" (web, MQTT, button, old forms), logs |
| `sleep` | SoC <= 10 % or lowest cell (lifted by its sag) < 3.15 V, **and** at least 30 min after the load was cut | ESP deep sleep schedule: awake 3 min, asleep 30 min |
| `critical` | SoC <= 3 % or lowest cell < 2.95 V (any current) | as sleep but 6 h asleep; discharge FET off (only if enabled), charge FET stays on |

Leaving: `load-cut` needs **SoC >= 30 % and lowest cell (lifted by its sag) >= 3.20 V**; `sleep` needs SoC >= 15 % and cell >= 3.25 V;
`critical` needs SoC >= 8 % and cell >= 3.05 V. Without a known SoC (BMS data stale) a level is **held**, never released.
When the cut ends the load is switched on again only if the user last wanted it on.

The charger's own cut (its `BatUvp`, see below) and the BMS's discharge cutoff are expected to act at about 15 %.
They are not relied upon: a load that is on while the level is `load-cut` is switched off, with the log line
saying the charger and the BMS did not cut it.

## Why both SoC and cell voltage (data: InfluxDB `Status`, 2023-2026, resting samples)

Average cell voltage at rest per BMS SoC: 5-10 % 3.14 V, 10-15 % 3.215 V, 15-20 % 3.230 V, 20-25 % 3.257 V,
25-30 % 3.285 V, 40-50 % 3.295 V, 60-70 % 3.277 V.

* **Below about 12 %** the curve is steep (about 15 mV per point): the voltage tells the SoC to 1-2 points.
* **12-20 %** about 4 mV per point, scatter +/- 15 mV: +/- 4 points. Good enough for the cut level.
* **Above 20 %** it is flat (1-2 mV per point, plateau 3.26-3.30 V up to 90 %): the voltage cannot tell 25 % from
  40 %. The 30 % "back on" **cannot** be derived from the voltage.
* At the same reported SoC (20-30 %) the median voltage differed by 36 mV between two dark periods (3.235 V and
  3.271 V): counter drift, temperature and hysteresis together. In the flat region that is about 10 points.

So the cut has an independent witness (the weakest cell on the knee). The release uses the BMS counter: **the
resume level only has to be right to about +/- 5 points** (user decision 2026-10-23: back on at a real 25 % or 35 %
is precise enough), so the plateau does not matter for it, and no second witness is needed.

## Cell voltage under load: sag compensation

Under discharge the cells read lower than at rest, by I x R. Measured from 13,900 current steps (>= 1 A within 15 s)
in the island history, per cell, from the BMS pack voltage / 4:

| Cell temperature (battery box probe) | R per cell, p25 | median |
|---|---|---|
| >= 22 C | 1.04 mOhm | 1.42 mOhm |
| 15-22 C | 1.09 | 1.45 |
| 8-15 C | 1.28 | 1.63 |
| < 8 C | 1.56 | 2.01 |

At 35 A that is 40-70 mV, as much as the whole span between 15 % and 30 % on the flat curve, so the voltage
trigger cannot ignore the load. Rule: **effective cell voltage = lowest cell + discharge current x 1.0 mOhm**
(capped at +100 mV), compared with the cut, sleep and release levels. 1.0 mOhm is below the p25 of every temperature,
so the correction errs towards cutting early and releasing late, also in the cold (2.0 mOhm). While charging the
cell reads high, so a low value is really low: no correction. The critical level uses the **raw** voltage at any
current. The event text shows the correction: `lowest cell 3150 mV + 35 mV sag at 35.0 A < 3190 mV`.
(The current is the one the BMS measures: consumers wired past the BMS shunt are not in it, see
[wiring-check.md](wiring-check.md).)

## The BMS counter

In normal cycling the counter re-syncs when the pack is full: it reached "100 %" 1474 times in the history and read
98-99 % just before in 1465 of them. The risk is a long dark period without a full charge: the BMS does not see the
charger's and the ESP's current (about 1 W, 0.7 % of the capacity per day), so the counter reads too high by roughly
**10 points after 14 days and 20 after a month**.

Planned (not implemented): (1) subtract the unmetered idle draw (about 1 W, 0.03 points per hour) from the counter
for as long as no full charge has happened, which keeps it within the +/- 5 points after about two weeks and is safe
when it is a little too large (cut earlier, resume later); (2) when the knee is reached while the counter still
says > 15 %, remember the offset until the next full charge. An energy ledger from the charger's counters was
considered for the release and dropped: with the +/- 5 points tolerance it is not needed. Without the correction a
long dark period can make the counter 10-20 points too high; the cell voltage witness then cuts at the knee anyway
and the load may flap around the knee until the pack is charged properly.

## Observability: who did what and why

Every action is one event, in four places:

* syslog (NOTICE): `PROTECTION [software] load off: <why> (level load-cut, soc 13 %, lowest cell 3190 mV, pack 12.81 V)`
* MQTT `LiFePO_Island/<n>/status/Protection` (retained JSON: level, source, action, why, soc, cellMv, packV)
* InfluxDB measurement `Protection` (tag `Source`, fields `Action`, `Why`, `Level`, `Soc`, `CellMin`, `PackV`)
* `GET /api/live` `protection` (`level`, `loadInhibited`, `enforce`, `lastEvent`) and the page (Protection card)

Sources: `software` (this policy), `charger` (a load change this device did not command: its own protection, a
schedule or the switch on the charger; the line says if the charger reports battery low voltage), `web`, `mqtt`,
`button` (refused "load on" requests). Wake-ups: `PROTECTION woke from deep sleep after N min, level ...`.

## What is enforced (build flags)

| Flag | Island | Bench | Meaning |
|---|---|---|---|
| `PROT_ENFORCE_LOAD` | 1 | 1 | force the load off / on again |
| `PROT_ENFORCE_SLEEP` | 1 | 1 | deep sleep schedule |
| `PROT_ENFORCE_FET` | **0** | 1 | discharge FET off at critical: not before the wiring is verified (the FET may also cut the charger and the ESP) |

With a flag off the event is logged as `would ...`. The bench uses short timers (`[bench_reserve]` in
`platformio.ini`) so every level runs in minutes. State (level, debounce, time since the cut, user intent) is kept in
RTC memory across deep sleep and restarts; a power cycle starts at `normal` and finds the level again within a minute.

## Deep sleep

Awake `PROT_AWAKE_S` (180 s) after every wake-up, then the sleep is announced (event, MQTT, InfluxDB), taken 15 s
later, and the timer wakes the ESP again. Over-the-air updates only work in the awake window. The charger and the
BMS keep running; the charger's load switch keeps its state. After a wake-up the restored level holds for one
debounce period (a minute) even if the sun has charged the pack meanwhile: the load may be forced off once and comes
back at the release (seen on the bench).

## Hardware layers (to be aligned, not yet done)

* **Charger:** its real load cutoff is probably `BatUvp` (10.8 V as set), not `LoadUvp` (12.0 V), see the eSmart
  manual: "low voltage output protection point default 10.5 V, recovery 11 V". Test in daylight by raising
  `BatUvp` above the present voltage; then set about 12.8 V (cut) / 13.2 V (recovery). The recovery is voltage
  based and will come early in the morning (charging voltage); this software keeps the load off until 30 %.
* **BMS:** its cell under-voltage level is about 2.5 V (December 2024 data), far too low for a **larger inverter
  that hangs directly behind the BMS and is cut only by the BMS**. Read the real values with *Read from BMS* and
  raise cell UVP to about 2.9-3.0 V (release about 3.1 V) with the BMS app. Check the BMS rating (35 A = about
  450 W at 12.8 V) against the inverter.
* **Always-on consumers:** charger and ESP seem to be wired past the BMS discharge FET. They must stay powered so the
  sun can restart the charger; the damaging range is avoided by the levels above (long sleep at critical), not by
  the BMS.

## Open points

1. ESP sleep level: "a bit after the load cut, maybe at 18 %" cannot be above the 15 % cut when discharging.
   Implemented as 10 % and at least 30 minutes after the cut; adjust.
2. The idle-draw correction of the BMS counter: see "The BMS counter".
3. Whether `critical` should be able to isolate the pack completely (no automatic recovery): needs rewiring.
