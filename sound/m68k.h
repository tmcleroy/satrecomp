// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// The sound CPU: a Motorola 68000 (MC68EC000), which runs the game's sound driver from sound RAM.
//
// An interpreter: Step runs one instruction (or takes an interrupt) and says how many clock cycles it took.
// Bus and address errors are not modelled (the sound driver causes none).
#pragma once

#include <cstdint>

namespace satrecomp {
class Archive;
}

namespace satrecomp::sound {

// What the CPU is connected to: 24-bit addresses, big-endian words
struct M68KBus {
    virtual ~M68KBus() = default;
    virtual uint8_t Read8(uint32_t address) = 0;
    virtual uint16_t Read16(uint32_t address) = 0;
    virtual void Write8(uint32_t address, uint8_t value) = 0;
    virtual void Write16(uint32_t address, uint16_t value) = 0;
};

class M68K {
public:
    explicit M68K(M68KBus &bus)
        : m_bus(bus) {}

    // The reset line: the stack pointer and PC from the first two vectors
    void Reset(bool hard);
    // The level on the interrupt lines (0: none); taken before an instruction when above the mask in SR
    void SetInterruptLevel(uint8_t level) {
        m_interruptLevel = level;
    }
    // One instruction or interrupt; the clock cycles it took
    uint32_t Step();

    uint32_t d[8] = {}, a[8] = {}; // a[7] is the stack pointer in use
    uint32_t otherSP = 0;          // the user or supervisor stack pointer not in use
    uint32_t pc = 0;               // of the next instruction
    bool flagC = false, flagV = false, flagZ = false, flagN = false, flagX = false;
    bool supervisor = true, trace = false;
    uint8_t interruptMask = 7;
    bool stopped = false; // (after STOP: until an interrupt)

    uint16_t SR() const;
    void SetSR(uint16_t value);
    void Serialize(Archive &ar); // save states (runtime/state.h)

private:
    M68KBus &m_bus;
    uint8_t m_interruptLevel = 0;
    uint32_t m_cycles = 0; // of the instruction being run

    struct Operand {
        enum class Kind : uint8_t { Data, Address, Memory, Immediate } kind;
        uint32_t at; // register number, memory address or immediate value
    };
    uint16_t Fetch();
    uint32_t Fetch32();
    uint32_t Read(uint32_t address, unsigned size);
    void Write(uint32_t address, unsigned size, uint32_t value);
    Operand Locate(unsigned mode, unsigned reg, unsigned size);
    uint32_t Load(const Operand &operand, unsigned size);
    void Store(const Operand &operand, unsigned size, uint32_t value);
    void Push32(uint32_t value);
    void Push16(uint16_t value);
    uint32_t Pop32();
    uint16_t Pop16();
    bool Condition(unsigned cc) const;
    void Exception(unsigned vector, uint32_t returnAddress, int level);
    uint32_t Add(uint32_t a, uint32_t b, unsigned size, bool withX, bool keepZ);
    uint32_t Sub(uint32_t a, uint32_t b, unsigned size, bool withX, bool keepZ, bool setX = true);
    void Logic(uint32_t result, unsigned size);
    uint32_t Shift(unsigned kind, bool left, uint32_t value, unsigned count, unsigned size);
    void Execute(uint16_t op);
    void Illegal(uint16_t op, uint32_t address);
};

} // namespace satrecomp::sound
