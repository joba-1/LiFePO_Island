/*
Simulated eSmart3/4 charger and JBD BMS for test builds (-DSIMULATE).

Same methods as the Joba_ESmart3 / Joba_JbdBms objects that main.cpp uses, so
polling, publishing, load and MOSFET switching run the real code paths on a
board without RS485 hardware (the Atom Echo test bench, env atom_echo).

A simulated day lasts SIM_DAY_MS (20 min): PV follows a sine during the
"day", the battery integrates charge minus load, cell voltages follow the
state of charge. simControl lets /api/sim take a device offline or set fault
bits, to test how the UI shows stale data and faults.
*/
#pragma once

#include <Arduino.h>
#include <math.h>
#include <esmart3.h>
#include <jbdbms.h>

struct SimControl {
    bool chargerOnline = true;
    bool bmsOnline = true;
    uint16_t chargerFaults = 0;    // or-ed into ChgSts.wFault
    uint16_t bmsFaults = 0;        // or-ed into Status.fault
    bool noSleep = false;          // /api/sim?nosleep=1: no deep sleep (scripted tests keep their connection)
    bool socHold = false;          // /api/sim?soc=N: the state of charge stays at socValue (0..1)
    float socValue = 0.8f;
    int16_t loadW = -1;            // /api/sim?loadw=N: fixed load power while the load is on (-1: the default 22 W wave)
};
static SimControl simControl;

class SimWorld {
public:
    static constexpr uint32_t SIM_DAY_MS = 20UL * 60 * 1000;
    static constexpr float CAPACITY_AH = 272.0f;

    bool load = true;
    uint8_t mosfets = JbdBms::MOSFET_BOTH;

    // advance the model to now; cheap, called by every getter
    void step() {
        uint32_t now = millis();
        float dt_h = (now - _last) / 3600000.0f * 72.0f;   // 1 sim day = 20 min: x72
        _last = now;
        float phase = (now % SIM_DAY_MS) / (float)SIM_DAY_MS;      // 0 = midnight
        float sun = sinf((phase - 0.25f) * 2 * (float)M_PI);        // > 0 from 6:00 to 18:00
        sun = sun > 0 ? sun : 0;
        pvV = sun > 0.02f ? 30.0f + 8.0f * sun : 2.0f * sun;
        bool canCharge = (mosfets & JbdBms::MOSFET_CHARGE) && soc < 0.995f;
        chgW = canCharge ? 320.0f * sun : 0;
        bool canLoad = load && (mosfets & JbdBms::MOSFET_DISCHARGE) && soc > 0.10f;
        loadW = !canLoad ? 0 : simControl.loadW >= 0 ? simControl.loadW : 22.0f + 3.0f * sinf(now / 7000.0f);
        batV = 12.8f + 0.8f * soc + (chgW - loadW) / 1000.0f;
        float amps = (chgW - loadW) / batV;
        soc += amps * dt_h / CAPACITY_AH;
        soc = soc < 0 ? 0 : (soc > 1 ? 1 : soc);
        if (simControl.socHold) soc = simControl.socValue;
        amps_ = amps;
        tempC = 14.0f + 6.0f * sun;
    }

    float pvV = 0, batV = 13.3f, chgW = 0, loadW = 0, amps_ = 0, tempC = 14;
    float soc = 0.80f;

private:
    uint32_t _last = 0;
};
static SimWorld simWorld;

namespace sim_detail {
inline void words(uint16_t* dst, size_t nwords, const char* s) {
    char* b = (char*)dst;
    for (size_t i = 0; i < nwords * 2; i++) b[i] = *s ? *s++ : ' ';
}
inline uint16_t rnd(uint16_t spread) { return (uint16_t)(esp_random() % (spread + 1)); }
}  // namespace sim_detail

class SimESmart3 {
public:
    void begin(int) {}

    bool getInformation(ESmart3::Information_t& d) {
        if (!simControl.chargerOnline) return fail();
        memset(&d, 0, sizeof(d));
        sim_detail::words(d.wSerial, 4, "SIM00009");
        sim_detail::words(d.wDate, 4, "20261013");
        sim_detail::words(d.wFirmWare, 2, "0042");
        sim_detail::words(d.wModel, 8, "eSmart-SIM-60A");
        return ok();
    }

    bool getChgSts(ESmart3::ChgSts_t& d) {
        if (!simControl.chargerOnline) return fail();
        simWorld.step();
        memset(&d, 0, sizeof(d));
        d.wChgMode = simWorld.chgW > 1 ? ESmart3::CHG_MPPT : ESmart3::CHG_WAIT;
        d.wPvVolt = (uint16_t)(simWorld.pvV * 10);
        d.wBatVolt = (uint16_t)(simWorld.batV * 10);
        d.wChgCurr = (uint16_t)(simWorld.chgW / simWorld.batV * 10);
        d.wOutVolt = d.wBatVolt;
        d.wLoadVolt = simWorld.load ? d.wBatVolt : 0;
        d.wLoadCurr = (uint16_t)(simWorld.loadW / simWorld.batV * 10);
        d.wChgPower = (uint16_t)simWorld.chgW;
        d.wLoadPower = (uint16_t)simWorld.loadW;
        d.wBatTemp = (int16_t)simWorld.tempC;
        d.wInnerTemp = (int16_t)(simWorld.tempC + simWorld.chgW / 40);
        d.wBatCap = (uint16_t)(simWorld.soc * 100 + 0.5f);
        d.dwCO2 = 271398;
        d.wFault = (simWorld.pvV < 12 ? 0x040 : 0) | simControl.chargerFaults;
        return ok();
    }

    bool getBatParam(ESmart3::BatParam_t& d) {
        if (!simControl.chargerOnline) return fail();
        d = _bat;
        return ok();
    }
    bool getLog(ESmart3::Log_t& d) {
        if (!simControl.chargerOnline) return fail();
        memset(&d, 0, sizeof(d));
        d.dwRunTime = millis() / 1000;
        d.wStartCnt = 42;
        d.dwTotalEng = 123456;
        return ok();
    }
    bool getParameters(ESmart3::Parameters_t& d) {
        if (!simControl.chargerOnline) return fail();
        memset(&d, 0, sizeof(d));
        d.wPvVoltRatio = 5238;
        d.wBatVoltRatio = 1638;
        return ok();
    }
    bool getLoadParam(ESmart3::LoadParam_t& d) {
        if (!simControl.chargerOnline) return fail();
        memset(&d, 0, sizeof(d));
        d.wLoadSts = simWorld.load;
        return ok();
    }
    bool getProParam(ESmart3::ProParam_t& d) {
        if (!simControl.chargerOnline) return fail();
        d = _pro;
        return ok();
    }
    bool getLoad(bool& on) {
        if (!simControl.chargerOnline) return fail();
        on = simWorld.load;
        return ok();
    }

    bool setLoad(bool on) {
        if (!simControl.chargerOnline) return fail();
        simWorld.load = on;
        return ok();
    }
    bool setTime(struct tm&) { return simControl.chargerOnline; }
    bool setBatParam(ESmart3::BatParam_t& d) {
        if (!simControl.chargerOnline) return fail();
        memcpy((uint16_t*)&_bat + 1, (uint16_t*)&d + 1, (sizeof(d) / 2 - 3) * 2);   // words [1, end-2)
        writes++;
        return ok();
    }
    bool setProParam(ESmart3::ProParam_t& d) {
        if (!simControl.chargerOnline) return fail();
        memcpy((uint16_t*)&_pro + 1, (uint16_t*)&d + 1, (sizeof(d) / 2 - 2) * 2);   // words [1, end-1)
        writes++;
        return ok();
    }

    uint32_t writes = 0;   // parameter writes since boot (setup_LiFePO test)

private:
    bool ok() { delay(15); return true; }      // roughly the bus time of a real request
    bool fail() { delay(200); return false; }  // roughly the library's read timeout
    ESmart3::BatParam_t _bat = {};
    ESmart3::ProParam_t _pro = {};
};

class SimJbdBms {
public:
    void begin(int) {}

    bool getHardware(JbdBms::Hardware_t& d) {
        if (!simControl.bmsOnline) return fail();
        memset(&d, 0, sizeof(d));
        strncpy(d.id, "JBD-SIM-4S-272AH", sizeof(d.id) - 1);
        return ok();
    }

    bool getStatus(JbdBms::Status_t& d) {
        if (!simControl.bmsOnline) return fail();
        simWorld.step();
        memset(&d, 0, sizeof(d));
        d.voltage = (uint16_t)(simWorld.batV * 100);
        d.current = (int16_t)(simWorld.amps_ * 100);
        d.nominalCapacity = (uint16_t)(SimWorld::CAPACITY_AH * 100);
        d.remainingCapacity = (uint16_t)(simWorld.soc * SimWorld::CAPACITY_AH * 100);
        d.cycles = 123;
        d.productionDate = ((2022 - 2000) << 9) | (7 << 5) | 30;
        d.balanceLow = simWorld.soc > 0.97f ? 0x1 : 0;
        d.fault = simControl.bmsFaults;
        d.version = 34;
        d.currentCapacity = (uint8_t)(simWorld.soc * 100 + 0.5f);
        d.mosfetStatus = simWorld.mosfets;
        d.cells = 4;
        d.ntcs = 2;
        for (int i = 0; i < 2; i++) {
            uint16_t k = (uint16_t)((simWorld.tempC + i) * 10 + 2731);
            d.temperatures[i].hi = k >> 8;
            d.temperatures[i].lo = k & 0xff;
        }
        return ok();
    }

    bool getCells(JbdBms::Cells_t& d) {
        if (!simControl.bmsOnline) return fail();
        memset(&d, 0, sizeof(d));
        uint16_t base = (uint16_t)(simWorld.batV * 250);    // mV per cell of 4
        static const int16_t offset[4] = { 4, 0, 1, -1 };
        for (int i = 0; i < 4; i++) d.voltages[i] = base + offset[i] + sim_detail::rnd(1);
        return ok();
    }

    bool setMosfetStatus(JbdBms::mosfet_t status) {
        if (!simControl.bmsOnline) return fail();
        simWorld.mosfets = status;
        return ok();
    }

    // Settings as a typical LiFePO4 4S JBD BMS ships them; takes about as long as the real read
    bool getSettings(JbdBms::Settings_t& d) {
        if (!simControl.bmsOnline) return fail();
        delay(2500);
        memset(&d, 0, sizeof(d));
        d.designCapacity = 27200; d.cycleCapacity = 27200;
        d.cellVoltage100 = 3650; d.cellVoltage80 = 3350; d.cellVoltage60 = 3320; d.cellVoltage40 = 3290;
        d.cellVoltage20 = 3250; d.cellVoltage0 = 2800; d.selfDischarge = 10;
        d.packOvp = 1480; d.packOvpRelease = 1440; d.packUvp = 900; d.packUvpRelease = 1000;
        d.cellOvp = 3700; d.cellOvpRelease = 3500; d.cellUvp = 2500; d.cellUvpRelease = 2700;
        d.cellOvpHigh = 3900; d.cellUvpHigh = 2300;
        d.cellUvpDelay = 2; d.cellOvpDelay = 2; d.packUvpDelay = 2; d.packOvpDelay = 2;
        d.highDelays = 0x80 | 0x10;
        d.chargeOcp = 3500; d.dischargeOcp = -3500; d.chargeOcpDelay = 2; d.dischargeOcpDelay = 2;
        d.chargeOvertemp = 3331; d.chargeOvertempRelease = 3281; d.chargeUndertemp = 2731; d.chargeUndertempRelease = 2781;
        d.dischargeOvertemp = 3331; d.dischargeOvertempRelease = 3281; d.dischargeUndertemp = 2531; d.dischargeUndertempRelease = 2581;
        d.balanceStart = 3400; d.balanceWindow = 20; d.shuntResistance = 10; d.functionConfig = 0x0c;
        d.ntcConfig = 3; d.cells = 4;
        return true;
    }
    bool inFactory() const { return false; }
    bool exitFactory() { return ok(); }

private:
    bool ok() { delay(20); return true; }
    bool fail() { delay(200); return false; }
};
