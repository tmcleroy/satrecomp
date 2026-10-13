// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// Native BIOS (see hle_bios.h). The behaviour follows what the BIOS routines do, as observed on a real BIOS:
// same tables in low work RAM, same stack frames, same register and hardware effects.
#ifdef SATRECOMP_OWN_MACHINE
#include "../machine/machine.h"
#include "backup.h"
#else
#define SATRECOMP_OPS_ONLY
#include <ymir/hw/sh2/sh2.cpp> // (for the CPU's memory accessors)
#endif

#include "access.h"
#include "hle_bios.h"
#include "recomp.h"
#include "state.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

namespace satrecomp {

// Where the machine keeps high work RAM and its save memory, and what is on its bus
#ifdef SATRECOMP_OWN_MACHINE
namespace bupns = bup;
namespace {
    bup::BackupMemory g_backup;
    uint8_t *RamHigh(Machine &m) {
        return m.ramHigh;
    }
    constexpr size_t kRamHighSize = Machine::kRamSize;
    bup::BackupMemory &Backup(Machine &) {
        return g_backup;
    }
    uint16_t Peek16(Machine &m, uint32_t address) {
        return m.BusRead16(address);
    }
} // namespace
namespace hle {
    void OpenBackup(const std::string &path) {
        g_backup.Open(path);
    }
} // namespace hle
#else
namespace bupns = ymir::bup;
namespace {
    uint8_t *RamHigh(Machine &m) {
        return m.mem.WRAMHigh.data();
    }
    constexpr size_t kRamHighSize = ymir::sys::kWRAMHighSize;
    auto &Backup(Machine &m) {
        return m.mem.GetInternalBackupRAM();
    }
    uint16_t Peek16(Machine &m, uint32_t address) {
        return m.mainBus.Peek<uint16_t>(address);
    }
} // namespace
#endif

uint8_t Access::Read8(SH2 &c, uint32_t address) {
    return c.MemReadByte<false>(address);
}
uint16_t Access::Read16(SH2 &c, uint32_t address) {
    return c.MemReadWord<false>(address);
}
uint32_t Access::Read32(SH2 &c, uint32_t address) {
    return c.MemReadLong<false>(address);
}
void Access::Write8(SH2 &c, uint32_t address, uint8_t value) {
    c.MemWriteByte<false, false>(address, value);
}
void Access::Write16(SH2 &c, uint32_t address, uint16_t value) {
    c.MemWriteWord<false, false>(address, value);
}
void Access::Write32(SH2 &c, uint32_t address, uint32_t value) {
    c.MemWriteLong<false, false>(address, value);
}

namespace hle {

namespace {

    // --- addresses -------------------------------------------------------------------------------------

    // Native routines, by their address in the ROM area
    enum Routine : uint32_t {
        kReset = 0x1000,     // power on and reset, both CPUs
        kReturnFromInt,      // default interrupt handler: return
        kHalt,               // CPU exceptions (illegal instruction, address error)
        kNMI,
        kReturn,             // default for unset handlers: a function that does nothing
        kIdle,               // default slave program: wait
        kIntReturn,          // where an SCU interrupt handler returns to
        kClockWait,          // the master waits here while the clock changes
        kBootWait,           // the master waits here for the CD drive before the disc is started
        kSetSInt,            // SYS_SETSINT
        kGetSInt,
        kSetUInt,
        kGetUInt,
        kChgSysCk,
        kChgUIPr,
        kTasSem,
        kClrSem,
        kSetScuIm,
        kChgScuIm,
        kScuAck,
        kBupInit,
        kBupFirst,                  // the ten backup library functions
        kBupLast = kBupFirst + 9,
        kIntFirst,                  // SCU interrupts, vectors 0x40-0x5F
        kIntLast = kIntFirst + 31,
        kRoutineEnd,
        kUnimplemented = 0x2000,    // + the service's table offset: BIOS services not written yet
        kUnimplementedEnd = 0x2400,
    };

    // ROM addresses are spaced so that none is an odd address or looks like a delay slot of another
    constexpr uint32_t Address(uint32_t routine) {
        return routine < kUnimplemented ? 0x1000 + (routine - 0x1000) * 4 : routine;
    }

    // The BIOS's variables and tables in low work RAM
    constexpr uint32_t kRam = 0x06000000;
    constexpr uint32_t kSlaveVectors = kRam + 0x400;
    constexpr uint32_t kSlaveEntry = kRam + 0x250;    // the slave's program
    constexpr uint32_t kSlaveStack = kRam + 0x2AC;    // its stack, if not the default
    constexpr uint32_t kClockMode = kRam + 0x324;
    constexpr uint32_t kScuMask = kRam + 0x348;       // the SCU interrupt mask as last set through the BIOS
    constexpr uint32_t kBupVector = kRam + 0x354;
    constexpr uint32_t kHandlers = kRam + 0x900;      // + vector * 4: SCU interrupt handlers (0xA00 on)
    constexpr uint32_t kPriorities = kRam + 0x980;    // + vector * 4: SR and SCU mask while handling it
    constexpr uint32_t kSemaphores = kRam + 0xB00;

    constexpr uint32_t kScu = 0x25FE0000;
    constexpr uint32_t kSmpcComreg = 0x2010001F;
    constexpr uint32_t kSmpcSF = 0x20100063;

    struct State {
        Machine *saturn = nullptr;
        BootImage image;
        // Clock change in progress (SYS_CHGSYSCK): what to go back to once the SMPC has done it
        bool clockChanging = false;
        uint32_t clockMode = 0;
        uint32_t clockReturn = 0;
        uint32_t clockSR = 0;
        uint32_t bootWaited = 0; // how long the master has waited for the CD drive
        std::set<uint32_t> reported;
    };
    State g;

    // --- helpers ---------------------------------------------------------------------------------------

    uint32_t R32(SH2 &c, uint32_t a) {
        return Access::Read32(c, a);
    }
    void W32(SH2 &c, uint32_t a, uint32_t v) {
        Access::Write32(c, a, v);
    }
    void Push(SH2 &c, uint32_t v) {
        auto &R = Access::R(c);
        R[15] -= 4;
        W32(c, R[15], v);
    }
    uint32_t Pop(SH2 &c) {
        auto &R = Access::R(c);
        const uint32_t v = R32(c, R[15]);
        R[15] += 4;
        return v;
    }
    void Return(SH2 &c) {
        Access::SetPC(c, Access::PR(c));
    }
    void ReturnFromException(SH2 &c) {
        const uint32_t pc = Pop(c);
        const uint32_t sr = Pop(c);
        Access::SetPC(c, pc);
        Access::SetSR(c, sr);
    }

    // What a vector is set to when a game asks for the default
    uint32_t DefaultVector(uint32_t vector) {
        if (vector == 4 || vector == 6 || vector == 9 || vector == 10) {
            return Address(kHalt);
        }
        if (vector == 11) {
            return Address(kNMI);
        }
        if ((vector >= 0x40 && vector <= 0x4D) || (vector >= 0x50 && vector <= 0x5F)) {
            return Address(kIntFirst + (vector - 0x40));
        }
        return Address(kReturnFromInt);
    }

    // The SH-2's own peripherals as the BIOS sets them (interrupt priorities and vector numbers; the master
    // takes interrupt vectors from the SCU)
    void InitOnChip(SH2 &c, bool master) {
        Access::Write16(c, 0xFFFFFEE0, master ? 1 : 0); // ICR
        Access::Write16(c, 0xFFFFFEE2, 0x0000);         // IPRA
        Access::Write16(c, 0xFFFFFE60, 0x0F00);         // IPRB
        Access::Write16(c, 0xFFFFFE62, 0x6061);         // VCRA
        Access::Write16(c, 0xFFFFFE64, 0x6263);         // VCRB
        Access::Write16(c, 0xFFFFFE66, 0x6465);         // VCRC
        Access::Write16(c, 0xFFFFFE68, 0x6600);         // VCRD
        Access::Write16(c, 0xFFFFFEE4, 0x6869);         // VCRWDT
        W32(c, 0xFFFFFFA8, 108);                        // VCRDMA1
        W32(c, 0xFFFFFFA0, 109);                        // VCRDMA0
        W32(c, 0xFFFFFF0C, 110);                        // VCRDIV
        Access::Write8(c, 0xFFFFFE10, 0x81);            // TIER
    }

    // SCU as the BIOS leaves it: DMA idle, timers off, A-bus set up
    void InitScu(SH2 &c) {
        for (uint32_t ch = 0; ch < 3; ch++) {
            for (uint32_t reg = 0; reg < 0x14; reg += 4) {
                W32(c, kScu + ch * 0x20 + reg, 0);
            }
            W32(c, kScu + ch * 0x20 + 0x14, 7);
        }
        W32(c, kScu + 0x60, 0);
        W32(c, kScu + 0x80, 0);
        W32(c, kScu + 0xB0, 0x1FF01FF0);
        W32(c, kScu + 0xB4, 0x1FF01FF0);
        W32(c, kScu + 0xB8, 31);
        W32(c, kScu + 0xA8, 1);
        W32(c, kScu + 0x90, 0x3FF);
        W32(c, kScu + 0x94, 0x1FF);
        W32(c, kScu + 0x98, 0);
    }

    // Sets the SCU interrupt mask; `changed` are the mask bits just written (the BIOS acknowledges pending
    // interrupts with them, and the A-bus interrupt if it is being let through)
    void SetScuMask(SH2 &c, uint32_t mask, uint32_t changed) {
        W32(c, kScuMask, mask);
        W32(c, kScu + 0xA0, mask);
        const uint32_t status = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(changed)));
        W32(c, kScu + 0xA4, status);
        if (static_cast<int32_t>(status) >= 0) {
            W32(c, kScu + 0xA8, 1);
        }
    }

    // --- start-up --------------------------------------------------------------------------------------

    void Poke32(uint32_t offset, uint32_t value) {
        uint8_t *p = RamHigh(*g.saturn) + offset;
        p[0] = static_cast<uint8_t>(value >> 24);
        p[1] = static_cast<uint8_t>(value >> 16);
        p[2] = static_cast<uint8_t>(value >> 8);
        p[3] = static_cast<uint8_t>(value);
    }

    void BootMaster(SH2 &c) {
        uint8_t *ram = RamHigh(*g.saturn);

        // Vector tables: master at 0x06000000, slave at 0x06000400
        for (uint32_t v = 0; v < 0x80; v++) {
            Poke32(v * 4, DefaultVector(v));
            Poke32(0x400 + v * 4, v == 4 || v == 6 || v == 9 || v == 10 ? Address(kHalt) : Address(kReturnFromInt));
        }

        // Variables and the table of system services
        for (uint32_t a = 0x200; a < 0x400; a += 4) {
            Poke32(a, 0);
        }
        Poke32(0x234, 0x2AC);
        Poke32(0x238, 0x2BC);
        Poke32(0x23C, 0x350);
        Poke32(0x240, 0x32524459);
        Poke32(0x248, 0x2000);
        Poke32(0x250, Address(kIdle));
        Poke32(0x254, 0x06002100);
        Poke32(0x258, Address(kReturn));
        Poke32(0x25C, Address(kReturn));
        Poke32(0x260, 0x06000D00);
        for (uint32_t a : {0x268u, 0x26Cu, 0x270u, 0x274u, 0x284u, 0x288u, 0x28Cu, 0x298u, 0x29Cu, 0x2C8u, 0x2CCu, 0x2D0u,
                           0x2D4u, 0x2D8u, 0x2DCu, 0x328u, 0x32Cu, 0x35Cu}) {
            Poke32(a, kUnimplemented + a);
        }
        Poke32(0x280, Address(kChgUIPr));
        Poke32(0x290, 3);
        Poke32(0x2A0, 0x8000);
        Poke32(0x2B0, g.image.firstReadAddress);
        Poke32(0x2C4, 0x06001120);
        for (uint32_t a = 0x300; a < 0x380; a += 4) {
            Poke32(a, Address(kReturn));
        }
        Poke32(0x300, Address(kSetSInt));
        Poke32(0x304, Address(kGetSInt));
        Poke32(0x310, Address(kSetUInt));
        Poke32(0x314, Address(kGetUInt));
        Poke32(0x320, Address(kChgSysCk));
        Poke32(0x324, 0);
        Poke32(0x328, kUnimplemented + 0x328);
        Poke32(0x32C, kUnimplemented + 0x32C);
        Poke32(0x330, Address(kTasSem));
        Poke32(0x334, Address(kClrSem));
        Poke32(0x340, Address(kSetScuIm));
        Poke32(0x344, Address(kChgScuIm));
        Poke32(0x348, 0xFFFFFFFF);
        Poke32(0x34C, Address(kScuAck));
        Poke32(0x350, 0);
        Poke32(0x354, 0);
        Poke32(0x358, Address(kBupInit));
        Poke32(0x35C, kUnimplemented + 0x35C);

        // SCU interrupt handlers (none) and what is masked while each is handled
        for (uint32_t i = 0; i < 32; i++) {
            Poke32(0xA00 + i * 4, Address(kReturn));
            static const uint32_t kMasks[] = {0x00F0FFFF, 0x00E0FFFE, 0x00D0FFFC, 0x00C0FFF8, 0x00B0FFF0,
                                              0x00A0FFE0, 0x0090FFC0, 0x0080FF80, 0x0080FF80};
            Poke32(0xA80 + i * 4, i < 9 ? kMasks[i] : 0x0070FE00);
        }
        memset(ram + 0xB00, 0, 0x100);

        // The disc's initial program and its first file
        memcpy(ram + 0x2000, g.image.ip.data(), std::min<size_t>(g.image.ip.size(), 0x8000));
        const uint32_t at = g.image.firstReadAddress & 0xFFFFF;
        memcpy(ram + at, g.image.firstRead.data(), std::min<size_t>(g.image.firstRead.size(), kRamHighSize - at));
        CodeChanged();

        InitOnChip(c, true);
        InitScu(c);
        W32(c, kScu + 0xA0, 0xFFFFFFFF);
        g.clockChanging = false;

        // The initial program starts with everything cleared. Its first part (the licence screen) is the
        // BIOS's business; the game's own start-up code follows it and the area symbols.
        auto &R = Access::R(c);
        R.fill(0);
        R[15] = 0x06002000;
        Access::SetVBR(c, kRam);
        Access::SetGBR(c, 0);
        Access::SetPR(c, 0);
        Access::ClearMAC(c);
        Access::SetSR(c, 0);
        Access::SetPC(c, 0x06002E00);
    }

    // The slave starts when the game switches it on: registers cleared, its own vector table, then the
    // program the game has set
    void BootSlave(SH2 &c) {
        auto &R = Access::R(c);
        InitOnChip(c, false);
        W32(c, kRam + 0x244, 0x32524453);
        R.fill(0);
        const uint32_t stack = R32(c, kSlaveStack);
        R[15] = stack ? stack : 0x06001000;
        Access::SetVBR(c, kSlaveVectors);
        Access::SetGBR(c, 0);
        Access::SetPR(c, 0);
        Access::ClearMAC(c);
        Access::SetSR(c, 0);
        Access::SetPC(c, R32(c, kSlaveEntry));
    }

    // --- interrupts ------------------------------------------------------------------------------------

    // An SCU interrupt: registers saved on the stack, lower-priority interrupts masked, then the game's handler
    void EnterInterrupt(SH2 &c, uint32_t vector) {
        auto &R = Access::R(c);
        const uint32_t oldMask = R32(c, kScuMask);
        const uint32_t priority = R32(c, kPriorities + vector * 4);
        Push(c, R[0]);
        Push(c, R[1]);
        Push(c, R[2]);
        Push(c, R[3]);
        Push(c, oldMask);
        Push(c, R[4]);
        Push(c, R[5]);
        const uint32_t mask = oldMask | static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(priority)));
        Access::SetSR(c, priority >> 16);
        W32(c, kScuMask, mask);
        W32(c, kScu + 0xA0, mask);
        Push(c, R[6]);
        Push(c, R[7]);
        Push(c, Access::PR(c));
        Push(c, Access::GBR(c));
        R[0] = vector * 4;
        Access::SetPR(c, Address(kIntReturn));
        Access::SetPC(c, R32(c, kHandlers + vector * 4));
    }

    void LeaveInterrupt(SH2 &c) {
        auto &R = Access::R(c);
        Access::SetGBR(c, Pop(c));
        Access::SetPR(c, Pop(c));
        R[7] = Pop(c);
        R[6] = Pop(c);
        R[5] = Pop(c);
        R[4] = Pop(c);
        const uint32_t oldMask = Pop(c);
        Access::SetSR(c, 0xF0);
        W32(c, kScuMask, oldMask);
        R[3] = Pop(c);
        W32(c, kScu + 0xA0, oldMask);
        R[2] = Pop(c);
        R[1] = Pop(c);
        R[0] = Pop(c);
        ReturnFromException(c);
    }

    // --- clock change ----------------------------------------------------------------------------------

    // SYS_CHGSYSCK: the SMPC switches the system clock (320 or 352 pixels a line), which resets the video
    // and sound hardware and the SCU and stops the slave; it tells the master when it is done with an NMI
    void ChangeClock(SH2 &c) {
        g.clockMode = Access::R(c)[4] & 1;
        g.clockReturn = Access::PR(c);
        g.clockSR = Access::SR(c);
        g.clockChanging = true;
        Access::SetSR(c, 0xF0);
        W32(c, kClockMode, g.clockMode);
        W32(c, kScu + 0xA8, 0);
        W32(c, kScu + 0xB8, 0);
        Access::Write8(c, kSmpcSF, 1);
        Access::Write8(c, kSmpcComreg, g.clockMode ? 0x0E : 0x0F);
        Access::SetPC(c, Address(kClockWait));
    }

    void ClockChanged(SH2 &c) {
        Access::R(c)[15] += 8; // (the NMI's return address and SR: the wait is over, nothing to go back to)
        g.clockChanging = false;
        Access::Write16(c, 0xFFFFFEE0, 1);
        W32(c, kScu + 0xC4, 1);
        Access::Write16(c, 0x25F80000, static_cast<uint16_t>(g.clockMode));
        Access::Write8(c, 0xFFFFFE10, 0x81);
        InitScu(c);
        Access::SetVBR(c, kRam);
        const uint32_t mask = R32(c, kScuMask);
        SetScuMask(c, mask, mask);
        Access::SetSR(c, g.clockSR);
        Access::SetPC(c, g.clockReturn);
    }

    // --- backup memory library -------------------------------------------------------------------------

    enum BupResult : uint32_t {
        kBupOk = 0,
        kBupNone = 1, // no such device
        kBupUnformatted = 2,
        kBupNoSpace = 4,
        kBupNotFound = 5,
        kBupFound = 6,
        kBupNoMatch = 7,
    };

    std::string GuestString(SH2 &c, uint32_t a, unsigned max) {
        std::string s;
        for (unsigned i = 0; i < max; i++) {
            const char ch = static_cast<char>(Access::Read8(c, a + i));
            if (!ch) {
                break;
            }
            s += ch;
        }
        return s;
    }

    void PutGuestString(SH2 &c, uint32_t a, const std::string &s, unsigned size) {
        for (unsigned i = 0; i < size; i++) {
            Access::Write8(c, a + i, i < s.size() && i + 1 < size ? static_cast<uint8_t>(s[i]) : 0);
        }
    }

    // BUP_Init: the game gives memory for the library and for its table of functions
    void BupInit(SH2 &c) {
        auto &R = Access::R(c);
        const uint32_t work = R[5];
        const uint32_t config = R[6];
        W32(c, kBupVector, work);
        W32(c, work, R[4]);
        for (uint32_t i = 0; i < 10; i++) {
            W32(c, work + 4 + i * 4, Address(kBupFirst + i));
        }
        // Devices: internal memory (one partition); no cartridge, no external device
        Access::Write16(c, config + 0, 1);
        Access::Write16(c, config + 2, 1);
        for (uint32_t i = 4; i < 12; i += 2) {
            Access::Write16(c, config + i, 0);
        }
        R[0] = 0;
    }

    // Date: minutes since 1980-01-01 <-> year (since 1980), month, day, hour, minute, weekday
    bool LeapYear(unsigned year) {
        return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    }
    constexpr unsigned kMonthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    void BupGetDate(SH2 &c, uint32_t date, uint32_t tb) {
        uint32_t days = date / (24 * 60);
        const uint32_t minutes = date % (24 * 60);
        const uint32_t week = (days + 2) % 7; // 1980-01-01 was a Tuesday
        unsigned year = 1980;
        while (days >= (LeapYear(year) ? 366u : 365u)) {
            days -= LeapYear(year) ? 366 : 365;
            year++;
        }
        unsigned month = 0;
        for (;;) {
            const unsigned len = kMonthDays[month] + (month == 1 && LeapYear(year) ? 1 : 0);
            if (days < len) {
                break;
            }
            days -= len;
            month++;
        }
        Access::Write8(c, tb + 0, static_cast<uint8_t>(year - 1980));
        Access::Write8(c, tb + 1, static_cast<uint8_t>(month + 1));
        Access::Write8(c, tb + 2, static_cast<uint8_t>(days + 1));
        Access::Write8(c, tb + 3, static_cast<uint8_t>(minutes / 60));
        Access::Write8(c, tb + 4, static_cast<uint8_t>(minutes % 60));
        Access::Write8(c, tb + 5, static_cast<uint8_t>(week));
    }

    uint32_t BupSetDate(SH2 &c, uint32_t tb) {
        const unsigned year = 1980 + Access::Read8(c, tb + 0);
        const unsigned month = std::clamp<unsigned>(Access::Read8(c, tb + 1), 1, 12);
        const unsigned day = std::max<unsigned>(Access::Read8(c, tb + 2), 1);
        uint32_t days = 0;
        for (unsigned y = 1980; y < year; y++) {
            days += LeapYear(y) ? 366 : 365;
        }
        for (unsigned m = 0; m + 1 < month; m++) {
            days += kMonthDays[m] + (m == 1 && LeapYear(year) ? 1 : 0);
        }
        days += day - 1;
        return (days * 24 + Access::Read8(c, tb + 3)) * 60 + Access::Read8(c, tb + 4);
    }

    // The ten library functions, in the order of the library's table
    void Bup(SH2 &c, unsigned function) {
        auto &R = Access::R(c);
        auto &mem = Backup(*g.saturn);
        const uint32_t device = R[4];
        uint32_t result = kBupOk;
        if (function == 8) { // BUP_GetDate(date, table)
            BupGetDate(c, R[4], R[5]);
            return;
        }
        if (function == 9) { // BUP_SetDate(table)
            R[0] = BupSetDate(c, R[4]);
            return;
        }
        if (device != 0 || mem.Size() == 0) { // (only the internal memory, and only if the port was given one)
            R[0] = kBupNone;
            return;
        }
        if (function == 1) { // BUP_Format(device)
            mem.Format();
            R[0] = kBupOk;
            return;
        }
        if (function != 0 && !mem.IsHeaderValid()) {
            R[0] = kBupUnformatted;
            return;
        }
        switch (function) {
        case 0: // BUP_SelPart(device, partition)
            break;
        case 2: { // BUP_Stat(device, data size, table)
            const uint32_t blockSize = mem.GetBlockSize();
            const uint32_t total = mem.GetTotalBlocks();
            const uint32_t free = total - std::min(total, mem.GetUsedBlocks());
            // A file takes a header (34 bytes) and its data, in blocks that each give up 4 bytes for a tag
            // and 2 per block for the list of the file's blocks
            const uint32_t perFile = (R[5] + 34 + blockSize - 7) / (blockSize - 6);
            const uint32_t tb = R[6];
            W32(c, tb + 0, mem.Size());
            W32(c, tb + 4, total);
            W32(c, tb + 8, blockSize);
            W32(c, tb + 12, free * blockSize);
            W32(c, tb + 16, free);
            W32(c, tb + 20, perFile ? free / perFile : 0);
            break;
        }
        case 3: { // BUP_Write(device, directory entry, data, 0 = may overwrite)
            const uint32_t tb = R[5];
            bupns::BackupFile file;
            file.header.filename = GuestString(c, tb + 0, 11);
            file.header.comment = GuestString(c, tb + 12, 10);
            file.header.language = static_cast<bupns::Language>(Access::Read8(c, tb + 23));
            file.header.date = R32(c, tb + 24);
            const uint32_t size = R32(c, tb + 28);
            file.data.resize(size);
            for (uint32_t i = 0; i < size; i++) {
                file.data[i] = Access::Read8(c, R[6] + i);
            }
            switch (mem.Import(file, (R[7] & 0xFF) == 0)) {
            case bupns::BackupFileImportResult::FileExists: result = kBupFound; break;
            case bupns::BackupFileImportResult::NoSpace: result = kBupNoSpace; break;
            default: break;
            }
            break;
        }
        case 4:   // BUP_Read(device, file name, data)
        case 7: { // BUP_Verify(device, file name, data)
            const auto file = mem.Export(GuestString(c, R[5], 11));
            if (!file) {
                result = kBupNotFound;
                break;
            }
            for (uint32_t i = 0; i < file->data.size(); i++) {
                if (function == 4) {
                    Access::Write8(c, R[6] + i, file->data[i]);
                } else if (Access::Read8(c, R[6] + i) != file->data[i]) {
                    result = kBupNoMatch;
                    break;
                }
            }
            break;
        }
        case 5: // BUP_Delete(device, file name)
            if (!mem.Delete(GuestString(c, R[5], 11))) {
                result = kBupNotFound;
            }
            break;
        case 6: { // BUP_Dir(device, start of file name, table size, table): returns the number of files
            const std::string prefix = GuestString(c, R[5], 11);
            const uint32_t capacity = R[6] & 0xFFFF;
            uint32_t count = 0;
            for (const auto &info : mem.List()) {
                if (info.header.filename.compare(0, prefix.size(), prefix) != 0) {
                    continue;
                }
                if (count < capacity) {
                    const uint32_t tb = R[7] + count * 36;
                    const auto file = mem.Export(info.header.filename);
                    PutGuestString(c, tb + 0, info.header.filename, 12);
                    PutGuestString(c, tb + 12, info.header.comment, 11);
                    Access::Write8(c, tb + 23, static_cast<uint8_t>(info.header.language));
                    W32(c, tb + 24, info.header.date);
                    W32(c, tb + 28, file ? static_cast<uint32_t>(file->data.size()) : info.size);
                    Access::Write16(c, tb + 32, static_cast<uint16_t>(info.numBlocks));
                }
                count++;
            }
            // (more files than the table holds: the negative count)
            result = count > capacity ? static_cast<uint32_t>(-static_cast<int32_t>(count)) : count;
            break;
        }
        default: break;
        }
        R[0] = result;
    }

} // namespace

// ---------------------------------------------------------------------------------------------------

void Install(Machine &saturn, BootImage image) {
    g = {};
    g.saturn = &saturn;
    g.image = std::move(image);

    // The ROM area holds nothing but the reset vectors (power-on and manual reset: start address, stack)
    static std::array<uint8_t, 0x80000> rom;
    rom.fill(0xFF);
    const uint32_t vectors[4] = {Address(kReset), 0x06002000, Address(kReset), 0x06002000};
    for (unsigned i = 0; i < 4; i++) {
        rom[i * 4 + 0] = static_cast<uint8_t>(vectors[i] >> 24);
        rom[i * 4 + 1] = static_cast<uint8_t>(vectors[i] >> 16);
        rom[i * 4 + 2] = static_cast<uint8_t>(vectors[i] >> 8);
        rom[i * 4 + 3] = static_cast<uint8_t>(vectors[i]);
    }
#ifdef SATRECOMP_OWN_MACHINE
    saturn.LoadRom(rom.data(), rom.size());
#else
    saturn.LoadIPL(std::span<uint8_t, ymir::sys::kIPLSize>(rom));
#endif
}

bool Active() {
    return g.saturn != nullptr;
}

void Serialize(Archive &ar) {
    ar.Mark(0x42494F53);
    ar.Pod(g.clockChanging);
    ar.Pod(g.clockMode);
    ar.Pod(g.clockReturn);
    ar.Pod(g.clockSR);
    ar.Pod(g.bootWaited);
}

bool Run(SH2 &c, uint32_t offset) {
    if (!g.saturn) {
        return false;
    }
    auto &R = Access::R(c);
    uint64_t cycles = 8;

    if (offset >= kUnimplemented && offset < kUnimplementedEnd) {
        if (g.reported.insert(offset).second) {
            fprintf(stderr, "satrecomp: BIOS service at 0x%08X is not implemented (called from %08X)\n",
                    kRam + (offset - kUnimplemented), Access::PR(c));
        }
        R[0] = 0;
        Return(c);
        Access::AddCycles(c, cycles);
        return true;
    }
    if (offset < 0x1000 || (offset & 3) || (offset - 0x1000) / 4 >= kRoutineEnd - 0x1000) {
        return false;
    }
    const uint32_t routine = 0x1000 + (offset - 0x1000) / 4;

    if (routine >= kIntFirst && routine <= kIntLast) {
        EnterInterrupt(c, 0x40 + (routine - kIntFirst));
        Access::AddCycles(c, 40);
        return true;
    }
    if (routine >= kBupFirst && routine <= kBupLast) {
        Bup(c, routine - kBupFirst);
        Return(c);
        Access::AddCycles(c, 200);
        return true;
    }
    switch (routine) {
    case kReset:
        if (c.IsMaster()) {
            g.bootWaited = 0;
            Access::SetSR(c, 0xF0);
            Access::SetPC(c, Address(kBootWait));
        } else {
            BootSlave(c);
        }
        cycles = 1000;
        break;
    case kBootWait: {
        // The game expects the CD drive as the BIOS leaves it: disc recognised and authenticated, positioned
        // at the start of the data track and paused. The same commands are given here, with time in between
        // (the drive takes a while for each), then the disc's program is started.
        cycles = 4096;
        g.bootWaited++;
        constexpr uint32_t kHirq = 0x25890008, kCR1 = 0x25890018, kCR2 = 0x2589001C, kCR3 = 0x25890020,
                           kCR4 = 0x25890024;
        auto command = [&](uint16_t cr1, uint16_t cr2, uint16_t cr3, uint16_t cr4) {
            Access::Write16(c, kHirq, 0xFFFE); // (acknowledge the previous command)
            Access::Write16(c, kCR1, cr1);
            Access::Write16(c, kCR2, cr2);
            Access::Write16(c, kCR3, cr3);
            Access::Write16(c, kCR4, cr4);
        };
        if (g.bootWaited == 3000) {
            command(0xE000, 0, 0, 0); // authenticate the disc
        } else if (g.bootWaited == 9000) {
            command(0x1180, 0x0096, 0, 0); // seek to the start of the data track
        } else if (g.bootWaited > 12000) {
            const uint16_t cr1 = Peek16(*g.saturn, kCR1 & 0x7FFFFFF);
            const uint16_t cr2 = Peek16(*g.saturn, kCR2 & 0x7FFFFFF);
            const bool ready = (cr1 >> 8 & 0xF) == 1 && (cr2 & 0xFF) != 0xFF;
            if (ready || g.bootWaited > 60000) {
                if (!ready) {
                    fprintf(stderr, "satrecomp: the CD drive did not get ready (status %04X %04X); starting anyway\n",
                            cr1, cr2);
                }
                (void)Access::Read16(c, kCR4);     // (the last response has been taken)
                Access::Write16(c, kHirq, 0xFFDF); // (and the disc noticed: the "disc changed" flag is cleared)
                BootMaster(c);
            }
        }
        break;
    }
    case kReturnFromInt: ReturnFromException(c); break;
    case kHalt:
        if (g.reported.insert(routine + (c.IsMaster() ? 0 : 0x10000)).second) {
            fprintf(stderr, "satrecomp: %s CPU stopped by an exception (stack %08X)\n", c.IsMaster() ? "master" : "slave",
                    R[15]);
        }
        Access::SetSR(c, 0xF0);
        cycles = 256;
        break;
    case kNMI:
        if (g.clockChanging) {
            ClockChanged(c);
            cycles = 400;
        } else {
            ReturnFromException(c); // (reset button: nothing to do)
        }
        break;
    case kReturn: Return(c); break;
    case kIdle:
    case kClockWait: cycles = 64; break; // (the CPU stays here)
    case kIntReturn:
        LeaveInterrupt(c);
        cycles = 40;
        break;
    case kSetSInt: // SYS_SETSINT(vector, handler)
        W32(c, kHandlers + R[4] * 4, R[5] ? R[5] : Address(kReturn));
        Return(c);
        break;
    case kGetSInt:
        R[0] = R32(c, kHandlers + R[4] * 4);
        Return(c);
        break;
    case kSetUInt: // SYS_SETUINT(vector, handler)
        W32(c, Access::VBR(c) + R[4] * 4, R[5] ? R[5] : DefaultVector(R[4]));
        Return(c);
        break;
    case kGetUInt:
        R[0] = R32(c, Access::VBR(c) + R[4] * 4);
        Return(c);
        break;
    case kChgSysCk: ChangeClock(c); break;
    case kChgUIPr: // SYS_CHGUIPR(table of 32 priorities)
        for (uint32_t i = 0; i < 32; i++) {
            W32(c, kPriorities + 0x100 + i * 4, R32(c, R[4] + i * 4));
        }
        Return(c);
        cycles = 200;
        break;
    case kTasSem: { // SYS_TASSEM(number): 1 if the semaphore was free (it is taken now)
        const uint32_t a = kSemaphores + R[4];
        const uint8_t v = Access::Read8(c, a);
        Access::Write8(c, a, v | 0x80);
        Access::SetT(c, v == 0);
        R[0] = v == 0;
        Return(c);
        break;
    }
    case kClrSem:
        Access::Write8(c, kSemaphores + R[4], 0);
        Return(c);
        break;
    case kSetScuIm: // SYS_SETSCUIM(mask)
        SetScuMask(c, R[4], R[4]);
        Return(c);
        break;
    case kChgScuIm: { // SYS_CHGSCUIM(and mask, or mask)
        const uint32_t mask = (R32(c, kScuMask) & R[4]) | R[5];
        SetScuMask(c, mask, R[4] | R[5]);
        Return(c);
        break;
    }
    case kScuAck:
        W32(c, kScu + 0xA4, R[4]);
        W32(c, kScu + 0xA8, 1);
        R[0] = R32(c, kScu + 0xA8);
        Return(c);
        break;
    case kBupInit:
        BupInit(c);
        Return(c);
        cycles = 2000;
        break;
    default: return false;
    }
    Access::AddCycles(c, cycles);
    return true;
}

} // namespace hle
} // namespace satrecomp
