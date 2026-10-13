// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// What satrecomp tells the launcher the recomp ports share (recomp-common/launcher/src/launcher.h): the words
// for a disc image, the check of the player's, what the game is started with, the buttons that can be chosen.
// The game identifies itself through LAUNCHER_* definitions (from games/<id>/game.cfg).

#include "launcher.h"

#include "../../port/bindings.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace {

std::string lower_extension(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

std::string trimmed(const char* text, size_t size) {
    std::string s(text, size);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) {
        s.pop_back();
    }
    return s;
}

// The data track of a .cue image: its first FILE entry, next to the .cue.
std::string first_track_of(const std::string& cue) {
    std::ifstream in(cue);
    std::string line;
    while (std::getline(in, line)) {
        size_t at = line.find("FILE");
        size_t open = line.find('"');
        size_t close = line.rfind('"');
        if (at != std::string::npos && open != std::string::npos && close > open) {
            return (std::filesystem::path(cue).parent_path() / line.substr(open + 1, close - open - 1)).string();
        }
    }
    return {};
}

// The Saturn disc header (the first 256 bytes of the first sector): at the start of a 2048-byte
// sector image, or after the 16-byte sync and header of a raw 2352-byte sector.
bool read_disc_header(const std::string& file, char header[256]) {
    std::FILE* f = std::fopen(file.c_str(), "rb");  // read only, always
    if (!f) {
        return false;
    }
    char sector[16 + 256];
    size_t got = std::fread(sector, 1, sizeof sector, f);
    std::fclose(f);
    static const char kMagic[] = "SEGA SEGASATURN ";
    for (size_t offset : {size_t{0}, size_t{16}}) {
        if (got >= offset + 256 && std::memcmp(sector + offset, kMagic, 16) == 0) {
            std::memcpy(header, sector + offset, 256);
            return true;
        }
    }
    return false;
}

// A disc image: .cue (with its .bin files), .iso, .chd, .ccd or .mds. Reads the disc's header from .cue and
// .iso images to tell which game and release it is. The launcher only ever reads it: never copies, moves or
// changes it.
launcher::FileCheck check_disc(const std::string& path) {
    using Result = launcher::FileCheck::Result;
    launcher::FileCheck check;
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        check.message = "The file can't be opened.";
        return check;
    }
    std::string ext = lower_extension(path);
    std::string data;
    if (ext == ".cue") {
        data = first_track_of(path);
        if (data.empty() || !std::filesystem::is_regular_file(data, error)) {
            check.result = Result::NotUsable;
            check.message = "The .cue file's tracks (.bin files) aren't next to it.";
            return check;
        }
    } else if (ext == ".iso") {
        data = path;
    } else if (ext == ".chd" || ext == ".ccd" || ext == ".mds") {
        check.result = Result::Usable;
        check.message = "A disc image (this format isn't checked here; the game will say if it can't use it).";
        return check;
    } else {
        check.result = Result::NotUsable;
        check.message = "This isn't a disc image. Choose the .cue file (or a .chd or .iso).";
        return check;
    }
    char header[256];
    if (!read_disc_header(data, header)) {
        check.result = Result::NotUsable;
        check.message = "This isn't a Sega Saturn disc.";
        return check;
    }
    std::string product = trimmed(header + 0x20, 10);
    std::string version = trimmed(header + 0x2A, 6);
    if (product != LAUNCHER_DISC_PRODUCT) {
        check.result = Result::NotUsable;
        check.message = "This disc is a different game (" + trimmed(header + 0x60, 112) + "), not " LAUNCHER_GAME_NAME ".";
    } else if (version != LAUNCHER_DISC_VERSION) {
        check.result = Result::Usable;
        check.message = LAUNCHER_GAME_NAME ", but another release (" + version + ") than the known one: parts of it will run slower.";
    } else {
        check.result = Result::Good;
        check.message = LAUNCHER_DISC_NAME ", verified.";
    }
    return check;
}

// What the game is started with: the player's disc image (read only) and their saves folder.
std::vector<std::string> arguments(const launcher::Launch& launch) {
    std::vector<std::string> args = {"--disc", launch.file, "--backup", (launch.folder / "saves" / "backup.bin").string(),
                                     "--settings", (launch.folder / "settings.ini").string()};
    if (launch.on("fullscreen")) {
        args.push_back("--fullscreen");
    }
    if (!launch.on("high_resolution")) {
        args.push_back("--video");
        args.push_back("software");
    } else if (launch.on("widescreen")) {
        args.push_back("--widescreen");
    }
    return args;
}

// A console-like session, where a window is no use: SteamOS's Game Mode (Steam Deck, Steam Machine), which runs
// one program at a time full screen under gamescope. There the game fills the screen unless the player says not.
bool console_session() {
    auto is = [](const char* name, const char* value) {
        const char* found = std::getenv(name);
        return found && std::strcmp(found, value) == 0;
    };
    return is("XDG_CURRENT_DESKTOP", "gamescope") || is("SteamGamepadUI", "1") || is("SteamDeck", "1");
}

}  // namespace


const launcher::Description& launcher::describe() {
    static const Description about = [] {
        Description d;
        d.original = "on the Saturn";
        d.file_heading = "GAME DISC";
        d.file_intro = "Choose your own copy of the game (a disc image). It stays where it is: the launcher only reads it.";
        d.file_empty = "No game disc chosen. Drop its .cue file on this window or choose it.";
        d.file_not_set_up = "The game isn't set up yet. Open the launcher (without --play) once to choose your game disc.";
        d.file_filter = {"Saturn disc image", "cue;chd;iso;ccd;mds"};
        d.file_key = "disc";
        d.check = check_disc;
        d.arguments = arguments;
        d.saves_folder = true;
        d.display = {
            // (a window unless asked, as in the other ports; in SteamOS's Game Mode the other way round)
            {"fullscreen", "Full screen", "F11 switches while playing.", console_session(), nullptr},
            {"high_resolution", "High resolution", "Polygons drawn at the window's resolution. Off: the original's.", true, nullptr},
            {"widescreen", "Widescreen", "16:9: fights show more at the sides. Needs high resolution.", false, "high_resolution"},
        };
        d.bindings = port::BindingTable();
        d.controls_heading = "WHILE PLAYING";
        d.controls = {{"Keys", "Esc or F1: menu (save states, cheats, training aids, quit)   F11: full screen. On a gamepad: Back (Select), alone or with Start, or a stick's click opens and closes the menu. (A button given to the game above does only that.)"}};
        return d;
    }();
    return about;
}
