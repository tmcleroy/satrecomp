// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
#include "backup.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace satrecomp::bup {

namespace {
    const char kSignature[] = "BackUpRam Format";
    uint32_t Get32(const uint8_t *p) {
        return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3];
    }
    void Put32(uint8_t *p, uint32_t v) {
        p[0] = static_cast<uint8_t>(v >> 24);
        p[1] = static_cast<uint8_t>(v >> 16);
        p[2] = static_cast<uint8_t>(v >> 8);
        p[3] = static_cast<uint8_t>(v);
    }
    std::string Text(const uint8_t *p, size_t max) {
        std::string s(reinterpret_cast<const char *>(p), max);
        const size_t end = s.find('\0');
        if (end != std::string::npos) {
            s.resize(end);
        }
        return s;
    }
    constexpr uint32_t kMark = 4, kPayload = 60, kHeader = 30; // per block: the mark, then the file's stream
} // namespace

void BackupMemory::Open(const std::string &path, size_t size) {
    m_path = path;
    m_image.assign(size, 0);
    if (FILE *f = fopen(path.c_str(), "rb")) {
        const size_t got = fread(m_image.data(), 1, m_image.size(), f);
        (void)got;
        fclose(f);
    }
    if (!IsHeaderValid()) {
        Format();
    }
}

void BackupMemory::Save() const {
    if (m_path.empty()) {
        return;
    }
    if (FILE *f = fopen(m_path.c_str(), "wb")) {
        fwrite(m_image.data(), 1, m_image.size(), f);
        fclose(f);
    }
}

bool BackupMemory::IsHeaderValid() const {
    return m_image.size() >= kBlock && memcmp(m_image.data(), kSignature, 16) == 0;
}

void BackupMemory::Format() {
    std::fill(m_image.begin(), m_image.end(), 0);
    for (uint32_t i = 0; i < kBlock && i + 16 <= m_image.size(); i += 16) {
        memcpy(&m_image[i], kSignature, 16);
    }
    Save();
}

// Every file: its blocks, its header, where its data starts
std::vector<BackupMemory::Found> BackupMemory::Scan() const {
    std::vector<Found> files;
    const uint32_t total = GetTotalBlocks();
    for (uint32_t block = 2; block < total; block++) {
        const uint8_t *p = &m_image[block * kBlock];
        if (!(p[0] & 0x80)) {
            continue;
        }
        Found file;
        file.first = block;
        file.blocks.push_back(block);
        file.info.header.filename = Text(p + 4, 11);
        file.info.header.language = static_cast<Language>(p[15]);
        file.info.header.comment = Text(p + 16, 10);
        file.info.header.date = Get32(p + 26);
        file.info.size = Get32(p + 30);
        // The list of further blocks follows the header, itself running on into those blocks
        uint32_t position = kHeader; // in the file's stream
        auto byteAt = [&](uint32_t at, uint8_t &out) {
            const uint32_t index = at / kPayload;
            if (index >= file.blocks.size()) {
                return false;
            }
            out = m_image[file.blocks[index] * kBlock + kMark + at % kPayload];
            return true;
        };
        bool valid = true;
        for (;;) {
            uint8_t high, low;
            if (!byteAt(position, high) || !byteAt(position + 1, low)) {
                valid = false;
                break;
            }
            position += 2;
            const uint32_t next = (static_cast<uint32_t>(high) << 8) | low;
            if (next == 0) {
                break;
            }
            if (next >= total || file.blocks.size() > total) {
                valid = false;
                break;
            }
            file.blocks.push_back(next);
        }
        if (!valid) {
            continue;
        }
        file.dataOffset = position;
        file.info.numBlocks = static_cast<uint32_t>(file.blocks.size());
        files.push_back(std::move(file));
    }
    return files;
}

uint32_t BackupMemory::GetUsedBlocks() const {
    uint32_t used = 2; // (the signature block and the one after it)
    for (const Found &file : Scan()) {
        used += static_cast<uint32_t>(file.blocks.size());
    }
    return used;
}

std::vector<BackupFileInfo> BackupMemory::List() const {
    std::vector<BackupFileInfo> list;
    for (const Found &file : Scan()) {
        list.push_back(file.info);
    }
    return list;
}

std::optional<BackupFile> BackupMemory::Export(const std::string &filename) const {
    for (const Found &file : Scan()) {
        if (file.info.header.filename != filename) {
            continue;
        }
        BackupFile out;
        out.header = file.info.header;
        out.data.resize(file.info.size);
        for (uint32_t i = 0; i < file.info.size; i++) {
            const uint32_t at = file.dataOffset + i, index = at / kPayload;
            if (index >= file.blocks.size()) {
                out.data.resize(i);
                break;
            }
            out.data[i] = m_image[file.blocks[index] * kBlock + kMark + at % kPayload];
        }
        return out;
    }
    return std::nullopt;
}

bool BackupMemory::Delete(const std::string &filename) {
    for (const Found &file : Scan()) {
        if (file.info.header.filename == filename) {
            for (const uint32_t block : file.blocks) {
                std::fill_n(&m_image[block * kBlock], kBlock, 0);
            }
            Save();
            return true;
        }
    }
    return false;
}

BackupFileImportResult BackupMemory::Import(const BackupFile &file, bool overwrite) {
    const bool exists = Export(file.header.filename).has_value();
    if (exists && !overwrite) {
        return BackupFileImportResult::FileExists;
    }
    // Which blocks are free (as they would be with the old file gone)
    const uint32_t total = GetTotalBlocks();
    std::vector<bool> used(total, false);
    used[0] = used[1] = true;
    for (const Found &other : Scan()) {
        if (other.info.header.filename != file.header.filename) {
            for (const uint32_t block : other.blocks) {
                used[block] = true;
            }
        }
    }
    // The stream is the header, the list (two bytes a further block, and two for its end) and the data
    uint32_t blocks = 1;
    while (blocks * kPayload < kHeader + (blocks - 1) * 2 + 2 + file.data.size()) {
        blocks++;
    }
    std::vector<uint32_t> chosen;
    for (uint32_t block = 2; block < total && chosen.size() < blocks; block++) {
        if (!used[block]) {
            chosen.push_back(block);
        }
    }
    if (chosen.size() < blocks) {
        return BackupFileImportResult::NoSpace;
    }
    if (exists) {
        Delete(file.header.filename);
    }
    std::vector<uint8_t> stream(blocks * kPayload, 0);
    const size_t nameLength = std::min<size_t>(file.header.filename.size(), 11);
    memcpy(&stream[0], file.header.filename.data(), nameLength);
    stream[11] = static_cast<uint8_t>(file.header.language);
    memcpy(&stream[12], file.header.comment.data(), std::min<size_t>(file.header.comment.size(), 10));
    Put32(&stream[22], file.header.date);
    Put32(&stream[26], static_cast<uint32_t>(file.data.size()));
    size_t position = kHeader;
    for (uint32_t i = 1; i < blocks; i++) {
        stream[position++] = static_cast<uint8_t>(chosen[i] >> 8);
        stream[position++] = static_cast<uint8_t>(chosen[i]);
    }
    position += 2; // (the list's end)
    std::copy(file.data.begin(), file.data.end(), stream.begin() + static_cast<long>(position));
    for (uint32_t i = 0; i < blocks; i++) {
        uint8_t *p = &m_image[chosen[i] * kBlock];
        Put32(p, i == 0 ? 0x80000000u : 0);
        memcpy(p + kMark, &stream[i * kPayload], kPayload);
    }
    Save();
    return exists ? BackupFileImportResult::Overwritten : BackupFileImportResult::Imported;
}

} // namespace satrecomp::bup
