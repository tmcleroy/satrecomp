// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// Export pictures (pictures.h).
#include "pictures.h"

#include "game.h"
#include "png_write.h"

#include <filesystem>
#include <fstream>
#include <vector>

namespace port {

namespace {

// A name that is a file name everywhere: letters, digits, '-', '_' and '.'; anything else becomes '-'
std::string FileName(const std::string &name) {
    std::string out;
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        out += ok ? c : '-';
    }
    return out.empty() ? "picture" : out;
}

bool WriteFile(const std::filesystem::path &path, const std::vector<uint8_t> &bytes) {
    if (bytes.empty()) {
        return false;
    }
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(file);
}

} // namespace

std::string PicturesFolder() {
    const std::string beside = satrecomp::DataFilePath("pictures");
    return beside.empty() ? (std::filesystem::current_path() / "pictures").string() : beside;
}

int ExportPictures(const std::string &folder, std::string &error) {
    const satrecomp::Game &game = satrecomp::TheGame();
    if (!game.exportPictures) {
        error = "This game has no pictures to export.";
        return -1;
    }
    std::error_code fault;
    std::filesystem::create_directories(folder, fault);
    if (fault) {
        error = "Cannot make the folder " + folder + ": " + fault.message();
        return -1;
    }
    int written = 0, failed = 0;
    satrecomp::PictureOut out;
    out.picture = [&](const std::string &name, const uint8_t *pixels, int width, int height, int stride) {
        // (the Saturn's five bits of each colour to eight: the top bits again below them, so that white is 255)
        std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3);
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                const uint8_t *p = pixels + (static_cast<size_t>(y) * stride + x) * 2;
                const unsigned v = static_cast<unsigned>(p[0] << 8 | p[1]);
                uint8_t *q = &rgb[(static_cast<size_t>(y) * width + x) * 3];
                for (int c = 0; c < 3; c++) {
                    const unsigned five = (v >> (5 * c)) & 31;
                    q[c] = static_cast<uint8_t>(five << 3 | five >> 2);
                }
            }
        }
        if (WriteFile(std::filesystem::path(folder) / (FileName(name) + ".png"),
                      EncodePng(rgb.data(), static_cast<uint32_t>(width), static_cast<uint32_t>(height)))) {
            written++;
        } else {
            failed++;
        }
    };
    out.file = [&](const std::string &name, const std::vector<uint8_t> &bytes, const std::string &extension) {
        if (WriteFile(std::filesystem::path(folder) / (FileName(name) + "." + FileName(extension)), bytes)) {
            written++;
        } else {
            failed++;
        }
    };
    game.exportPictures(out);
    if (failed) {
        error = std::to_string(failed) + " could not be written to " + folder + ".";
        return -1;
    }
    return written;
}

} // namespace port
