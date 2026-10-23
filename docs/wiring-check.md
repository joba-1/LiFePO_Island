# Wiring check: what happens at the BMS cutoff

*Why:* in the December 2024 incident the BMS cut the discharge FET at a cell below ~2.49 V, but the pack
kept draining (about 0.08 A, shown as 0.00 A by the BMS) down to 1.2-1.4 V per cell. Something that is meant
to be "behind" the BMS is connected past its discharge switch. And since 2026-10-23 a larger inverter hangs
directly behind the BMS, which is then the only thing that can switch it off. This checklist finds out how
everything is really wired. Nothing here changes the wiring.

## Safety

* A 272 Ah LiFePO4 pack delivers well over 1 kA into a short circuit. Insulated tools, no rings or watches,
  never bridge terminals, fuses stay in. Measure; do not "try" connections.
* Work with the **inverters off** and the sun covered or the PV disconnected for the resistance checks.
* Do not run the checks at 1 % state of charge. Charge first (see the bottom).

## 0. Photos (so I can follow the wiring)

1. Overview of the whole assembly, then each device alone: BMS (all terminals and the labels), charger (all
   terminals), old inverter, **new inverter**, fuses/busbars, the ESP box.
2. BMS close-up: the markings `B-`, `P-`, `B+`, the balance lead plug, the UART/RJ45 port, the Bluetooth
   module, the rating sticker (35 A).
3. Both ends of every cable that touches `B-` or `P-` (which terminal, which lug).
4. The RJ45 communication cable(s) and where each pin goes (RS485 A/B, GND, 5 V), and what powers the ESP.
5. Any earth/PE wire and any metal enclosure or chassis that touches a DC negative.

## 1. Write down, for every device, where its two wires go

| Device | Positive wire to | Negative wire to (`B-` or `P-`?) | Fuse |
|---|---|---|---|
| Charger BAT terminals | | | |
| Charger LOAD terminals (old inverter) | | | |
| **New inverter** | | | |
| ESP supply (5 V from where?) | | | |
| RS485 ground / RJ45 GND to BMS, charger, ESP | | | |

Expected for a protected system: everything that must be switched off by the BMS has its **negative on `P-`**;
only the cells and the BMS itself are on `B-`. The charger is a special case: it has to be on `P-` to be
cut, but then it can no longer wake the pack from a BMS cutoff by itself (see "Decisions" in the spec).

## 2. De-energized checks (battery fuse out or `B+` lead removed, PV off, inverters off)

Use the continuity beeper or the resistance range.

1. **Charger BAT- to `B-`**: beeps or ~0 Ω means the charger sits directly on the cells.
2. **Charger BAT- to `P-`**: beeps means it is behind the BMS (the wanted case).
3. If 1 beeps: remove the RS485/RJ45 cable and measure again. If it no longer beeps, the **only** link is the
   communication ground, which bridges the BMS switch.
4. **ESP GND to `B-`** and **to `P-`**, same procedure (USB, RJ45 GND, the MAX485 board, any shield).
5. **New inverter negative to `B-`** and to `P-`; **its chassis/PE to every DC negative** and to the PV frame.
6. **`B-` to `P-`**: with the BMS dead the FETs are open (diode drop in one direction, no beep). A beep means a
   bridge around the BMS somewhere in the wiring.

Write down every beep: it is a path that bypasses the discharge FET.

## 3. Energized checks (everything running, loads off)

1. `B+` to `B-`: pack voltage (compare with the page: 11.8 V and so on).
2. `B-` to `P-`: close to 0 V (FETs on). A few tens of mV under load is normal.
3. Charger BAT- to `B-` and to `P-` (both should be ~0 V when the FETs are on, so this cannot tell on its own;
   the section 2 checks do).
4. **Current of charger + ESP**: in the evening with PV dark, put the multimeter (mA range, fused) in series in
   the charger's positive lead for a moment, or use a DC clamp that resolves 10 mA on the cable to the pack. The
   BMS shows **0.00 A** for the same current: if the clamp on the `B-` cable shows ~80 mA while the BMS shows 0,
   the shunt of the BMS does not see the consumers.

## 4. Switch test (only supervised, pack above 50 %, daylight)

Do not do this at low charge and not unattended: if charger and ESP are behind the discharge FET, the ESP dies
and cannot switch the FET back on, and the BMS lock may survive until you clear it with the BMS phone app.

1. Have the BMS app (Bluetooth) or a UART tool ready to clear the MOS lock.
2. Page: *Set MOSFETs*, charge on, **discharge off**. If the ESP stays up and the charger stays on, they bypass
   the BMS; if the ESP goes dark, they are behind it.
3. Restore (page or app) and confirm both FETs on in `/api/live`.

## 5. The new inverter

* **Rating:** the BMS `JBD-SP04S010A-L4S-35A-B` is a **35 A** BMS: about 450 W continuous at 12.8 V. A larger
  inverter at 12 V draws far more (1000 W is about 85 A; surge at start-up even more). Write down the
  inverter's continuous and surge rating, its DC cable cross-section and its fuse. The BMS discharge
  over-current protection (read it with *Read from BMS*) will trip, and may damage the FETs on repeated trips.
* **Fuse** close to `B+`, rated for the cable and below the BMS limit.
* **Idle draw:** note it (inverters often draw 20-50 W with no load). It is the main consumer when it is on.
* Because only the BMS switches it off, the BMS under-voltage cutoff must be raised (cell level around
  2.9-3.0 V instead of ~2.5 V; read the present values first), see the spec.

## 6. Before any test: charge the pack

The pack is at ~1 % (cells 2.93-2.97 V). Charge it from the sun or from a LiFePO4 charger (14.2-14.4 V,
current limited to what the BMS allows) to at least 50 % before the section 4 test and before running the
bigger inverter.
