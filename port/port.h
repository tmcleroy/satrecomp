// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// Shared state between the port's entry point (main.cpp) and its frontends.
#pragma once

#include "backend.h" // (which machine: runtime/backend.h)

#include "disc_files.h"
#include "menu.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace satrecomp::sound {
class Generator;
}
namespace satrecomp::video {
class Renderer;
class YmirAdapter;
class Gpu;
} // namespace satrecomp::video
struct SDL_GPUDevice;

namespace port {

// Saturn control pad buttons, as bits that are 1 while pressed (the hardware report is inverted).
enum PadButton : uint16_t {
    kPadRight = 1u << 15,
    kPadLeft = 1u << 14,
    kPadDown = 1u << 13,
    kPadUp = 1u << 12,
    kPadStart = 1u << 11,
    kPadA = 1u << 10,
    kPadC = 1u << 9,
    kPadB = 1u << 8,
    kPadR = 1u << 7,
    kPadX = 1u << 6,
    kPadY = 1u << 5,
    kPadZ = 1u << 4,
    kPadL = 1u << 3,
};

struct Options {
    std::string biosPath;
    std::string discPath;
    std::string backupPath;   // internal backup memory image (created when missing)
    std::string moviePlay;    // input movie to replay
    std::string movieRecord;  // input movie to write
    std::string shotPrefix;   // PNG prefix for --shot-every
    std::string hashLog;      // per-frame state hash log
    std::string recordBlocks; // block database to update (see runtime/recomp.h)
    std::string cpuMode = "native"; // native | free | recomp | interp | pure
    std::string video;        // gpu | software (default: gpu in a window, software headless)
    uint32_t internalScale = 0; // gpu: times the original resolution polygons are drawn at (0: as the window needs)
    uint32_t outputWidth = 0, outputHeight = 448; // gpu, headless: the picture's size (width 0: as its shape asks)
    float aspect = 4.0f / 3.0f; // the picture's shape; wider than 4:3 is widescreen (gpu only)
    std::string features;     // comma-separated feature names
    std::string settingsPath; // settings.ini with the player's feature choices (written when one is changed in the game)
    std::string stateSave, stateLoad; // tests: "FRAME:path": save or load a state when that frame is reached
    std::string dumpRam;      // prefix: write <prefix>high.bin / low.bin (work RAM) at exit
    uint64_t frames = 0;      // stop after this many frames (0 = run until closed)
    uint64_t autoSeed = 0;    // nonzero: random input (exploration), seeded
    uint64_t autoFrom = 0;    // first frame of random input
    uint32_t shotEvery = 0;
    bool headless = false;
    bool fullscreen = false;
    bool mute = false;
    bool unthrottled = false;
    bool stats = false;
    bool videoStats = false;
    int scale = 0;            // the window's size in times 240 lines (0: as the Display page's Window size says)
    std::string shader;       // a shader preset to draw through, whatever the Display page says
};

// An input movie: pad states that change at given frame numbers. Text format, one change per line:
//   <frame> <pad1 hex> <pad2 hex>
struct Movie {
    std::map<uint64_t, std::pair<uint16_t, uint16_t>> changes;
    bool Load(const std::string &path);
    bool Save(const std::string &path) const;
};

struct Port {
    Options opt;
    std::unique_ptr<satrecomp::Machine> saturn;

    // Latest finished frame (XBGR8888), written by the core's render callback
    std::mutex fbMutex;
    std::vector<uint32_t> fb;
    uint32_t fbWidth = 0, fbHeight = 0;
    uint64_t fbSerial = 0;

    // Pad states (pressed bits) for the current frame
    std::atomic<uint16_t> pad[2] = {0, 0};
    uint64_t frame = 0;

    Movie playMovie;
    Movie recMovie;
    uint16_t lastRec[2] = {0, 0};
    uint64_t autoState = 0;

    // Audio sink, set by the frontend (called on the emulation thread for every 44.1 kHz stereo sample)
    void (*audioSink)(int16_t l, int16_t r, void *ctx) = nullptr;
    void *audioCtx = nullptr;

    FILE *hashFile = nullptr;
    bool temporaryBackup = false;
    uint64_t audioHash = 0xcbf29ce484222325ull; // of every sound sample so far
    DiscFiles discFiles;
    satrecomp::video::YmirAdapter *videoAdapter = nullptr; // (owned by the video hardware)
    satrecomp::video::Renderer *videoRenderer = nullptr;   // (the adapter's)
    // Drawing by the GPU (video/gpu.h), when that is how the picture is made
    satrecomp::sound::Generator *soundGenerator = nullptr; // what makes the sound chip's samples (sound/generator.h)
    SDL_GPUDevice *gpuDevice = nullptr;
    satrecomp::video::Gpu *gpu = nullptr;

    // The in-game menu (menu.h). MenuAct does what a press in it asked for (save or load a state, in the
    // menu's slot) and closes it if that went well; MenuPicture gives the game's picture as it is shown, to
    // draw the menu over.
    Menu menu;
    void MenuAct(Menu::Action action);
    bool quit = false; // the player chose the menu's "Quit game": the run stops
    bool MenuPicture(std::vector<uint32_t> &pixels, uint32_t &width, uint32_t &height);
    std::string menuTest; // tests: "FRAME:KEYS[:shot.png]": open the menu at that frame and press (u d l r a s)

    // The readout (hud.h) as the GPU renderer shows it: made anew when what it shows or the picture's size
    // changes
    void UpdateHud();
    bool Init();
    // Applies movie input for this frame, runs it, then writes screenshots and hashes as configured.
    void RunFrame();
    // Run-ahead (the port's "run_ahead" feature: off, 1 or 2 frames): after the frame, the next ones are run
    // on the controls as they are and their picture shown; then the game is put back as it was after the
    // frame. So it goes on exactly as without it, and a press is seen a frame or two sooner. Not while the
    // frontend goes on a frame at a time (`stepping`), nor slowed, nor when the game says not.
    void RunAhead();
    bool stepping = false;
    // Widescreen switched while playing (the port's "widescreen" feature, which the menu changes): the
    // picture's shape and how much more the renderer draws at the sides. GPU only.
    void ApplyWidescreen(bool on);
    bool wideShown = false;
    void Shutdown();
    void SaveShot(const std::string &path);
    // Save states: the whole game as it is between two frames, to a file and back. False (and why, in
    // `error`) if that did not work; a failed load leaves the game as it was.
    bool SaveState(const std::string &path, std::string &error);
    void SaveStateData(std::vector<uint8_t> &data);
    bool LoadStateData(const std::vector<uint8_t> &data, std::string &error);
    // The reset point: a state kept in memory to go back to at a press, for practising a situation
    std::vector<uint8_t> resetPoint;
    bool SetResetPoint(std::string &error);
    bool GoToResetPoint(std::string &error);
    std::string resetTest; // tests: "SET:GO": the reset point set at one frame and gone back to at another
    bool LoadState(const std::string &path, std::string &error);
    // Where slot n's state is kept (beside the save memory)
    std::string StatePath(int slot) const;
    // The Display page's Shader: the preset to draw through (a .slangp file), or "" for none (port/shading.h)
    std::string ShaderPreset() const;
    uint64_t StateHash();
};

// Debugging aid (--video-stats): see video_stats.cpp
void VideoStatsFrame(Port &port);
void VideoStatsReport();

// Windowed frontend (SDL3). Returns the process exit code.
int RunWindowed(Port &port);

} // namespace port
