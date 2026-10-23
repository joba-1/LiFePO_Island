# Cell internal resistance over the years

Question (2026-10-23): how does the cells' internal resistance develop, and what does it say about their life?

## Method

* Source: InfluxDB `LiFePO_Island`, measurement `Status`, 2022-10 to 2026-10. Every pair of consecutive samples (<= 15 s apart) where the BMS current changes by >= 1 A (inverter or load switching) gives one value: R per cell = dV / dI, with dV the change of the BMS pack voltage / 4. 62,325 steps in 18,543 hours with load; 26,350 of them at a BMS SoC of 30-95 % (resistance rises towards empty, so the comparison uses one band).
* Temperature: battery box probe (`temperature2`). The steps give -1.9 % per degC (a fit of ln R over all steps). All values are normalised to 20 degC with it, so seasons do not look like ageing. The correction is linear; in winter the real cold increase is larger, hence the slightly higher winter months.
* This is the ohmic step response seen by the BMS: cell + busbars + its sense wiring, not the cell alone. A change over the years still means a change of the cells (or a loose connection).

## Per year (median R at 20 degC, mOhm per cell)

| Year | steps | median |
|---|---|---|
| 2022 | 323 | 1.57 |
| 2023 | 1616 | 1.36 |
| 2024 | 6311 | 1.36 |
| 2025 | 8312 | 1.34 |
| 2026 | 9788 | 1.38 |

## Per month (months with >= 25 steps)

| Month | steps | median | p25 | p75 |
|---|---|---|---|---|
| 2022-11 | 266 | 1.61 | 1.25 | 2.01 |
| 2022-12 | 57 | 1.39 | 1.28 | 2.23 |
| 2023-01 | 42 | 1.45 | 1.32 | 1.63 |
| 2023-03 | 37 | 1.53 | 1.29 | 1.70 |
| 2023-04 | 31 | 1.76 | 1.53 | 1.86 |
| 2023-05 | 27 | 1.32 | 1.16 | 1.70 |
| 2023-07 | 348 | 1.25 | 0.97 | 1.69 |
| 2023-08 | 838 | 1.31 | 1.03 | 1.77 |
| 2023-09 | 47 | 1.28 | 1.12 | 1.46 |
| 2023-10 | 105 | 1.35 | 1.09 | 1.87 |
| 2023-12 | 90 | 1.44 | 1.37 | 1.50 |
| 2024-01 | 64 | 1.47 | 1.36 | 2.44 |
| 2024-04 | 25 | 1.13 | 1.03 | 1.25 |
| 2024-05 | 36 | 1.32 | 1.13 | 2.10 |
| 2024-06 | 683 | 1.31 | 1.02 | 1.78 |
| 2024-07 | 789 | 1.32 | 1.04 | 1.80 |
| 2024-08 | 1601 | 1.40 | 1.06 | 1.94 |
| 2024-09 | 2550 | 1.34 | 1.03 | 1.78 |
| 2024-10 | 310 | 1.43 | 1.10 | 1.99 |
| 2024-12 | 212 | 1.50 | 1.23 | 1.76 |
| 2025-01 | 285 | 1.60 | 1.24 | 2.00 |
| 2025-04 | 77 | 1.42 | 1.04 | 1.73 |
| 2025-05 | 805 | 1.28 | 1.00 | 1.74 |
| 2025-06 | 1048 | 1.37 | 1.04 | 1.88 |
| 2025-07 | 1385 | 1.36 | 1.04 | 1.84 |
| 2025-08 | 579 | 1.40 | 1.05 | 1.78 |
| 2025-09 | 2076 | 1.28 | 1.02 | 1.71 |
| 2025-10 | 1706 | 1.32 | 1.05 | 1.71 |
| 2025-11 | 247 | 1.39 | 1.14 | 1.79 |
| 2025-12 | 70 | 1.64 | 1.25 | 2.02 |
| 2026-01 | 121 | 1.56 | 1.23 | 1.77 |
| 2026-03 | 807 | 1.40 | 1.12 | 1.74 |
| 2026-04 | 948 | 1.38 | 1.08 | 1.76 |
| 2026-05 | 1213 | 1.35 | 1.04 | 1.75 |
| 2026-06 | 1528 | 1.33 | 1.00 | 1.81 |
| 2026-07 | 914 | 1.40 | 1.06 | 1.91 |
| 2026-08 | 1226 | 1.33 | 1.04 | 1.75 |
| 2026-09 | 2163 | 1.46 | 1.11 | 1.84 |
| 2026-10 | 844 | 1.39 | 1.10 | 1.78 |

## Result

* **No ageing is visible in the resistance.** The robust trend (Theil-Sen over the monthly medians) is **+0.001 mOhm per year**, 90 % interval -0.028 ... +0.022 mOhm per year (bootstrap), on a level of about 1.4 mOhm per cell (5.6 mOhm for the pack). The measurement would show a rise of more than about 2 % per year; there is none.
* The first months (2022-11: 1.61) are higher, like a new pack settling; after that the level is flat at 1.3-1.4 mOhm.
* The December 2024 deep discharge (cells down to 1.2-1.4 V) left no lasting step: 2024-09 1.34, 2024-10 1.43, 2025-01 1.60 (winter), 2025-05 1.28.
* Winter months (Dec-Jan 1.4-1.64) are higher than summer (1.3-1.4) even after the temperature correction: that is the cold, not ageing (the same months of different years agree).
* **Limits:** the pessimistic bound would still take about 32 years to +50 % and 64 years to +100 % of the resistance (a common end-of-life criterion is +100 %). So the resistance is not what will end this pack's life.

## What this cannot tell

* **Capacity.** The BMS reports a fixed 272.00 Ah (it never learns), and the counter drifts by tens of Ah over the 50-80 day dark periods, so the history gives no capacity trend. The resistance of LiFePO4 cells often stays flat while they lose capacity.
* The BMS has counted **125 cycles in four years** (about 31 per year): this pack ages by calendar and by stress events (deep discharges, long storage at high SoC), not by cycling. Typical manufacturer figures (several thousand cycles to 70-80 %, calendar ageing about 1-2 % per year at moderate temperature) point to well over ten years, but these are the data sheet and not this pack: a measurement of the capacity is what is missing.
* To get it: a controlled capacity test (full charge to absorption, then a known load down to about 2.9 V per cell at 15-25 degC, the BMS counting Ah; the unseen 0.09 A of charger and ESP adds about 1 % over 30 hours), once or twice a year.

