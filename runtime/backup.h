// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// The Saturn's internal save memory (32 KiB), in the format the console's BIOS keeps it in, so that saves
// made on a console or with an emulator can be used and the other way round.
//
// The memory is blocks of 64 bytes. The first holds a signature; a file starts in a block marked as a start
// and goes on through the blocks its list names: a header (name, comment, language, date, size), the list of
// its further blocks, then its data. Every block gives its first four bytes to the mark.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace satrecomp::bup {

enum class Language : uint8_t { Japanese, English, French, German, Spanish, Italian };

struct BackupFileHeader {
    std::string filename; // up to 11 characters
    std::string comment;  // up to 10
    Language language = Language::Japanese;
    uint32_t date = 0; // minutes since 1 January 1980
};
struct BackupFileInfo {
    BackupFileHeader header;
    uint32_t size = 0;      // of the data
    uint32_t numBlocks = 0; // blocks the file takes
};
struct BackupFile {
    BackupFileHeader header;
    std::vector<uint8_t> data;
};
enum class BackupFileImportResult { Imported, Overwritten, FileExists, NoSpace };

class BackupMemory {
public:
    // The image file is read now (if there is one) and written back after every change
    void Open(const std::string &path, size_t size = 0x8000);
    size_t Size() const {
        return m_image.size();
    }
    bool IsHeaderValid() const;
    void Format();
    uint32_t GetBlockSize() const {
        return kBlock;
    }
    uint32_t GetTotalBlocks() const {
        return static_cast<uint32_t>(m_image.size() / kBlock);
    }
    uint32_t GetUsedBlocks() const;
    std::vector<BackupFileInfo> List() const;
    std::optional<BackupFile> Export(const std::string &filename) const;
    BackupFileImportResult Import(const BackupFile &file, bool overwrite);
    bool Delete(const std::string &filename);

private:
    static constexpr uint32_t kBlock = 64;
    std::vector<uint8_t> m_image;
    std::string m_path;

    struct Found {
        uint32_t first = 0;           // the file's first block
        std::vector<uint32_t> blocks; // all of its blocks, in order
        BackupFileInfo info;
        uint32_t dataOffset = 0; // where in the file's stream (after the marks) its data starts
    };
    std::vector<Found> Scan() const;
    void Save() const;
};

} // namespace satrecomp::bup
