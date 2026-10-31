// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// satrecomp runtime: block lookup and dispatch for statically recompiled SH-2 code.
//
// SH-2 code runs as blocks: straight runs of instructions that end at an unconditional branch. A block is
// identified by its address and its exact bytes, so code that gets replaced in RAM (programs loaded from disc
// over one another) can never run a stale translation. For each block reached, the dispatcher runs its
// recompiled function if the build has one; otherwise the interpreter runs the same instructions.
//
// Either way execution stops at exactly the instruction where the unmodified interpreter would end the CPU's
// time slice, so the whole machine stays cycle for cycle identical to `--cpu pure`. That matters: the two SH-2s
// are interleaved in slices of about 32 cycles, and games rely on it (Fighting Vipers has a race between the
// master and the slave CPU over a shared command queue that goes wrong with coarser interleaving). It also
// makes verification simple: a recompiled run must produce the same machine state, frame by frame, as the
// interpreter (tools/verify.sh).
#include "recomp.h"

#include "access.h"
#include "block_native.h"
#include "hle_bios.h"
#include "lift.h"
#include "state.h"
#include "blocks.h"

#ifdef SATRECOMP_OWN_MACHINE
#include "../machine/machine.h"
#else
#include <ymir/hw/sh2/sh2_decode.hpp>
#endif

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <map>
#include <set>
#include <vector>

namespace satrecomp {

uint8_t *g_ramHigh = nullptr;
uint8_t *g_ramLow = nullptr;
const uint8_t *g_rom = nullptr;

// What kind of instruction an opcode is, whether it may stand in a delay slot, and where the machine keeps
// its work RAM and ROM
#ifdef SATRECOMP_OWN_MACHINE
using OpcodeType = machine::Op;
namespace {
    inline OpcodeType Kind(uint16_t opcode) {
        return machine::g_opKinds[opcode];
    }
    inline bool BadInSlot(uint16_t opcode) {
        return machine::IllegalInSlot(machine::g_opKinds[opcode]);
    }
    struct Memory {
        uint8_t *ramHigh, *ramLow, *rom;
        uint32_t ramSize, romSize;
    };
    Memory MemoryOf(Machine &m) {
        return {m.ramHigh, m.ramLow, m.rom, static_cast<uint32_t>(Machine::kRamSize), static_cast<uint32_t>(Machine::kRomSize)};
    }
} // namespace
#else
using ymir::sh2::DecodeTable;
using ymir::sh2::OpcodeType;
namespace {
    inline OpcodeType Kind(uint16_t opcode) {
        return DecodeTable::s_instance.opcodes[0][opcode];
    }
    inline bool BadInSlot(uint16_t opcode) {
        return DecodeTable::s_instance.opcodes[1][opcode] == OpcodeType::IllegalSlot;
    }
    struct Memory {
        uint8_t *ramHigh, *ramLow, *rom;
        uint32_t ramSize, romSize;
    };
    Memory MemoryOf(Machine &m) {
        return {m.mem.WRAMHigh.data(), m.mem.WRAMLow.data(), m.mem.IPL.data(), static_cast<uint32_t>(m.mem.WRAMHigh.size()),
                static_cast<uint32_t>(m.mem.IPL.size())};
    }
} // namespace
#endif

namespace {

    // A place execution can enter a block: its first instruction, or a later one (where a time slice ended)
    struct Entry {
        const uint8_t *code; // big-endian instruction bytes from here to the end of the block
        BlockFn fn;          // the block's recompiled function, if the build has one
        NativeFn native;     // its native function (entry at the block's first instruction only)
        const FunctionDesc *function; // set if this is the entry of a native function of several blocks
        Entry *head;         // set if this is another block of a native function: the entry of its first
        uint16_t block;      // ... and which block (the function is entered there)
        const Lift *lift;    // set if a hand-written routine replaces the code from here (lift.h)
        Entry *next;         // other code seen at the same address
        uint64_t runs;
        uint64_t checked;    // the code epoch at which `matches` was worked out (see Matches)
        bool matches;        // memory held this code then
        uint16_t size;       // bytes from here to the end of the block
        uint8_t start;       // instruction index within the block
        bool single;         // cannot be part of a block (illegal, sleep, trapa): one interpreter step
        bool fresh;          // found during this run (not in the build)
        bool sync;           // starts at a sync point
    };

    struct Region {
        uint32_t base;
        uint32_t size;
        const uint8_t *mem;
        std::vector<Entry *> table; // by (offset / 2)
    };

    struct Runtime {
        Options opt;
        Region regions[3];
        std::deque<Entry> entries;
        std::deque<std::vector<uint8_t>> codeStore;
        // One byte per 64 bytes of work RAM (low, then high): translated code lies there. A write to such a
        // place moves the code epoch on (ymir::sys::SatrecompNoteWrite), and only then is an entry's code
        // compared with memory again.
        std::vector<uint8_t> codeMap;
        bool codeTracking = false;
        bool codeAudit = false; // SATRECOMP_VERIFY_CODE=1: compare every time anyway, and count where that disagrees
        uint64_t auditWrong = 0, compares = 0;
        uint64_t freshBlocks = 0;
        uint64_t builtBlocks = 0;
        uint64_t builtFunctions = 0;
        bool hle = false; // the native BIOS is in use
        std::set<std::string> explored; // static discovery: address (text) + code bytes of the blocks found

        // Waiting detection (free running), per CPU. A CPU is only waiting when its registers come round to
        // the values they had at the same address while nothing happened in between: no memory write and no
        // hardware access by either CPU, no interrupt (SATRECOMP_ACTIVITY stood still). It then
        // repeats itself until something outside changes. When both CPUs wait, the system runs each up to
        // the next hardware event at once (Saturn::Run) instead of in alternating slices of 32 cycles.
        struct Waiting {
            uint64_t activity = ~0ull;      // the activity count when `at` was taken
            Access::Registers at;           // the registers at a block entry since then
            unsigned age = 0;               // block entries since `at` was taken
        } waiting[2];
        bool syncPaused[2] = {false, false}; // free mode: this CPU has paused at the sync point it is at

        // Calls made as calls (Call, blocks.h), per CPU. A function that called another and did not get it
        // back (the time slice ended inside, an interrupt came up) is kept as a frame: when control arrives
        // at the return address, the function goes on from its block there. A function that stopped at one
        // of its blocks (slice over, interrupt pending) is kept as `stopped` and goes on from that block if
        // that is where the CPU is when it next runs.
        struct Frame {
            Entry *entry;
            uint32_t block; // of the entry's function
            uint32_t pc;    // where the CPU must be for it to go on
        };
        struct Linking {
            std::vector<Frame> frames;
            Frame stopped{nullptr, 0, 0};
            Entry *current = nullptr; // the entry whose code is running
            unsigned depth = 0;       // calls inside calls right now
        } linking[2];
        bool link = false; // calls are made as calls (free running, and nothing being recorded or traced)
        // Rewrites (lift.h): SATRECOMP_NO_REWRITES=1 runs the game's own code instead; SATRECOMP_LIFT_CHECK=n
        // runs both for every nth call of each and compares what they leave
        struct LiftCheck {
            uint64_t calls = 0, checked = 0, different = 0, scratchDifferent = 0, notComparable = 0, hardwareWrites = 0;
            unsigned reportsLeft = 6;
            std::vector<bool> reached; // which words of the game's routine the compared calls ran
        };
        Machine *machine = nullptr;
        unsigned inCall = 0;  // Guest::Call is running a routine
        bool inCheck = false; // a rewrite is being run for a check
        bool noRewrites = false;
        unsigned liftCheckEvery = 0;
        std::map<const Lift *, LiftCheck> liftChecks;
        uint64_t calls = 0, callsReturned = 0, framesResumed = 0, stopsResumed = 0, callsNoBlock = 0, callsNotLinked = 0;

        // Debugging aid (SATRECOMP_TRACE=path): a running hash of the CPU state after every dispatch, written
        // every million dispatches, and the full state for dispatches FROM..TO (SATRECOMP_TRACE_FROM/_TO).
        // Diffing the output of two runs finds the first place they differ.
        FILE *trace = nullptr;
        uint64_t traceFrom = ~0ull, traceTo = 0;
        uint64_t traceCount = 0;
        uint64_t traceHash = 0;
        uint32_t traceStartPC = 0;

        // Debugging aid (SATRECOMP_WATCH_PC=hex): when a block at this address is about to run, print the last
        // blocks that ran and exit
        // Debugging aid (SATRECOMP_CALLS=LO-HI, hex): every way control enters this address range from outside
        // it (the calls into a library, say), with how often and the argument registers of the first time
        uint32_t callLo = 0, callHi = 0;
        struct Call {
            uint64_t count = 0;
            uint32_t r[4] = {}, pr = 0;
        };
        std::map<std::pair<uint32_t, uint32_t>, Call> callLog;
        uint32_t watchPC = ~0u;
        uint32_t logPC = 0;
        struct Recent {
            bool master;
            uint32_t from, to;
            uint32_t sr, pr, r15;
        } recent[64] = {};
        unsigned recentPos = 0;
    };

    std::unique_ptr<Runtime> g_rt;
    std::vector<Lift> g_lifts;
    DiscFileReader g_discFileReader;
    DiscSectorReader g_discSectorReader;
    PadReader g_padReader;

    // How long a free-running CPU waits at a sync point
    constexpr uint64_t kSyncPauseCycles = 256;

    bool IsUnconditionalDelayed(OpcodeType t) {
        switch (t) {
        case OpcodeType::BRA:
        case OpcodeType::BRAF:
        case OpcodeType::BSR:
        case OpcodeType::BSRF:
        case OpcodeType::JMP:
        case OpcodeType::JSR:
        case OpcodeType::RTS:
        case OpcodeType::RTE: return true;
        default: return false;
        }
    }

    bool IsConditionalDelayed(OpcodeType t) {
        return t == OpcodeType::BTS || t == OpcodeType::BFS;
    }

    bool EndsBlock(OpcodeType t) {
        return t == OpcodeType::Illegal || t == OpcodeType::SLEEP || t == OpcodeType::TRAPA;
    }

    // Time of an instruction when free running: a fixed number of cycles by kind of instruction, not the
    // interpreter's bus-accurate count. Generated native code uses the same table (--dump-decode).
    unsigned NominalCycles(OpcodeType t) {
        switch (t) {
        case OpcodeType::BRA:
        case OpcodeType::BRAF:
        case OpcodeType::BSR:
        case OpcodeType::BSRF:
        case OpcodeType::JMP:
        case OpcodeType::JSR:
        case OpcodeType::RTS:
        case OpcodeType::BT:
        case OpcodeType::BF:
        case OpcodeType::BTS:
        case OpcodeType::BFS:
        case OpcodeType::MUL:
        case OpcodeType::DMULS:
        case OpcodeType::DMULU: return 2;
        case OpcodeType::MACW:
        case OpcodeType::MACL:
        case OpcodeType::AND_M:
        case OpcodeType::OR_M:
        case OpcodeType::XOR_M:
        case OpcodeType::TST_M:
        case OpcodeType::LDC_GBR_M:
        case OpcodeType::LDC_SR_M:
        case OpcodeType::LDC_VBR_M:
        case OpcodeType::STC_GBR_M:
        case OpcodeType::STC_SR_M:
        case OpcodeType::STC_VBR_M: return 3;
        case OpcodeType::TAS:
        case OpcodeType::RTE: return 4;
        default: return 1;
        }
    }

    uint8_t g_nominal[0x10000];

    uint16_t Be16(const uint8_t *p) {
        return static_cast<uint16_t>((p[0] << 8) | p[1]);
    }

    // Finds the block that starts at `code`: returns its instruction count (0 if the first instruction cannot
    // start a block). `slots` gets a bit for every instruction that only ever runs as a delay slot (the one
    // after an unconditional branch): execution cannot enter the block there.
    // `stops`: addresses (free mode sync points) a block may not run across; `pc` is the block's address.
    unsigned ScanBlock(const uint8_t *code, uint32_t avail, uint64_t &slots, uint32_t pc = 0,
                       const std::vector<uint32_t> *stops = nullptr) {
        unsigned n = 0;
        slots = 0;
        while (n < kMaxBlockInstrs && (n + 1) * 2 <= avail) {
            if (stops && n > 0 && std::find(stops->begin(), stops->end(), pc + n * 2) != stops->end()) {
                break;
            }
            const OpcodeType t = Kind(Be16(code + n * 2));
            if (EndsBlock(t)) {
                break;
            }
            const bool uncond = IsUnconditionalDelayed(t);
            if (uncond || IsConditionalDelayed(t)) {
                if (n + 2 > kMaxBlockInstrs || (n + 2) * 2 > avail) {
                    break;
                }
                const uint16_t slot = Be16(code + (n + 1) * 2);
                const OpcodeType plain = Kind(slot);
                if (BadInSlot(slot) || EndsBlock(plain)) {
                    break;
                }
                n += 2;
                if (uncond) {
                    slots |= 1ull << (n - 1);
                    break;
                }
                continue;
            }
            n++;
        }
        return n;
    }

    // --- static discovery ------------------------------------------------------------------------------
    //
    // While recording, the code a recorded block leads to is followed through memory as it is at that
    // moment: branches with fixed targets, the instruction after a call, and calls and jumps through a
    // register loaded from the literal pool. What is found goes into the block database with the blocks
    // that ran, so that code no recording run reached is translated too. A block found this way is only
    // ever run if the CPU gets to its address and finds the same bytes, like any other.

    // True if code at `code` reads like a routine's: the block there ends in a branch (or is cut at the
    // length limit), not because the next thing is no instruction. Keeps data out of the database.
    bool PlausibleBlock(const uint8_t *code, unsigned n, uint64_t slots) {
        if (n == 0) {
            return false;
        }
        bool allSame = true;
        for (unsigned i = 1; i < n; i++) {
            allSame = allSame && Be16(code + i * 2) == Be16(code);
        }
        if (allSame && n > 2) {
            return false; // (filler)
        }
        return n == kMaxBlockInstrs || ((slots >> (n - 1)) & 1);
    }

    void Explore(Runtime &rt, Region &r, uint32_t start) {
        if (&r != &rt.regions[0]) {
            return; // (the game's programs are in high work RAM)
        }
        std::vector<uint32_t> todo{start};
        while (!todo.empty()) {
            const uint32_t offset = todo.back();
            todo.pop_back();
            if (offset < 0x2000 || offset + 2 > r.size || (offset & 1)) {
                continue;
            }
            uint64_t slots;
            const uint8_t *code = r.mem + offset;
            const unsigned n = ScanBlock(code, r.size - offset, slots, r.base + offset, &rt.opt.syncPoints);
            if (!PlausibleBlock(code, n, slots)) {
                continue;
            }
            char addr[16];
            snprintf(addr, sizeof(addr), "%08X ", r.base + offset);
            if (!rt.explored.insert(addr + std::string(reinterpret_cast<const char *>(code), n * 2)).second) {
                continue;
            }
            // Where it can go on
            bool fallsThrough = true;
            for (unsigned i = 0; i < n; i++) {
                const uint16_t op = Be16(code + i * 2);
                const OpcodeType t = Kind(op);
                const uint32_t here = offset + i * 2;
                switch (t) {
                case OpcodeType::BT:
                case OpcodeType::BF:
                case OpcodeType::BTS:
                case OpcodeType::BFS:
                    todo.push_back(here + 4 + static_cast<int32_t>(static_cast<int8_t>(op & 0xFF)) * 2);
                    break;
                case OpcodeType::BRA:
                case OpcodeType::BSR:
                    todo.push_back(here + 4 + ((static_cast<int32_t>(op & 0xFFF) << 20) >> 19));
                    fallsThrough = t == OpcodeType::BSR; // (a call comes back to the instruction after its slot)
                    break;
                case OpcodeType::JSR:
                case OpcodeType::JMP: {
                    // The register's value, if a literal was loaded into it just before
                    const unsigned reg = (op >> 8) & 15;
                    for (unsigned k = i; k-- > 0;) {
                        const uint16_t prev = Be16(code + k * 2);
                        if ((prev >> 12) == 0xD && ((prev >> 8) & 15) == reg) {
                            const uint32_t at = ((offset + k * 2) & ~3u) + 4 + (prev & 0xFF) * 4;
                            if (at + 4 <= r.size) {
                                const uint32_t target = (static_cast<uint32_t>(Be16(r.mem + at)) << 16) | Be16(r.mem + at + 2);
                                if ((target & 0xC6000001u) == 0x06000000u) {
                                    todo.push_back(target & 0xFFFFF);
                                }
                            }
                            break;
                        }
                        if (((prev >> 8) & 15) == reg) {
                            break; // (something else may have set the register)
                        }
                    }
                    fallsThrough = t == OpcodeType::JSR;
                    break;
                }
                case OpcodeType::BSRF: fallsThrough = true; break;
                case OpcodeType::BRAF:
                case OpcodeType::RTS:
                case OpcodeType::RTE: fallsThrough = false; break;
                default: break;
                }
            }
            if (fallsThrough) {
                todo.push_back(offset + n * 2);
            }
        }
    }

    Region *RegionFor(Runtime &rt, uint32_t pc, uint32_t &offset) {
        // Cached (000) and cache-through (001) address spaces only; bits 28-27 are not connected
        if (pc >> 30) {
            return nullptr;
        }
        const uint32_t a = pc & 0x07FFFFFF;
        if (a >= 0x06000000) {
            offset = a & 0xFFFFF;
            return &rt.regions[0];
        }
        if ((a >> 20) == 0x002) {
            offset = a & 0xFFFFF;
            return &rt.regions[1];
        }
        if (a < 0x00080000) {
            offset = a;
            return &rt.regions[2];
        }
        return nullptr;
    }

    void MarkCode(Runtime &rt, const Region &r, int64_t offset, unsigned size);

    // Adds a block and an entry for each instruction execution can resume at. Returns the entry of the first.
    Entry *AddBlock(Runtime &rt, Region &r, uint32_t offset, const uint8_t *code, unsigned numInstrs, uint64_t slots,
                    BlockFn fn, NativeFn native, bool single, bool fresh) {
        Entry *first = nullptr;
        for (unsigned i = 0; i < numInstrs; i++) {
            if ((slots >> i) & 1) {
                continue;
            }
            Entry &e = rt.entries.emplace_back();
            e.code = code + i * 2;
            e.fn = fn;
            e.native = i == 0 ? native : nullptr;
            e.function = nullptr;
            e.head = nullptr;
            e.block = 0;
            e.lift = nullptr;
            e.runs = 0;
            e.checked = 0;
            e.matches = false;
            e.size = static_cast<uint16_t>((numInstrs - i) * 2);
            e.start = static_cast<uint8_t>(i);
            e.single = single;
            e.fresh = fresh;
            e.sync = std::find(rt.opt.syncPoints.begin(), rt.opt.syncPoints.end(), r.base + offset + i * 2) !=
                     rt.opt.syncPoints.end();
            if (i == 0) {
                MarkCode(rt, r, offset, numInstrs * 2);
            }
            // (lifted routines stay first at their address: they are asked before any code is run)
            Entry **link = &r.table[(offset >> 1) + i];
            while (*link && (*link)->lift) {
                link = &(*link)->next;
            }
            e.next = *link;
            *link = &e;
            if (i == 0) {
                first = &e;
            }
        }
        return first;
    }

    // A block the build doesn't know: take it from memory as it is now
    Entry *Discover(Runtime &rt, Region &r, uint32_t offset) {
        uint64_t slots;
        unsigned n = ScanBlock(r.mem + offset, r.size - offset, slots, r.base + offset, &rt.opt.syncPoints);
        const bool single = n == 0;
        if (single) {
            n = 1;
            if (offset + 2 > r.size) {
                return nullptr;
            }
        }
        auto &copy = rt.codeStore.emplace_back(r.mem + offset, r.mem + offset + n * 2);
        rt.freshBlocks++;
        return AddBlock(rt, r, offset, copy.data(), n, slots, nullptr, nullptr, single, true);
    }

    // The interpreter running (the rest of) a block, stopping where a recompiled block would (block_ops.h)
    void InterpretBlock(SH2 &cpu, const Entry &e, uint64_t target) {
        uint32_t expected = Access::PC(cpu);
        const uint32_t end = expected + e.size;
        if (expected & 2) {
            cpu.RecompRefillPipeline();
        }
        for (;;) {
            Access::AddCycles(cpu, cpu.RecompInterpretNext());
            if (Access::Cycles(cpu) >= target) {
                return;
            }
            expected += 2;
            if (Access::InDelaySlot(cpu)) {
                continue; // the delay slot instruction belongs to the branch
            }
            if (Access::PC(cpu) != expected || expected >= end || Access::InterruptRaised(cpu)) {
                return;
            }
        }
    }

    // How the interpreter left a block when free running
    enum class BlockEnd {
        Stalled,  // an instruction has to wait for the bus (it is retried), or an interrupt came up
        Fixed,    // went on to a fixed address: a branch with a fixed target, or off the end of the block
        Call,     // a call (jsr, bsr, bsrf): the PC is the callee
        Computed, // return or computed jump
    };

    // The interpreter running (the rest of) a block the free-running way: to its end, charging nominal cycles.
    // It leaves the block where a native block does (block_native.h), so the two can be compared exactly.
    BlockEnd InterpretBlockFree(SH2 &cpu, const uint8_t *code, unsigned size) {
        const uint32_t pc0 = Access::PC(cpu);
        const unsigned count = size / 2;
        if (pc0 & 2) {
            cpu.RecompRefillPipeline();
        }
        for (unsigned i = 0; i < count; i++) {
            const uint16_t op = Be16(code + i * 2);
            const OpcodeType type = Kind(op);
            (void)cpu.RecompInterpretNext();
            Access::AddCycles(cpu, g_nominal[op]);
            if (Access::InDelaySlot(cpu)) {
                // A branch: its delay slot instruction belongs to it. (Only delay slot instructions run with
                // InDelaySlot set, so this was the branch.)
                i++;
                (void)cpu.RecompInterpretNext();
                Access::AddCycles(cpu, g_nominal[Be16(code + i * 2)]);
                if (Access::InDelaySlot(cpu)) {
                    return BlockEnd::Stalled;
                }
                if (type == OpcodeType::JSR || type == OpcodeType::BSR || type == OpcodeType::BSRF) {
                    // (as in the translation: only a call that ends its block is made as a call)
                    return i + 1 == count ? BlockEnd::Call : BlockEnd::Computed;
                }
                return type == OpcodeType::BRA || type == OpcodeType::BTS || type == OpcodeType::BFS ? BlockEnd::Fixed
                                                                                                    : BlockEnd::Computed;
            }
            if (Access::InterruptRaised(cpu)) {
                return BlockEnd::Stalled;
            }
            if (Access::PC(cpu) != pc0 + (i + 1) * 2) {
                return type == OpcodeType::BT || type == OpcodeType::BF ? BlockEnd::Fixed : BlockEnd::Stalled;
            }
        }
        return BlockEnd::Fixed;
    }

    // The interpreter running a function of several blocks the way its native code does (block_native.h):
    // from block `start` on, from block to block along branches with fixed targets, calls made as calls
    // (Call), leaving at anything else; at a pending interrupt and, on a backward branch, when the time slice
    // is over, it stops at a block and says which (m_recompResume). `f` null: a block on its own, which may
    // branch back to its own start (`loops`: unless it starts at a sync point).
    void InterpretFunctionFree(SH2 &cpu, const FunctionDesc *f, const uint8_t *code, unsigned size, bool loops, unsigned start,
                               uint64_t target) {
        FunctionBlock single{0, static_cast<uint16_t>(size), code};
        const FunctionBlock *blocks = f ? f->blocks : &single;
        const unsigned numBlocks = f ? f->numBlocks : 1;
        const FunctionBlock *block = &blocks[start < numBlocks ? start : 0];
        const uint32_t entry = Access::PC(cpu) - static_cast<uint32_t>(block->offset);
        auto find = [&](int32_t offset) -> const FunctionBlock * {
            if (!f) {
                return offset == 0 && loops ? &single : nullptr;
            }
            for (unsigned i = 0; i < numBlocks; i++) {
                if (blocks[i].offset == offset) {
                    return &blocks[i];
                }
            }
            return nullptr;
        };
        for (;;) {
            const BlockEnd end = InterpretBlockFree(cpu, block->code, block->size);
            if (end == BlockEnd::Call) {
                const int32_t after = block->offset + block->size;
                const FunctionBlock *back = find(after);
                if (Call(cpu, target, back ? static_cast<int>(back - blocks) : -1, entry + static_cast<uint32_t>(after)) && back) {
                    block = back;
                    continue;
                }
                return;
            }
            if (end != BlockEnd::Fixed) {
                return;
            }
            const int32_t offset = static_cast<int32_t>(Access::PC(cpu) - entry);
            const FunctionBlock *next = find(offset);
            if (!next) {
                return;
            }
            if (Access::InterruptRaised(cpu) || (offset <= block->offset && Access::Cycles(cpu) >= target)) {
                Access::SetResume(cpu, static_cast<uint32_t>(next - blocks) + 1);
                return;
            }
            block = next;
        }
    }

    // Block code is compared against memory on every dispatch, so this is the hottest loop of the runtime
    inline bool SameCode(const uint8_t *a, const uint8_t *b, unsigned size) {
        unsigned i = 0;
        for (; i + 8 <= size; i += 8) {
            uint64_t x, y;
            memcpy(&x, a + i, 8);
            memcpy(&y, b + i, 8);
            if (x != y) {
                return false;
            }
        }
        for (; i < size; i += 2) {
            uint16_t x, y;
            memcpy(&x, a + i, 2);
            memcpy(&y, b + i, 2);
            if (x != y) {
                return false;
            }
        }
        return true;
    }

    // All of a function's blocks are in memory as they were translated (the entry block is checked before)
    inline bool SameFunction(const Region &r, uint32_t offset, const FunctionDesc &f) {
        for (unsigned i = 1; i < f.numBlocks; i++) {
            const FunctionBlock &b = f.blocks[i];
            const int64_t at = static_cast<int64_t>(offset) + b.offset;
            if (at < 0 || at + b.size > r.size || !SameCode(r.mem + at, b.code, b.size)) {
                return false;
            }
        }
        return true;
    }

    // Marks where an entry's code lies (so that writes there are noticed)
    void MarkCode(Runtime &rt, const Region &r, int64_t offset, unsigned size) {
        if (!rt.codeTracking || r.mem < SATRECOMP_CODE_BASE || offset < 0 || offset + size > r.size) {
            return;
        }
        const size_t at = static_cast<size_t>(r.mem - SATRECOMP_CODE_BASE) + static_cast<size_t>(offset);
        for (size_t g = at >> 6; g <= (at + size - 1) >> 6 && g < rt.codeMap.size(); g++) {
            rt.codeMap[g] = 1;
        }
    }

    // Is the entry's code (all of its function's) what is in memory now? Worked out by comparing, and again
    // only after a write to memory that holds translated code.
    inline bool Matches(Runtime &rt, const Region &r, uint32_t offset, Entry &e) {
        const uint64_t epoch = SATRECOMP_CODE_EPOCH;
        if (e.checked == epoch && rt.codeTracking && !rt.codeAudit) [[likely]] {
            return e.matches;
        }
        const bool matches = offset + e.size <= r.size && SameCode(r.mem + offset, e.code, e.size) &&
                             (!e.function || SameFunction(r, offset, *e.function));
        rt.compares++;
        if (rt.codeAudit && e.checked == epoch && matches != e.matches) {
            if (rt.auditWrong++ < 8) {
                fprintf(stderr, "satrecomp: code at %08X changed without a write being noticed\n", r.base + offset);
            }
        }
        e.checked = epoch;
        e.matches = matches;
        return matches;
    }

    // The address an entry's code starts at, given that its block `block` is at `pc`
    inline uint32_t EntryStart(const Entry &e, uint32_t block, uint32_t pc) {
        return e.function && block < e.function->numBlocks ? pc - static_cast<uint32_t>(e.function->blocks[block].offset) : pc;
    }

    // Runs an entry's code from one of its blocks: its translation, or the interpreter doing the same
    void Run(Runtime &rt, SH2 &cpu, Entry &e, unsigned block, uint64_t target) {
        Runtime::Linking &lk = rt.linking[cpu.IsMaster() ? 0 : 1];
        Entry *outer = lk.current;
        lk.current = &e;
        Access::SetResume(cpu, 0);
        if (e.native && rt.opt.mode == Mode::Native) {
            e.native(cpu, target, block);
        } else {
            InterpretFunctionFree(cpu, e.function, e.code, e.size, !e.sync, block, target);
        }
        lk.current = outer;
        if (const uint32_t resume = Access::Resume(cpu)) {
            lk.stopped = {&e, resume - 1, Access::PC(cpu)};
            Access::SetResume(cpu, 0);
        }
    }

    // Is the code of a kept frame still in memory as it was translated?
    bool FrameValid(Runtime &rt, const Runtime::Frame &frame) {
        uint32_t offset;
        Region *r = RegionFor(rt, EntryStart(*frame.entry, frame.block, frame.pc), offset);
        return r && r != &rt.regions[2] && Matches(rt, *r, offset, *frame.entry);
    }

#ifdef SATRECOMP_OWN_MACHINE
    using LiftCheck = Runtime::LiftCheck;

    // A rewritten routine against the game's own code, for one call: the rewrite runs first; then memory and
    // registers are put back and the game's code is run by the interpreter until it returns; what the two
    // left is compared. The game goes on from what its own code left. False if the rewrite declined the call.
    //
    // Not compared: registers the convention lets a routine leave as it likes (unless the lift says its
    // callers read them), and the stack below where the routine found it. A call during which either side
    // touched hardware, or the game's code did not come back, cannot be compared and is counted.
    bool CheckRewrite(Runtime &rt, SH2 &cpu, const Lift &lift, LiftCheck &check) {
        struct Snapshot {
            Access::Registers regs;
            std::vector<uint8_t> high, low;
        };
        // SATRECOMP_LIFT_POKE=name:address=byte,address=byte...[;name:...]: bytes of work RAM set before each
        // call of that rewrite, to send both it and the game's code down a way the movie does not reach
        static const char *pokes = getenv("SATRECOMP_LIFT_POKE");
        for (const char *poke = pokes; poke && *poke; poke = strchr(poke, ';') ? strchr(poke, ';') + 1 : nullptr) {
            const size_t length = strlen(lift.name);
            if (strncmp(poke, lift.name, length) != 0 || poke[length] != ':') {
                continue;
            }
            for (const char *p = poke + length; *p == ':' || *p == ',';) {
                unsigned address, value;
                if (sscanf(++p, "%x=%x", &address, &value) == 2) {
                    Access::Write8(cpu, address, static_cast<uint8_t>(value));
                }
                p += strcspn(p, ",;");
            }
        }
        static Snapshot before, rewritten;
        // (the whole machine, as a save state holds it: what the rewrite does to hardware is undone as well, so
        // the game's code finds what the rewrite found)
        static std::vector<uint8_t> machineBefore;
        machineBefore.clear();
        {
            Archive ar(machineBefore, true);
            rt.machine->Serialize(ar);
        }
        const size_t ramSize = rt.regions[0].size;
        auto take = [&](Snapshot &s) {
            s.regs = Access::GetRegisters(cpu);
            s.high.assign(g_ramHigh, g_ramHigh + ramSize);
            s.low.assign(g_ramLow, g_ramLow + ramSize);
        };
        auto put = [&](const Snapshot &s) {
            Access::R(cpu) = s.regs.R;
            Access::SetPC(cpu, s.regs.PC);
            Access::SetPR(cpu, s.regs.PR);
            Access::SetGBR(cpu, s.regs.GBR);
            Access::SetVBR(cpu, s.regs.VBR);
            cpu.MAC.u64 = s.regs.MAC;
            cpu.SR.u32 = s.regs.SR;
            memcpy(g_ramHigh, s.high.data(), ramSize);
            memcpy(g_ramLow, s.low.data(), ramSize);
        };
        take(before);
        const uint64_t cycles = Access::Cycles(cpu);
        static std::vector<machine::HardwareWrite> rewriteWrote, gameWrote;
        rewriteWrote.clear();
        gameWrote.clear();
        machine::g_hardwareWrites = &rewriteWrote;
        Guest guest(cpu);
        rt.inCheck = true;
        const bool taken = lift.fn(guest);
        // Runs the game's code from where the CPU is to the routine's return: the address in PR at the call,
        // with the stack as it was, or near it (some of this kind of code comes back with the stack moved and
        // has its caller put it right; the stack pointer is compared like any register)
        const uint32_t returnTo = before.regs.PR, stack = before.regs.R[15];
        auto toReturn = [&](bool note) {
            for (uint32_t steps = 0; steps < 20'000'000; steps++) {
                if (Access::PC(cpu) == returnTo && Access::R15(cpu) - stack + 0x1000 <= 0x2000 && !Access::InDelaySlot(cpu)) {
                    return true;
                }
                cpu.m_intrFlags.pending = false; // (no interrupt in the middle: it would not be part of the routine)
                if (const uint32_t at = (Access::PC(cpu) - before.regs.PC) / 2; note && at < check.reached.size()) {
                    check.reached[at] = true;
                }
                (void)cpu.RecompInterpretNext();
            }
            return false;
        };
        const bool rewriteReturned = taken && toReturn(false);
        rt.inCheck = false;
        machine::g_hardwareWrites = nullptr;
        if (!taken) {
            return false;
        }
        take(rewritten);
        {
            Archive ar(machineBefore, false);
            rt.machine->Serialize(ar);
        }
        put(before);
        cpu.m_cyclesExecuted = cycles;

        // The game's code, to its return (the address in PR, with the stack as it was)
        machine::g_hardwareWrites = &gameWrote;
        const bool interruptWaiting = cpu.m_intrFlags.pending;
        check.reached.resize(lift.size / 2);
        const bool returned = toReturn(true) && rewriteReturned;
        machine::g_hardwareWrites = nullptr;
        cpu.RecalcInterrupts();
        (void)interruptWaiting;
        cpu.m_cyclesExecuted = cycles + 100;
        ++SATRECOMP_CODE_EPOCH; // (memory was put back and run over: translations are compared again)
        if (!returned) {
            check.notComparable++;
            if (check.reportsLeft) {
                check.reportsLeft--;
                fprintf(stderr, "rewrite %s: a call cannot be compared (the game's code did not come back)\n", lift.name);
            }
            return true;
        }
        check.checked++;
        const Access::Registers now = Access::GetRegisters(cpu);
        std::string what;
        char text[160];
        // What was written outside work RAM (video, sound, on-chip registers): the same, in the same order
        if (gameWrote != rewriteWrote) {
            size_t i = 0;
            while (i < gameWrote.size() && i < rewriteWrote.size() && gameWrote[i] == rewriteWrote[i]) {
                i++;
            }
            snprintf(text, sizeof(text), " hardware writes %zu/%zu, the first to differ: no. %zu", gameWrote.size(), rewriteWrote.size(), i);
            what += text;
            if (i < gameWrote.size()) {
                snprintf(text, sizeof(text), " [%08X] = %X", gameWrote[i].address, gameWrote[i].value);
                what += text;
            }
            if (i < rewriteWrote.size()) {
                snprintf(text, sizeof(text), " / [%08X] = %X", rewriteWrote[i].address, rewriteWrote[i].value);
                what += text;
            }
        }
        if (gameWrote != rewriteWrote && getenv("SATRECOMP_LIFT_CHECK_WRITES")) {
            for (size_t i = 0; i < std::max(gameWrote.size(), rewriteWrote.size()); i++) {
                fprintf(stderr, "  %3zu", i);
                for (const auto *log : {&gameWrote, &rewriteWrote}) {
                    if (i < log->size()) {
                        fprintf(stderr, "   [%08X] = %-8X", (*log)[i].address, (*log)[i].value);
                    }
                }
                fprintf(stderr, "\n");
            }
        }
        check.hardwareWrites += gameWrote.size();
        // Every register but those the lift names as free for it to leave as it likes
        bool freeDiffer = false;
        for (unsigned r = 0; r < 16; r++) {
            if (now.R[r] != rewritten.regs.R[r]) {
                if ((lift.free >> r) & 1) {
                    freeDiffer = true;
                } else {
                    snprintf(text, sizeof(text), " r%u %08X/%08X", r, now.R[r], rewritten.regs.R[r]);
                    what += text;
                }
            }
        }
        if (now.PC != rewritten.regs.PC || now.PR != rewritten.regs.PR || now.GBR != rewritten.regs.GBR) {
            snprintf(text, sizeof(text), " pc %08X/%08X pr %08X/%08X gbr %08X/%08X", now.PC, rewritten.regs.PC, now.PR, rewritten.regs.PR, now.GBR,
                     rewritten.regs.GBR);
            what += text;
        }
        if ((now.SR ^ rewritten.regs.SR) & 1) {
            if ((lift.free >> 16) & 1) {
                freeDiffer = true;
            } else {
                what += now.SR & 1 ? " T 1/0" : " T 0/1";
            }
        }
        if ((now.SR ^ rewritten.regs.SR) & 0x3F2) {
            what += " SR";
        }
        const bool scratch = freeDiffer;
        // Memory, but for the stack below the routine's
        unsigned shown = 0;
        for (int part = 0; part < 2; part++) {
            const uint8_t *game = part ? g_ramLow : g_ramHigh;
            const std::vector<uint8_t> &mine = part ? rewritten.low : rewritten.high;
            if (memcmp(game, mine.data(), ramSize) == 0) {
                continue;
            }
            const uint32_t base = part ? 0x00200000u : 0x06000000u;
            for (uint32_t i = 0; i < ramSize; i++) {
                if (game[i] == mine[i]) {
                    continue;
                }
                const uint32_t address = base + i;
                if (part == 0 && ((address ^ stack) & 0x07F00000u) == 0 && (address & 0x07FFFFFFu) < (stack & 0x07FFFFFFu) &&
                    (stack & 0x07FFFFFFu) - (address & 0x07FFFFFFu) <= 0x4000) {
                    continue;
                }
                if (shown++ < 4) {
                    snprintf(text, sizeof(text), " [%08X] %02X/%02X", address, game[i], mine[i]);
                    what += text;
                }
            }
        }
        if (shown > 4) {
            snprintf(text, sizeof(text), " (and %u more bytes)", shown - 4);
            what += text;
        }
        if (!what.empty()) {
            check.different++;
            if (check.reportsLeft) {
                check.reportsLeft--;
                fprintf(stderr, "rewrite %s differs (the game's code/the rewrite):%s; called with r4-r7 %08X %08X %08X %08X from %08X\n", lift.name,
                        what.c_str(), before.regs.R[4], before.regs.R[5], before.regs.R[6], before.regs.R[7], before.regs.PR);
            }
        } else if (scratch) {
            check.scratchDifferent++;
        }
        return true;
    }
#endif

    // Runs blocks until the CPU has used up its time slice (`target` cycles). Returns false if it ran nothing
    // and the interpreter must execute the next instruction itself.
    bool Dispatch(SH2 &cpu, uint64_t target, void *ctx) {
        Runtime &rt = *static_cast<Runtime *>(ctx);
        const bool recompiled = rt.opt.mode == Mode::Recompiled;
        const bool native = rt.opt.mode == Mode::Native;
        const bool freeRunning = native || rt.opt.mode == Mode::Free;
        bool ran = false;
        uint32_t pc;
        for (;;) {
            pc = Access::PC(cpu);
            uint32_t offset;
            Region *r;
            Entry *e;
            // An interrupt is serviced by the interpreter. One that is pending but held off for an instruction
            // (after ldc/lds/stc/sts) is serviced right after the next instruction; blocks only check for that
            // where it can happen inside them, so here the interpreter steps over it.
            if (Access::InterruptRaised(cpu) || (pc & 1) || !(r = RegionFor(rt, pc, offset))) {
                break;
            }
            // An address in the ROM area that stands for a native BIOS routine (hle_bios.h)
            if (r == &rt.regions[2] && rt.hle && hle::Run(cpu, offset)) {
                ran = true;
                Access::SetWaiting(cpu, false);
                ++SATRECOMP_ACTIVITY;
                if (Access::Cycles(cpu) >= target) {
                    return true;
                }
                continue;
            }
            // A function that stopped here, or one that is waiting for a call to come back here, goes on
            unsigned block = 0;
            Entry *resumed = nullptr;
            if (freeRunning) {
                Runtime::Linking &lk = rt.linking[cpu.IsMaster() ? 0 : 1];
                if (lk.stopped.entry) {
                    if (lk.stopped.pc == pc && FrameValid(rt, lk.stopped)) {
                        resumed = lk.stopped.entry;
                        block = lk.stopped.block;
                        rt.stopsResumed++;
                    }
                    lk.stopped.entry = nullptr;
                }
                if (!resumed) {
                    for (size_t n = lk.frames.size(); n-- > 0;) {
                        if (lk.frames[n].pc == pc) {
                            if (FrameValid(rt, lk.frames[n])) {
                                resumed = lk.frames[n].entry;
                                block = lk.frames[n].block;
                                rt.framesResumed++;
                            }
                            lk.frames.resize(n); // (the calls made after it never came back, or have)
                            break;
                        }
                    }
                }
            }
            // (Free running, blocks are only entered at their first instruction: a branch into the middle of
            // one starts a block of its own.)
            for (e = resumed ? nullptr : r->table[offset >> 1]; e; e = e->next) {
                if (e->lift) [[unlikely]] {
                    // A lifted routine: the code here must be the routine it was written from. Its bytes
                    // are not part of the build; they are taken from memory the first time the hash matches.
                    if (!freeRunning || offset + e->size > r->size) {
                        continue;
                    }
                    if (!e->code) {
                        if (LiftHash(r->mem + offset, e->size) != e->lift->hash) {
                            continue;
                        }
                        e->code = rt.codeStore.emplace_back(r->mem + offset, r->mem + offset + e->size).data();
                    }
                    if (!SameCode(r->mem + offset, e->code, e->size)) {
                        continue;
                    }
#ifdef SATRECOMP_OWN_MACHINE
                    if (e->lift->rewrite) {
                        // (SATRECOMP_LIFT_SKIP=name,name: the game's code for these, so that the routines they
                        // call, which a rewrite calls as C++, are met and checked on their own)
                        static const char *skip = getenv("SATRECOMP_LIFT_SKIP");
                        if (rt.noRewrites || (skip && (std::string(",") + skip + ",").find(std::string(",") + e->lift->name + ",") != std::string::npos)) {
                            continue; // (the original runs)
                        }
                        LiftCheck &check = rt.liftChecks[e->lift];
                        if (rt.liftCheckEvery && check.calls++ % rt.liftCheckEvery == 0) {
                            if (!CheckRewrite(rt, cpu, *e->lift, check)) {
                                continue;
                            }
                            Access::SetWaiting(cpu, false);
                            ++SATRECOMP_ACTIVITY;
                            e->runs++;
                            break;
                        }
                    }
#else
                    if (e->lift->rewrite) {
                        continue; // (the reference machine runs the game's code, and is compared on it)
                    }
#endif
                    Guest guest(cpu);
                    if (e->lift->fn(guest)) {
                        Access::SetWaiting(cpu, false);
                        ++SATRECOMP_ACTIVITY;
                        e->runs++;
                        break;
                    }
                    continue;
                }
                if (!(freeRunning ? e->start == 0 : !e->function)) {
                    continue;
                }
                if (e->head) {
                    // A block of a function other than its first: the function is entered there
                    const int64_t start = static_cast<int64_t>(offset) - e->function->blocks[e->block].offset;
                    if (start >= 0 && Matches(rt, *r, static_cast<uint32_t>(start), *e->head)) {
                        block = e->block;
                        e = e->head;
                        break;
                    }
                } else if (Matches(rt, *r, offset, *e)) {
                    break;
                }
            }
            if (resumed) {
                e = resumed;
            }
            if (e && e->lift) {
                ran = true;
                if (Access::Cycles(cpu) >= target) {
                    return true;
                }
                continue;
            }
            if (!e && !(e = Discover(rt, *r, offset))) {
                break;
            }
            e->runs++;
            if (e->runs == 1 && !rt.opt.recordPath.empty() && !e->lift && e->start == 0 && block == 0) [[unlikely]] {
                // Recording: follow where this code leads (see Explore)
                Explore(rt, *r, offset);
                if (e->function) {
                    for (unsigned i = 1; i < e->function->numBlocks; i++) {
                        Explore(rt, *r, offset + e->function->blocks[i].offset);
                    }
                }
            }
            if (freeRunning) {
                Runtime::Waiting &w = rt.waiting[cpu.IsMaster() ? 0 : 1];
                const Access::Registers now = Access::GetRegisters(cpu);
                if (w.activity != SATRECOMP_ACTIVITY) {
                    w.activity = SATRECOMP_ACTIVITY;
                    w.at = now;
                    w.age = 0;
                    Access::SetWaiting(cpu, false);
                } else if (now == w.at) {
                    Access::SetWaiting(cpu, true);
                } else if (++w.age > 256) {
                    // (the point taken may not be one the CPU comes back to in the same state: take another)
                    w.at = now;
                    w.age = 0;
                }
            }
            if (rt.logPC && (pc & 0x07FFFFFF) == rt.logPC) [[unlikely]] {
                // (SATRECOMP_LOG_PC=hex: registers each time this block is entered)
                Access::PrintRegs(cpu);
            }
            if (rt.watchPC != ~0u) [[unlikely]] {
                if ((pc & 0x07FFFFFF) == rt.watchPC) {
                    for (unsigned i = 0; i < 64; i++) {
                        const auto &q = rt.recent[(rt.recentPos + i) % 64];
                        fprintf(stderr, "%c %08X -> %08X sr=%03X pr=%08X sp=%08X\n", q.master ? 'M' : 'S', q.from, q.to,
                                q.sr, q.pr, q.r15);
                    }
                    fprintf(stderr, "watch: %c reached %08X\n", cpu.IsMaster() ? 'M' : 'S', pc);
                    Access::PrintRegs(cpu);
                    if (const char *path = getenv("SATRECOMP_WATCH_DUMP")) {
                        if (FILE *f = fopen(path, "wb")) {
                            fwrite(rt.regions[0].mem, 1, rt.regions[0].size, f);
                            fclose(f);
                        }
                    }
                    exit(3);
                }
            }
            if (e->single) {
                break;
            }
            if (freeRunning) {
                // A sync point: this CPU waits here for a while (the time is charged to it), which gives the
                // other CPU that time before this one goes on
                if (e->sync) [[unlikely]] {
                    bool &paused = rt.syncPaused[cpu.IsMaster() ? 0 : 1];
                    if (!paused) {
                        paused = true;
                        Access::AddCycles(cpu, kSyncPauseCycles);
                        Access::Yield(cpu);
                        return true;
                    }
                    paused = false;
                }
                Run(rt, cpu, *e, block, target);
            } else if (e->fn && recompiled) {
                e->fn(cpu, target, e->start);
            } else {
                InterpretBlock(cpu, *e, target);
            }
            ran = true;
            if (rt.callHi) [[unlikely]] {
                const uint32_t to = Access::PC(cpu) & 0x07FFFFFF, from = pc & 0x07FFFFFF;
                if (to >= rt.callLo && to < rt.callHi && (from < rt.callLo || from >= rt.callHi)) {
                    auto &call = rt.callLog[{to, from}];
                    if (call.count++ == 0) {
                        const Access::Registers regs = Access::GetRegisters(cpu);
                        for (int i = 0; i < 4; i++) {
                            call.r[i] = regs.R[4 + i];
                        }
                        call.pr = Access::PR(cpu);
                    }
                }
            }
            if (rt.watchPC != ~0u) [[unlikely]] {
                rt.recent[rt.recentPos++ % 64] = {cpu.IsMaster(), pc, Access::PC(cpu), Access::SR(cpu), Access::PR(cpu),
                                                  Access::R15(cpu)};
            }
            if (rt.trace) [[unlikely]] {
                const uint64_t index = rt.traceCount++;
                if (rt.traceStartPC && (pc & 0x07FFFFFF) == rt.traceStartPC) {
                    // (SATRECOMP_TRACE_START_PC: full trace from the first time this block runs)
                    rt.traceFrom = index;
                    rt.traceTo += index;
                    rt.traceStartPC = 0;
                }
                Access::Trace(cpu, rt.trace, index, pc, rt.traceHash, index >= rt.traceFrom && index < rt.traceTo);
                if (index % 1000000 == 0) {
                    fprintf(rt.trace, "# %llu %016llx\n", (unsigned long long)index, (unsigned long long)rt.traceHash);
                }
            }
            if (Access::InDelaySlot(cpu)) {
                // The time slice ended between a branch and its delay slot instruction, or a bus wait held up
                // the slot instruction: the interpreter runs it. It takes instructions from a fetch buffer that
                // blocks don't keep up to date.
                if (Access::PC(cpu) & 2) {
                    cpu.RecompRefillPipeline();
                }
                return true;
            }
            if (Access::Cycles(cpu) >= target) {
                return true;
            }
        }
        // (an interrupt, a native BIOS routine or an instruction outside any block: not waiting)
        Access::SetWaiting(cpu, false);
        ++SATRECOMP_ACTIVITY;
        if (ran) {
            return true;
        }
        if (pc & 2) {
            cpu.RecompRefillPipeline(); // (as above)
        }
        return false;
    }

    std::string HexBytes(const uint8_t *p, size_t n) {
        static const char kHex[] = "0123456789ABCDEF";
        std::string s;
        for (size_t i = 0; i < n; i++) {
            s += kHex[p[i] >> 4];
            s += kHex[p[i] & 15];
        }
        return s;
    }

    // The block database is a text file with one block per line: address and code bytes in hex
    void SaveBlocks(Runtime &rt) {
        std::set<std::string> lines;
        if (FILE *f = fopen(rt.opt.recordPath.c_str(), "r")) {
            char buf[512];
            while (fgets(buf, sizeof(buf), f)) {
                std::string s = buf;
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
                    s.pop_back();
                }
                if (!s.empty() && s[0] != '#') {
                    lines.insert(s);
                }
            }
            fclose(f);
        }
        const size_t before = lines.size();
        for (auto &r : rt.regions) {
            for (size_t i = 0; i < r.table.size(); i++) {
                for (Entry *e = r.table[i]; e; e = e->next) {
                    if (!e->single && e->runs && e->start == 0 && !e->function && !e->lift) {
                        char addr[16];
                        snprintf(addr, sizeof(addr), "%08X ", r.base + static_cast<uint32_t>(i * 2));
                        lines.insert(addr + HexBytes(e->code, e->size));
                    }
                }
            }
        }
        size_t found = 0;
        for (const std::string &block : rt.explored) {
            // (address text, then the code bytes)
            found += lines.insert(block.substr(0, 9) + HexBytes(reinterpret_cast<const uint8_t *>(block.data()) + 9,
                                                                 block.size() - 9)).second;
        }
        if (found) {
            fprintf(stderr, "satrecomp: static discovery added %zu blocks that did not run\n", found);
        }
        FILE *f = fopen(rt.opt.recordPath.c_str(), "w");
        if (!f) {
            fprintf(stderr, "satrecomp: cannot write %s\n", rt.opt.recordPath.c_str());
            return;
        }
        fprintf(f, "# satrecomp block database: address, code bytes\n");
        for (auto &l : lines) {
            fprintf(f, "%s\n", l.c_str());
        }
        fclose(f);
        fprintf(stderr, "satrecomp: block database %s: %zu blocks (%zu new)\n", rt.opt.recordPath.c_str(), lines.size(),
                lines.size() - before);
    }

} // namespace

void Attach(Machine &saturn, const Options &options) {
    if (g_lifts.empty()) {
        RegisterGame();
    }
    g_rt = std::make_unique<Runtime>();
#ifdef SATRECOMP_OWN_MACHINE
    g_rt->machine = &saturn;
#endif
    Runtime &rt = *g_rt;
    rt.opt = options;
    const Memory memory = MemoryOf(saturn);
    rt.regions[0] = {0x06000000, memory.ramSize, memory.ramHigh, {}};
    rt.regions[1] = {0x00200000, memory.ramSize, memory.ramLow, {}};
    rt.regions[2] = {0x00000000, memory.romSize, memory.rom, {}};
    for (auto &r : rt.regions) {
        r.table.assign(r.size / 2, nullptr);
    }

    rt.hle = hle::Active();
    g_ramHigh = memory.ramHigh;
    g_ramLow = memory.ramLow;
    g_rom = memory.rom;
    {
        // Writes to work RAM that holds translated code are noticed from here on (SATRECOMP_NO_CODE_TRACKING=1:
        // compare code with memory at every entry instead, as before)
        const uint8_t *base = std::min(g_ramLow, g_ramHigh);
        const size_t size = static_cast<size_t>(std::max(g_ramLow, g_ramHigh) - base) + 0x100000;
        if (size <= 0x4000000 && !getenv("SATRECOMP_NO_CODE_TRACKING")) {
            rt.codeMap.assign((size + 63) >> 6, 0);
            rt.codeTracking = true;
            rt.codeAudit = getenv("SATRECOMP_VERIFY_CODE") != nullptr;
            SATRECOMP_CODE_BASE = base;
            SATRECOMP_CODE_MAP = rt.codeMap.data();
            SATRECOMP_CODE_SIZE = size;
        }
    }
    for (unsigned op = 0; op < 0x10000; op++) {
        g_nominal[op] = static_cast<uint8_t>(NominalCycles(Kind(static_cast<uint16_t>(op))));
    }

    // (SATRECOMP_NO_BLOCKS=1: the build's translations are left out, so that everything runs in the
    // interpreter: a test of the interpreter against the translated code)
    const bool noBlocks = getenv("SATRECOMP_NO_BLOCKS") != nullptr;
    for (size_t t = 0; t < g_blockTableCount && !noBlocks; t++) {
        for (size_t i = 0; i < g_blockTables[t].count; i++) {
            const BlockDesc &d = g_blockTables[t].blocks[i];
            uint32_t offset;
            Region *r = RegionFor(rt, d.pc, offset);
            uint64_t slots;
            if (!r || ScanBlock(d.code, d.numInstrs * 2u, slots, d.pc, &rt.opt.syncPoints) != d.numInstrs) {
                fprintf(stderr, "satrecomp: generated block %08X doesn't match this runtime's block rules\n", d.pc);
                continue;
            }
            AddBlock(rt, *r, offset, d.code, d.numInstrs, slots, d.fn, d.native, false, false);
            rt.builtBlocks++;
        }
    }

    if (const char *v = getenv("SATRECOMP_WATCH_WRITE")) {
        // (use with --cpu free: native code writes work RAM directly)
#ifndef SATRECOMP_OWN_MACHINE
        ymir::sh2::g_satrecompWatchWrite = static_cast<uint32_t>(strtoul(v, nullptr, 16));
#else
        (void)v;
#endif
    }
    rt.noRewrites = getenv("SATRECOMP_NO_REWRITES") != nullptr;
    if (const char *v = getenv("SATRECOMP_LIFT_CHECK")) {
        rt.liftCheckEvery = static_cast<unsigned>(std::max(1, atoi(v)));
    }
    if (const char *v = getenv("SATRECOMP_LOG_PC")) {
        rt.logPC = static_cast<uint32_t>(strtoul(v, nullptr, 16));
    }
    if (const char *v = getenv("SATRECOMP_WATCH_PC")) {
        rt.watchPC = static_cast<uint32_t>(strtoul(v, nullptr, 16));
    }
    if (const char *path = getenv("SATRECOMP_TRACE")) {
        rt.trace = fopen(path, "w");
        if (const char *v = getenv("SATRECOMP_TRACE_FROM")) {
            rt.traceFrom = strtoull(v, nullptr, 10);
        }
        if (const char *v = getenv("SATRECOMP_TRACE_START_PC")) {
            rt.traceStartPC = static_cast<uint32_t>(strtoul(v, nullptr, 16));
            rt.traceFrom = ~0ull;
        }
        if (const char *v = getenv("SATRECOMP_TRACE_TO")) {
            rt.traceTo = strtoull(v, nullptr, 10);
        }
    }

    if (const char *v = getenv("SATRECOMP_CALLS")) {
        sscanf(v, "%x-%x", &rt.callLo, &rt.callHi);
    }
    // Free running, the CPUs take turns at hardware events and where one waits for the other
    // (SATRECOMP_SHORT_SLICES=1: every 32 cycles, as before)
#ifndef SATRECOMP_OWN_MACHINE
    ymir::sh2::g_satrecompFreeRunning =
        (rt.opt.mode == Mode::Native || rt.opt.mode == Mode::Free) && !getenv("SATRECOMP_SHORT_SLICES");
#endif

    // Calls as calls: when free running and nothing wants to see every block as it is entered
    // (SATRECOMP_NO_CALLS=1: every call through the dispatcher, as before)
    rt.link = (rt.opt.mode == Mode::Native || rt.opt.mode == Mode::Free) && rt.opt.recordPath.empty() && !rt.trace && rt.watchPC == ~0u &&
              !rt.logPC && !rt.callHi && !getenv("SATRECOMP_NO_CALLS");

    // Native functions of several blocks: an entry of their own at their first block, found before the block's
    if (!getenv("SATRECOMP_NO_FUNCTIONS") && !noBlocks) {
        for (size_t t = 0; t < g_blockTableCount; t++) {
            for (size_t i = 0; i < g_blockTables[t].functionCount; i++) {
                const FunctionDesc &f = g_blockTables[t].functions[i];
                uint32_t offset;
                Region *r = RegionFor(rt, f.pc, offset);
                uint64_t slots;
                if (!r || ScanBlock(f.blocks[0].code, f.blocks[0].size, slots, f.pc, &rt.opt.syncPoints) !=
                              f.blocks[0].size / 2u) {
                    continue;
                }
                Entry &e = rt.entries.emplace_back();
                e = {};
                e.code = f.blocks[0].code;
                e.size = f.blocks[0].size;
                e.native = f.fn;
                e.function = &f;
                for (unsigned b = 0; b < f.numBlocks; b++) {
                    MarkCode(rt, *r, static_cast<int64_t>(offset) + f.blocks[b].offset, f.blocks[b].size);
                }
                e.next = r->table[offset >> 1];
                r->table[offset >> 1] = &e;
                rt.builtFunctions++;
                // The function can be entered at any of its blocks (not while recording: blocks are then
                // followed one by one)
                for (unsigned b = 1; b < f.numBlocks && rt.opt.recordPath.empty(); b++) {
                    const int64_t at = static_cast<int64_t>(offset) + f.blocks[b].offset;
                    if (at < 0 || at + f.blocks[b].size > r->size) {
                        continue;
                    }
                    Entry &m = rt.entries.emplace_back();
                    m = {};
                    m.code = f.blocks[b].code;
                    m.size = f.blocks[b].size;
                    m.native = f.fn;
                    m.function = &f;
                    m.head = &e;
                    m.block = static_cast<uint16_t>(b);
                    m.next = r->table[at >> 1];
                    r->table[at >> 1] = &m;
                }
            }
        }
    }

    // Lifted routines: found before anything else at their address
    if (!getenv("SATRECOMP_NO_LIFTS")) {
        for (const Lift &lift : g_lifts) {
            uint32_t offset;
            if (Region *r = RegionFor(rt, lift.address, offset)) {
                Entry &e = rt.entries.emplace_back();
                e = {};
                e.size = static_cast<uint16_t>(lift.size);
                e.lift = &lift;
                e.next = r->table[offset >> 1];
                r->table[offset >> 1] = &e;
            }
        }
    }

    saturn.masterSH2.SetRecompDispatcher(Dispatch, &rt);
    saturn.slaveSH2.SetRecompDispatcher(Dispatch, &rt);
}

// --- lifted routines (lift.h) ---------------------------------------------------------------------------

uint32_t &Guest::R(unsigned n) {
    return Access::R(m_cpu)[n];
}
uint32_t Guest::GBR() const {
    return Access::GetRegisters(m_cpu).GBR;
}
uint32_t Guest::PR() const {
    return Access::PR(m_cpu);
}
uint8_t Guest::Read8(uint32_t address) {
    return Access::Read8(m_cpu, address);
}
uint16_t Guest::Read16(uint32_t address) {
    return Access::Read16(m_cpu, address);
}
uint32_t Guest::Read32(uint32_t address) {
    return Access::Read32(m_cpu, address);
}
void Guest::Write8(uint32_t address, uint8_t value) {
    Access::Write8(m_cpu, address, value);
}
void Guest::Write16(uint32_t address, uint16_t value) {
    Access::Write16(m_cpu, address, value);
}
void Guest::Write32(uint32_t address, uint32_t value) {
    Access::Write32(m_cpu, address, value);
}
std::string Guest::String(uint32_t address, size_t max) {
    std::string s;
    for (size_t i = 0; i < max; i++) {
        const char ch = static_cast<char>(Read8(address + static_cast<uint32_t>(i)));
        if (!ch) {
            break;
        }
        s += ch;
    }
    return s;
}
bool Call(SH2 &cpu, uint64_t target, int retBlock, uint32_t ret) {
    constexpr unsigned kMaxDepth = 128;
    constexpr size_t kMaxFrames = 256;
    Runtime &rt = *g_rt;
    Runtime::Linking &lk = rt.linking[cpu.IsMaster() ? 0 : 1];
    const uint32_t pc = Access::PC(cpu);
    uint32_t offset;
    Region *r;
    if (!rt.link || Access::InterruptRaised(cpu) || Access::Cycles(cpu) >= target || lk.depth >= kMaxDepth || (pc & 1) ||
        !(r = RegionFor(rt, pc, offset)) || r == &rt.regions[2]) {
        return false;
    }
    // The translation of the code that is at the callee's address now: found as the dispatcher finds it. Not
    // where a hand-written routine or a sync point is: those are the dispatcher's business.
    Entry *e;
    unsigned block = 0;
    for (e = r->table[offset >> 1]; e; e = e->next) {
        if (e->lift) {
            return false;
        }
        if (e->start != 0) {
            continue;
        }
        if (e->head) {
            const int64_t start = static_cast<int64_t>(offset) - e->function->blocks[e->block].offset;
            if (start >= 0 && Matches(rt, *r, static_cast<uint32_t>(start), *e->head)) {
                block = e->block;
                e = e->head;
                break;
            }
        } else if (Matches(rt, *r, offset, *e)) {
            break;
        }
    }
    if (!e || !e->native || e->single || e->sync) {
        rt.callsNotLinked++;
        return false;
    }
    Entry *caller = lk.current;
    rt.callsNoBlock += retBlock < 0;
    const bool comesBack = retBlock >= 0 && caller != nullptr;
    if (comesBack) {
        if (lk.frames.size() >= kMaxFrames) {
            lk.frames.erase(lk.frames.begin());
        }
        lk.frames.push_back({caller, static_cast<uint32_t>(retBlock), ret});
    }
    const size_t mark = lk.frames.size();
    e->runs++;
    rt.calls++;
    lk.depth++;
    Run(rt, cpu, *e, block, target);
    lk.depth--;
    if (!comesBack || lk.stopped.entry || Access::InDelaySlot(cpu) || Access::InterruptRaised(cpu) || Access::Cycles(cpu) >= target ||
        Access::PC(cpu) != ret) {
        return false; // (the caller's frame stays: it goes on when control arrives at `ret`)
    }
    // Back where the caller expects it: the caller goes on, if its frame is still there and its code is
    // still what it was (calls made further in that never came back are forgotten)
    if (mark == 0 || lk.frames.size() < mark) {
        return false;
    }
    const Runtime::Frame frame = lk.frames[mark - 1];
    if (frame.entry != caller || frame.block != static_cast<uint32_t>(retBlock) || frame.pc != ret || !FrameValid(rt, frame)) {
        return false;
    }
    lk.frames.resize(mark - 1);
    rt.callsReturned++;
    return true;
}

void CodeChanged() {
    ++SATRECOMP_CODE_EPOCH;
}
void Guest::CopyIn(uint32_t address, const uint8_t *data, size_t size) {
    size_t i = 0;
    // Work RAM directly; anything else (video RAM, sound RAM) through the hardware layer, in words where the
    // address allows
    if (uint8_t *p = Native::WritePtr(address); p && Native::WritePtr(address + static_cast<uint32_t>(size) - 1)) {
        memcpy(p, data, size);
        CodeChanged();
        return;
    }
    if ((address & 1) && size) {
        Write8(address, data[0]);
        i = 1;
    }
    for (; i + 2 <= size; i += 2) {
        Write16(address + static_cast<uint32_t>(i), static_cast<uint16_t>((data[i] << 8) | data[i + 1]));
    }
    if (i < size) {
        Write8(address + static_cast<uint32_t>(i), data[i]);
    }
}
void Guest::Call(uint32_t address) {
#ifdef SATRECOMP_OWN_MACHINE
    // (An address no code is at stands for "back in the caller")
    constexpr uint32_t kBack = 0x0FFFFFF0;
    Runtime &rt = *g_rt;
    const uint32_t pr = Access::PR(m_cpu), pc = Access::PC(m_cpu);
    Access::SetPR(m_cpu, kBack);
    Access::SetPC(m_cpu, address);
    rt.inCall++;
    const uint64_t cyclesAtCall = Access::Cycles(m_cpu);
    for (uint32_t steps = 0;; steps++) {
        m_cpu.m_intrFlags.pending = false;
        // (back at the address only this call returns to; the stack is not looked at: some of this game's
        // routines come back with it elsewhere and have their caller put it right)
        if (Access::PC(m_cpu) == kBack && !Access::InDelaySlot(m_cpu)) {
            break;
        }
        // The routine runs as the rest of the game does: translated code, and the rewrites of what it calls
        // (while a rewrite is being checked: all by the interpreter, as the game's side of the check is)
        if (rt.inCheck || Access::InDelaySlot(m_cpu) || !Dispatch(m_cpu, Access::Cycles(m_cpu) + (1ull << 40), &rt)) {
            Access::AddCycles(m_cpu, m_cpu.RecompInterpretNext());
        }
        // (No time passes for the rest of the machine during a call: a routine that waits for an interrupt, the
        // other CPU or the disc never comes back, and cannot be called this way)
        if (Access::Cycles(m_cpu) - cyclesAtCall > 100'000'000 || steps == 50'000'000) {
            fprintf(stderr, "Guest::Call(%08X): the routine did not come back (does it wait for something?)\n", address);
            abort();
        }
    }
    rt.inCall--;
    m_cpu.RecalcInterrupts();
    Access::SetPR(m_cpu, pr);
    Access::SetPC(m_cpu, pc);
#else
    fprintf(stderr, "Guest::Call(%08X): only on satrecomp's own machine\n", address);
    abort();
#endif
}
void Guest::Return(uint64_t cycles) {
    Access::SetPC(m_cpu, Access::PR(m_cpu));
    Access::AddCycles(m_cpu, cycles);
}

uint64_t LiftHash(const uint8_t *data, size_t size) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < size; i++) {
        h = (h ^ data[i]) * 0x100000001B3ull;
    }
    return h;
}

void RegisterLifts(const Lift *lifts, size_t count) {
    g_lifts.insert(g_lifts.end(), lifts, lifts + count);
}

void SetDiscFileReader(DiscFileReader reader) {
    g_discFileReader = std::move(reader);
}

bool ReadDiscFile(const std::string &name, std::vector<uint8_t> &data) {
    return g_discFileReader && g_discFileReader(name, data);
}

void SetDiscSectorReader(DiscSectorReader reader) {
    g_discSectorReader = std::move(reader);
}
bool ReadDiscSector(uint32_t frameAddress, uint8_t *out) {
    return g_discSectorReader && g_discSectorReader(frameAddress, out);
}
void SetPadReader(PadReader reader) {
    g_padReader = std::move(reader);
}

uint16_t ReadPad(int player) {
    return g_padReader ? g_padReader(player) : 0;
}

#ifdef SATRECOMP_OWN_MACHINE
// (in the reference machine's format, so that the two tables can be compared: the kinds of all opcodes, the
// same for delay slots, the nominal cycles)
bool DumpDecodeTable(const std::string &path) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    const unsigned kinds = static_cast<unsigned>(machine::Op::Illegal) + 1;
    for (int slot = 0; slot < 2; slot++) {
        for (unsigned op = 0; op < 0x10000; op++) {
            const machine::Op kind = machine::g_opKinds[op];
            unsigned v = static_cast<unsigned>(kind);
            if (slot) {
                v = machine::IllegalInSlot(kind) ? kinds * 2 - 2 : v + kinds;
            }
            const uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
            fwrite(b, 1, 2, f);
        }
    }
    for (unsigned op = 0; op < 0x10000; op++) {
        fputc(machine::g_opCycles[op], f);
    }
    fclose(f);
    return true;
}
#else
bool DumpDecodeTable(const std::string &path) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    for (auto &table : DecodeTable::s_instance.opcodes) {
        for (OpcodeType t : table) {
            const uint16_t v = static_cast<uint16_t>(t);
            const uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
            fwrite(b, 1, 2, f);
        }
    }
    for (OpcodeType t : DecodeTable::s_instance.opcodes[0]) {
        fputc(static_cast<int>(NominalCycles(t)), f);
    }
    DumpM68KDecodeTable(f);
    fclose(f);
    return true;
}

#endif

void StateLoaded() {
    if (!g_rt) {
        return;
    }
    for (int cpu = 0; cpu < 2; cpu++) {
        g_rt->linking[cpu] = {};
        g_rt->waiting[cpu] = {};
        g_rt->syncPaused[cpu] = false;
    }
    ++SATRECOMP_CODE_EPOCH;
}

namespace {
    struct KeptDispatcher {
        Runtime::Waiting waiting[2];
        bool syncPaused[2] = {false, false};
        Runtime::Linking linking[2];
        uint64_t activity = 0;
    } g_keptDispatcher;
} // namespace

void KeepDispatcher() {
    if (!g_rt) {
        return;
    }
    for (int cpu = 0; cpu < 2; cpu++) {
        g_keptDispatcher.linking[cpu] = g_rt->linking[cpu];
        g_keptDispatcher.waiting[cpu] = g_rt->waiting[cpu];
        g_keptDispatcher.syncPaused[cpu] = g_rt->syncPaused[cpu];
    }
    g_keptDispatcher.activity = SATRECOMP_ACTIVITY;
}

void PutDispatcherBack() {
    if (!g_rt) {
        return;
    }
    for (int cpu = 0; cpu < 2; cpu++) {
        g_rt->linking[cpu] = g_keptDispatcher.linking[cpu];
        g_rt->waiting[cpu] = g_keptDispatcher.waiting[cpu];
        g_rt->syncPaused[cpu] = g_keptDispatcher.syncPaused[cpu];
    }
    // (the activity count as it was, so that loops known to be waiting are as they were; the code is compared
    // again, as memory was put back without writes being seen)
    SATRECOMP_ACTIVITY = g_keptDispatcher.activity;
}

void ResetRunCounts() {
    if (g_rt) {
        for (auto &e : g_rt->entries) {
            e.runs = 0;
        }
    }
}

void Finish() {
    if (g_rt && g_rt->callHi) {
        for (auto &[key, call] : g_rt->callLog) {
            fprintf(stderr, "call %08X from block %08X (pr %08X): %llu times, first r4-r7 %08X %08X %08X %08X\n", key.first, key.second,
                    call.pr, static_cast<unsigned long long>(call.count), call.r[0], call.r[1], call.r[2], call.r[3]);
        }
    }
    if (!g_rt) {
        return;
    }
    Runtime &rt = *g_rt;
    // SATRECOMP_LIFT_COVERAGE=path: what the compared calls reached is added to what earlier runs wrote there
    // (so several runs, with different movies and pokes, can cover a routine between them)
    const char *coveragePath = rt.liftCheckEvery ? getenv("SATRECOMP_LIFT_COVERAGE") : nullptr;
    if (coveragePath) {
        std::map<std::string, std::string> earlier;
        std::map<std::string, uint32_t> addresses;
        if (FILE *f = fopen(coveragePath, "r")) {
            char name[128], bits[8192];
            unsigned address;
            while (fscanf(f, " %127[^\t]\t%x\t%8191s", name, &address, bits) == 3) {
                earlier[name] = bits;
                addresses[name] = address;
            }
            fclose(f);
        }
        for (auto &[lift, check] : rt.liftChecks) {
            const std::string &bits = earlier[lift->name];
            check.reached.resize(lift->size / 2);
            for (size_t i = 0; i < check.reached.size() && i < bits.size(); i++) {
                check.reached[i] = check.reached[i] || bits[i] == '1';
            }
            std::string now;
            for (const bool reached : check.reached) {
                now += reached ? '1' : '0';
            }
            earlier[lift->name] = now;
            addresses[lift->name] = lift->address;
        }
        if (FILE *f = fopen(coveragePath, "w")) {
            for (const auto &[name, bits] : earlier) {
                fprintf(f, "%s\t%08X\t%s\n", name.c_str(), addresses[name], bits.c_str());
            }
            fclose(f);
        }
    }
    for (const auto &[lift, check] : rt.liftChecks) {
        if (rt.liftCheckEvery) {
            fprintf(stderr, "rewrite %s: %llu calls, %llu compared with the game's code: %llu differ; %llu could not be compared; in %llu only "
                            "registers no caller reads differ; %llu writes to hardware compared\n",
                    lift->name, static_cast<unsigned long long>(check.calls), static_cast<unsigned long long>(check.checked),
                    static_cast<unsigned long long>(check.different), static_cast<unsigned long long>(check.notComparable),
                    static_cast<unsigned long long>(check.scratchDifferent), static_cast<unsigned long long>(check.hardwareWrites));
            // The parts of the game's routine no compared call went through: what the rewrite does in their
            // place has not been checked (data among the code, such as its constants, is listed too)
            std::string notReached;
            size_t count = 0;
            for (size_t i = 0; i < check.reached.size(); i++) {
                if (check.reached[i]) {
                    continue;
                }
                size_t end = i;
                while (end + 1 < check.reached.size() && !check.reached[end + 1]) {
                    end++;
                }
                char text[40];
                snprintf(text, sizeof(text), " %08X-%08X", lift->address + static_cast<uint32_t>(i * 2), lift->address + static_cast<uint32_t>(end * 2));
                notReached += text;
                count += end - i + 1;
                i = end;
            }
            if (count) {
                fprintf(stderr, "    not reached in the game's routine%s (%zu of %zu words):%s\n", coveragePath ? " in any run so far" : "", count,
                        check.reached.size(), notReached.c_str());
            }
        }
    }
    // Debugging aid (SATRECOMP_LIST_BELOW=hex): the blocks below this address that ran, e.g. BIOS code
    if (const char *v = getenv("SATRECOMP_LIST_BELOW")) {
        const uint32_t limit = static_cast<uint32_t>(strtoul(v, nullptr, 16));
        for (auto &r : rt.regions) {
            for (size_t i = 0; i < r.table.size(); i++) {
                for (Entry *e = r.table[i]; e; e = e->next) {
                    const uint32_t pc = r.base + static_cast<uint32_t>(i * 2);
                    if (e->runs && pc < limit && e->start == 0) {
                        fprintf(stderr, "ran %08X x %llu (%u bytes)%s\n", pc, (unsigned long long)e->runs, e->size,
                                e->function ? " function" : "");
                    }
                }
            }
        }
    }
    if (!rt.opt.recordPath.empty()) {
        SaveBlocks(rt);
    }
    if (rt.opt.stats) {
        // Counted in dispatches (entries into a block), not instructions
        uint64_t recompiled = 0, interpreted = 0, single = 0, lifted = 0;
        for (auto &e : rt.entries) {
            if (e.single) {
                single += e.runs;
            } else if (e.lift) {
                lifted += e.runs;
            } else if ((e.fn && rt.opt.mode == Mode::Recompiled) || (e.native && rt.opt.mode == Mode::Native)) {
                recompiled += e.runs;
            } else {
                interpreted += e.runs;
            }
        }
        // The most run blocks that the interpreter ran
        std::vector<std::pair<uint64_t, uint32_t>> top;
        for (auto &r : rt.regions) {
            for (size_t i = 0; i < r.table.size(); i++) {
                for (Entry *e = r.table[i]; e; e = e->next) {
                    const bool own = e->lift || (e->fn && rt.opt.mode == Mode::Recompiled) ||
                                     (e->native && rt.opt.mode == Mode::Native);
                    if (e->runs && !own) {
                        top.emplace_back(e->runs, r.base + static_cast<uint32_t>(i * 2));
                    }
                }
            }
        }
        std::sort(top.rbegin(), top.rend());
        for (size_t i = 0; i < top.size() && i < 8; i++) {
            fprintf(stderr, "satrecomp: interpreted: %08X x %llu\n", top[i].second, (unsigned long long)top[i].first);
        }
        const uint64_t total = recompiled + interpreted + single;
        fprintf(stderr,
                "satrecomp: calls made as calls %llu (%llu came straight back to their caller, %llu callers taken up later; %llu "
                "from code with no block of its own function to come back to), %llu not: no translation to call; "
                "functions taken up where they stopped %llu\n"
                "satrecomp: code compared with memory %llu times; code memory written %llu times%s\n"
                "satrecomp: blocks in build %llu (and %llu functions of several blocks), found this run %llu\n"
                "satrecomp: block runs: recompiled %llu (%.2f%%), interpreted %llu (%.2f%%), "
                "single-stepped %llu, lifted routines run %llu times\n",
                static_cast<unsigned long long>(rt.calls), static_cast<unsigned long long>(rt.callsReturned),
                static_cast<unsigned long long>(rt.framesResumed), static_cast<unsigned long long>(rt.callsNoBlock),
                static_cast<unsigned long long>(rt.callsNotLinked), static_cast<unsigned long long>(rt.stopsResumed),
                static_cast<unsigned long long>(rt.compares), static_cast<unsigned long long>(SATRECOMP_CODE_EPOCH - 1),
                rt.codeAudit ? (rt.auditWrong ? " (AUDIT: changes went unnoticed)" : " (audit: none went unnoticed)") : "",
                static_cast<unsigned long long>(rt.builtBlocks), static_cast<unsigned long long>(rt.builtFunctions),
                static_cast<unsigned long long>(rt.freshBlocks),
                static_cast<unsigned long long>(recompiled), total ? 100.0 * recompiled / total : 0.0,
                static_cast<unsigned long long>(interpreted), total ? 100.0 * interpreted / total : 0.0,
                static_cast<unsigned long long>(single), static_cast<unsigned long long>(lifted));
    }
    if (rt.trace) {
        fclose(rt.trace);
    }
    g_rt.reset();
}

} // namespace satrecomp
