// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
#include "disc_files.h"

#include <array>
#include <cstring>

namespace port {

namespace {
    uint32_t Le32(const uint8_t *p) {
        return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
    }
    std::string Upper(std::string s) {
        for (char &ch : s) {
            if (ch >= 'a' && ch <= 'z') {
                ch = static_cast<char>(ch - 'a' + 'A');
            }
        }
        return s;
    }
} // namespace

bool DiscFiles::ReadSector(uint32_t lba, uint8_t *out) const {
    return m_disc.ReadUserData(lba + 150, out);
}

uint32_t DiscFiles::AudioSectors(int track) const {
    for (const DiscImage::Track &t : m_disc.Tracks()) {
        if (t.number == track) {
            return t.audio && t.endFAD >= t.startFAD ? t.endFAD - t.startFAD + 1 : 0;
        }
    }
    return 0;
}

bool DiscFiles::ReadAudioSector(int track, uint32_t sector, uint8_t *out) const {
    for (const DiscImage::Track &t : m_disc.Tracks()) {
        if (t.number == track && t.audio) {
            return m_disc.ReadRaw(t, sector, out);
        }
    }
    return false;
}

bool DiscFiles::ReadExtent(const File &file, std::vector<uint8_t> &data) const {
    const uint32_t sectors = (file.size + 2047) / 2048;
    data.resize(static_cast<size_t>(sectors) * 2048);
    for (uint32_t s = 0; s < sectors; s++) {
        if (!ReadSector(file.lba + s, data.data() + static_cast<size_t>(s) * 2048)) {
            return false;
        }
    }
    data.resize(file.size);
    return true;
}

void DiscFiles::ReadDirectory(uint32_t lba, uint32_t size, bool root, int depth) {
    std::vector<uint8_t> dir;
    if (depth > 8 || !ReadExtent({lba, size}, dir)) {
        return;
    }
    for (size_t sector = 0; sector < dir.size(); sector += 2048) {
        for (size_t at = sector; at < sector + 2048 && at < dir.size() && dir[at];) {
            const uint8_t *rec = dir.data() + at;
            const File file{Le32(rec + 2), Le32(rec + 10)};
            const bool directory = rec[25] & 2;
            std::string name(reinterpret_cast<const char *>(rec + 33), rec[32]);
            if (const auto semicolon = name.find(';'); semicolon != std::string::npos) {
                name.resize(semicolon);
            }
            if (directory) {
                // (the first two entries are the directory itself and its parent: one byte, 0 or 1)
                if (!(rec[32] == 1 && (rec[33] == 0 || rec[33] == 1))) {
                    ReadDirectory(file.lba, file.size, false, depth + 1);
                }
            } else {
                if (root && !m_first.lba) {
                    m_first = file;
                }
                if (root) {
                    m_files[Upper(name)] = file;
                } else {
                    m_files.emplace(Upper(name), file);
                }
            }
            at += rec[0];
        }
    }
}

bool DiscFiles::Open(const std::string &path) {
    if (!m_disc.Open(path) || m_disc.Tracks().empty()) {
        return false;
    }
    m_systemArea.resize(16 * 2048);
    for (uint32_t lba = 0; lba < 16; lba++) {
        if (!ReadSector(lba, m_systemArea.data() + lba * 2048)) {
            return false;
        }
    }
    if (memcmp(m_systemArea.data(), "SEGA SEGASATURN ", 16) != 0) {
        return false;
    }
    // The primary volume descriptor names the root directory
    std::array<uint8_t, 2048> pvd;
    if (!ReadSector(16, pvd.data()) || memcmp(pvd.data() + 1, "CD001", 5) != 0) {
        return false;
    }
    ReadDirectory(Le32(pvd.data() + 156 + 2), Le32(pvd.data() + 156 + 10), true, 0);
    return m_first.lba != 0;
}

bool DiscFiles::Read(const std::string &name, std::vector<uint8_t> &data) const {
    const auto it = m_files.find(Upper(name));
    return it != m_files.end() && ReadExtent(it->second, data);
}

bool DiscFiles::ReadFirstFile(std::vector<uint8_t> &data) const {
    return m_first.lba && ReadExtent(m_first, data);
}

} // namespace port
