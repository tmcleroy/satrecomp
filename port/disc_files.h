// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// The files of the game disc, read straight from the disc image (ISO 9660 on the data track).
#pragma once

#include "disc_image.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace port {

class DiscFiles {
public:
    // Opens the disc image and reads its directories. False if it is not a Saturn disc.
    bool Open(const std::string &path);

    // The system area: the first 16 sectors (disc header and initial program)
    const std::vector<uint8_t> &SystemArea() const {
        return m_systemArea;
    }

    // Reads a whole file by name (upper or lower case; in any directory if the name is not in the root)
    bool Read(const std::string &name, std::vector<uint8_t> &data) const;

    // One sector (2048 bytes) of the data track by frame address
    bool ReadDataSector(uint32_t frameAddress, uint8_t *out) const {
        return frameAddress >= 150 && ReadSector(frameAddress - 150, out);
    }

    // Audio tracks: how many sectors track `track` (2..99) has, 0 if it is not an audio track; and one
    // sector of it (2352 bytes of 16-bit little-endian stereo samples)
    uint32_t AudioSectors(int track) const;
    bool ReadAudioSector(int track, uint32_t sector, uint8_t *out) const;

    // The tracks and where they are (for the CD drive's table of contents)
    const DiscImage &Image() const {
        return m_disc;
    }
    // The first file of the root directory: the program the BIOS loads
    bool ReadFirstFile(std::vector<uint8_t> &data) const;

private:
    struct File {
        uint32_t lba;
        uint32_t size;
    };

    bool ReadSector(uint32_t lba, uint8_t *out) const;
    bool ReadExtent(const File &file, std::vector<uint8_t> &data) const;
    void ReadDirectory(uint32_t lba, uint32_t size, bool root, int depth);

    DiscImage m_disc;
    std::vector<uint8_t> m_systemArea;
    std::map<std::string, File> m_files; // by upper-case name; the root directory's names win
    File m_first{0, 0};
};

} // namespace port
