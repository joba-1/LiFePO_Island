# Capacity test

The resistance of the cells is flat (see [cell-resistance.md](cell-resistance.md)), but that says little about the
capacity, and the BMS never learns it (its capacity is a fixed 272.00 Ah). This is how to measure it once or twice a
year with what is already on the island. **No known load is needed.**

## Why no known load

A capacity is a charge: the current integrated over time. The BMS measures the current, so **the BMS is the
instrument**, and any load or charge current will do. What matters is that

1. the two ends of the measurement are well defined (anchors),
2. the BMS saw all the current (the charger and the ESP are not seen, about 0.09 A: corrected),
3. the data is gap-free, and
4. the temperature is noted (capacity is 5-10 % lower at 5 degC).

## Anchors

* **Low anchor:** the average cell voltage at rest (|I| < 0.1 A, for at least 15 minutes) is <= 3.08 V. Below about
  12 % the voltage is steep (about 1 % per 15 mV), so this point is sharp. In winter the pack reaches it by itself.
* **Full anchor:** the BMS counter reads 100 %. The BMS sets it at the end of a full charge; use the same charger
  settings every time and the same anchor is reached every time.

## Variant A: charge side (preferred, no discharge)

1. Start from a low anchor: loads off (both inverters), the pack at rest, cells <= 3.08 V for 15 minutes. Check:
   `python3 tools/capacity_test.py` prints `Running charge test: low anchor ...`.
2. Charge to full: sun, or a LiFePO4 charger on the battery terminals (14.2-14.4 V, current within the BMS rating).
   Do not discharge meanwhile, and keep the island connected: gaps make the result unreliable.
3. Done when the BMS counter reads 100 %. `python3 tools/capacity_test.py` then prints the capacity.
   `--write` stores it in InfluxDB (`CapacityTest`), `--all` lists every earlier episode.
4. Note the result with date and temperature in the wiki (LiFePoIsland) and compare with the earlier ones.

Quick tests count: 1-3 days of charging keep the unseen idle draw small (0.09 A x 72 h = 6.5 Ah, uncertainty +/- 2 Ah).
A charge that takes weeks (winter sun) is worth much less: the idle correction alone is uncertain by 10-30 Ah.

## Variant B: discharge side (any load behind the BMS)

From full (counter 100 %) down to the low anchor, with whatever load you have: the BMS integrates it. Use the
inverter that hangs behind the BMS, **not** the charger's load output (the reserve policy cuts that at 13 %).
`python3 tools/capacity_test.py --mode discharge`. Stop at 3.05 V per cell: the BMS cuts at about 2.5 V and its
cell 4 falls off the cliff there. 10-25 A (130-320 W) takes 11-27 hours; stay below the BMS rating of 35 A.

## One calibration: the BMS current

Once, at 10-20 A, compare the BMS current (page, `/api/live`) with a DC clamp meter or with a load of known power
(a 12 V halogen lamp, a heater on the inverter with a power meter: P / battery voltage). A gain error of the BMS
scales the capacity directly (1 % error = 1 % capacity). If it is off, divide the result by the ratio.

## Error budget (a clean 1-3 day charge at 15-25 degC)

| Source | Size |
|---|---|
| BMS current gain (until calibrated) | +/- 1-2 % |
| Low anchor (3.08 V +/- 10 mV, steep) | +/- 1 % |
| Full anchor (BMS "100 %" at the same settings) | +/- 1 % |
| Unseen idle draw (0.09 A +/- 0.03 A) over 1-3 days | +/- 0.5 % |
| Temperature (record it, stay at 15-25 degC) | 0-5 % if ignored |

About **+/- 3-4 %**: a fade of 6 % or more would show. New cells are rated 272 Ah (assumed here; the real value of
this pack when new is unknown), end of life is conventionally 80 % = 218 Ah.

## What the history says (tool on the 2022-2026 data)

Only three episodes had a low anchor followed by a full charge, and none is clean:

| Episode | Days | Counted by the BMS | Idle correction | Capacity | Notes |
|---|---|---|---|---|---|
| 2023-01-30 to 02-15 | 16 | 270.1 Ah | 19.4 Ah | 250.7 Ah (92 %) | 4 degC, 5 data gaps, +/- 12 Ah |
| 2024-12-30 to 2025-02-05 | 37 | 275.4 Ah | 33.6 Ah | 241.8 Ah (89 %) | 4 degC, anchor 2.75 V (deeper), 3 gaps, +/- 27 Ah |
| 2025-10-11 to 10-18 | 7 | 263.9 Ah | 9.2 Ah | 254.7 Ah (94 %) | 13 degC, no gaps, anchor 3.03 V, +/- 5 Ah |

So the counted charge was 264-275 Ah each time, near the nominal 272 Ah; whether that means a capacity of 89 % or of
100 % depends on the unseen idle draw, which is an assumption. The only fairly clean one (October 2025, one week,
13 degC) says about 255 +/- 6 Ah, i.e. 94 % of the nominal, with an anchor a little deeper than the standard
3.08 V. It is a number to compare the next test with, not a trend.

## Right now (2026-10-23)

The pack is at a low anchor since 19:12 UTC (cells 2.947 V at rest, counter 1 %). If both inverters stay off and the
island stays online, the next sunny days are a charge-side test: run the tool when the counter reads 100 %. If the
sun needs more than about a week, put a LiFePO4 charger on it instead (the result would be worth little otherwise).

## Tool

`tools/capacity_test.py` (Python standard library only; tests: `python3 tools/test_capacity_test.py`, synthetic data
with known answers). It reads `Status` from InfluxDB (1-minute means), finds the anchors, integrates the BMS current
and reports gaps, duration and the idle correction. `--idle-a` changes the 0.09 A, `--anchor` the 3.08 V.
