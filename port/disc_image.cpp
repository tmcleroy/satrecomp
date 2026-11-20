// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
#include "disc_image.h"

#include <libchdr/chd.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

namespace port {

// A .chd: CD frames of 2448 bytes (a sector's 2352, then 96 of subcode), several to a hunk, the hunk the unit
// it is compressed and read in. Each track's frames start at a multiple of 4 frames. A sector is kept as the
// track's kind gives it (2048 bytes of user data, or all 2352), at its frame's start; audio's samples
// big-endian. The few hunks last read are kept, for the sector after and for the CD's data and its music
// read in turns (they may be read from two threads: so a lock).
struct DiscImage::Chd {
    static constexpr uint32_t kFrameBytes = 2448;
    static constexpr int kHunksKept = 4;
    chd_file *file = nullptr;
    uint32_t hunkBytes = 0;
    uint32_t framesPerHunk = 0;
    uint32_t hunks = 0;
    struct Kept {
        uint32_t hunk = UINT32_MAX;
        uint64_t used = 0;
        std::vector<uint8_t> data;
    };
    std::array<Kept, kHunksKept> kept;
    uint64_t reads = 0;
    std::mutex lock;

    ~Chd() {
        if (file) {
            chd_close(file);
        }
    }
    // A hunk's bytes (null: it cannot be read). Called with the lock held.
    const uint8_t *Hunk(uint32_t hunk) {
        Kept *oldest = &kept[0];
        for (Kept &k : kept) {
            if (k.hunk == hunk) {
                k.used = ++reads;
                return k.data.data();
            }
            if (k.used < oldest->used) {
                oldest = &k;
            }
        }
        oldest->data.resize(hunkBytes);
        oldest->hunk = UINT32_MAX;
        if (hunk >= hunks || chd_read(file, hunk, oldest->data.data()) != CHDERR_NONE) {
            return nullptr;
        }
        oldest->hunk = hunk;
        oldest->used = ++reads;
        return oldest->data.data();
    }
};

DiscImage::DiscImage() = default;

DiscImage::~DiscImage() {
    for (FILE *f : m_files) {
        if (f) {
            fclose(f);
        }
    }
}

// A .chd's tracks, from its metadata (one entry a track, as chdman writes them):
//   TRACK:1 TYPE:MODE1_RAW SUBTYPE:NONE FRAMES:12345 PREGAP:0 PGTYPE:MODE1 PGSUB:NONE POSTGAP:0
// FRAMES counts the frames the file holds of the track: its pregap too when PGTYPE begins with V (the pregap's
// sectors were in the dump), not when the pregap was only said (silence the file does not hold). Older files
// say only TRACK, TYPE, SUBTYPE and FRAMES.
bool DiscImage::OpenChd(const std::string &path) {
    auto chd = std::make_unique<Chd>();
    if (chd_open(path.c_str(), CHD_OPEN_READ, nullptr, &chd->file) != CHDERR_NONE) {
        chd->file = nullptr;
        return false;
    }
    const chd_header *header = chd_get_header(chd->file);
    chd->hunkBytes = header->hunkbytes;
    chd->hunks = header->totalhunks;
    chd->framesPerHunk = header->hunkbytes / Chd::kFrameBytes;
    if (chd->framesPerHunk == 0 || header->hunkbytes % Chd::kFrameBytes != 0) {
        return false; // not a CD's
    }
    uint64_t frame = 0;    // in the file
    int64_t position = 0;  // on the disc, from the first track's start (its pregap's, if it has one)
    for (uint32_t index = 0;; index++) {
        char text[256] = {};
        uint32_t length = 0;
        if (chd_get_metadata(chd->file, CDROM_TRACK_METADATA2_TAG, index, text, sizeof text - 1, &length, nullptr, nullptr) !=
                CHDERR_NONE &&
            chd_get_metadata(chd->file, CDROM_TRACK_METADATA_TAG, index, text, sizeof text - 1, &length, nullptr, nullptr) !=
                CHDERR_NONE) {
            break;
        }
        text[std::min<uint32_t>(length, sizeof text - 1)] = 0;
        int number = 0, frames = 0, pregap = 0, postgap = 0;
        char type[32] = {}, subtype[32] = {}, pregapType[32] = {}, pregapSub[32] = {};
        const int got = sscanf(text, "TRACK:%d TYPE:%31s SUBTYPE:%31s FRAMES:%d PREGAP:%d PGTYPE:%31s PGSUB:%31s POSTGAP:%d",
                               &number, type, subtype, &frames, &pregap, pregapType, pregapSub, &postgap);
        if (got < 4 || frames <= 0 || pregap < 0 || postgap < 0) {
            return false;
        }
        const bool pregapHeld = got >= 6 && pregapType[0] == 'V';
        const std::string kind = type;
        Track track;
        track.number = number;
        track.audio = kind == "AUDIO";
        if (kind == "MODE1" || kind == "MODE1/2048" || kind == "MODE2_FORM1" || kind == "MODE2/2048") {
            track.sectorSize = 2048;
            track.userOffset = 0;
        } else if (kind == "MODE1_RAW" || kind == "MODE1/2352" || kind == "AUDIO") {
            track.sectorSize = 2352;
            track.userOffset = 16;
        } else if (kind == "MODE2_RAW" || kind == "MODE2/2352" || kind == "CDI/2352") {
            track.sectorSize = 2352;
            track.userOffset = 24;
        } else if (kind == "MODE2" || kind == "MODE2/2336" || kind == "MODE2_FORM_MIX") {
            track.sectorSize = 2336;
            track.userOffset = 8;
        } else {
            return false;
        }
        const uint32_t held = pregapHeld ? static_cast<uint32_t>(std::min(pregap, frames)) : 0;
        track.file = 0;
        track.fileOffset = frame + held;
        track.startFAD = static_cast<uint32_t>(position + pregap); // (made right below, once the first is known)
        track.endFAD = track.startFAD + static_cast<uint32_t>(frames) - held - 1;
        m_tracks.push_back(track);
        frame += (static_cast<uint64_t>(frames) + 3) / 4 * 4;
        position += (pregapHeld ? frames : pregap + frames) + postgap;
    }
    if (m_tracks.empty() || frame > static_cast<uint64_t>(chd->hunks) * chd->framesPerHunk) {
        m_tracks.clear();
        return false;
    }
    // (the first track's start is the disc's frame address 150, as in a cue sheet)
    const uint32_t first = m_tracks.front().startFAD;
    for (Track &track : m_tracks) {
        track.startFAD = track.startFAD - first + 150;
        track.endFAD = track.endFAD - first + 150;
    }
    m_leadOut = static_cast<uint32_t>(position) - first + 150;
    m_chd = std::move(chd);
    return true;
}

bool DiscImage::Open(const std::string &path) {
    namespace fs = std::filesystem;
    const fs::path image(path);
    std::string extension = image.extension().string();
    for (char &ch : extension) {
        ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    }
    auto openFile = [&](const fs::path &file, uint64_t &size) -> int {
        FILE *f = fopen(file.string().c_str(), "rb");
        if (!f) {
            return -1;
        }
        std::error_code ec;
        size = fs::file_size(file, ec);
        m_files.push_back(f);
        return static_cast<int>(m_files.size()) - 1;
    };

    if (extension == ".chd") {
        return OpenChd(path);
    }
    if (extension != ".cue") {
        // A single file: a data track's user data (.iso), or raw sectors
        uint64_t size = 0;
        const int file = openFile(image, size);
        if (file < 0) {
            return false;
        }
        Track track;
        track.number = 1;
        track.file = file;
        track.sectorSize = extension == ".iso" ? 2048 : 2352;
        track.userOffset = extension == ".iso" ? 0 : 16;
        track.startFAD = 150;
        track.endFAD = 150 + static_cast<uint32_t>(size / track.sectorSize) - 1;
        m_tracks.push_back(track);
        m_leadOut = track.endFAD + 1;
        return size >= track.sectorSize;
    }

    // The cue sheet: FILE lines name the files; under each, TRACK lines with the track's kind and INDEX
    // lines with where in the file its pause (index 0) and its start (index 1) are, as minutes:seconds:frames
    std::ifstream cue(path);
    if (!cue) {
        return false;
    }
    int file = -1;
    uint64_t fileSize = 0;
    uint32_t fileStartFAD = 150; // where on the disc the current file's first sector is
    struct Pending {
        Track track;
        uint32_t fileFrame = 0;
        int file = -1;
    };
    std::vector<Pending> pending;
    std::vector<std::pair<uint32_t, uint32_t>> fileSpans; // (first FAD, frames) of each file
    uint32_t sectorSize = 2352;
    auto closeFile = [&]() {
        if (file >= 0) {
            const uint32_t frames = static_cast<uint32_t>(fileSize / sectorSize);
            fileSpans.resize(m_files.size());
            fileSpans[static_cast<size_t>(file)] = {fileStartFAD, frames};
            fileStartFAD += frames;
        }
    };
    std::string line;
    while (std::getline(cue, line)) {
        std::istringstream in(line);
        std::string word;
        in >> word;
        if (word == "FILE") {
            closeFile();
            const size_t open = line.find('"'), close = line.rfind('"');
            std::string name;
            if (open != std::string::npos && close > open) {
                name = line.substr(open + 1, close - open - 1);
            } else {
                in >> name;
            }
            file = openFile(image.parent_path() / name, fileSize);
            if (file < 0) {
                return false;
            }
        } else if (word == "TRACK") {
            Pending p;
            std::string kind;
            in >> p.track.number >> kind;
            p.track.audio = kind == "AUDIO";
            p.track.sectorSize = kind.find("2048") != std::string::npos ? 2048 : 2352;
            p.track.userOffset = p.track.sectorSize == 2048 ? 0 : kind.rfind("MODE2", 0) == 0 ? 24 : 16;
            p.file = file;
            sectorSize = p.track.sectorSize;
            pending.push_back(p);
        } else if (word == "INDEX" && !pending.empty()) {
            int index = 0;
            unsigned minutes = 0, seconds = 0, frames = 0;
            char colon;
            in >> index >> minutes >> colon >> seconds >> colon >> frames;
            if (index == 1) {
                pending.back().fileFrame = (minutes * 60 + seconds) * 75 + frames;
            }
        }
    }
    closeFile();
    if (pending.empty()) {
        return false;
    }
    m_leadOut = fileStartFAD;
    for (size_t i = 0; i < pending.size(); i++) {
        Track track = pending[i].track;
        const auto [firstFAD, frames] = fileSpans[static_cast<size_t>(pending[i].file)];
        track.file = pending[i].file;
        track.startFAD = firstFAD + pending[i].fileFrame;
        track.fileOffset = static_cast<uint64_t>(pending[i].fileFrame) * track.sectorSize;
        // (it ends where the next track's file or pause begins)
        uint32_t end = firstFAD + frames;
        if (i + 1 < pending.size() && pending[i + 1].file == pending[i].file) {
            end = firstFAD + pending[i + 1].fileFrame;
        }
        track.endFAD = end - 1;
        m_tracks.push_back(track);
    }
    return true;
}

bool DiscImage::Read(const Track &track, uint32_t sector, uint32_t offset, uint32_t length, uint8_t *out) const {
    if (track.file < 0 || track.startFAD + sector > track.endFAD) {
        return false;
    }
    if (m_chd) {
        Chd &chd = *m_chd;
        const uint64_t frame = track.fileOffset + sector;
        std::lock_guard<std::mutex> hold(chd.lock);
        const uint8_t *hunk = chd.Hunk(static_cast<uint32_t>(frame / chd.framesPerHunk));
        if (!hunk) {
            return false;
        }
        const uint8_t *at = hunk + (frame % chd.framesPerHunk) * Chd::kFrameBytes + offset;
        if (track.audio) {
            for (uint32_t i = 0; i + 1 < length; i += 2) { // (to the little-endian samples a .bin holds)
                out[i] = at[i + 1];
                out[i + 1] = at[i];
            }
        } else {
            memcpy(out, at, length);
        }
        return true;
    }
    FILE *f = m_files[static_cast<size_t>(track.file)];
    const uint64_t at = track.fileOffset + static_cast<uint64_t>(sector) * track.sectorSize + offset;
    return fseeko(f, static_cast<off_t>(at), SEEK_SET) == 0 && fread(out, 1, length, f) == length;
}

bool DiscImage::ReadRaw(const Track &track, uint32_t sector, uint8_t *out) const {
    if (track.sectorSize == 2352) {
        return Read(track, sector, 0, 2352, out);
    }
    // (a sector kept without its sync and header, 16 bytes: they are left zero, as is what it was kept without)
    memset(out, 0, 2352);
    return Read(track, sector, 0, track.sectorSize, out + 16);
}

bool DiscImage::ReadUserData(uint32_t fad, uint8_t *out) const {
    for (const Track &track : m_tracks) {
        if (track.audio || fad < track.startFAD || fad > track.endFAD) {
            continue;
        }
        return Read(track, fad - track.startFAD, track.userOffset, 2048, out);
    }
    return false;
}

} // namespace port
