// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// Routines several games carry byte for byte (shared_lifts.h). First written for Fighting Vipers.
#include "shared_lifts.h"
#include "game.h"
#include "music.h"
#include "state.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using satrecomp::Guest;
namespace music = satrecomp::music;

// --- Files ---

// LoadFile(name, destination): reads a whole file from the disc into memory and returns when it is there.
//
// The original opens the file through the game's CD file system, starts a read into `destination` and waits
// for it to finish, a frame at a time. Here the file comes straight from the disc image.
// interface: r4 = name, r5 = destination; callee-saved registers untouched
bool Am2LoadFile(Guest &g) {
    const uint32_t kLoadingName = g.Read32(g.PC() + 0x80); // name of the file being loaded
    const uint32_t kLoadError = g.Read32(g.PC() + 0x84);   // (byte) cleared when a load starts
    constexpr uint32_t kFinished = 0x100;         // what the wait loop leaves in r0 when the read is complete

    const uint32_t name = g.R(4);
    const uint32_t destination = g.R(5);
    std::vector<uint8_t> data;
    if (!satrecomp::ReadDiscFile(g.String(name, 32), data)) {
        return false; // let the original deal with it (it reports the error)
    }
    g.Write32(kLoadingName, name);
    g.Write8(kLoadError, 0);
    g.CopyIn(destination, data.data(), data.size());
    g.R(0) = kFinished;
    g.Return(2000);
    return true;
}

// The file library the game is linked with, as far as the game uses it: a file is opened by name, a read
// into memory is started, and the game then asks every frame how it is getting on until it is complete.
// (LoadFile above is the game's own routine that does all of that and waits.) Here an open file is a number
// the library would never give out, and a read is complete at once.
constexpr uint32_t kHandleBase = 0x7F100000, kHandleCount = 0x10000;
constexpr uint32_t kComplete = 0x100, kIdle = 0x101, kOpened = 0x103;

struct OpenFile {
    std::vector<uint8_t> data;
    uint32_t state = kIdle; // what asking about it gives
    bool open = false;
};
std::vector<OpenFile> g_files;

OpenFile *FileOf(uint32_t handle) {
    const uint32_t index = handle - kHandleBase;
    return index < g_files.size() ? &g_files[index] : nullptr;
}
bool IsHandle(uint32_t handle) {
    return handle - kHandleBase < kHandleCount;
}

// Open(name, directory): r4 = name (with or without a directory in front). Returns the open file, 0 if there
// is none of that name.
bool GfsOpen(Guest &g) {
    std::string name = g.String(g.R(4), 64);
    if (const size_t slash = name.find_last_of("/\\"); slash != std::string::npos) {
        name = name.substr(slash + 1);
    }
    OpenFile file;
    if (!satrecomp::ReadDiscFile(name, file.data)) {
        return false; // (the original reports it)
    }
    file.open = true;
    file.state = kOpened;
    size_t index = 0;
    while (index < g_files.size() && g_files[index].open) {
        index++;
    }
    if (index == g_files.size()) {
        if (g_files.size() >= kHandleCount) {
            return false;
        }
        g_files.emplace_back();
    }
    g_files[index] = std::move(file);
    g.R(0) = kHandleBase + static_cast<uint32_t>(index);
    g.Return(500);
    return true;
}

// Start(file, what): r5 -> records of { kind, destination, unit, count }, ended by one of kind 2. Kind 0 with
// a destination reads up to count units (of 2048 bytes) of the file there; without one nothing is read yet.
bool GfsStart(Guest &g) {
    if (!IsHandle(g.R(4))) {
        return false;
    }
    OpenFile *file = FileOf(g.R(4));
    const uint32_t what = g.R(5);
    if (file && file->open) {
        const uint32_t kind = g.Read32(what), destination = g.Read32(what + 4);
        const uint64_t room = static_cast<uint64_t>(g.Read32(what + 8)) * g.Read32(what + 12);
        if (kind == 0 && destination != 0 && room != 0) {
            g.CopyIn(destination, file->data.data(), static_cast<size_t>(std::min<uint64_t>(file->data.size(), room)));
            file->state = kComplete;
        } else {
            file->state = kOpened;
        }
    }
    g.R(0) = 0;
    g.Return(500);
    return true;
}

// Poll(file, progress): how the read is getting on: complete (the file is then closed), or nothing going on.
bool GfsPoll(Guest &g) {
    if (!IsHandle(g.R(4))) {
        return false;
    }
    OpenFile *file = FileOf(g.R(4));
    uint32_t state = kIdle;
    if (file && file->open) {
        state = file->state;
        if (state == kComplete) {
            *file = OpenFile{};
        }
    }
    g.R(0) = state;
    g.Return(300);
    return true;
}

// Close(file)
bool GfsClose(Guest &g) {
    if (!IsHandle(g.R(4))) {
        return false;
    }
    if (OpenFile *file = FileOf(g.R(4))) {
        *file = OpenFile{};
    }
    g.R(0) = 0;
    g.Return(300);
    return true;
}

// --- CD music ---
//
// A game's music is audio tracks on the disc. It starts and stops them with three routines of the CD library,
// which send a command to the CD drive each. Here the port plays the tracks itself (runtime/music.h), so the
// drive is not involved.

// A position as the library takes it: a kind, then a track and index or a sector address
enum : uint32_t { kPosDefault = 0, kPosSector = 1, kPosTrack = 2, kPosNoChange = 3 };
// Drive states as the library reports them
enum : uint8_t { kStatusPause = 1, kStatusPlay = 3 };

// CdPlay(what): r4 -> { start position, end position, mode }. Plays from the start of a track to the end of
// a track (the same one, in the games so far), the low four bits of the mode times more (15: until stopped); or,
// with "no change" as the start, goes on from where it was paused. Returns 0.
bool CdPlay(Guest &g) {
    const uint32_t what = g.R(4);
    const uint32_t startKind = g.Read32(what), endKind = g.Read32(what + 8);
    const int startTrack = g.Read8(what + 4), endTrack = g.Read8(what + 12);
    const uint8_t mode = g.Read8(what + 16);
    if (startKind == kPosTrack && music::IsAudioTrack(startTrack)) {
        music::Play(startTrack, endKind == kPosTrack ? endTrack : startTrack, mode == 0xFF ? 0 : mode & 0xF);
    } else if (startKind == kPosNoChange && music::GetState() == music::State::Paused) {
        music::Resume();
    } else {
        // (data, or nothing this port plays: the drive's business. It cannot play both.)
        music::Stop();
        return false;
    }
    g.R(0) = 0;
    g.Return(300);
    return true;
}

// CdSeek(where): r4 -> position. "Default" stops the disc, "no change" pauses it. Returns 0.
bool CdSeek(Guest &g) {
    if (music::GetState() == music::State::Stopped) {
        return false;
    }
    const uint32_t kind = g.Read32(g.R(4));
    if (kind == kPosNoChange) {
        music::Pause();
    } else {
        music::Stop();
        if (kind != kPosDefault) {
            return false; // (a seek to somewhere: the drive's business)
        }
    }
    g.R(0) = 0;
    g.Return(300);
    return true;
}

// GetCurStat(status): r4 -> { state, (pad), flags, control, track, index, sector address }. Returns 0.
bool CdGetCurStat(Guest &g) {
    if (music::GetState() == music::State::Stopped) {
        return false;
    }
    const uint32_t status = g.R(4);
    g.Write8(status, music::GetState() == music::State::Playing ? kStatusPlay : kStatusPause);
    g.Write8(status + 4, 0);    // repeat count and flags
    g.Write8(status + 5, 0x01); // an audio track
    g.Write8(status + 6, static_cast<uint8_t>(music::CurrentTrack()));
    g.Write8(status + 7, 1);
    g.Write32(status + 8, music::CurrentSector());
    g.R(0) = 0;
    g.Return(300);
    return true;
}


// --- Controllers ---

// ReadPads(): the game's pad state, per player (at the address in the routine's pool): buttons held (Saturn pad layout, 1 = pressed), buttons newly pressed
// this frame, and the buttons held the frame before.
// ReadPads(): once a frame, takes both players' buttons and works out which were newly pressed.
//
// The original takes them from what the SMPC library last read from the control ports. Here they come
// straight from the host's controllers (or the input movie being replayed).
// interface: no arguments; r0-r3, r6, r7 are scratch
bool Am2ReadPads(Guest &g) {
    const uint32_t kPads = g.Read32(g.PC() + 0x48);
    const uint32_t kHeld[2] = {kPads + 2, kPads + 26};
    const uint32_t kPressed[2] = {kPads + 4, kPads + 28};
    const uint32_t kHeldBefore[2] = {kPads + 6, kPads + 30};
    for (int player = 0; player < 2; player++) {
        const uint16_t before = g.Read16(kHeld[player]);
        const uint16_t held = satrecomp::ReadPad(player);
        g.Write16(kHeldBefore[player], before);
        g.Write16(kHeld[player], held);
        g.Write16(kPressed[player], static_cast<uint16_t>((before ^ held) & held));
    }
    g.Return(40);
    return true;
}

// The peripheral library the game is linked with asks the SMPC (the chip behind the control ports, which also
// keeps the clock) once a frame for the state of the controllers, and about once a second for the system's
// status as well: the "INTBACK" command. The chip answers through an interrupt, whose handler in the library
// copies the answer into the library's buffers, where the game reads it.
//
// Here the answer is put into those buffers directly, as the handler leaves them, and the chip is not asked.
//
// SendCommand(command, wait): r4 -> { command, -, which parameters to set up }, r5 = wait for it to finish.
// Only INTBACK is replaced; the few other commands (reset button on/off, sound and second CPU on at start-up)
// still go to the chip.
// The library's work area (0x060FFF80 in the games so far) is named in the routine's pool; its buffers sit at
// fixed places after its code.
struct PerAddresses {
    uint32_t parameters;   // INTBACK: what to report (top byte: 1 = status too; next: 8 = controllers)
    uint32_t rawCount;     // bytes of controller data taken so far
    uint32_t done;         // bit 7: the answer is complete
    uint32_t lastStatus;   // the chip's status register as the handler last saw it
    uint32_t control;      // what the handler told the chip last (continue, break)
    uint32_t statusBuffer;
    uint32_t rawBufferPointer;
};
PerAddresses PerAt(Guest &g) {
    const uint32_t work = g.Read32(g.PC() + 0x210) - 0x28;
    return {work + 0x0C, work, work + 0x1D, work + 0x22, work + 0x23, g.PC() + 0x30C, g.PC() + 0x2E0};
}
constexpr uint8_t kAreaCode = 0x04; // (North America: the disc's)

struct Smpc {
    bool resetDisabled = false, soundOff = false;
    uint64_t requests = 0; // one a frame: the clock
    // check (SATRECOMP_PER_CHECK=1): what was worked out at the request before, to compare with what the chip gave
    bool pending = false, pendingStatus = false;
    uint8_t status[20] = {}, raw[32] = {}, sr = 0;
    uint64_t checked = 0, statusDifferent = 0, layoutDifferent = 0, buttonsDifferent = 0;
} g_smpc;

uint8_t Bcd(unsigned value) {
    return static_cast<uint8_t>(((value / 10) << 4) | (value % 10));
}

// The clock: starts on 1 January 1994 (a Saturday) when the game does and runs with the frames shown, so that
// every run is the same
void ClockInto(uint8_t *out, uint64_t frames) {
    static const unsigned kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    uint64_t seconds = frames * 1001 / 60000;
    uint64_t days = seconds / 86400;
    seconds %= 86400;
    unsigned year = 1994, month = 0, weekday = static_cast<unsigned>((6 + days) % 7);
    for (;;) {
        const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
        const unsigned length = kDays[month] + (month == 1 && leap ? 1 : 0);
        if (days < length) {
            break;
        }
        days -= length;
        if (++month == 12) {
            month = 0;
            year++;
        }
    }
    out[0] = Bcd(year / 100);
    out[1] = Bcd(year % 100);
    out[2] = static_cast<uint8_t>((weekday << 4) | (month + 1));
    out[3] = Bcd(static_cast<unsigned>(days) + 1);
    out[4] = Bcd(static_cast<unsigned>(seconds / 3600));
    out[5] = Bcd(static_cast<unsigned>(seconds / 60 % 60));
    out[6] = Bcd(static_cast<unsigned>(seconds % 60));
}

bool PerSendCommand(Guest &g) {
    static const bool check = getenv("SATRECOMP_PER_CHECK") != nullptr;
    const PerAddresses a = PerAt(g);
    const uint32_t kParameters = a.parameters, kRawCount = a.rawCount, kDone = a.done, kLastStatus = a.lastStatus,
                   kControl = a.control, kStatusBuffer = a.statusBuffer, kRawBufferPointer = a.rawBufferPointer;
    const uint8_t command = g.Read8(g.R(4));
    switch (command) {
    case 0x06: g_smpc.soundOff = false; break;
    case 0x07: g_smpc.soundOff = true; break;
    case 0x19: g_smpc.resetDisabled = false; break;
    case 0x1A: g_smpc.resetDisabled = true; break;
    default: break;
    }
    if (command != 0x10 || g.Read8(g.R(4) + 2) != 4) {
        return false;
    }
    if (check && g_smpc.pending) {
        // What the chip and the library's handler made of the request before, against what was worked out
        g_smpc.checked++;
        bool statusSame = true, layoutSame = g.Read8(kLastStatus) == g_smpc.sr, buttonsSame = true;
        for (uint32_t i = 0; i < 20 && g_smpc.pendingStatus; i++) {
            if (i >= 4 && i < 11) {
                continue; // (the clock: not the chip's to the second)
            }
            statusSame = statusSame && g.Read8(kStatusBuffer + i) == g_smpc.status[i];
        }
        const uint32_t raw = g.Read32(kRawBufferPointer);
        for (uint32_t i = 0; i < 32; i++) {
            const bool same = g.Read8(raw + i) == g_smpc.raw[i];
            (i == 2 || i == 3 || i == 6 || i == 7 ? buttonsSame : layoutSame) &= same;
        }
        if (!buttonsSame && g_smpc.buttonsDifferent < 8) {
            fprintf(stderr, "peripherals: buttons %02X %02X %02X %02X chip, %02X %02X %02X %02X here\n", g.Read8(raw + 2), g.Read8(raw + 3), g.Read8(raw + 6),
                    g.Read8(raw + 7), g_smpc.raw[2], g_smpc.raw[3], g_smpc.raw[6], g_smpc.raw[7]);
        }
        g_smpc.statusDifferent += !statusSame;
        g_smpc.layoutDifferent += !layoutSame;
        g_smpc.buttonsDifferent += !buttonsSame;
        if ((!statusSame || !layoutSame) && g_smpc.statusDifferent + g_smpc.layoutDifferent < 6) {
            fprintf(stderr, "peripherals: differs: status");
            for (uint32_t i = 0; i < 20; i++) fprintf(stderr, " %02X/%02X", g.Read8(kStatusBuffer + i), g_smpc.status[i]);
            fprintf(stderr, " raw");
            for (uint32_t i = 0; i < 10; i++) fprintf(stderr, " %02X/%02X", g.Read8(raw + i), g_smpc.raw[i]);
            fprintf(stderr, " sr %02X/%02X (chip/here)\n", g.Read8(kLastStatus), g_smpc.sr);
        }
    }

    const uint32_t parameters = g.Read32(kParameters);
    const bool wantStatus = (parameters >> 24) == 0x01, wantControllers = (parameters >> 16) & 0x08;
    if (!wantStatus && !wantControllers) {
        return false;
    }
    g_smpc.requests++;
    uint8_t status[20] = {};
    uint8_t sr = 0;
    uint8_t control = g.Read8(kControl);
    if (wantStatus) {
        // Reset button state, the clock, cartridge and area codes, system state, the chip's four bytes of memory
        status[0] = static_cast<uint8_t>(0x80 | (g_smpc.resetDisabled ? 0x40 : 0));
        ClockInto(&status[4], g_smpc.requests);
        status[12] = 0x00;
        status[13] = kAreaCode;
        status[14] = static_cast<uint8_t>(0x7C | (g_smpc.soundOff ? 0x01 : 0));
        status[15] = 0x00;
        sr = wantControllers ? 0x60 : 0x40;
        if (wantControllers) {
            control ^= 0x80; // (the handler asks the chip to go on to the controllers)
        }
    }
    // Controllers: each port reports one device, a digital pad, with two bytes of buttons (0 = pressed)
    uint8_t raw[32];
    memset(raw, 0xFF, sizeof(raw));
    if (wantControllers) {
        for (int player = 0; player < 2; player++) {
            const uint16_t held = satrecomp::ReadPad(player);
            raw[player * 4] = 0xF1;
            raw[player * 4 + 1] = 0x02;
            raw[player * 4 + 2] = static_cast<uint8_t>(~(held >> 8));
            raw[player * 4 + 3] = static_cast<uint8_t>(~held | 0x07);
        }
        sr = 0xC0;
        control |= 0x40; // (... and then that it has all it wants)
    }
    if (check) {
        g_smpc.pending = true;
        g_smpc.pendingStatus = wantStatus;
        memcpy(g_smpc.status, status, sizeof(status));
        memcpy(g_smpc.raw, raw, sizeof(raw));
        g_smpc.sr = sr;
        return false;
    }

    if (wantStatus) {
        g.Write8(kStatusBuffer, status[0]);
        for (uint32_t i = 4; i < 11; i++) {
            g.Write8(kStatusBuffer + i, status[i]);
        }
        for (uint32_t i = 12; i < 20; i++) {
            g.Write8(kStatusBuffer + i, status[i]);
        }
    }
    if (wantControllers) {
        const uint32_t count = g.Read32(kRawCount);
        if (count < 0x200) {
            const uint32_t buffer = g.Read32(kRawBufferPointer);
            for (uint32_t i = 0; i < 32; i++) {
                g.Write8(buffer + count + i, raw[i]);
            }
            g.Write32(kRawCount, count + 32);
        }
    }
    g.Write8(kLastStatus, sr);
    g.Write8(kControl, control);
    g.Write8(kDone, static_cast<uint8_t>(g.Read8(kDone) | 0x80));
    g.R(0) = 0;
    g.Return(200);
    return true;
}


void ReportPer() {
    if (g_smpc.checked) {
        fprintf(stderr, "peripherals: %llu requests checked against the chip: status differs in %llu, the controller data's layout in %llu, "
                        "buttons in %llu\n",
                static_cast<unsigned long long>(g_smpc.checked), static_cast<unsigned long long>(g_smpc.statusDifferent),
                static_cast<unsigned long long>(g_smpc.layoutDifferent), static_cast<unsigned long long>(g_smpc.buttonsDifferent));
    }
}

} // namespace

namespace satrecomp::shared {

const Routine kGfsOpen = {"GFS Open", 0x40, 0xD6F3BEFA9FD2D85Cull, GfsOpen};
const Routine kGfsStart = {"GFS Start", 0x40, 0x8F991EFB4E2B8472ull, GfsStart};
const Routine kGfsPoll = {"GFS Poll", 0x40, 0x8F0DC6C95F511448ull, GfsPoll};
const Routine kGfsClose = {"GFS Close", 0x40, 0x7B59151D7BEC30D8ull, GfsClose};
const Routine kCdPlay = {"CdPlay", 0x44, 0xCB5EB1EAEFB73570ull, CdPlay};
const Routine kCdSeek = {"CdSeek", 0x26, 0x4797762B59FD886Full, CdSeek};
const Routine kCdGetCurStat = {"GetCurStat", 0x44, 0x84988AAF02EEAA2Aull, CdGetCurStat};
const Routine kPerSendCommand = {"SendCommand", 0x3C, 0x01D89F5B50197446ull, PerSendCommand};
const Routine kAm2LoadFile = {"LoadFile", 0x7A, 0x81D32D978DE13A21ull, Am2LoadFile};
const Routine kAm2ReadPads = {"ReadPads", 0x44, 0xEBD0C5382F550A7Dull, Am2ReadPads};

void Register(const Routine &routine, uint32_t address, const char *name) {
    static bool reporting = false;
    if (routine.fn == PerSendCommand && !reporting) {
        reporting = true;
        atexit(ReportPer);
    }
    const Lift lift = {name ? name : routine.name, address, routine.size, routine.hash, routine.fn};
    RegisterLifts(&lift, 1);
}

void State(Archive &ar) {
    // the files open through the library
    uint32_t count = static_cast<uint32_t>(g_files.size());
    ar.Pod(count);
    if (!ar.Saving()) {
        g_files.assign(ar.Ok() && count <= kHandleCount ? count : 0, OpenFile{});
    }
    for (OpenFile &file : g_files) {
        ar.Vec(file.data);
        ar.Pod(file.state);
        ar.Pod(file.open);
    }
    // what the game has told the system manager, and its clock's count
    ar.Pod(g_smpc.resetDisabled);
    ar.Pod(g_smpc.soundOff);
    ar.Pod(g_smpc.requests);
    if (!ar.Saving()) {
        g_smpc.pending = false;
    }
}

void WidenAm2PanelLimits(std::initializer_list<uint32_t> loads) {
    constexpr uint16_t kLimit = 224, kHalfWidth = 176;
    constexpr uint16_t kNegR1R2 = 0x621B;
    const float wider = PictureAspect() / (4.0f / 3.0f) - 1.0f;
    // In a wider picture: out by as much as the picture is wider, and by another half 4:3 picture, since a
    // panel at the side is listed only when its far corners are inside too (Megamix's fences are wide; at
    // 16:9 one at the right edge needed a limit of nearly 300, and limits up to 2000 showed nothing more)
    const uint16_t limit = wider > 0.0f
        ? static_cast<uint16_t>(kLimit + std::lround(wider * kHalfWidth) + kHalfWidth)
        : kLimit;
    for (const uint32_t at : loads) {
        const uint16_t op = Ram16(at);
        if ((op & 0xFF00) != 0x9100) { // mov.w @(disp,PC),r1
            continue;
        }
        if (Ram16(at + 2) != kNegR1R2 && Ram16(at + 4) != kNegR1R2) {
            continue;
        }
        const uint32_t word = at + 4 + (op & 0xFF) * 2;
        const uint16_t now = Ram16(word);
        if (now != limit && now >= kLimit) { // (the game's, or one set here)
            SetRam16(word, limit);
        }
    }
}

} // namespace satrecomp::shared
