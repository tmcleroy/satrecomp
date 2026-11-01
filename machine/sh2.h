// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// One of the Saturn's two main CPUs (Hitachi SH7604, "SH-2") with the parts of its on-chip peripherals games
// use: the interrupt controller, the free-running timer, the divider and the DMA controller.
//
// The game's code runs as translated C++ (tools/sh2emit.py) that works on this state directly; the
// interpreter here runs code the build has no translation for, and services interrupts. Time is counted in
// nominal cycles per instruction (the same numbers the translated code counts), not bus-accurate ones.
#pragma once

#include <vector>

#include "sh2_ops.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace satrecomp {
template <int N>
struct BlockSet;
struct Access;
class Archive;
} // namespace satrecomp

namespace satrecomp::machine {

namespace types {
    using uint8 = uint8_t;
    using uint16 = uint16_t;
    using uint32 = uint32_t;
    using uint64 = uint64_t;
    using sint8 = int8_t;
    using sint16 = int16_t;
    using sint32 = int32_t;
    using sint64 = int64_t;
} // namespace types
using namespace types;

class Machine;
class SH2;

// Counts everything that can change what a waiting CPU would see: memory writes, hardware accesses,
// interrupts (the dispatcher's waiting detection)
extern uint64 g_activity;
// Counts the CPUs' accesses to anything but work RAM (hardware, ROM, their own registers)
extern uint64 g_hardware;
// While a rewrite is being checked (runtime/recomp.cpp): the writes made outside work RAM, in order
struct HardwareWrite {
    uint32 address, value, size;
    bool operator==(const HardwareWrite &) const = default;
};
extern std::vector<HardwareWrite> *g_hardwareWrites;

// Where translated code lies in work RAM: a write there moves the code epoch on, after which the dispatcher
// compares translations with memory again before running them
extern const uint8 *g_codeBase;
extern const uint8 *g_codeMap; // one byte per 64 bytes
extern size_t g_codeSize;
extern uint64 g_codeEpoch;
inline void NoteWrite(const uint8 *at) {
    const size_t offset = static_cast<size_t>(at - g_codeBase);
    if (offset < g_codeSize && g_codeMap[offset >> 6]) [[unlikely]] {
        ++g_codeEpoch;
    }
}

// What kind of instruction an opcode is; whether it may not stand in a delay slot; its nominal cycles
extern Op g_opKinds[0x10000];
extern uint8 g_opCycles[0x10000];
bool IllegalInSlot(Op op);

// The dispatcher for translated code: runs code from the CPU's PC for up to `target` cycles; false if it ran
// nothing (the interpreter then runs one instruction)
using DispatchFn = bool (*)(SH2 &cpu, uint64 target, void *context);

class SH2 {
public:
    SH2(Machine &machine, bool master);

    void Reset(bool hard);
    // Runs until `cycles` have been executed (m_cyclesExecuted starts at `spillover`); what was executed
    uint64 Advance(uint64 cycles, uint64 spillover);
    bool IsMaster() const {
        return m_master;
    }
    void SetRecompDispatcher(DispatchFn fn, void *context) {
        m_dispatch = fn;
        m_dispatchContext = context;
    }
    // One instruction (or an interrupt taken), as the dispatcher asks for when it has no translation
    uint64 RecompInterpretNext();
    // Code the interpreter takes its instructions from instead of memory while the PC is in it (the
    // dispatcher's model of a loop held in this CPU's cache: runtime/recomp.cpp, CachedLoop); null: memory
    const uint8 *recompFetchCode = nullptr;
    uint32 recompFetchBase = 0, recompFetchSize = 0;
    void RecompRefillPipeline() {}
    bool RecompWaiting() const {
        return m_recompWaiting;
    }

    // An interrupt line from outside (the SCU for the master, the video timing for the slave): level 0 = none
    void SetExternalInterrupt(uint8 level, uint8 vector);
    void SetNMI();
    // The other CPU pulsed this one's timer input (a write to the MINIT / SINIT area)
    void TriggerInputCapture();
    // Ends this CPU's turn at its next instruction that looks for interrupts (soon), so that the other CPU
    // can react to something this one just did
    void RequestYield() {
        m_yieldRequest = true;
        m_intrFlags.pending = true; // (translated code leaves its block when it sees this)
    }

    // --- state, as translated code works on it ---
    std::array<uint32, 16> R{};
    uint32 PC = 0, PR = 0, GBR = 0, VBR = 0;
    union {
        uint64 u64 = 0;
        struct {
            uint32 L, H;
        };
    } MAC;
    union {
        uint32 u32 = 0;
        struct {
            uint32 T : 1;
            uint32 S : 1;
            uint32 : 2;
            uint32 ILevel : 4;
            uint32 Q : 1;
            uint32 M : 1;
        };
    } SR;
    struct IntrFlags {
        bool pending = false; // an interrupt above the mask is waiting
        bool allow = true;    // false for the one instruction after ldc/lds/stc/sts, which hold interrupts off
    } m_intrFlags;
    bool m_delaySlot = false; // the next instruction is a branch's delay slot instruction
    uint32 m_delaySlotTarget = 0;
    uint8 m_wbReg = 0xFF; // (kept for translated code written for the reference machine's timing; unused)
    uint64 m_cyclesExecuted = 0;
    uint32 m_recompResume = 0; // where a translated function stopped (block_native.h)
    bool m_recompYield = false, m_recompWaiting = false;
    bool m_sleep = false;
    bool m_yieldRequest = false;

    struct {
        struct {
            uint8 level = 0, vector = 0, source = 0;
        } pending; // the interrupt that would be taken next
        uint8 externalLevel = 0, externalVector = 0;
        bool nmi = false;
        uint16 ICR = 0, IPRA = 0, IPRB = 0, VCRA = 0, VCRB = 0, VCRC = 0, VCRD = 0, VCRWDT = 0, VCRDIV = 0;
        uint32 VCRDMA[2] = {};
    } INTC;

    // The bus never makes translated code wait here
    struct {
        bool IsBusWait(uint32, uint32, bool) const {
            return false;
        }
    } m_bus;

    void SetupDelaySlot(uint32 target) {
        m_delaySlot = true;
        m_delaySlotTarget = target;
    }

    // Memory as the CPU sees it (work RAM, ROM, hardware, its own registers)
    template <bool = false>
    uint8 MemReadByte(uint32 address) {
        return Read8(address);
    }
    template <bool = false>
    uint16 MemReadWord(uint32 address) {
        return Read16(address);
    }
    template <bool = false>
    uint32 MemReadLong(uint32 address) {
        return Read32(address);
    }
    template <bool = false, bool = false>
    void MemWriteByte(uint32 address, uint8 value) {
        Write8(address, value);
    }
    template <bool = false, bool = false>
    void MemWriteWord(uint32 address, uint16 value) {
        Write16(address, value);
    }
    template <bool = false, bool = false>
    void MemWriteLong(uint32 address, uint32 value) {
        Write32(address, value);
    }
    uint8 Read8(uint32 address);
    uint16 Read16(uint32 address);
    uint32 Read32(uint32 address);
    void Write8(uint32 address, uint8 value);
    void Write16(uint32 address, uint16 value);
    void Write32(uint32 address, uint32 value);

    // Works out INTC.pending and m_intrFlags.pending from every interrupt source
    void RecalcInterrupts();
    // Save states (runtime/state.h)
    void Serialize(Archive &ar);

private:
    Machine &m_machine;
    const bool m_master;
    DispatchFn m_dispatch = nullptr;
    void *m_dispatchContext = nullptr;

    // --- on-chip peripherals ---
    struct {
        uint8 TIER = 0x01, FTCSR = 0, TCR = 0, TOCR = 0xE0;
        uint16 FRC = 0, OCRA = 0xFFFF, OCRB = 0xFFFF, FICR = 0;
        uint8 latch = 0;    // (the low byte of a 16-bit register, taken when the high byte is read or written)
        uint64 partial = 0; // cycles not yet counted
    } m_frt;
    struct {
        uint32 DVSR = 0, DVDNT = 0, DVCR = 0, DVDNTH = 0, DVDNTL = 0, DVDNTUH = 0, DVDNTUL = 0;
    } m_divu;
    struct DmaChannel {
        uint32 source = 0, target = 0, count = 0, control = 0;
        uint8 request = 0; // DRCR
    } m_dma[2];
    uint32 m_dmaOperation = 0; // DMAOR
    uint8 m_cacheControl = 0;
    uint64 m_timerSynced = 0; // the part of this time slice the timer has counted
    std::array<uint8, 0x1000> m_cacheData{}; // (the cache's data array, which programs can use as memory)

    uint32 ReadRegister(uint32 address, unsigned size);
    void WriteRegister(uint32 address, unsigned size, uint32 value);
    void Divide32();
    void Divide64();
    void RunDma(unsigned channel);
    void SyncTimer();
    void NoteWriter(uint32 address);
    void NoteCall(uint32 site, uint32 target);

    void EnterException(uint8 vector, uint32 returnAddress);
    void TakeInterrupt();
    void Execute(uint16 opcode);
    void Branch(uint32 target, bool delayed);
};

} // namespace satrecomp::machine
