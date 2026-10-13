// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
#include "disc_image.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace port {

DiscImage::~DiscImage() {
    for (FILE *f : m_files) {
        if (f) {
            fclose(f);
        }
    }
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

bool DiscImage::ReadRaw(const Track &track, uint32_t sector, uint8_t *out) const {
    if (track.file < 0 || track.startFAD + sector > track.endFAD) {
        return false;
    }
    FILE *f = m_files[static_cast<size_t>(track.file)];
    if (fseeko(f, static_cast<off_t>(track.fileOffset + static_cast<uint64_t>(sector) * track.sectorSize), SEEK_SET) != 0) {
        return false;
    }
    if (track.sectorSize == 2352) {
        return fread(out, 1, 2352, f) == 2352;
    }
    memset(out, 0, 2352);
    return fread(out + 16, 1, 2048, f) == 2048;
}

bool DiscImage::ReadUserData(uint32_t fad, uint8_t *out) const {
    for (const Track &track : m_tracks) {
        if (track.audio || fad < track.startFAD || fad > track.endFAD) {
            continue;
        }
        FILE *f = m_files[static_cast<size_t>(track.file)];
        const uint64_t at = track.fileOffset + static_cast<uint64_t>(fad - track.startFAD) * track.sectorSize + track.userOffset;
        return fseeko(f, static_cast<off_t>(at), SEEK_SET) == 0 && fread(out, 1, 2048, f) == 2048;
    }
    return false;
}

} // namespace port
