// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// Sound CPU (MC68EC000) recompilation: block lookup, dispatch and recording.
//
// The sound driver runs as blocks: runs of instructions as they were executed once, identified by the address
// of the first and the opcode word of each. A recompiled block is the interpreter's handlers called with
// constant opcodes, with the interpreter's checks between them, so the sound CPU stays cycle for cycle
// identical to the interpreter and the sound it makes is the same sample for sample (`make verify KIND=exact`
// compares it with the unmodified interpreter).
#include "m68k_access.h"
#include "m68k_blocks.h"
#include "recomp.h"

#include <ymir/hw/m68k/m68k_decode.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace satrecomp {

using ymir::m68k::MC68EC000;


namespace {

    constexpr uint32_t kRamSize = 0x80000; // sound RAM, at address 0 of the sound CPU

    struct Entry {
        std::vector<M68KInstr> own; // (for blocks found during this run)
        const M68KInstr *instrs;
        M68KBlockFn fn;
        Entry *next;
        uint64_t runs;
        uint16_t numInstrs;
    };

    struct Runtime {
        const uint8_t *ram = nullptr;
        std::vector<Entry *> table; // by (address / 2)
        std::deque<Entry> entries;
        std::string recordPath;
        bool stats = false;
        uint64_t built = 0, fresh = 0;
    };
    std::unique_ptr<Runtime> g;

    inline uint16_t Word(const uint8_t *ram, uint32_t address) {
        return static_cast<uint16_t>((ram[address] << 8) | ram[address + 1]);
    }

    inline bool Matches(const Runtime &rt, const Entry &e, uint32_t address) {
        for (unsigned i = 0; i < e.numInstrs; i++) {
            const uint32_t at = address + e.instrs[i].offset;
            if (at + 2 > kRamSize || Word(rt.ram, at) != e.instrs[i].opcode) {
                return false;
            }
        }
        return true;
    }

    Entry &Add(Runtime &rt, uint32_t address, const M68KInstr *instrs, unsigned count, M68KBlockFn fn) {
        Entry &e = rt.entries.emplace_back();
        e.instrs = instrs;
        e.numInstrs = static_cast<uint16_t>(count);
        e.fn = fn;
        e.runs = 0;
        e.next = rt.table[address >> 1];
        rt.table[address >> 1] = &e;
        return e;
    }

    // The interpreter running a block that the build doesn't have, leaving it where generated code does
    uint64_t Interpret(MC68EC000 &cpu, const Entry &e, uint32_t address, uint64_t budget) {
        uint64_t cy = 0;
        for (unsigned i = 0;; i++) {
            cy += cpu.RecompInterpretNext();
            if (i + 1 >= e.numInstrs || cy >= budget || M68KAccess::PC(cpu) - 4 != address + e.instrs[i + 1].offset) {
                return cy;
            }
        }
    }

    // Code reached for the first time: run it in the interpreter and note the instructions as they come, up
    // to the first that doesn't simply follow the one before
    uint64_t Record(Runtime &rt, MC68EC000 &cpu, uint32_t address, uint64_t budget) {
        std::vector<M68KInstr> instrs;
        uint64_t cy = 0;
        uint32_t at = address;
        for (;;) {
            instrs.push_back({static_cast<uint16_t>(at - address), Word(rt.ram, at)});
            cy += cpu.RecompInterpretNext();
            const uint32_t next = M68KAccess::PC(cpu) - 4;
            // (an instruction is at most 10 bytes; anything else is a branch, a call, a return or an interrupt)
            if (next <= at || next - at > 10 || next + 2 > kRamSize || (next & 1) || instrs.size() >= kMaxM68KBlockInstrs ||
                cy >= budget) {
                break;
            }
            at = next;
        }
        Entry &e = Add(rt, address, nullptr, static_cast<unsigned>(instrs.size()), nullptr);
        e.own = std::move(instrs);
        e.instrs = e.own.data();
        e.runs = 1;
        rt.fresh++;
        return cy;
    }

    uint64_t Dispatch(MC68EC000 &cpu, uint64_t budget, void *ctx) {
        Runtime &rt = *static_cast<Runtime *>(ctx);
        const uint32_t address = M68KAccess::PC(cpu) - 4;
        if (address >= kRamSize - 2 || (address & 1)) {
            return cpu.RecompInterpretNext();
        }
        for (Entry *e = rt.table[address >> 1]; e; e = e->next) {
            if (Matches(rt, *e, address)) {
                e->runs++;
                return e->fn ? e->fn(cpu, budget) : Interpret(cpu, *e, address, budget);
            }
        }
        return Record(rt, cpu, address, budget);
    }

} // namespace

void AttachM68K(ymir::Saturn &saturn, const std::string &recordPath, bool stats) {
    g = std::make_unique<Runtime>();
    Runtime &rt = *g;
    rt.ram = saturn.SCSP.GetWRAMData();
    rt.table.assign(kRamSize / 2, nullptr);
    rt.recordPath = recordPath;
    rt.stats = stats;
    for (size_t t = 0; t < g_m68kBlockTableCount; t++) {
        for (size_t i = 0; i < g_m68kBlockTables[t].count; i++) {
            const M68KBlockDesc &d = g_m68kBlockTables[t].blocks[i];
            if (d.address < kRamSize && !(d.address & 1)) {
                Add(rt, d.address, d.instrs, d.numInstrs, d.fn);
                rt.built++;
            }
        }
    }
    saturn.SCSP.GetM68K().SetRecompDispatcher(Dispatch, &rt);
}

void FinishM68K() {
    if (!g) {
        return;
    }
    Runtime &rt = *g;
    if (!rt.recordPath.empty()) {
        // The sound CPU's block database: address, then offset:opcode for each instruction
        std::set<std::string> lines;
        if (FILE *f = fopen(rt.recordPath.c_str(), "r")) {
            char buf[1024];
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
        for (size_t i = 0; i < rt.table.size(); i++) {
            for (Entry *e = rt.table[i]; e; e = e->next) {
                if (!e->runs) {
                    continue;
                }
                char text[32];
                snprintf(text, sizeof(text), "%06X", static_cast<unsigned>(i * 2));
                std::string line = text;
                for (unsigned k = 0; k < e->numInstrs; k++) {
                    snprintf(text, sizeof(text), " %X:%04X", e->instrs[k].offset, e->instrs[k].opcode);
                    line += text;
                }
                lines.insert(line);
            }
        }
        if (FILE *f = fopen(rt.recordPath.c_str(), "w")) {
            fprintf(f, "# satrecomp sound CPU block database: address, offset:opcode of each instruction\n");
            for (auto &l : lines) {
                fprintf(f, "%s\n", l.c_str());
            }
            fclose(f);
            fprintf(stderr, "satrecomp: sound CPU block database %s: %zu blocks (%zu new)\n", rt.recordPath.c_str(),
                    lines.size(), lines.size() - before);
        }
    }
    if (rt.stats) {
        uint64_t recompiled = 0, interpreted = 0;
        for (auto &e : rt.entries) {
            (e.fn ? recompiled : interpreted) += e.runs;
        }
        const uint64_t total = recompiled + interpreted;
        fprintf(stderr, "satrecomp: sound CPU: blocks in build %llu, found this run %llu; block runs recompiled %llu (%.2f%%)\n",
                static_cast<unsigned long long>(rt.built), static_cast<unsigned long long>(rt.fresh),
                static_cast<unsigned long long>(recompiled), total ? 100.0 * recompiled / total : 0.0);
    }
    g.reset();
}

bool DumpM68KDecodeTable(FILE *f) {
    for (auto t : ymir::m68k::g_decodeTable.opcodeTypes) {
        fputc(static_cast<int>(t), f);
    }
    return true;
}

} // namespace satrecomp
