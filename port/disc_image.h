// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// A CD image as dumped: a cue sheet naming one or more files of raw sectors (or a single .iso file of a
// data track's user data). Gives the tracks, where they are on the disc, and their sectors.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace port {

class DiscImage {
public:
    struct Track {
        int number = 0;
        bool audio = false;
        uint32_t startFAD = 0;    // frame address of its index 1
        uint32_t endFAD = 0;      // of its last sector
        uint32_t sectorSize = 2352; // bytes a sector takes in the file
        uint32_t userOffset = 16; // where a data sector's 2048 bytes of user data start
        int file = -1;
        uint64_t fileOffset = 0;  // of the sector at startFAD
    };

    ~DiscImage();
    bool Open(const std::string &path);
    const std::vector<Track> &Tracks() const {
        return m_tracks;
    }
    uint32_t LeadOutFAD() const {
        return m_leadOut;
    }
    // A sector's 2048 bytes of user data (data tracks), by frame address
    bool ReadUserData(uint32_t fad, uint8_t *out) const;
    // A whole sector of a track (2352 bytes: for audio, 588 stereo samples, little-endian), by its number
    // within the track
    bool ReadRaw(const Track &track, uint32_t sector, uint8_t *out) const;

private:
    std::vector<Track> m_tracks;
    std::vector<FILE *> m_files;
    uint32_t m_leadOut = 150;
};

} // namespace port
