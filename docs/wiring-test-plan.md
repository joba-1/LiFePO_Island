# Wiring and protection test plan (draft)

Basis: photos of 2026-10-24 (`~/Nextcloud/Photos/2026/10/IMG_7553-7560`), the draft diagram
[wiring-draft.svg](wiring-draft.svg), and [wiring-check.md](wiring-check.md) (de-energized checks).
The diagram is a draft: what the photos do not show is dashed.

## What the photos show (and what the owner confirmed on site, 2026-10-28)

Photos: 7553 DC switch, 7554 switch + tubes + charger, 7555 charger + old inverter + adapters, 7556 overview,
7557 thin cable with 4-pin plug, 7558 ESP box, 7559 pack, 7560 BMS.

* **The "Photovoltaic DC switch" is the battery switch of the charger**, not a PV switch (it carries a LiFePO4 SoC
  sticker). The pack leads come up to **MC4 Y-splitters above the switch**. One branch goes through the switch and
  on as brown / blue (one of the `BAT` / `PV` tubes) to the charger's BAT terminals. (7553, 7554, owner)
* **The new, larger inverter is tapped from the other branch, before the switch,** with red / black 6 mm² solar
  cable (7554 left, 7555, owner). It is **not** behind the charger's load switch, so neither the charger nor this
  firmware switches it, and the DC switch does not disconnect it. Only the BMS can, if that negative is behind the
  BMS (P-). The BMS read 0.00 A all night 27/28 Oct: the new inverter was off then (its idle draw would show as ~1 A).
* **The charger's LOAD output (bottom right) feeds only the old 600 W inverter**, through blue / brown (owner). That is
  the load this firmware switches; switching the old inverter off at its own switch on 2026-10-27 brought the
  charger's load power to 0 W.
* PV: the panel's black solar cables go straight to the charger PV terminals (through the `PV` tube). The charger
  display reads PV 27.4 V, 12 V system, MPPT (7554).
* Four Basen Green cells in series with yellow busbars, in a cardboard box. **Brown** leaves cell 1 directly
  (pack +) with a ring lug, **no fuse visible** (7559). **Blue** leaves cell 4 into the BMS, so the BMS sits in the
  negative line.
* The BMS is the `JBD-SP04S010A` 35 A type; plugs labelled RS485, NTC, UART (7560). The side with the thick-wire
  terminals (B-, P-, C-) is hidden on every photo.
* **The island's ESP is an ESP32-CAM** on a perfboard (SD slot and camera connector visible, 7558), with an RS485
  module (red LED), a blue screw terminal and a small module with a USB-C socket (probably the 5 V supply), fed by
  a 3-wire cable (red / black / white). It runs the `mhetesp32minikit_ota` build: the RS485 pins (14/13) and the load
  LED (GPIO 33 = the ESP32-CAM's red LED) are not board specific; the health LED (GPIO 2) has nothing attached.
  The MH-ET MiniKit is the test board.
* **The ESP is not fed from the LOAD output:** it kept running when the idle switch-off turned the load off
  (2026-10-27 15:30). Where it is fed from is still open.
* A 4-pin plug in the thin cable (7557) is probably the charger's RS485 to the ESP box.
* Mains-coloured cable (brown/blue) is used for DC. Label it, so nobody takes it for AC.
* Near the ESP box hang two mains adapters (Biogenta 12 V / 1 A and an Einhell charger, 7555). If one of them powers
  the ESP, that explains why the BMS does not see its current; but on mains from the old inverter it would have
  died with the load at 15:30.

## Open questions (they decide the checks below)

1. Where does the pack's blue negative end: is the new inverter's black and the switch branch behind the BMS (P-)
   or directly on cell 4 (B-)? Only P- lets the BMS cut the new inverter.
2. Where does the thin BMS cable (RS485/UART) go, and where is the ESP's GND tied?
3. Which tube carries which leg (pack to splitters, or switch to charger), and are the tubes only cable protection
   or do they hide fuse holders?
4. Is there any fuse in the pack leads? (None visible near cell 1 or at the splitters.)
5. Where does the ESP's 12 V come from: the battery side before the switch, the charger, or a mains adapter?

## Missing photos

1. BMS from the side where the thick wires are soldered or screwed: `B-`, `P-`, `C-` markings, wire colours.
2. The MC4 Y-splitters above the switch with the cables leaving them, and where the pack leads arrive there.
3. Charger terminal block with cables pulled to the side, so each cable can be followed to its hole.
4. The new inverter and its DC terminals (+ and -) with the red / black cable.
5. Underside of the ESP box: where the 3-wire cable into the USB-C module and the screw terminal come from.

## Test equipment

* Adjustable supply 0-60 V / 0-5 A: current limit set first, polarity checked, fuse 6 A in series, and never together
  with the real pack or the panels on the same terminal.
* Multimeter, a lamp or resistor load up to 60 W, optional clamp meter, BMS phone app (to clear MOS lock).

## Test 1: de-energized (no PSU)

Section 2 of [wiring-check.md](wiring-check.md), with the pack fuse or `B+` lead removed. Every beep between a
device and `B-` is a path around the discharge FET.

## Test 2: charger thresholds with the PSU as battery (pack and PV disconnected)

Purpose: which charger setting cuts the load (`BatUvp` 10.8 V or `LoadUvp` 12.0 V), at which voltage it comes back,
and what the charger does at low voltage. Safe because the PSU can only deliver 5 A and is limited.

1. Open the DC switch (it separates the charger from the pack; the new inverter stays on the pack) and unplug the
   panel's MC4 plugs. Connect PSU to charger BAT+/BAT- (current limit 3 A, 13.0 V).
2. Lamp (about 25 W) on LOAD+/LOAD-. Charger up, load on, read the settings in the page (`ProParam`).
3. Lower the PSU voltage in steps of 0.1 V every 2 minutes from 13.0 V. Note the voltage where the load switches
   off and any fault bits. Raise again and note where it switches on.
4. Repeat with the load switched manually off, to see the charger's own cutoff of the supply.

Expected result: load off at `LoadUvp` or `BatUvp`. Abort criteria: smoke, smell, PSU current limit reached for
more than 10 s.

## Test 3: PSU as PV source (pack connected, PV disconnected, charge test)

Purpose: controlled charging for the capacity test, and a look at how the MPPT behaves with a lab supply.

1. Pack at rest, state of charge known. Unplug the panel at its MC4 plugs (the DC switch is the battery switch, not
   a PV switch) and connect the PSU to the charger's PV cable (current limit 1 A, 30 V first).
   Low-light check first (the charger sticks at 17-19 V in "wait" below ~20 W, docs: Trac LiFePoIsland): with
   30-35 V and limits of 0.1, 0.2, 0.5, 1 A, note the PV voltage the charger pulls the supply to and the mode
   (`ChgSts.ChgMode`). A good MPPT stays near 30 V.
2. Raise the limit to 4-5 A and 36 V in steps while watching the charger (charge current, `ChgSts`).
3. A lab supply can make the MPPT hunt: if the current oscillates, set a constant voltage instead (about 25 V).
4. Let it run supervised, then restore the panels.

## Test 4: BMS cutoff and ESP behaviour (daylight, pack above 50 %, supervised)

Section 4 of [wiring-check.md](wiring-check.md): discharge FET off through the page, watch which devices keep
running. Do not do this at low state of charge. Switch the new inverter on beforehand (no load on it): if it goes
off with the FET, its negative is behind the BMS (answers open question 1).

## Test 5: reserve policy on the bench

The ESP does the reserve policy in software, so the bench (MH-ET/XIAO with the simulator) covers it: see
[test.md](test.md). The PSU cannot simulate cell voltages for the BMS.
