// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// The CD drive's host interface, as far as the games still use it. Files, movie data and CD music are mostly
// read by the port itself (games/<id>/src: the game's library routines replaced); what is left are the
// commands a CD library gives when it starts: status, table of contents, set-up of the buffer's filters,
// seeks, the drive's own reading of directories and files (for a game whose file library has not been
// replaced); and data sectors read into the buffer and taken through the data port, for library code that has
// not been replaced (Fighters Megamix's second program reads the disc's directory that way). Of the
// selector's filters only the range of frame addresses is compared (no subheader conditions).
#include "machine.h"
#include "lift.h"
#include "music.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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
    for (unsigned i = 0; i < std::size(m_cd.filters); i++) {
        CdResetFilter(i);
    }
    m_cd.periodicDue = m_now + m_clock / 75;
}

uint16 Machine::CdRead(uint32 address) {
    auto &cd = m_cd;
    if (address < 0x90000 || address >= 0x98000 || (address & 0xFFFF) < 0x08) { // the data port (0x25818000, 0x25898000)
        if (cd.transferOffset >= cd.transferSize) {
            return 0xFFFF;
        }
        const uint32 offset = cd.transferOffset++;
        if (cd.transferSectors == 0) {
            return cd.transfer[offset];
        }
        // (a sector's data, read from the disc when the transfer comes to it)
        static uint8 sector[2048];
        static uint32 sectorFAD = 0xFFFFFFFF;
        const uint32 fad = cd.transferFAD[offset / 1024];
        if (fad != sectorFAD) {
            sectorFAD = fad;
            if (!satrecomp::ReadDiscSector(fad, sector)) {
                std::fill(std::begin(sector), std::end(sector), 0);
            }
        }
        const uint32 at = (offset % 1024) * 2;
        return static_cast<uint16>((sector[at] << 8) | sector[at + 1]);
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
        cd.transferSectors = 0;
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
            cd.CR[0] = static_cast<uint16>((cd.status << 8) | ((cd.transferOffset >> 16) & 0xFF));
            cd.CR[1] = static_cast<uint16>(cd.transferOffset);
        }
        cd.CR[2] = cd.CR[3] = 0;
        cd.transferSize = 0;
        cd.transferOffset = 0;
        cd.transferSectors = 0;
        flags |= kEHST;
        break;
    case 0x10: { // play: from a frame address for a number of sectors, or from a track to a track
        const uint32 start = ((cd.command[0] & 0xFF) << 16) | cd.command[1];
        const uint32 end = ((cd.command[2] & 0xFF) << 16) | cd.command[3];
        const uint8 mode = static_cast<uint8>(cd.command[2] >> 8);
        cd.fileRead = false;
        auto trackOf = [&](uint32 number) -> const Disc::Track * {
            for (const Disc::Track &t : m_disc.tracks) {
                if (t.number == number) {
                    return &t;
                }
            }
            return nullptr;
        };
        if (start == 0xFFFFFF) { // (from where it is: music that was paused goes on)
            if (cd.audio && satrecomp::music::GetState() == satrecomp::music::State::Paused) {
                satrecomp::music::Resume();
                cd.status = 0x03;
                CdReport(cd.status);
                break;
            }
            cd.playEnd = (end & 0x800000) ? cd.fad + (end & 0x7FFFFF) : m_disc.leadOutFAD;
        } else if (start & 0x800000) {
            CdStopAudio(false);
            cd.fad = start & 0x7FFFFF;
            cd.playEnd = (end & 0x800000) ? cd.fad + (end & 0x7FFFFF) : m_disc.leadOutFAD;
        } else { // a track (and an index in it, taken as the track's start)
            const uint32 first = (start >> 8) ? (start >> 8) : 1;
            uint32 last = (end == 0xFFFFFF || (end & 0x800000)) ? first : (end >> 8);
            if (last == 0 && !m_disc.tracks.empty()) {
                last = m_disc.tracks.back().number;
            }
            const Disc::Track *track = trackOf(first);
            if (track && !(track->control & 4)) { // audio: played by the port into the sound chip's CD input
                satrecomp::music::Play(static_cast<int>(first), static_cast<int>(std::max(first, last)),
                                       mode == 0xFF ? 0 : mode & 0xF);
                cd.audio = true;
                cd.reading = false;
                cd.fad = track->startFAD;
                cd.status = 0x03;
                CdReport(0x04);
                break;
            }
            CdStopAudio(false);
            if (track) {
                cd.fad = track->startFAD;
            }
            const Disc::Track *after = trackOf(std::max(first, last) + 1);
            cd.playEnd = after ? after->startFAD : m_disc.leadOutFAD;
        }
        cd.status = 0x03;
        cd.reading = true;
        CdReport(0x04); // (it says "seeking" first)
        break;
    }
    case 0x11: { // seek: to a frame address and pause, or stop, or pause where it is
        const uint32 target = ((cd.command[0] & 0xFF) << 16) | cd.command[1];
        cd.reading = false;
        cd.fileRead = false;
        CdStopAudio(target == 0xFFFFFF); // (paused where it is: the music can go on from there)
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
        cd.connection = static_cast<uint8>(cd.command[2] >> 8);
        CdReport(cd.status);
        flags |= kESEL;
        break;
    case 0x40: // a filter's range of frame addresses
        if (const unsigned n = cd.command[2] >> 8; n < std::size(cd.filters)) {
            cd.filters[n].rangeStart = ((cd.command[0] & 0xFF) << 16) | cd.command[1];
            cd.filters[n].rangeCount = ((cd.command[2] & 0xFF) << 16) | cd.command[3];
        }
        CdReport(cd.status);
        flags |= kESEL;
        break;
    case 0x44: // a filter's mode (what it compares; only the range of frame addresses is compared here)
        if (const unsigned n = cd.command[2] >> 8; n < std::size(cd.filters)) {
            if (cd.command[0] & 0x80) {
                CdResetFilter(n);
            } else {
                cd.filters[n].mode = static_cast<uint8>(cd.command[0]);
            }
        }
        CdReport(cd.status);
        flags |= kESEL;
        break;
    case 0x46: // a filter's outputs: the partition its sectors go to, the filter the others go on to
        if (const unsigned n = cd.command[2] >> 8; n < std::size(cd.filters)) {
            if (cd.command[0] & 1) {
                cd.filters[n].trueOutput = static_cast<uint8>(cd.command[1] >> 8);
            }
            if (cd.command[0] & 2) {
                cd.filters[n].falseOutput = static_cast<uint8>(cd.command[1]);
            }
        }
        CdReport(cd.status);
        flags |= kESEL;
        break;
    case 0x42: // a filter's subheader conditions (not compared)
    case 0x60: // the sector length
        CdReport(cd.status);
        flags |= kESEL;
        break;
    case 0x48: { // reset the selector: one partition is emptied, or all of them and the filters as asked
        const uint8 what = static_cast<uint8>(cd.command[0]);
        const uint8 partition = static_cast<uint8>(cd.command[2] >> 8);
        uint32 kept = 0;
        for (uint32 i = 0; i < cd.buffered; i++) {
            if (what == 0 ? cd.bufferPartition[i] != partition : !(what & 0x04)) {
                cd.bufferFAD[kept] = cd.bufferFAD[i];
                cd.bufferPartition[kept++] = cd.bufferPartition[i];
            }
        }
        cd.buffered = kept;
        for (unsigned i = 0; i < std::size(cd.filters); i++) {
            if (what & 0x10) {
                cd.filters[i].mode = 0;
                cd.filters[i].rangeStart = cd.filters[i].rangeCount = 0;
            }
            if (what & 0x40) {
                cd.filters[i].trueOutput = static_cast<uint8>(i);
            }
            if (what & 0x80) {
                cd.filters[i].falseOutput = 0xFF;
            }
        }
        cd.HIRQ &= ~kBFUL;
        CdResumeIfRoom();
        CdReport(cd.status);
        flags |= kESEL;
        break;
    }
    case 0x50: // buffer size: free sectors, filters, sectors in all
        cd.CR[0] = static_cast<uint16>(cd.status << 8);
        cd.CR[1] = static_cast<uint16>(kBufferSectors - cd.buffered);
        cd.CR[2] = 0x1800;
        cd.CR[3] = kBufferSectors;
        break;
    case 0x51: // sectors in a buffer partition
        cd.CR[0] = static_cast<uint16>(cd.status << 8);
        cd.CR[1] = cd.CR[2] = 0;
        cd.CR[3] = static_cast<uint16>(CdPartitionSectors(static_cast<uint8>(cd.command[2] >> 8)));
        break;
    case 0x52: { // calculate the actual size of sectors: offset, partition, count
        uint32 first = 0, count = 0;
        CdSectorRange(first, count);
        cd.actualSize = count * 1024;
        CdReport(cd.status);
        flags |= kESEL;
        break;
    }
    case 0x53: // the actual size (words)
        cd.CR[0] = static_cast<uint16>((cd.status << 8) | ((cd.actualSize >> 16) & 0xFF));
        cd.CR[1] = static_cast<uint16>(cd.actualSize);
        cd.CR[2] = cd.CR[3] = 0;
        break;
    case 0x54: { // a sector's information: its frame address, file and channel, submode and coding
        uint32 first = 0, count = 0;
        CdSectorRange(first, count);
        const uint32 fad = count ? cd.bufferFAD[CdPartitionIndex(static_cast<uint8>(cd.command[2] >> 8), first)] : 0;
        cd.CR[0] = static_cast<uint16>((cd.status << 8) | ((fad >> 16) & 0xFF));
        cd.CR[1] = static_cast<uint16>(fad);
        cd.CR[2] = cd.CR[3] = 0;
        flags |= kESEL;
        break;
    }
    case 0x61: // sectors' data to the data port
    case 0x62: // sectors deleted from the buffer
    case 0x63: { // both
        uint32 first = 0, count = 0;
        CdSectorRange(first, count);
        const uint8 partition = static_cast<uint8>(cd.command[2] >> 8);
        if (command != 0x62) {
            for (uint32 i = 0; i < count; i++) {
                cd.transferFAD[i] = cd.bufferFAD[CdPartitionIndex(partition, first + i)];
            }
            cd.transferSectors = count;
            cd.transferSize = count * 1024;
            cd.transferOffset = 0;
            flags |= kDRDY;
        }
        if (command != 0x61) {
            // (the partition's sectors `first` to `first + count - 1` are taken out of the buffer)
            uint32 kept = 0, n = 0;
            for (uint32 i = 0; i < cd.buffered; i++) {
                const bool taken = cd.bufferPartition[i] == partition && n++ >= first && n - 1 < first + count;
                if (!taken) {
                    cd.bufferFAD[kept] = cd.bufferFAD[i];
                    cd.bufferPartition[kept++] = cd.bufferPartition[i];
                }
            }
            cd.buffered = kept;
            cd.HIRQ &= ~kBFUL;
            CdResumeIfRoom();
            if (command == 0x62) {
                flags |= kEHST;
            }
        }
        CdReport(cd.status);
        break;
    }
    case 0x70: { // change directory: to a directory of the current one, or (FFFFFF) the disc's first
        const uint32 id = ((cd.command[2] & 0xFF) << 16) | cd.command[3];
        uint32 fad = 0, size = 0;
        bool found = false;
        if (id == 0xFFFFFF) {
            found = CdRootDirectory(fad, size);
        } else if (const CdFileInfo *file = CdFile(id); file && (file->attributes & 0x02)) {
            fad = file->fad;
            size = file->size;
            found = true;
        }
        if (found) {
            CdReadDirectory(fad, size, 2);
        }
        CdReport(cd.status);
        flags |= kEFLS;
        break;
    }
    case 0x71: { // read directory: the current one's entries from a file number on
        const uint32 id = ((cd.command[2] & 0xFF) << 16) | cd.command[3];
        if (cd.directoryRead || CdRootDirectory(cd.self.fad, cd.self.size)) {
            CdReadDirectory(cd.self.fad, cd.self.size, id == 0xFFFFFF ? 2 : id);
        }
        CdReport(cd.status);
        flags |= kEFLS;
        break;
    }
    case 0x72: // the entries held: how many, whether they are the directory's last, the first one's number
        if (!cd.directoryRead) {
            uint32 fad = 0, size = 0;
            if (CdRootDirectory(fad, size)) {
                CdReadDirectory(fad, size, 2);
            }
        }
        cd.CR[0] = static_cast<uint16>(cd.status << 8);
        cd.CR[1] = static_cast<uint16>(cd.fileCount);
        cd.CR[2] = static_cast<uint16>((cd.directoryEnd ? 0x0100 : 0) | ((cd.fileOffset >> 16) & 0xFF));
        cd.CR[3] = static_cast<uint16>(cd.fileOffset);
        flags |= kEFLS;
        break;
    case 0x73: { // file information, 12 bytes each, at the data port: one file's, or (FFFFFF) all those held
        const uint32 id = ((cd.command[2] & 0xFF) << 16) | cd.command[3];
        uint32 words = 0;
        auto put = [&](const CdFileInfo &file) {
            cd.transfer[words++] = static_cast<uint16>(file.fad >> 16);
            cd.transfer[words++] = static_cast<uint16>(file.fad);
            cd.transfer[words++] = static_cast<uint16>(file.size >> 16);
            cd.transfer[words++] = static_cast<uint16>(file.size);
            cd.transfer[words++] = static_cast<uint16>((file.unit << 8) | file.gap);
            cd.transfer[words++] = static_cast<uint16>((file.number << 8) | file.attributes);
        };
        if (id == 0xFFFFFF) {
            for (uint32 i = 0; i < cd.fileCount; i++) {
                put(cd.files[i]);
            }
        } else if (const CdFileInfo *file = CdFile(id)) {
            put(*file);
        }
        if (words) {
            cd.transferSize = words;
            cd.transferOffset = 0;
            cd.transferSectors = 0;
            cd.CR[0] = static_cast<uint16>(cd.status << 8);
            cd.CR[1] = static_cast<uint16>(words);
            cd.CR[2] = cd.CR[3] = 0;
            flags |= kDRDY;
        } else {
            cd.CR[0] = static_cast<uint16>((cd.status << 8) | 0xFF); // (no such file: the command is refused)
            cd.CR[1] = cd.CR[2] = cd.CR[3] = 0xFFFF;
        }
        break;
    }
    case 0x74: { // read a file: its sectors into the buffer, from a sector of it on
        const uint32 id = ((cd.command[2] & 0xFF) << 16) | cd.command[3];
        const uint32 offset = ((cd.command[0] & 0xFF) << 16) | cd.command[1];
        if (const CdFileInfo *file = CdFile(id)) {
            const uint32 sectors = (file->size + 2047) / 2048;
            cd.fad = file->fad + std::min(offset, sectors);
            cd.playEnd = file->fad + sectors;
            cd.status = 0x03;
            cd.reading = true;
            cd.fileRead = true;
            CdReport(0x04);
        } else {
            CdReport(cd.status);
            flags |= kEFLS;
        }
        break;
    }
    case 0x75: // abort file
        if (cd.fileRead) {
            cd.fileRead = false;
            cd.reading = false;
            cd.status = 0x01;
        }
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
    // Debugging aid (SATRECOMP_CD_TRACE=1): every command and the response
    static const bool trace = getenv("SATRECOMP_CD_TRACE") != nullptr;
    if (trace) {
        fprintf(stderr, "cd cmd %04X %04X %04X %04X -> %04X %04X %04X %04X (HIRQ %04X)\n", cd.command[0], cd.command[1], cd.command[2],
                cd.command[3], cd.CR[0], cd.CR[1], cd.CR[2], cd.CR[3], cd.HIRQ);
    }
}

// 150 times a second while the drive reads (data is read at double speed): a sector goes into the buffer,
// until that is full or the end of what was asked for is reached
void Machine::CdPeriodic() {
    auto &cd = m_cd;
    cd.periodicDue += m_clock / 150;
    cd.HIRQ |= kSCDQ; // (with every periodic report, the drive stopped too: Fighters Megamix's file library waits for it)
    if (cd.responseTaken) {
        CdReport(cd.status | 0x20); // the periodic report (marked as one)
    }
    if (cd.audio) { // music: where the port's player is, and its end
        const auto state = satrecomp::music::GetState();
        if (state == satrecomp::music::State::Playing) {
            for (const Disc::Track &t : m_disc.tracks) {
                if (t.number == satrecomp::music::CurrentTrack()) {
                    cd.fad = t.startFAD + satrecomp::music::CurrentSector();
                }
            }
        } else if (cd.status == 0x03) {
            cd.status = 0x01;
            cd.HIRQ |= kPEND;
            ++g_activity;
        }
        return;
    }
    if (!cd.reading) {
        return;
    }
    if (cd.fad >= cd.playEnd) {
        cd.reading = false;
        cd.status = 0x01;
        cd.HIRQ |= kPEND;
        if (cd.fileRead) {
            cd.fileRead = false;
            cd.HIRQ |= kEFLS;
        }
        ++g_activity;
        return;
    }
    CdStore(cd.fad);
    cd.fad++;
    cd.HIRQ |= kCSCT;
    if (cd.fad >= cd.playEnd) {
        // (the last sector asked for: the drive pauses there at once, as Ymir's does; Fighters Megamix's movie
        // player looks at the status straight after taking that sector)
        cd.reading = false;
        cd.status = 0x01;
        cd.HIRQ |= kPEND;
        if (cd.fileRead) {
            cd.fileRead = false;
            cd.HIRQ |= kEFLS;
        }
    } else if (cd.buffered >= kBufferSectors) {
        cd.reading = false; // (it waits there, still "playing", until the buffer is emptied)
        cd.HIRQ |= kBFUL;
    }
    ++g_activity;
}

// The disc's first directory, from the volume descriptor (the data track's sector 16)
bool Machine::CdRootDirectory(uint32 &fad, uint32 &size) {
    uint8 sector[2048];
    if (!satrecomp::ReadDiscSector(150 + 16, sector) || sector[0] != 1 || sector[1] != 'C' || sector[2] != 'D') {
        return false;
    }
    const uint8 *record = sector + 156;
    fad = 150 + (record[2] | (record[3] << 8) | (record[4] << 16) | (static_cast<uint32>(record[5]) << 24));
    size = record[10] | (record[11] << 8) | (record[12] << 16) | (static_cast<uint32>(record[13]) << 24);
    return true;
}

// Makes a directory the current one and holds its entries from number `from` on (up to 254 of them; 0 and
// 1 are the directory itself and the one it is in)
bool Machine::CdReadDirectory(uint32 fad, uint32 size, uint32 from) {
    auto &cd = m_cd;
    cd.fileCount = 0;
    cd.fileOffset = std::max<uint32>(from, 2);
    cd.directoryEnd = true;
    cd.directoryRead = true;
    uint8 sector[2048];
    uint32 number = 0;
    for (uint32 at = 0; at < size; at += 2048) {
        if (!satrecomp::ReadDiscSector(fad + at / 2048, sector)) {
            return false;
        }
        for (uint32 p = 0; p + 33 < 2048 && sector[p] != 0; p += sector[p]) {
            const uint8 *record = sector + p;
            CdFileInfo file;
            file.fad = 150 + (record[2] | (record[3] << 8) | (record[4] << 16) | (static_cast<uint32>(record[5]) << 24));
            file.size = record[10] | (record[11] << 8) | (record[12] << 16) | (static_cast<uint32>(record[13]) << 24);
            file.attributes = record[25];
            file.unit = record[26];
            file.gap = record[27];
            // (the file number is in the extended attributes that follow the name, when the disc has them)
            const uint32 extra = 33 + record[32] + ((record[32] & 1) ? 0 : 1);
            if (extra + 14 <= record[0] && record[extra + 6] == 'X' && record[extra + 7] == 'A') {
                file.number = record[extra + 8];
            }
            if (number == 0) {
                cd.self = file;
            } else if (number == 1) {
                cd.parent = file;
            } else if (number >= cd.fileOffset) {
                if (cd.fileCount < std::size(cd.files)) {
                    cd.files[cd.fileCount++] = file;
                } else {
                    cd.directoryEnd = false;
                }
            }
            number++;
        }
    }
    return true;
}

// The entry of the current directory with a file number, if it is held
const Machine::CdFileInfo *Machine::CdFile(uint32 id) {
    auto &cd = m_cd;
    if (!cd.directoryRead) {
        uint32 fad = 0, size = 0;
        if (!CdRootDirectory(fad, size) || !CdReadDirectory(fad, size, 2)) {
            return nullptr;
        }
    }
    if (id == 0) {
        return &cd.self;
    }
    if (id == 1) {
        return &cd.parent;
    }
    if (id >= cd.fileOffset && id - cd.fileOffset < cd.fileCount) {
        return &cd.files[id - cd.fileOffset];
    }
    return nullptr;
}

// The sectors a command names: CR2 the first (0xFFFF: the last one), CR4 how many (0xFFFF: to the end), in
// the buffer as it is (one partition)
void Machine::CdSectorRange(uint32 &first, uint32 &count) {
    auto &cd = m_cd;
    const uint32 offset = cd.command[1], wanted = cd.command[3];
    const uint32 held = CdPartitionSectors(static_cast<uint8>(cd.command[2] >> 8));
    first = offset == 0xFFFF ? (held ? held - 1 : 0) : std::min<uint32>(offset, held);
    count = wanted == 0xFFFF ? held - first : std::min<uint32>(wanted, held - first);
}

uint32 Machine::CdPartitionSectors(uint8 partition) const {
    uint32 count = 0;
    for (uint32 i = 0; i < m_cd.buffered; i++) {
        count += m_cd.bufferPartition[i] == partition;
    }
    return count;
}

// Where in the buffer a partition's nth sector is
uint32 Machine::CdPartitionIndex(uint8 partition, uint32 n) const {
    for (uint32 i = 0; i < m_cd.buffered; i++) {
        if (m_cd.bufferPartition[i] == partition && n-- == 0) {
            return i;
        }
    }
    return 0;
}

void Machine::CdResetFilter(unsigned n) {
    m_cd.filters[n] = {};
    m_cd.filters[n].trueOutput = static_cast<uint8>(n);
}

// A sector read by the drive goes through the selector into a partition of the buffer, or nowhere
void Machine::CdStore(uint32 fad) {
    auto &cd = m_cd;
    uint8 filter = cd.connection;
    for (unsigned hops = 0; hops < std::size(cd.filters) && filter < std::size(cd.filters); hops++) {
        const auto &f = cd.filters[filter];
        if (!(f.mode & 0x40) || (fad >= f.rangeStart && fad - f.rangeStart < f.rangeCount)) {
            if (f.trueOutput < std::size(cd.filters) && cd.buffered < kBufferSectors) {
                cd.bufferFAD[cd.buffered] = fad;
                cd.bufferPartition[cd.buffered++] = f.trueOutput;
            }
            return;
        }
        filter = f.falseOutput;
    }
}

// The music stops when the drive is told to do something else
void Machine::CdStopAudio(bool pause) {
    if (m_cd.audio) {
        if (pause) {
            satrecomp::music::Pause();
        } else {
            satrecomp::music::Stop();
            m_cd.audio = false;
        }
    }
}

// A play that stopped with the buffer full (still "playing") goes on when sectors are taken out of it, as the
// drive does: Fighters Megamix's movie player reads ahead until the buffer is full, then takes sectors as it
// shows the movie
void Machine::CdResumeIfRoom() {
    auto &cd = m_cd;
    if (cd.status == 0x03 && !cd.reading && cd.fad < cd.playEnd && cd.buffered < kBufferSectors) {
        cd.reading = true;
    }
}

} // namespace satrecomp::machine
