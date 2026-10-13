// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// The CD drive's host interface, as far as the game still uses it. Files, the opening movie's data and CD
// music are read by the port itself (games/<id>/src); what is left are the commands the game's CD library
// gives when it starts (and when the movie player does): status, table of contents, set-up of the buffer's
// filters, seeks, and a "play" that only has to be seen to be going.
#include "machine.h"

#include <algorithm>
#include <cstdio>
#include <iterator>

namespace satrecomp::machine {

namespace {
    // Interrupt request bits (HIRQ)
    enum : uint16 {
        kCMOK = 0x0001, // command accepted, response ready
        kDRDY = 0x0002, // data ready at the data port
        kCSCT = 0x0004, // a sector was read
        kBFUL = 0x0008, // the buffer is full
        kPEND = 0x0010, // the end of what was to be played
        kDCHG = 0x0020, // the disc was changed
        kESEL = 0x0040, // selector set-up done
        kEHST = 0x0080, // data transfer ended
        kEFLS = 0x0200, // file system command done
        kSCDQ = 0x0400, // subcode read
    };
    constexpr uint32 kBufferSectors = 200;
} // namespace

void Machine::CdReset() {
    m_cd = {};
    m_cd.periodicDue = m_now + m_clock / 75;
}

uint16 Machine::CdRead(uint32 address) {
    auto &cd = m_cd;
    if (address >= 0x98000 || (address & 0xFFFF) < 0x08) { // the data port
        return cd.transferOffset < cd.transferSize ? cd.transfer[cd.transferOffset++] : 0xFFFF;
    }
    switch (address & 0x3C) {
    case 0x08: return cd.HIRQ;
    case 0x0C: return cd.HIRQMask;
    case 0x18: return cd.CR[0];
    case 0x1C: return cd.CR[1];
    case 0x20: return cd.CR[2];
    case 0x24:
        cd.responseTaken = true; // (from now on the drive reports its status by itself again)
        return cd.CR[3];
    default: return 0;
    }
}

void Machine::CdWrite(uint32 address, uint16 value) {
    auto &cd = m_cd;
    switch (address & 0x3C) {
    case 0x08: cd.HIRQ &= value; break; // (writing 0 to a bit clears it)
    case 0x0C: cd.HIRQMask = value; break;
    case 0x18: cd.command[0] = value; break;
    case 0x1C: cd.command[1] = value; break;
    case 0x20: cd.command[2] = value; break;
    case 0x24:
        cd.command[3] = value;
        CdCommand();
        break;
    default: break;
    }
}

// The usual response: status, what is under the pickup (track 1's control bits, track and index), where
void Machine::CdReport(uint8 status) {
    auto &cd = m_cd;
    if (cd.status == 0x02) { // (stopped: no position)
        cd.CR[0] = static_cast<uint16>((status << 8) | 0xFF);
        cd.CR[1] = cd.CR[2] = cd.CR[3] = 0xFFFF;
        return;
    }
    uint8 track = 1, control = 0x41;
    for (const Disc::Track &t : m_disc.tracks) {
        if (cd.fad >= t.startFAD) {
            track = t.number;
            control = static_cast<uint8>((t.control << 4) | 1);
        }
    }
    cd.CR[0] = static_cast<uint16>((status << 8) | 0x80);
    cd.CR[1] = static_cast<uint16>((control << 8) | track);
    cd.CR[2] = static_cast<uint16>(0x0100 | ((cd.fad >> 16) & 0xFF));
    cd.CR[3] = static_cast<uint16>(cd.fad);
}

void Machine::CdCommand() {
    auto &cd = m_cd;
    const uint8 command = cd.command[0] >> 8;
    cdCommands[command]++;
    ++g_activity;
    uint16 flags = kCMOK;
    switch (command) {
    case 0x00: CdReport(cd.status); break; // status
    case 0x02: { // table of contents: 102 long words at the data port
        std::fill(std::begin(cd.transfer), std::end(cd.transfer), 0xFFFF);
        cd.transferSize = 204;
        auto entry = [&](size_t index, uint32 value) {
            cd.transfer[index * 2] = static_cast<uint16>(value >> 16);
            cd.transfer[index * 2 + 1] = static_cast<uint16>(value);
        };
        uint8 first = 1, last = 1, firstControl = 0x41, lastControl = 0x41;
        for (size_t i = 0; i < m_disc.tracks.size(); i++) {
            const Disc::Track &t = m_disc.tracks[i];
            const uint8 control = static_cast<uint8>((t.control << 4) | 1);
            if (t.number >= 1 && t.number <= 99) {
                entry(t.number - 1u, (static_cast<uint32>(control) << 24) | t.startFAD);
            }
            if (i == 0) {
                first = t.number;
                firstControl = control;
            }
            last = t.number;
            lastControl = control;
        }
        entry(99, (static_cast<uint32>(firstControl) << 24) | (static_cast<uint32>(first) << 16));
        entry(100, (static_cast<uint32>(lastControl) << 24) | (static_cast<uint32>(last) << 16));
        entry(101, (static_cast<uint32>(lastControl) << 24) | m_disc.leadOutFAD);
        cd.transferOffset = 0;
        cd.CR[0] = static_cast<uint16>(cd.status << 8);
        cd.CR[1] = 204;
        cd.CR[2] = cd.CR[3] = 0;
        flags |= kDRDY;
        break;
    }
    case 0x03: // session information
        cd.CR[0] = static_cast<uint16>(cd.status << 8);
        cd.CR[1] = 0;
        cd.CR[2] = 0x0100;
        cd.CR[3] = 0;
        break;
    case 0x04: // initialise
        CdReport(cd.status);
        flags |= kESEL;
        break;
    case 0x06: // end of a data transfer: how much was taken
        if (cd.transferSize == 0) {
            cd.CR[0] = static_cast<uint16>((cd.status << 8) | 0xFF);
            cd.CR[1] = 0xFFFF;
        } else {
            cd.CR[0] = static_cast<uint16>(cd.status << 8);
            cd.CR[1] = static_cast<uint16>(cd.transferOffset);
        }
        cd.CR[2] = cd.CR[3] = 0;
        cd.transferSize = 0;
        cd.transferOffset = 0;
        flags |= kEHST;
        break;
    case 0x10: { // play from a frame address, for a number of sectors
        const uint32 start = ((cd.command[0] & 0xFF) << 16) | cd.command[1];
        const uint32 end = ((cd.command[2] & 0xFF) << 16) | cd.command[3];
        if (start != 0xFFFFFF) {
            cd.fad = start & 0x7FFFFF;
        }
        cd.playEnd = (end & 0x800000) ? cd.fad + (end & 0x7FFFFF) : m_disc.leadOutFAD;
        cd.status = 0x03;
        cd.reading = true;
        CdReport(0x04); // (it says "seeking" first)
        break;
    }
    case 0x11: { // seek: to a frame address and pause, or stop, or pause where it is
        const uint32 target = ((cd.command[0] & 0xFF) << 16) | cd.command[1];
        cd.reading = false;
        if (target == 0xFFFFFF) {
            cd.status = 0x01;
        } else if (target & 0x800000) {
            cd.fad = target & 0x7FFFFF;
            cd.status = 0x01;
        } else if (target == 0) {
            cd.status = 0x02;
        } else {
            cd.status = 0x01;
        }
        CdReport(cd.status);
        break;
    }
    case 0x30: // which filter the drive's sectors go to
    case 0x42: // a filter's subheader conditions, mode and outputs
    case 0x44:
    case 0x46:
    case 0x60: // the sector length
        CdReport(cd.status);
        flags |= kESEL;
        break;
    case 0x48: // reset the selector: the buffer is emptied
        cd.buffered = 0;
        cd.HIRQ &= ~kBFUL;
        CdReport(cd.status);
        flags |= kESEL;
        break;
    case 0x50: // buffer size: free sectors, filters, sectors in all
        cd.CR[0] = static_cast<uint16>(cd.status << 8);
        cd.CR[1] = static_cast<uint16>(kBufferSectors - cd.buffered);
        cd.CR[2] = 0x1800;
        cd.CR[3] = kBufferSectors;
        break;
    case 0x51: // sectors in a buffer partition
        cd.CR[0] = static_cast<uint16>(cd.status << 8);
        cd.CR[1] = cd.CR[2] = 0;
        cd.CR[3] = 0;
        break;
    case 0x75: // abort file
        CdReport(cd.status);
        flags |= kEFLS;
        break;
    case 0xE0: // authenticate the disc
        cd.CR[0] = static_cast<uint16>((cd.status << 8) | 0xFF);
        cd.CR[1] = cd.CR[2] = cd.CR[3] = 0xFFFF;
        flags |= kEFLS;
        break;
    default: {
        static bool reported[256];
        if (!reported[command]) {
            reported[command] = true;
            fprintf(stderr, "CD drive: command %02X is not one this machine knows (%04X %04X %04X %04X)\n", command, cd.command[0],
                    cd.command[1], cd.command[2], cd.command[3]);
        }
        CdReport(cd.status);
        break;
    }
    }
    cd.HIRQ |= flags;
    cd.responseTaken = false;
}

// 150 times a second while the drive reads (data is read at double speed): a sector goes into the buffer,
// until that is full or the end of what was asked for is reached
void Machine::CdPeriodic() {
    auto &cd = m_cd;
    cd.periodicDue += m_clock / 150;
    if (cd.status != 0x02) {
        cd.HIRQ |= kSCDQ; // (a disc that is turning: its subcode is read all the time)
    }
    if (cd.responseTaken) {
        CdReport(cd.status | 0x20); // the periodic report (marked as one)
    }
    if (!cd.reading) {
        return;
    }
    if (cd.fad >= cd.playEnd) {
        cd.reading = false;
        cd.status = 0x01;
        cd.HIRQ |= kPEND;
        ++g_activity;
        return;
    }
    cd.fad++;
    cd.buffered++;
    cd.HIRQ |= kCSCT;
    if (cd.buffered >= kBufferSectors) {
        cd.reading = false; // (it waits there, still "playing", until the buffer is emptied)
        cd.HIRQ |= kBFUL;
    }
    ++g_activity;
}

} // namespace satrecomp::machine
