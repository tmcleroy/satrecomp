// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// satrecomp port entry point: sets up the Saturn hardware, attaches the recompiled game code and runs it,
// either in a window (frontend_sdl.cpp) or headless for tests and tools.
#include "hud.h"
#include "port.h"

#include "png_write.h"
#include "hle_bios.h"
#ifdef SATRECOMP_OWN_MACHINE
#include "../machine/machine.h"
#else
#include "../sound/generator.h"
#include "../video/ymir_adapter.h"
#endif
#include "music.h"
#include "../video/gpu.h"
#include "../video/renderer.h"
#include "game.h"
#include "lift.h"
#include "state.h"
#include "recomp.h"

#include "game_info.h" // generated from games/<id>/game.cfg

#include <algorithm>
#include <cstdlib>
#include <map>
#include <array>
#include <unistd.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifndef SATRECOMP_OWN_MACHINE
// SATRECOMP_IO_STATS=1: which hardware the game's CPUs touch, and how often (large memories as one entry
// each, registers by address)
namespace {
    std::map<uint32_t, std::array<uint64_t, 2>> g_ioStats;
    void IoTrace(uint32_t address, uint32_t size, bool write) {
        uint32_t key = address;
        if (address < 0xFFFFFE00u) {
            key &= 0x07FFFFFFu;
            const uint32_t region = key >> 16;
            if (region >= 0x5A0 && region < 0x5B0) { // sound RAM
                key = 0x05A00000;
            } else if (region >= 0x5C0 && region < 0x5D0) { // VDP1 VRAM and framebuffer
                key = key & 0xFFF80000u;
            } else if (region >= 0x5E0 && region < 0x5F0) { // VDP2 VRAM
                key = 0x05E00000;
            } else if (region == 0x5F0) { // colour RAM
                key = 0x05F00000;
            } else if (region >= 0x180 && region < 0x200) { // backup memory
                key = 0x01800000;
            } else if (region >= 0x200 && region < 0x500) { // cartridge
                key = 0x02000000;
            }
        }
        g_ioStats[key & ~1u][write] += 1;
        (void)size;
    }
} // namespace
#endif


namespace port {

bool Movie::Load(const std::string &path) {
    FILE *f = fopen(path.c_str(), "r");
    if (!f) {
        return false;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        unsigned long long frame;
        unsigned p1 = 0, p2 = 0;
        if (line[0] == '#' || sscanf(line, "%llu %x %x", &frame, &p1, &p2) < 2) {
            continue;
        }
        changes[frame] = {static_cast<uint16_t>(p1), static_cast<uint16_t>(p2)};
    }
    fclose(f);
    return true;
}

bool Movie::Save(const std::string &path) const {
    FILE *f = fopen(path.c_str(), "w");
    if (!f) {
        return false;
    }
    fprintf(f, "# satrecomp input movie: frame pad1 pad2 (pressed bits, hex)\n");
    for (auto &[frame, pads] : changes) {
        fprintf(f, "%llu %04x %04x\n", static_cast<unsigned long long>(frame), pads.first, pads.second);
    }
    fclose(f);
    return true;
}

// The port's own features, beside the game's: in the launcher's list (but for widescreen, whose switch is the
// launcher's own) and the menu, as the game's are
static void RegisterPortFeatures() {
    satrecomp::Game &game = satrecomp::TheGame();
    game.features.push_back({"run_ahead", "Game", "Run-ahead",
                             "Shows the game a frame or two further on than it is, so that what you press is seen sooner (input lag "
                             "taken away). The game runs exactly as without it; it takes two or three times the processor.",
                             0, {"Off", "1 frame", "2 frames"}});
    game.features.push_back({"widescreen", "Display", "Widescreen",
                             "16:9: fights show more at the sides. The launcher's switch changes with it. Needs high resolution.",
                             satrecomp::kFeatureFirstPage});
}

static bool ReadFile(const std::string &path, std::vector<uint8_t> &out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), {});
    return true;
}

#ifndef SATRECOMP_OWN_MACHINE
// Debugging aid (SATRECOMP_CD_TRACE=1): every CD block command and its response, with the frame number
struct CDTracer final : ymir::debug::ICDBlockTracer {
    const uint64_t *frame = nullptr;
    ymir::Saturn *saturn = nullptr;
    void ProcessCommand(uint16_t cr1, uint16_t cr2, uint16_t cr3, uint16_t cr4) final {
        fprintf(stderr, "cd %6llu cmd %04X %04X %04X %04X (HIRQ %04X)\n", static_cast<unsigned long long>(*frame), cr1,
                cr2, cr3, cr4, saturn->mainBus.Peek<uint16_t>(0x5890008));
        if (getenv("SATRECOMP_CD_CALLERS")) {
            // (who asked: the master CPU's PC and PR, and what on its stack looks like return addresses)
            auto &probe = saturn->masterSH2.GetProbe();
            fprintf(stderr, "cd        from PC %08X PR %08X stack:", probe.PC(), probe.PR());
            const uint32_t sp = probe.R()[15];
            for (uint32_t i = 0, shown = 0; i < 96 && shown < 10; i++) {
                const uint32_t v = saturn->mainBus.Peek<uint32_t>(sp + i * 4);
                if ((v & 0xFFF00001) == 0x06000000 && v >= 0x06004000) {
                    fprintf(stderr, " %08X", v);
                    shown++;
                }
            }
            fprintf(stderr, "\n");
        }
    }
    void ProcessCommandResponse(uint16_t cr1, uint16_t cr2, uint16_t cr3, uint16_t cr4) final {
        fprintf(stderr, "cd %6llu  -> %04X %04X %04X %04X\n", static_cast<unsigned long long>(*frame), cr1, cr2, cr3, cr4);
    }
};
static CDTracer g_cdTracer;

static uint32_t Peek32(ymir::Saturn &machine, uint32_t address) {
    return machine.mainBus.Peek<uint32_t>(address);
}

bool Port::Init() {
    saturn = std::make_unique<ymir::Saturn>();
    auto &sat = *saturn;

    // Tests (headless) run everything on one thread, so that every run is the same. Played in a window, the
    // video hardware renders on threads of its own (SATRECOMP_VIDEO_THREADS=0: don't).
    const char *videoThreads = getenv("SATRECOMP_VIDEO_THREADS");
    const bool threadedVideo = videoThreads ? atoi(videoThreads) != 0 : !opt.headless;
    sat.configuration.swRenderer.threadedVDP1 = threadedVideo;
    sat.configuration.swRenderer.threadedVDP2 = threadedVideo;
    sat.configuration.swRenderer.threadedDeinterlacer = threadedVideo;
    sat.configuration.rtc.mode = ymir::core::config::rtc::Mode::Virtual;
    sat.configuration.rtc.virtHardResetStrategy = ymir::core::config::rtc::HardResetStrategy::ResetToFixedTime;
    sat.configuration.NotifyObservers();

    // The disc's files, for starting it and for the game's native file loading
    if (!discFiles.Open(opt.discPath)) {
        fprintf(stderr, "Cannot read '%s' as a Saturn disc image\n", opt.discPath.c_str());
        return false;
    }
    satrecomp::music::SetDisc({[this](int track) { return discFiles.AudioSectors(track); },
                               [this](int track, uint32_t sector, uint8_t *out) { return discFiles.ReadAudioSector(track, sector, out); }});
    satrecomp::SetDiscSectorReader([this](uint32_t frameAddress, uint8_t *out) { return discFiles.ReadDataSector(frameAddress, out); });
    satrecomp::SetPadReader([this](int player) { return static_cast<uint16_t>(pad[player & 1].load() & 0xFFF8); });
    satrecomp::SetDiscFileReader(
        [this](const std::string &name, std::vector<uint8_t> &data) { return discFiles.Read(name, data); });

    if (!opt.biosPath.empty()) {
        std::vector<uint8_t> bios;
        if (!ReadFile(opt.biosPath, bios) || bios.size() != ymir::sys::kIPLSize) {
            fprintf(stderr, "Cannot read the Saturn BIOS image (512 KiB) at '%s'\n", opt.biosPath.c_str());
            return false;
        }
        sat.LoadIPL(std::span<uint8_t, ymir::sys::kIPLSize>(bios.data(), ymir::sys::kIPLSize));
    } else {
        // No BIOS image: the native BIOS starts the disc (runtime/hle_bios.h)
        if (opt.cpuMode == "pure") {
            fprintf(stderr, "--cpu pure needs a BIOS image (--bios)\n");
            return false;
        }
        satrecomp::hle::BootImage image;
        image.ip = discFiles.SystemArea();
        image.firstReadAddress = static_cast<uint32_t>((image.ip[0xF0] << 24) | (image.ip[0xF1] << 16) |
                                                       (image.ip[0xF2] << 8) | image.ip[0xF3]);
        if (!discFiles.ReadFirstFile(image.firstRead)) {
            fprintf(stderr, "Cannot read the start-up program from the disc image '%s'\n", opt.discPath.c_str());
            return false;
        }
        satrecomp::hle::Install(sat, std::move(image));
    }

    // System settings already made (language English), so the BIOS goes straight to the disc
    ymir::smpc::PersistentSMPCData smpc{};
    smpc.STE = true;
    sat.SMPC.LoadPersistentData(smpc);

    if (opt.backupPath.empty()) {
        // The game always gets backup memory. Without --backup it is a fresh one that is thrown away at exit
        // (tests: every run starts the same).
        opt.backupPath = (std::filesystem::temp_directory_path() /
                          ("satrecomp-backup-" + std::to_string(static_cast<long long>(getpid())) + ".bin"))
                             .string();
        temporaryBackup = true;
    }
    {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(opt.backupPath).parent_path(), ec);
        sat.LoadInternalBackupMemoryImage(opt.backupPath, false, ec);
        if (ec) {
            fprintf(stderr, "Backup memory '%s': %s\n", opt.backupPath.c_str(), ec.message().c_str());
        }
    }

    ymir::media::Disc disc;
    if (!ymir::media::LoadDisc(opt.discPath, disc, true, [](ymir::media::MessageType, std::string msg) {
            fprintf(stderr, "disc: %s\n", msg.c_str());
        })) {
        fprintf(stderr, "Cannot load the disc image '%s'\n", opt.discPath.c_str());
        return false;
    }
    sat.LoadDisc(std::move(disc));

    // The picture: satrecomp's renderer (video/renderer.h), unless SATRECOMP_VIDEO=reference asks for the
    // hardware model's own. Either way finished frames arrive here.
    const ymir::vdp::CBSoftwareFrameComplete onFrame{this, [](uint32_t *pixels, uint32_t width, uint32_t height, void *ctx) {
                                                         auto &p = *static_cast<Port *>(ctx);
                                                         std::lock_guard lock(p.fbMutex);
                                                         p.fb.assign(pixels, pixels + static_cast<size_t>(width) * height);
                                                         p.fbWidth = width;
                                                         p.fbHeight = height;
                                                         p.fbSerial++;
                                                     }};
    const char *video = getenv("SATRECOMP_VIDEO");
    if (video && std::string(video) == "reference") {
        sat.VDP.SetSoftwareRenderCallback(onFrame);
    } else {
        auto result = sat.VDP.UseExternalRenderer<satrecomp::video::YmirAdapter>(sat.VDP.GetState(), sat.VDP.vdp2DebugRenderOptions,
                                                                             sat.VDP.vdp2AccessPatternsConfig);
        if (!result) {
            fprintf(stderr, "Cannot set up the video renderer\n");
            return false;
        }
        videoAdapter = result.Value();
        videoRenderer = &videoAdapter->Core();
        auto &reference = videoAdapter->Reference();
        reference.EnableThreadedVDP1(threadedVideo);
        reference.EnableThreadedVDP2(threadedVideo);
        reference.EnableThreadedDeinterlacer(threadedVideo);
        // SATRECOMP_VIDEO=passthrough: every call handed to the reference; =check: the picture made here with
        // every layer and frame compared against the reference's; otherwise made here
        using Mode = satrecomp::video::Renderer::Mode;
        const std::string mode = video ? video : "";
        const bool useGpu = mode.empty() && (opt.video.empty() ? !opt.headless : opt.video == "gpu");
        if (useGpu) {
            // Drawn by the GPU: polygons at a multiple of the original resolution (video/gpu.h). The device
            // works without a window, so headless runs can draw (and save pictures) this way too.
            if (!SDL_Init(SDL_INIT_VIDEO)) {
                fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
                return false;
            }
            gpuDevice = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL, getenv("SATRECOMP_GPU_DEBUG") != nullptr, nullptr);
            gpu = new satrecomp::video::Gpu;
            if (!gpuDevice || !gpu->Init(gpuDevice)) {
                fprintf(stderr, "No GPU renderer (%s): drawing in software\n", SDL_GetError());
                delete gpu;
                gpu = nullptr;
            }
        }
        if (gpu) {
            gpu->SetScale(opt.internalScale);
            gpu->SetOutputSize(opt.outputWidth ? opt.outputWidth : 704, opt.outputHeight);
            videoAdapter->SetMode(Mode::Gpu, onFrame);
            // SATRECOMP_GPU_CHECK=1 (with --internal-scale 1): the GPU's VDP1 framebuffer against the reference's
            const bool gpuCheck = getenv("SATRECOMP_GPU_CHECK") != nullptr;
            videoAdapter->SetGpuCheck(gpuCheck);
            // SATRECOMP_INTERLACE=1: an interlaced picture a field per frame, as the hardware gives it
            videoRenderer->SetBothFields(getenv("SATRECOMP_INTERLACE") == nullptr);
            // SATRECOMP_FLOOR=image: the rotation screen at the original resolution, drawn on the CPU
            videoRenderer->SetGpuFloor(!(getenv("SATRECOMP_FLOOR") && std::string(getenv("SATRECOMP_FLOOR")) == "image"));
            videoRenderer->SetGpuFrameCallback([this, gpuCheck](const satrecomp::video::Frame &frame) {
                gpu->Draw(frame);
                if (gpuCheck) {
                    std::vector<uint16_t> words;
                    uint32_t width = 0, height = 0;
                    if (gpu->ReadSprites(words, width, height)) {
                        videoRenderer->CompareSprites(words.data(), width, height);
                    }
                }
                if (opt.headless) {
                    // (no window: the picture is read back, for --shot-every and the state hash)
                    std::vector<uint32_t> pixels;
                    if (gpu->Read(pixels)) {
                        std::lock_guard lock(fbMutex);
                        fb.swap(pixels);
                        fbWidth = gpu->OutputWidth();
                        fbHeight = gpu->OutputHeight();
                        fbSerial++;
                    }
                }
            });
        } else {
            videoAdapter->SetMode(mode == "passthrough" ? Mode::Reference : mode == "check" ? Mode::Check : Mode::Software, onFrame);
        }
    }
    // The sound chip's samples: satrecomp's generator (sound/generator.h). SATRECOMP_SOUND=reference: the
    // hardware model's own; =check: both, every step compared
    if (const char *sound = getenv("SATRECOMP_SOUND"); !sound || std::string(sound) != "reference") {
        soundGenerator = new satrecomp::sound::Generator;
        soundGenerator->Install(sat.SCSP, sound && std::string(sound) == "check");
        // CD music the port plays itself (runtime/music.h) arrives at the chip's CD audio input
        soundGenerator->SetCdAudioSource(&satrecomp::music::Next);
    }
    sat.SCSP.SetSampleCallback({this, [](int16_t l, int16_t r, void *ctx) {
                                    auto &p = *static_cast<Port *>(ctx);
                                    // (a running hash of the sound output, part of the state hash)
                                    p.audioHash = (p.audioHash ^ static_cast<uint16_t>(l)) * 0x100000001b3ull;
                                    p.audioHash = (p.audioHash ^ static_cast<uint16_t>(r)) * 0x100000001b3ull;
                                    // (SATRECOMP_AUDIO_DUMP=path: every sample that goes out, 16-bit stereo, 44100 Hz)
                                    static FILE *dump = getenv("SATRECOMP_AUDIO_DUMP") ? fopen(getenv("SATRECOMP_AUDIO_DUMP"), "wb") : nullptr;
                                    if (dump) {
                                        const int16_t pair[2] = {l, r};
                                        fwrite(pair, sizeof(pair), 1, dump);
                                    }
                                    if (p.audioSink) {
                                        p.audioSink(l, r, p.audioCtx);
                                    }
                                }});

    using ymir::peripheral::PeripheralReport;
    auto &port1 = sat.SMPC.GetPeripheralPort1();
    auto &port2 = sat.SMPC.GetPeripheralPort2();
    port1.ConnectControlPad();
    port2.ConnectControlPad();
    port1.SetPeripheralReportCallback({this, [](PeripheralReport &report, void *ctx) {
                                           auto &p = *static_cast<Port *>(ctx);
                                           if (report.type == ymir::peripheral::PeripheralType::ControlPad) {
                                               report.report.controlPad.buttons =
                                                   static_cast<ymir::peripheral::Button>(~p.pad[0].load() & 0xFFF8);
                                           }
                                       }});
    port2.SetPeripheralReportCallback({this, [](PeripheralReport &report, void *ctx) {
                                           auto &p = *static_cast<Port *>(ctx);
                                           if (report.type == ymir::peripheral::PeripheralType::ControlPad) {
                                               report.report.controlPad.buttons =
                                                   static_cast<ymir::peripheral::Button>(~p.pad[1].load() & 0xFFF8);
                                           }
                                       }});

    sat.Reset(true);
    if (getenv("SATRECOMP_CD_TRACE")) {
        g_cdTracer.frame = &frame;
        g_cdTracer.saturn = &sat;
        sat.CDBlock.UseTracer(&g_cdTracer);
    }

    if (!opt.moviePlay.empty() && !playMovie.Load(opt.moviePlay)) {
        fprintf(stderr, "Cannot read the movie '%s'\n", opt.moviePlay.c_str());
        return false;
    }
    if (!opt.hashLog.empty()) {
        hashFile = fopen(opt.hashLog.c_str(), "w");
    }

    satrecomp::Options ro;
    ro.mode = opt.cpuMode == "interp"   ? satrecomp::Mode::Interpreter
              : opt.cpuMode == "recomp" ? satrecomp::Mode::Recompiled
              : opt.cpuMode == "free"   ? satrecomp::Mode::Free
                                        : satrecomp::Mode::Native;
    ro.recordPath = opt.recordBlocks;
    // Sync points: from the game's game.cfg (SATRECOMP_SYNC_POINTS overrides, for experiments)
    const char *sync = getenv("SATRECOMP_SYNC_POINTS");
    for (const char *v = sync ? sync : GAME_SYNC_POINTS; *v;) {
        char *end;
        ro.syncPoints.push_back(static_cast<uint32_t>(strtoul(v, &end, 16)));
        if (end == v) {
            break;
        }
        v = end;
        while (*v == ' ' || *v == ',') {
            v++;
        }
    }
    ro.stats = opt.stats;
    if (getenv("SATRECOMP_IO_STATS")) {
        ymir::sh2::g_satrecompIoTrace = IoTrace;
    }
    if (opt.cpuMode != "pure") {
        satrecomp::Attach(sat, ro);
        // The sound CPU's driver, recompiled (cycle exact in every mode; SATRECOMP_NO_M68K=1: interpreter)
        if (const char *check = getenv("SATRECOMP_M68K"); check && std::string(check) == "check" && soundGenerator) {
            soundGenerator->InstallCpuCheck();
        } else if (!getenv("SATRECOMP_NO_M68K")) {
            satrecomp::AttachM68K(sat, opt.recordBlocks.empty() ? "" : opt.recordBlocks + ".m68k", opt.stats);
        }
    }
    return true;
}

#else // SATRECOMP_OWN_MACHINE

static uint32_t Peek32(satrecomp::Machine &machine, uint32_t address) {
    return machine.masterSH2.Read32(address);
}

// satrecomp's own machine (machine/machine.h): no hardware model, no BIOS image
bool Port::Init() {
    saturn = std::make_unique<satrecomp::Machine>();
    auto &sat = *saturn;
    satrecomp::RegisterGame(); // (the game's own sources: lifted routines, features, widescreen layout)
    RegisterPortFeatures();
    // (widescreen as the launcher said, by --widescreen, whatever settings.ini has: launcher.ini keeps it)
    satrecomp::LoadFeatures(opt.settingsPath, opt.features + (opt.aspect > 1.34f ? ",widescreen" : ",-widescreen"));

    // The disc's files, for starting it and for the game's native file loading
    if (!discFiles.Open(opt.discPath)) {
        fprintf(stderr, "Cannot read '%s' as a Saturn disc image\n", opt.discPath.c_str());
        return false;
    }
    satrecomp::music::SetDisc({[this](int track) { return discFiles.AudioSectors(track); },
                               [this](int track, uint32_t sector, uint8_t *out) { return discFiles.ReadAudioSector(track, sector, out); }});
    satrecomp::SetDiscSectorReader([this](uint32_t frameAddress, uint8_t *out) { return discFiles.ReadDataSector(frameAddress, out); });
    satrecomp::SetPadReader([this](int player) { return static_cast<uint16_t>(pad[player & 1].load() & 0xFFF8); });
    satrecomp::SetDiscFileReader(
        [this](const std::string &name, std::vector<uint8_t> &data) { return discFiles.Read(name, data); });
    {
        // (what the CD drive's table of contents says)
        satrecomp::machine::Disc disc;
        for (const DiscImage::Track &t : discFiles.Image().Tracks()) {
            disc.tracks.push_back({static_cast<uint8_t>(t.number), static_cast<uint8_t>(t.audio ? 0 : 4), t.startFAD});
        }
        disc.leadOutFAD = discFiles.Image().LeadOutFAD();
        sat.SetDisc(disc);
    }
    sat.readPad = [this](int player) { return static_cast<uint16_t>(pad[player & 1].load() & 0xFFF8); };

    // The native BIOS starts the disc (runtime/hle_bios.h)
    if (!opt.biosPath.empty()) {
        fprintf(stderr, "(--bios is not used: this build has its own start-up code)\n");
    }
    satrecomp::hle::BootImage image;
    image.ip = discFiles.SystemArea();
    image.firstReadAddress = static_cast<uint32_t>((image.ip[0xF0] << 24) | (image.ip[0xF1] << 16) | (image.ip[0xF2] << 8) | image.ip[0xF3]);
    if (!discFiles.ReadFirstFile(image.firstRead)) {
        fprintf(stderr, "Cannot read the start-up program from the disc image '%s'\n", opt.discPath.c_str());
        return false;
    }
    satrecomp::hle::Install(sat, std::move(image));

    if (opt.backupPath.empty()) {
        // The game always gets backup memory. Without --backup it is a fresh one that is thrown away at exit
        // (tests: every run starts the same).
        opt.backupPath = (std::filesystem::temp_directory_path() /
                          ("satrecomp-backup-" + std::to_string(static_cast<long long>(getpid())) + ".bin"))
                             .string();
        temporaryBackup = true;
    }
    {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(opt.backupPath).parent_path(), ec);
        satrecomp::hle::OpenBackup(opt.backupPath);
    }

    // The picture: drawn by the GPU, or in software at the original resolution
    videoRenderer = &sat.Video();
    using Mode = satrecomp::video::Renderer::Mode;
    const auto onFrame = [this](uint32_t *pixels, uint32_t width, uint32_t height) {
        std::lock_guard lock(fbMutex);
        fb.assign(pixels, pixels + static_cast<size_t>(width) * height);
        fbWidth = width;
        fbHeight = height;
        fbSerial++;
    };
    const bool useGpu = opt.video.empty() ? !opt.headless : opt.video == "gpu";
    if (useGpu) {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
            return false;
        }
        gpuDevice = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL, getenv("SATRECOMP_GPU_DEBUG") != nullptr, nullptr);
        gpu = new satrecomp::video::Gpu;
        if (!gpuDevice || !gpu->Init(gpuDevice)) {
            fprintf(stderr, "No GPU renderer (%s): drawing in software\n", SDL_GetError());
            delete gpu;
            gpu = nullptr;
        }
    }
    if (gpu) {
        gpu->SetScale(opt.internalScale);
        ApplyWidescreen(opt.aspect > 1.34f);
        {
            std::vector<satrecomp::video::Renderer::WideSplit> splits;
            for (const satrecomp::WideSplit &split : satrecomp::TheGame().wideSplits) {
                splits.push_back({split.layer, split.rowFrom, split.rowTo, split.leftEnd, split.rightStart});
            }
            videoRenderer->SetWideSplits(std::move(splits));
        }
        videoRenderer->SetMode(Mode::Gpu, onFrame);
        videoRenderer->SetBothFields(getenv("SATRECOMP_INTERLACE") == nullptr);
        videoRenderer->SetGpuFloor(!(getenv("SATRECOMP_FLOOR") && std::string(getenv("SATRECOMP_FLOOR")) == "image"));
        // SATRECOMP_GPU_NO_READ=1: headless, the picture is drawn and not read back (so no pictures and a hash
        // without it): for timing the renderer as a window has it, where nothing waits for the GPU each frame
        static const bool noRead = getenv("SATRECOMP_GPU_NO_READ") != nullptr;
        videoRenderer->SetGpuFrameCallback([this](const satrecomp::video::Frame &frame) {
            gpu->Draw(frame);
            if (opt.headless && !noRead) {
                // (no window: the picture is read back, for --shot-every and the state hash)
                std::vector<uint32_t> pixels;
                if (gpu->Read(pixels)) {
                    std::lock_guard lock(fbMutex);
                    fb.swap(pixels);
                    fbWidth = gpu->OutputWidth();
                    fbHeight = gpu->OutputHeight();
                    fbSerial++;
                }
            }
        });
    } else {
        videoRenderer->SetMode(Mode::Software, onFrame);
        // (no widescreen in software: its row would do nothing)
        auto &features = satrecomp::TheGame().features;
        features.erase(std::remove_if(features.begin(), features.end(), [](const satrecomp::Feature &f) { return !strcmp(f.name, "widescreen"); }),
                       features.end());
    }

    // Sound: the chip's samples, with CD music the port plays itself (runtime/music.h) at its CD audio input
    sat.Sound().cdAudioSource = &satrecomp::music::Next;
    sat.Sound().onSample = [this](int16_t l, int16_t r) {
        // (frames run ahead are taken back: their sound is not heard, and the hash is put back with the state)
        if (satrecomp::RunningAhead()) {
            return;
        }
        // (a running hash of the sound output, part of the state hash)
        audioHash = (audioHash ^ static_cast<uint16_t>(l)) * 0x100000001b3ull;
        audioHash = (audioHash ^ static_cast<uint16_t>(r)) * 0x100000001b3ull;
        // (SATRECOMP_AUDIO_DUMP=path: every sample that goes out, 16-bit stereo, 44100 Hz)
        static FILE *dump = getenv("SATRECOMP_AUDIO_DUMP") ? fopen(getenv("SATRECOMP_AUDIO_DUMP"), "wb") : nullptr;
        if (dump) {
            const int16_t sample[2] = {l, r};
            fwrite(sample, 2, 2, dump);
        }
        if (audioSink) {
            audioSink(l, r, audioCtx);
        }
    };

    sat.Reset(true);

    if (!opt.moviePlay.empty() && !playMovie.Load(opt.moviePlay)) {
        fprintf(stderr, "Cannot read the movie '%s'\n", opt.moviePlay.c_str());
        return false;
    }
    if (!opt.hashLog.empty()) {
        hashFile = fopen(opt.hashLog.c_str(), "w");
    }

    satrecomp::Options ro;
    ro.mode = satrecomp::Mode::Native;
    ro.recordPath = opt.recordBlocks;
    const char *sync = getenv("SATRECOMP_SYNC_POINTS");
    for (const char *v = sync ? sync : GAME_SYNC_POINTS; *v;) {
        char *end;
        ro.syncPoints.push_back(static_cast<uint32_t>(strtoul(v, &end, 16)));
        if (end == v) {
            break;
        }
        v = end;
        while (*v == ' ' || *v == ',') {
            v++;
        }
    }
    ro.stats = opt.stats;
    satrecomp::Attach(sat, ro);
    return true;
}

#endif // SATRECOMP_OWN_MACHINE

void Port::RunFrame() {
    // Debugging aid (SATRECOMP_PEEK=ADDR,ADDR,... hex): these longwords of memory, whenever one has changed
    // since the frame before
    static const char *peek = getenv("SATRECOMP_PEEK");
    if (peek) {
        static std::vector<uint32_t> addresses, last;
        if (addresses.empty()) {
            for (const char *at = peek; *at;) {
                addresses.push_back(static_cast<uint32_t>(strtoul(at, const_cast<char **>(&at), 16)));
                at += *at == ',';
            }
            last.assign(addresses.size(), 0xDEADBEEF);
        }
        bool changed = false;
        std::vector<uint32_t> now;
        for (size_t i = 0; i < addresses.size(); i++) {
            now.push_back(Peek32(*saturn, addresses[i]));
            changed = changed || now[i] != last[i];
        }
        if (changed) {
            fprintf(stderr, "peek %6llu:", static_cast<unsigned long long>(frame));
            for (uint32_t v : now) {
                fprintf(stderr, " %08X", v);
            }
            fprintf(stderr, "\n");
            last = now;
        }
    }
    if (!opt.moviePlay.empty()) {
        auto it = playMovie.changes.find(frame);
        if (it != playMovie.changes.end()) {
            pad[0] = it->second.first;
            pad[1] = it->second.second;
        }
    }
    if (opt.autoSeed && frame >= opt.autoFrom) {
        // Exploration: random input that changes a few times a second, with Start tapped now and then so the
        // game moves through its menus. Record it (--record) to get a movie that replays the same run.
        auto rnd = [&]() {
            autoState ^= autoState << 13;
            autoState ^= autoState >> 7;
            autoState ^= autoState << 17;
            return autoState;
        };
        if (autoState == 0) {
            autoState = opt.autoSeed * 0x9E3779B97F4A7C15ull | 1;
        }
        if (frame % 6 == 0) {
            for (int i = 0; i < 2; i++) {
                const uint64_t r = rnd();
                uint16_t state = 0;
                static const uint16_t kDirs[] = {0, 0, kPadLeft, kPadRight, kPadUp, kPadDown,
                                                 kPadLeft | kPadDown, kPadRight | kPadDown, kPadRight, kPadLeft};
                static const uint16_t kButtons[] = {kPadA, kPadB, kPadC, kPadX, kPadY, kPadZ, kPadL, kPadR};
                state |= kDirs[r % 10];
                if ((r >> 8) % 3) {
                    state |= kButtons[(r >> 16) % 8];
                }
                if ((r >> 24) % 4 == 0) {
                    state |= kButtons[(r >> 32) % 8];
                }
                // Player 1 taps Start about every two seconds; player 2 hardly ever (it would join the game)
                if ((r >> 40) % (i == 0 ? 20 : 2000) == 0) {
                    state |= kPadStart;
                }
                pad[i] = state;
            }
        }
    }
    if (!opt.movieRecord.empty()) {
        uint16_t now[2] = {pad[0].load(), pad[1].load()};
        if (frame == 0 || now[0] != lastRec[0] || now[1] != lastRec[1]) {
            recMovie.changes[frame] = {now[0], now[1]};
            lastRec[0] = now[0];
            lastRec[1] = now[1];
        }
    }

    // (tests: --menu FRAME:KEYS[:shot.png]: the menu opened at that frame and those buttons pressed in it)
    if (!menuTest.empty() && strtoull(menuTest.c_str(), nullptr, 10) == frame) {
        const size_t first = menuTest.find(':'), second = menuTest.find(':', first + 1);
        const std::string keys = menuTest.substr(first + 1, second == std::string::npos ? std::string::npos : second - first - 1);
        menu.Open(true);
        for (const char key : keys) {
            const uint16_t button = key == 'u' ? kPadUp : key == 'd' ? kPadDown : key == 'l' ? kPadLeft : key == 'r' ? kPadRight : key == 'a' ? kPadA : key == 's' ? kPadStart : 0;
            MenuAct(menu.Buttons(button));
        }
        std::vector<uint32_t> pixels;
        uint32_t width = 0, height = 0;
        if (second != std::string::npos && menu.IsOpen() && MenuPicture(pixels, width, height)) {
            menu.Draw(pixels.data(), static_cast<int>(width), static_cast<int>(height));
            WritePng(menuTest.substr(second + 1).c_str(), pixels.data(), width, height);
        }
        menu.Close();
    }
    // (tests: --reset-test SET:GO)
    if (!resetTest.empty()) {
        std::string error;
        if (frame == strtoull(resetTest.c_str(), nullptr, 10)) {
            SetResetPoint(error);
        } else if (frame == strtoull(resetTest.c_str() + resetTest.find(':') + 1, nullptr, 10) && !GoToResetPoint(error)) {
            fprintf(stderr, "reset point: %s\n", error.c_str());
        }
    }
    // (tests: --state-save FRAME:path, --state-load FRAME:path; several, separated by commas)
    for (const std::string *specs : {&opt.stateLoad, &opt.stateSave}) {
        std::stringstream list(*specs);
        std::string spec;
        while (std::getline(list, spec, ',')) {
            const size_t colon = spec.find(':');
            if (colon != std::string::npos && strtoull(spec.c_str(), nullptr, 10) == frame) {
                std::string error;
                const bool ok = specs == &opt.stateSave ? SaveState(spec.substr(colon + 1), error) : LoadState(spec.substr(colon + 1), error);
                if (!ok) {
                    fprintf(stderr, "state: %s\n", error.c_str());
                }
            }
        }
    }
    // (widescreen switched in the menu: from this frame on)
    if (gpu && satrecomp::FeatureOn("widescreen") != wideShown) {
        ApplyWidescreen(!wideShown);
    }
    saturn->RunFrame();
    frame++;
    if (satrecomp::TheGame().frame) {
        satrecomp::TheGame().frame();
    }
    UpdateHud();
#ifdef SATRECOMP_OWN_MACHINE
    // (debugging aid, SATRECOMP_RAM_TRACE=path:from:every: high work RAM appended to a file every so many frames)
    static const char *trace = getenv("SATRECOMP_RAM_TRACE");
    if (trace) {
        static FILE *file = nullptr;
        static unsigned long long from = 0, every = 1;
        if (!file) {
            const std::string spec = trace;
            const size_t colon = spec.find(':');
            file = fopen(spec.substr(0, colon).c_str(), "wb");
            sscanf(spec.c_str() + colon + 1, "%llu:%llu", &from, &every);
        }
        if (file && frame >= from && (frame - from) % every == 0) {
            fwrite(saturn->ramHigh, 1, satrecomp::Machine::kRamSize, file);
        }
    }
#endif
#ifndef SATRECOMP_OWN_MACHINE
    if (opt.videoStats) {
        VideoStatsFrame(*this);
    }
#endif
    if (const char *v = getenv("SATRECOMP_COUNT_FROM_FRAME"); v && frame == strtoull(v, nullptr, 10)) {
        satrecomp::ResetRunCounts();
    }

    if (opt.shotEvery && frame % opt.shotEvery == 0) {
        char path[1024];
        snprintf(path, sizeof(path), "%s%06llu.png", opt.shotPrefix.c_str(), static_cast<unsigned long long>(frame));
        SaveShot(path);
    }
    if (hashFile) {
        fprintf(hashFile, "%llu %016llx\n", static_cast<unsigned long long>(frame),
                static_cast<unsigned long long>(StateHash()));
    }
    // (after the pictures and the hash: those are of the frame itself)
    RunAhead();
}

void Port::MenuAct(Menu::Action action) {
    if (action == Menu::Action::None) {
        return;
    }
    if (action == Menu::Action::Quit) {
        quit = true;
        menu.Close();
        return;
    }
    std::string error;
    const std::string path = StatePath(menu.Slot());
    const bool ok = action == Menu::Action::Save             ? SaveState(path, error)
                    : action == Menu::Action::Load           ? LoadState(path, error)
                    : action == Menu::Action::SetResetPoint  ? SetResetPoint(error)
                                                             : GoToResetPoint(error);
    if (ok) {
        menu.Close();
    } else {
        menu.Note(error);
    }
}

bool Port::MenuPicture(std::vector<uint32_t> &pixels, uint32_t &width, uint32_t &height) {
    if (gpu) {
        if (!gpu->Read(pixels)) {
            return false;
        }
        width = gpu->OutputWidth();
        height = gpu->OutputHeight();
        return true;
    }
    std::lock_guard lock(fbMutex);
    pixels = fb;
    width = fbWidth;
    height = fbHeight;
    return width && height;
}

std::string Port::StatePath(int slot) const {
    return (std::filesystem::path(opt.backupPath).parent_path() / ("state" + std::to_string(slot) + ".bin")).string();
}

#ifdef SATRECOMP_OWN_MACHINE
namespace {
    constexpr uint32_t kStateMagic = 0x53525354, kStateVersion = 8;

    // Everything a later moment depends on, in one order for writing and reading
    void StateOf(Port &port, satrecomp::Archive &ar) {
        port.saturn->Serialize(ar);
        satrecomp::hle::Serialize(ar);
        satrecomp::music::Serialize(ar);
        if (satrecomp::TheGame().state) {
            satrecomp::TheGame().state(ar);
        }
        ar.Pod(port.audioHash);
        ar.Mark(0x454E4453);
    }
} // namespace

void Port::RunAhead() {
    const int ahead = satrecomp::FeatureValue("run_ahead");
    // (nor while going on a frame at a time or slowed: there the frame shown is the frame)
    if (ahead <= 0 || stepping || menu.Speed() != 100 || !videoRenderer ||
        (satrecomp::TheGame().noRunAhead && satrecomp::TheGame().noRunAhead())) {
        return;
    }
    // What the frames run ahead change: what a save state holds; the renderer whole (a state leaves out what
    // goes from one frame to the next there, the frame being put together and the layers' pictures); the
    // GPU's copies of the VDP1's framebuffers; and what the dispatcher keeps about where the CPUs are
    static std::vector<uint8_t> kept;
    kept.clear();
    {
        satrecomp::Archive ar(kept, true);
        StateOf(*this, ar);
    }
    satrecomp::video::Renderer renderer = *videoRenderer;
    satrecomp::KeepDispatcher();
    if (gpu) {
        gpu->KeepSprites();
    }
    satrecomp::SetRunningAhead(true); // (no sound comes out of them: Init, onSample)
    for (int n = 0; n < ahead; n++) {
        saturn->RunFrame();
        if (satrecomp::TheGame().frame) {
            satrecomp::TheGame().frame();
        }
    }
    UpdateHud(); // (the readout of the frame shown)
    {
        satrecomp::Archive ar(kept, false);
        StateOf(*this, ar);
    }
    *videoRenderer = std::move(renderer);
    satrecomp::PutDispatcherBack();
    if (gpu) {
        gpu->PutSpritesBack();
    }
    satrecomp::SetRunningAhead(false);
}

void Port::ApplyWidescreen(bool on) {
    wideShown = on;
    if (!gpu || !videoRenderer) {
        return;
    }
    // So many more of the VDP1's pixels at each side (its 352 are a 4:3 picture's): 16:9, or the shape --aspect asked
    const float aspect = opt.aspect > 1.34f ? opt.aspect : 16.0f / 9.0f;
    const uint32_t extra = on ? static_cast<uint32_t>((352.0f * aspect * 0.75f - 352.0f) / 2.0f + 0.5f) : 0;
    satrecomp::SetPictureAspect(static_cast<float>(352 + 2 * extra) / 352.0f * 4.0f / 3.0f);
    gpu->SetAspect(extra ? (352.0f + 2.0f * extra) / 352.0f * 4.0f / 3.0f : 4.0f / 3.0f);
    gpu->SetOutputSize(opt.outputWidth ? opt.outputWidth : 704 + 4 * extra, opt.outputHeight);
    videoRenderer->SetWidescreen(extra);
}

void Port::SaveStateData(std::vector<uint8_t> &data) {
    data.clear();
    satrecomp::Archive ar(data, true);
    uint32_t header[2] = {kStateMagic, kStateVersion};
    ar.Pod(header);
    char game[32] = GAME_ID;
    ar.Pod(game);
    StateOf(*this, ar);
}

bool Port::SetResetPoint(std::string &) {
    SaveStateData(resetPoint);
    return true;
}
bool Port::GoToResetPoint(std::string &error) {
    if (resetPoint.empty()) {
        error = "No reset point has been set.";
        return false;
    }
    return LoadStateData(resetPoint, error);
}

bool Port::SaveState(const std::string &path, std::string &error) {
    std::vector<uint8_t> data;
    SaveStateData(data);
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!file) {
        error = "The state could not be written.";
        return false;
    }
    return true;
}

bool Port::LoadState(const std::string &path, std::string &error) {
    std::vector<uint8_t> data;
    if (!ReadFile(path, data)) {
        error = "There is no state in this slot.";
        return false;
    }
    return LoadStateData(data, error);
}

bool Port::LoadStateData(const std::vector<uint8_t> &stored, std::string &error) {
    std::vector<uint8_t> data = stored;
    // (read into the machine only once it is known to be whole: the state as it is now is kept to go back to)
    std::vector<uint8_t> before;
    {
        satrecomp::Archive keep(before, true);
        StateOf(*this, keep);
    }
    satrecomp::Archive ar(data, false);
    uint32_t header[2] = {};
    ar.Pod(header);
    char game[32] = {};
    ar.Pod(game);
    bool ok = ar.Ok() && header[0] == kStateMagic && header[1] == kStateVersion && strncmp(game, GAME_ID, sizeof(game)) == 0;
    if (ok) {
        StateOf(*this, ar);
        ok = ar.Ok();
        if (!ok) {
            satrecomp::Archive back(before, false);
            StateOf(*this, back);
        }
    }
    if (!ok) {
        error = "This state is from another version of the game program.";
        return false;
    }
    satrecomp::StateLoaded();
    return true;
}
#else
void Port::RunAhead() {}
void Port::ApplyWidescreen(bool on) {
    wideShown = on;
}
bool Port::SaveState(const std::string &, std::string &error) {
    error = "The reference build has no save states.";
    return false;
}
bool Port::LoadState(const std::string &, std::string &error) {
    error = "The reference build has no save states.";
    return false;
}
void Port::SaveStateData(std::vector<uint8_t> &) {}
bool Port::LoadStateData(const std::vector<uint8_t> &, std::string &error) {
    error = "The reference build has no save states.";
    return false;
}
bool Port::SetResetPoint(std::string &error) {
    error = "The reference build has no save states.";
    return false;
}
bool Port::GoToResetPoint(std::string &error) {
    error = "The reference build has no save states.";
    return false;
}
#endif

void Port::UpdateHud() {
    if (!gpu) {
        return;
    }
    static uint64_t serial = ~0ull;
    static uint32_t outputWidth = 0, outputHeight = 0;
    if (serial == HudShownSerial() && outputWidth == gpu->OutputWidth() && outputHeight == gpu->OutputHeight()) {
        return;
    }
    serial = HudShownSerial();
    outputWidth = gpu->OutputWidth();
    outputHeight = gpu->OutputHeight();
    if (!HudShows() || !outputWidth || !outputHeight) {
        gpu->ClearHudLayer();
        return;
    }
    // (at most 720 lines: it is sent to the GPU whenever it changes, and is stretched over the picture)
    const uint32_t height = std::min<uint32_t>(outputHeight, 720), width = std::max<uint32_t>(outputWidth * height / outputHeight, 1);
    static std::vector<uint32_t> layer;
    layer.assign(static_cast<size_t>(width) * height, 0);
    DrawHud(layer.data(), static_cast<int>(width), static_cast<int>(height));
    gpu->SetHudLayer(layer.data(), width, height);
}

void Port::SaveShot(const std::string &path) {
    if (gpu) {
        // (as the window shows it: with the readout blended in by the GPU)
        std::vector<uint32_t> shown;
        if (gpu->ReadShown(shown)) {
            WritePng(path.c_str(), shown.data(), gpu->OutputWidth(), gpu->OutputHeight());
            return;
        }
    }
    std::lock_guard lock(fbMutex);
    if (fbWidth && fbHeight) {
        std::vector<uint32_t> shown = fb; // (with the readout, as the window shows it)
        DrawHud(shown.data(), static_cast<int>(fbWidth), static_cast<int>(fbHeight));
        WritePng(path.c_str(), shown.data(), fbWidth, fbHeight);
    }
}

static uint64_t Fnv(uint64_t h, const void *data, size_t size) {
    // FNV-1a over 64-bit words (sizes here are multiples of 8, apart from the tail)
    const uint8_t *p = static_cast<const uint8_t *>(data);
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x100000001b3ull;
    }
    for (; i < size; i++) {
        h = (h ^ p[i]) * 0x100000001b3ull;
    }
    return h;
}

uint64_t Port::StateHash() {
    auto &sat = *saturn;
    uint64_t h = 0xcbf29ce484222325ull;
#ifdef SATRECOMP_OWN_MACHINE
    h = Fnv(h, sat.ramHigh, satrecomp::Machine::kRamSize);
    h = Fnv(h, sat.ramLow, satrecomp::Machine::kRamSize);
    for (auto *cpu : {&sat.masterSH2, &sat.slaveSH2}) {
        h = Fnv(h, cpu->R.data(), sizeof(uint32_t) * 16);
        uint32_t regs[2] = {cpu->PC, cpu->PR};
        h = Fnv(h, regs, sizeof(regs));
    }
#else
    h = Fnv(h, sat.mem.WRAMHigh.data(), sat.mem.WRAMHigh.size());
    h = Fnv(h, sat.mem.WRAMLow.data(), sat.mem.WRAMLow.size());
    for (auto *cpu : {&sat.masterSH2, &sat.slaveSH2}) {
        auto &probe = cpu->GetProbe();
        h = Fnv(h, probe.R().data(), sizeof(uint32_t) * 16);
        uint32_t regs[2] = {probe.PC(), probe.PR()};
        h = Fnv(h, regs, sizeof(regs));
    }
#endif
    // (SATRECOMP_HASH_NO_PICTURE=1: leave the picture out, e.g. to compare runs with threaded video, where a
    // frame's picture arrives a little later)
    if (!getenv("SATRECOMP_HASH_NO_PICTURE")) {
        std::lock_guard lock(fbMutex);
        h = Fnv(h, fb.data(), fb.size() * sizeof(uint32_t));
    }
    h = Fnv(h, &audioHash, sizeof(audioHash));
    return h;
}

void Port::Shutdown() {
#ifdef SATRECOMP_OWN_MACHINE
    if (opt.stats) {
        // What the game asked of the system manager and the CD drive
        fprintf(stderr, "hardware: SMPC commands:");
        for (unsigned i = 0; i < 256; i++) {
            if (saturn->smpcCommands[i]) {
                fprintf(stderr, " %02X x%llu", i, static_cast<unsigned long long>(saturn->smpcCommands[i]));
            }
        }
        fprintf(stderr, "; CD drive commands:");
        for (unsigned i = 0; i < 256; i++) {
            if (saturn->cdCommands[i]) {
                fprintf(stderr, " %02X x%llu", i, static_cast<unsigned long long>(saturn->cdCommands[i]));
            }
        }
        fprintf(stderr, "\n");
    }
#else
    if (!g_ioStats.empty()) {
        fprintf(stderr, "hardware accesses (address: reads, writes):\n");
        for (const auto &[address, counts] : g_ioStats) {
            fprintf(stderr, "  %08X: %llu, %llu\n", address, static_cast<unsigned long long>(counts[0]), static_cast<unsigned long long>(counts[1]));
        }
    }
    if (opt.videoStats) {
        VideoStatsReport();
    }
    if (opt.stats) {
        // What the game asked of the hardware that is still the model's: commands to the SMPC (the chip
        // behind the controller ports, the clock and the reset logic), instructions run by the SCU's DSP
        fprintf(stderr, "hardware: SMPC commands:");
        for (unsigned i = 0; i < 256; i++) {
            if (ymir::smpc::g_satrecompCommands[i]) {
                fprintf(stderr, " %02X x%llu", i, static_cast<unsigned long long>(ymir::smpc::g_satrecompCommands[i]));
            }
        }
        fprintf(stderr, "; SCU DSP instructions: %llu\n", static_cast<unsigned long long>(ymir::scu::g_satrecompDspSteps));
    }
#endif
    if (videoRenderer) {
        videoRenderer->Report();
    }
#ifndef SATRECOMP_OWN_MACHINE
    if (soundGenerator) {
        soundGenerator->Report();
    }
#endif
    if (gpu) {
        gpu->Shutdown();
        delete gpu;
        gpu = nullptr;
    }
    if (gpuDevice) {
        SDL_DestroyGPUDevice(gpuDevice);
        gpuDevice = nullptr;
    }
    SDL_Quit();
    satrecomp::Finish();
#ifndef SATRECOMP_OWN_MACHINE
    satrecomp::FinishM68K();
#endif
    if (temporaryBackup) {
        std::error_code ec;
        std::filesystem::remove(opt.backupPath, ec);
    }
    if (!opt.dumpRam.empty()) {
#ifdef SATRECOMP_OWN_MACHINE
        std::ofstream(opt.dumpRam + "high.bin", std::ios::binary)
            .write(reinterpret_cast<const char *>(saturn->ramHigh), satrecomp::Machine::kRamSize);
        std::ofstream(opt.dumpRam + "low.bin", std::ios::binary)
            .write(reinterpret_cast<const char *>(saturn->ramLow), satrecomp::Machine::kRamSize);
        for (auto *cpu : {&saturn->masterSH2, &saturn->slaveSH2}) {
            fprintf(stderr, "%s PC=%08X PR=%08X R:", cpu->IsMaster() ? "master" : "slave ", cpu->PC, cpu->PR);
            for (uint32_t r : cpu->R) {
                fprintf(stderr, " %08X", r);
            }
            fprintf(stderr, "\n");
        }
#else
        std::ofstream(opt.dumpRam + "high.bin", std::ios::binary)
            .write(reinterpret_cast<const char *>(saturn->mem.WRAMHigh.data()), saturn->mem.WRAMHigh.size());
        std::ofstream(opt.dumpRam + "low.bin", std::ios::binary)
            .write(reinterpret_cast<const char *>(saturn->mem.WRAMLow.data()), saturn->mem.WRAMLow.size());
        for (auto *cpu : {&saturn->masterSH2, &saturn->slaveSH2}) {
            auto &probe = cpu->GetProbe();
            fprintf(stderr, "%s PC=%08X PR=%08X R:", cpu->IsMaster() ? "master" : "slave ", probe.PC(), probe.PR());
            for (uint32_t r : probe.R()) {
                fprintf(stderr, " %08X", r);
            }
            fprintf(stderr, "\n");
        }
#endif
    }
    if (!opt.movieRecord.empty()) {
        recMovie.Save(opt.movieRecord);
    }
    if (hashFile) {
        fclose(hashFile);
        hashFile = nullptr;
    }
}

} // namespace port

static void Usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s --disc GAME.cue [options]\n"
            "  --bios BIOS.bin        start through a Saturn BIOS image instead of the native BIOS\n"
            "  --backup PATH          internal backup memory image (saves)\n"
            "  --cpu MODE             native (default) | free | recomp | interp | pure\n"
            "                         native: satrecomp's own translation of the game code, free running\n"
            "                         free:   the interpreter run the same way (exact reference for native)\n"
            "                         recomp: cycle-exact recompiled blocks (the interpreter's handlers)\n"
            "                         interp: the interpreter by blocks (exact reference for recomp)\n"
            "                         pure:   the unmodified interpreter, instruction by instruction\n"
            "  --dump-decode PATH     write the SH-2 opcode decode table (used by tools/recomp.py) and exit\n"
            "  --record-blocks PATH   add every newly reached code block to this block database\n"
            "  --features A,B         enable optional features (--list-features lists them; -NAME: off)\n"
            "  --settings PATH        settings.ini with the player's feature choices\n"
            "  --fullscreen --scale N --mute --unthrottled\n"
            "  --video MODE           gpu (default in a window): polygons drawn by the GPU at high resolution\n"
            "                         software (default headless): the original resolution, exact\n"
            "  --internal-scale N     gpu: draw polygons at N times the original resolution (default: to fit the window)\n"
            "  --widescreen           gpu: a 16:9 picture: fights show more at the sides (--aspect W:H: another shape)\n"
            "  --output WxH           gpu, headless: the size of the picture (default 704x448)\n"
            "  --headless --frames N  run without a window for N frames\n"
            "  --shot-every N:PREFIX  write PREFIX<frame>.png every N frames\n"
            "  --hash-log PATH        write a state hash per frame\n"
            "  --movie PATH           replay an input movie\n"
            "  --record PATH          record an input movie\n"
            "  --auto SEED[:FROM]     random input from frame FROM on (exploration)\n"
            "  --dump-ram PREFIX      write work RAM (PREFIXhigh.bin, PREFIXlow.bin) and CPU registers at exit\n"
            "  --video-stats          print what the game asked of the video hardware, at exit\n"
            "  --stats                print recompiled/interpreted statistics at exit\n",
            argv0);
}

int main(int argc, char **argv) {
    port::Port p;
    auto &o = p.opt;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                Usage(argv[0]);
                exit(2);
            }
            return argv[++i];
        };
        if (a == "--bios") {
            o.biosPath = next();
        } else if (a == "--disc") {
            o.discPath = next();
        } else if (a == "--backup") {
            o.backupPath = next();
        } else if (a == "--cpu") {
            o.cpuMode = next();
        } else if (a == "--list-features") {
            // For the launcher: one optional feature per line (recomp-common/settings.h)
            satrecomp::RegisterGame();
            port::RegisterPortFeatures();
            // (not those whose switch is on the launcher's first page: game.h, kFeatureFirstPage)
            std::vector<recomp::FeatureInfo> list = satrecomp::FeatureList();
            list.erase(std::remove_if(list.begin(), list.end(), [](const recomp::FeatureInfo &f) { return f.flags & satrecomp::kFeatureFirstPage; }),
                       list.end());
            fputs(recomp::FeatureListText(list).c_str(), stdout);
            return 0;
        } else if (a == "--menu") {
            p.menuTest = next();
        } else if (a == "--state-save") {
            o.stateSave = next();
        } else if (a == "--state-load") {
            o.stateLoad = next();
        } else if (a == "--settings") {
            o.settingsPath = next();
        } else if (a == "--dump-decode") {
            return satrecomp::DumpDecodeTable(next()) ? 0 : 1;
        } else if (a == "--record-blocks") {
            o.recordBlocks = next();
        } else if (a == "--reset-test") {
            p.resetTest = next();
        } else if (a == "--features") {
            o.features = next();
        } else if (a == "--fullscreen") {
            o.fullscreen = true;
        } else if (a == "--scale") {
            o.scale = atoi(next().c_str());
        } else if (a == "--video") {
            p.opt.video = next();
        } else if (a == "--internal-scale") {
            p.opt.internalScale = static_cast<uint32_t>(atoi(next().c_str()));
        } else if (a == "--widescreen") {
            p.opt.aspect = 16.0f / 9.0f;
        } else if (a == "--aspect") {
            unsigned w = 0, h = 0;
            if (sscanf(next().c_str(), "%u:%u", &w, &h) == 2 && w && h) {
                p.opt.aspect = static_cast<float>(w) / static_cast<float>(h);
            }
        } else if (a == "--output") {
            unsigned w = 0, h = 0;
            if (sscanf(next().c_str(), "%ux%u", &w, &h) == 2 && w && h) {
                p.opt.outputWidth = w;
                p.opt.outputHeight = h;
            }
        } else if (a == "--mute") {
            o.mute = true;
        } else if (a == "--unthrottled") {
            o.unthrottled = true;
        } else if (a == "--headless") {
            o.headless = true;
        } else if (a == "--frames") {
            o.frames = strtoull(next().c_str(), nullptr, 10);
        } else if (a == "--shot-every") {
            std::string v = next();
            auto colon = v.find(':');
            o.shotEvery = atoi(v.substr(0, colon).c_str());
            o.shotPrefix = colon == std::string::npos ? "shot_" : v.substr(colon + 1);
        } else if (a == "--hash-log") {
            o.hashLog = next();
        } else if (a == "--movie") {
            o.moviePlay = next();
        } else if (a == "--record") {
            o.movieRecord = next();
        } else if (a == "--auto") {
            std::string v = next();
            auto colon = v.find(':');
            o.autoSeed = strtoull(v.substr(0, colon).c_str(), nullptr, 10);
            o.autoFrom = colon == std::string::npos ? 0 : strtoull(v.substr(colon + 1).c_str(), nullptr, 10);
        } else if (a == "--dump-ram") {
            o.dumpRam = next();
        } else if (a == "--video-stats") {
            o.videoStats = true;
        } else if (a == "--stats") {
            o.stats = true;
        } else {
            Usage(argv[0]);
            return 2;
        }
    }
    if (o.discPath.empty()) {
        Usage(argv[0]);
        return 2;
    }
    if (!p.Init()) {
        return 1;
    }

    int rc = 0;
    if (o.headless) {
        for (uint64_t f = 0; (o.frames == 0 || f < o.frames) && !p.quit; f++) {
            p.RunFrame();
        }
    } else {
        rc = port::RunWindowed(p);
    }
    p.Shutdown();
    return rc;
}
