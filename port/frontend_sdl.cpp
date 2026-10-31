// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// Windowed frontend: SDL3 window, audio and input around Port::RunFrame().
#include "bindings.h"
#include "game.h"
#include "hud.h"
#include "png_write.h"
#include "port.h"
#include "shading.h"

#include "../video/gpu.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>

namespace port {

namespace {

    struct Audio {
        SDL_AudioStream *stream = nullptr;
        std::vector<int16_t> pending; // samples produced during the current frame
    };

    // What each player's keys and gamepad buttons do: the player's choices (bindings.h; the launcher's
    // Controllers page), or the usual layout. Read once when the window opens; while playing only this table
    // is looked at.
    Bindings g_bindings = DefaultBindings();

    static_assert(kBoundActions[0].bit == kPadUp && kBoundActions[1].bit == kPadDown && kBoundActions[2].bit == kPadLeft &&
                  kBoundActions[3].bit == kPadRight && kBoundActions[4].bit == kPadStart && kBoundActions[5].bit == kPadA &&
                  kBoundActions[6].bit == kPadB && kBoundActions[7].bit == kPadC && kBoundActions[8].bit == kPadX &&
                  kBoundActions[9].bit == kPadY && kBoundActions[10].bit == kPadZ && kBoundActions[11].bit == kPadL &&
                  kBoundActions[12].bit == kPadR);

    bool ButtonBound(int button) {
        return PadInputBound(g_bindings, {button, -1});
    }

    // Some player has this key bound
    bool KeyBound(SDL_Scancode code) {
        for (int player = 0; player < kBoundPlayers; player++) {
            for (int action = 0; action < ActionCount(); action++) {
                if (g_bindings.key[player][action] == code) {
                    return true;
                }
            }
        }
        return false;
    }

    uint16_t ReadKeyboard(const bool *keys, int player) {
        uint16_t pad = 0;
        for (int action = 0; action < ActionCount(); action++) {
            const SDL_Scancode key = g_bindings.key[player][action];
            if (key != SDL_SCANCODE_UNKNOWN && keys[key]) {
                pad |= ActionBits(action);
            }
        }
        return pad;
    }

    uint16_t ReadGamepad(SDL_Gamepad *gp, int player) {
        const int kDead = 16000; // how far a stick or a trigger must go to count
        uint16_t pad = 0;
        for (int action = 0; action < ActionCount(); action++) {
            const PadInput input = g_bindings.pad[player][action];
            if ((input.button >= 0 && SDL_GetGamepadButton(gp, static_cast<SDL_GamepadButton>(input.button))) ||
                (input.axis >= 0 && SDL_GetGamepadAxis(gp, static_cast<SDL_GamepadAxis>(input.axis)) > kDead)) {
                pad |= ActionBits(action);
            }
        }
        const int x = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX);
        const int y = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY);
        if (x < -kDead) pad |= kPadLeft;
        if (x > kDead) pad |= kPadRight;
        if (y < -kDead) pad |= kPadUp;
        if (y > kDead) pad |= kPadDown;
        return pad;
    }

} // namespace

int RunWindowed(Port &port) {
    // Test aids, so that what only happens in a window can be tried with nothing shown (docs/FEATURES.md):
    //   SATRECOMP_WINDOW_HIDDEN=1          the window is made but not shown, and the program stays in the background
    //   SATRECOMP_WINDOW_KEYS=PASS:KEY,... on that pass of the loop below the key goes down as from the keyboard
    //                                      (F1 to F12, ESC, MINUS, EQUALS); or SHOT=path, the picture as it was
    //                                      last shown; or FEATURE=name=value, as the menu would set it; or QUIT
    //   SATRECOMP_WINDOW_LOG=1             a line whenever the game stops or goes on, or the speed or the note
    //                                      changes, and every 60 frames, with the time
    const bool testHidden = getenv("SATRECOMP_WINDOW_HIDDEN") != nullptr, testLog = getenv("SATRECOMP_WINDOW_LOG") != nullptr;
    struct TestStep {
        uint64_t pass;
        std::string what;
    };
    std::vector<TestStep> testSteps;
    if (const char *keys = getenv("SATRECOMP_WINDOW_KEYS")) {
        std::string list = keys;
        for (size_t at = 0; at < list.size();) {
            const size_t end = std::min(list.find(',', at), list.size()), colon = list.find(':', at);
            if (colon < end) {
                testSteps.push_back({strtoull(list.substr(at, colon - at).c_str(), nullptr, 10), list.substr(colon + 1, end - colon - 1)});
            }
            at = end + 1;
        }
    }
    if (testHidden) {
        SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    g_bindings = LoadBindings(port.opt.settingsPath);
    // (the Display page's Window size, in times 240 lines, unless --scale says)
    static const int kWindowTimes[] = {3, 2, 4, 5, 6};
    const int scale = port.opt.scale > 0 ? port.opt.scale : kWindowTimes[std::clamp(satrecomp::FeatureValue("window_size"), 0, 4)];
    // The picture is drawn by the GPU (video/gpu.h) into the window's swapchain, or in software and shown
    // through an SDL renderer; or, through a shader (the Display page's Shader: shading.h), in an OpenGL window
    // that the picture goes to at the Saturn's own size, drawn by the GPU without a window or in software
    // (as wide as the picture: 4:3, or wider in widescreen)
    const int windowWidth = port.gpu ? static_cast<int>(240.0f * static_cast<float>(scale) * port.gpu->Aspect() + 0.5f) : 320 * scale;
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    Shading shading;
    bool shaded = false;
    int shownShader = satrecomp::FeatureValue("display_shader");
    std::string preset = port.ShaderPreset();
    // Makes the window (`width` x `height`, with `flags`) for the way the picture is shown: shaded when there is a
    // preset and librashader can draw it, otherwise as always; false if it cannot be drawn to at all
    std::function<bool(int, int, SDL_WindowFlags)> openWindow = [&](int width, int height, SDL_WindowFlags flags) -> bool {
        std::string error;
        const bool shade = !preset.empty() && Shading::Available(error);
        if (!preset.empty() && !shade) {
            fprintf(stderr, "shader: %s\n", error.c_str());
        }
        flags |= SDL_WINDOW_RESIZABLE | (port.gpu || shade ? SDL_WINDOW_HIGH_PIXEL_DENSITY : 0) | (shade ? SDL_WINDOW_OPENGL : 0) |
                 (testHidden ? SDL_WINDOW_HIDDEN : 0);
        window = SDL_CreateWindow("satrecomp", width, height, flags);
        if (!window) {
            fprintf(stderr, "SDL window: %s\n", SDL_GetError());
            return false;
        }
        shaded = false;
        if (shade) {
            if (shading.Open(window, error) && shading.Load(preset, error)) {
                shaded = true;
            } else {
                // (no shader after all: a window of the usual kind instead)
                fprintf(stderr, "shader: %s\n", error.c_str());
                shading.Close();
                SDL_DestroyWindow(window);
                window = nullptr;
                preset.clear();
                return openWindow(width, height, flags & ~SDL_WINDOW_OPENGL);
            }
            if (port.gpu) {
                port.gpu->SetNative(true);
            }
            return true;
        }
        if (port.gpu) {
            port.gpu->SetNative(false);
            if (!SDL_ClaimWindowForGPUDevice(port.gpuDevice, window)) {
                fprintf(stderr, "SDL window: %s\n", SDL_GetError());
                return false;
            }
            // (frames are paced here, to the Saturn's field rate; no waiting for the display on top of that)
            if (SDL_WindowSupportsGPUPresentMode(port.gpuDevice, window, SDL_GPU_PRESENTMODE_IMMEDIATE)) {
                SDL_SetGPUSwapchainParameters(port.gpuDevice, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_IMMEDIATE);
            }
            int pixelsWide = 0, pixelsHigh = 0;
            SDL_GetWindowSizeInPixels(window, &pixelsWide, &pixelsHigh);
            port.gpu->SizeFor(pixelsWide, pixelsHigh);
            return true;
        }
        renderer = SDL_CreateRenderer(window, nullptr);
        if (!renderer) {
            fprintf(stderr, "SDL window: %s\n", SDL_GetError());
            return false;
        }
        SDL_SetRenderVSync(renderer, 0);
        return true;
    };
    auto closeWindow = [&]() {
        if (shaded) {
            shading.Close();
        } else if (port.gpu) {
            SDL_WaitForGPUIdle(port.gpuDevice);
            SDL_ReleaseWindowFromGPUDevice(port.gpuDevice, window);
        } else if (renderer) {
            SDL_DestroyRenderer(renderer);
        }
        renderer = nullptr;
        SDL_DestroyWindow(window);
        window = nullptr;
    };
    if (!openWindow(windowWidth, 240 * scale, port.opt.fullscreen ? SDL_WINDOW_FULLSCREEN : 0)) {
        return 1;
    }
    if (port.gpu) {
        port.gpu->SetFit(satrecomp::FeatureValue("display_scaling"), satrecomp::FeatureValue("display_shape"));
    }

    // (--mute starts with no sound; the menu's Sound row switches it on, so the device is opened all the same)
    Audio audio;
    port.menu.muted = port.opt.mute;
    bool silent = false;
    {
        const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 44100};
        audio.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (audio.stream) {
            SDL_ResumeAudioStreamDevice(audio.stream);
            port.audioCtx = &audio;
            port.audioSink = [](int16_t l, int16_t r, void *ctx) {
                auto &a = *static_cast<Audio *>(ctx);
                a.pending.push_back(l);
                a.pending.push_back(r);
            };
        } else {
            fprintf(stderr, "Audio: %s\n", SDL_GetError());
        }
    }

    std::vector<SDL_Gamepad *> gamepads;
    SDL_Texture *texture = nullptr;
    uint32_t texW = 0, texH = 0;
    uint64_t shownSerial = 0;
    std::vector<uint32_t> frameCopy;
    std::vector<uint32_t> shadedPixels, softwareShown; // (the GPU's picture read back for a shader; the software one)

    // The picture shown in an OpenGL window through the preset (`shade` false: the menu, only fitted, smoothly)
    auto showShaded = [&](const uint32_t *pixels, uint32_t width, uint32_t height, bool shade) {
        int wide = 0, high = 0;
        shading.Size(wide, high);
        const SDL_FRect where = port.gpu ? port.gpu->Fit(wide, high)
                                         : satrecomp::video::FitPicture(wide, high, static_cast<float>(width), static_cast<float>(height), 4.0f / 3.0f,
                                                                        satrecomp::FeatureValue("display_scaling"), satrecomp::FeatureValue("display_shape"));
        shading.Show(pixels, static_cast<int>(width), static_cast<int>(height), where, shade);
    };
    // The software picture through the SDL renderer, fitted as the Display page says
    auto showSoftware = [&]() {
        if (!renderer) {
            return;
        }
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        if (texture) {
            int wide = 0, high = 0;
            SDL_GetCurrentRenderOutputSize(renderer, &wide, &high);
            const int scaling = satrecomp::FeatureValue("display_scaling");
            SDL_SetTextureScaleMode(texture, scaling == 2 ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
            const SDL_FRect where = satrecomp::video::FitPicture(wide, high, static_cast<float>(texW), static_cast<float>(texH), 4.0f / 3.0f, scaling,
                                                                 satrecomp::FeatureValue("display_shape"));
            SDL_RenderTexture(renderer, texture, nullptr, &where);
        }
        SDL_RenderPresent(renderer);
    };
    shading.keepLast = !testSteps.empty(); // (for SATRECOMP_WINDOW_KEYS' SHOT)

    // The in-game menu: opened and closed by Esc, F1, a gamepad's Back/Select button (alone, or with Start
    // held) or a stick's click (the game is left from its "Quit game"). While it is open the game stands
    // still and the pad works the menu; buttons held as it closes do not reach the game until let go.
    uint16_t previousPad = 0, heldAtClose = 0;
    unsigned previousMenu = 0;
    bool bothHeld = false;    // a gamepad's Start and Select (Back) together: the menu
    bool backToggled = false; // Back's own press has opened or closed the menu, and Back is still held
    std::vector<uint32_t> menuBase, menuPicture;
    uint32_t menuWidth = 0, menuHeight = 0;
    bool menuDirty = false;
    auto toggleMenu = [&]() {
        if (port.menu.IsOpen()) {
            port.menu.Close();
        } else if (port.MenuPicture(menuBase, menuWidth, menuHeight)) {
            port.menu.Open(true);
            menuDirty = true;
        }
    };
    auto quickState = [&](bool save) {
        std::string error;
        const std::string path = port.StatePath(port.menu.Slot());
        if (!(save ? port.SaveState(path, error) : port.LoadState(path, error))) {
            fprintf(stderr, "state %d: %s\n", port.menu.Slot(), error.c_str());
        }
    };

    // For practice: the reset point (F2 sets it, F3 or the right stick's click goes back to it), the game
    // stopped (F4) and gone on a frame at a time (F8), and its speed (- and =, or the menu). A note at the
    // top says what was done, for a while.
    bool paused = false, stepFrame = false;
    uint64_t noteUntil = 0;
    std::string noteShown;
    auto showNote = [&](const std::string &text) {
        noteShown = text;
        SetHudNote(text);
    };
    auto note = [&](const std::string &text) {
        showNote(text);
        noteUntil = SDL_GetTicksNS() + 1'500'000'000ull;
    };
    auto resetPoint = [&](bool set) {
        std::string error;
        if (set ? port.SetResetPoint(error) : port.GoToResetPoint(error)) {
            note(set ? "RESET POINT SET" : "RESET");
        } else {
            note(error);
        }
    };
    auto setSpeed = [&](int speed) {
        port.menu.SetSpeed(speed);
        note("SPEED " + std::to_string(port.menu.Speed()) + "%");
    };

    const double kFrameNs = 1e9 / 59.8261; // NTSC Saturn field rate
    uint64_t nextFrame = SDL_GetTicksNS();
    bool running = true;
    uint64_t pass = 0, loggedFrame = ~0ull;
    const uint64_t startedAt = SDL_GetTicksNS();
    std::string logged;
    while (running && !port.quit && (port.opt.frames == 0 || port.frame < port.opt.frames)) {
        SDL_Event ev;
        pass++;
        // The Display page as it is now (the menu may change it): the picture's fit, and the shader, for which the
        // window is made again when it goes from none to one or back (an OpenGL window, or the GPU's)
        if (port.gpu) {
            port.gpu->SetFit(satrecomp::FeatureValue("display_scaling"), satrecomp::FeatureValue("display_shape"));
        }
        if (const int shader = satrecomp::FeatureValue("display_shader"); shader != shownShader) {
            shownShader = shader;
            const std::string wanted = port.ShaderPreset();
            if (wanted != preset) {
                std::string error;
                if (shaded && !wanted.empty()) {
                    if (shading.Load(wanted, error)) {
                        preset = wanted;
                    } else {
                        fprintf(stderr, "shader: %s\n", error.c_str());
                    }
                } else {
                    int wide = 0, high = 0;
                    SDL_GetWindowSize(window, &wide, &high);
                    const SDL_WindowFlags full = SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN;
                    if (texture) {
                        SDL_DestroyTexture(texture);
                        texture = nullptr;
                    }
                    closeWindow();
                    preset = wanted;
                    if (!openWindow(wide, high, full)) {
                        break;
                    }
                    shownSerial = 0;
                    menuDirty = true;
                }
            }
        }
        for (const TestStep &step : testSteps) {
            if (step.pass != pass) {
                continue;
            }
            static const struct {
                const char *name;
                SDL_Scancode code;
            } kKeys[] = {{"ESC", SDL_SCANCODE_ESCAPE}, {"MINUS", SDL_SCANCODE_MINUS}, {"EQUALS", SDL_SCANCODE_EQUALS}, {"F1", SDL_SCANCODE_F1}, {"F2", SDL_SCANCODE_F2},
                         {"F3", SDL_SCANCODE_F3},  {"F4", SDL_SCANCODE_F4},       {"F5", SDL_SCANCODE_F5},         {"F6", SDL_SCANCODE_F6}, {"F7", SDL_SCANCODE_F7},
                         {"F8", SDL_SCANCODE_F8},  {"F9", SDL_SCANCODE_F9},       {"F10", SDL_SCANCODE_F10},       {"F11", SDL_SCANCODE_F11}, {"F12", SDL_SCANCODE_F12}};
            if (step.what == "QUIT") {
                running = false;
            } else if (step.what.rfind("FEATURE=", 0) == 0) {
                const size_t equals = step.what.find('=', 8);
                if (equals != std::string::npos) {
                    satrecomp::SetFeatureValue(step.what.substr(8, equals - 8).c_str(), atoi(step.what.c_str() + equals + 1));
                }
            } else if (step.what.rfind("SHOT=", 0) == 0) {
                // (shaded: what the preset drew, as the window shows it)
                std::vector<uint32_t> drawn;
                int drawnWidth = 0, drawnHeight = 0;
                if (shaded && shading.ReadBack(drawn, drawnWidth, drawnHeight)) {
                    WritePng(step.what.substr(5).c_str(), drawn.data(), static_cast<uint32_t>(drawnWidth), static_cast<uint32_t>(drawnHeight));
                } else {
                    port.SaveShot(step.what.substr(5));
                }
            }
            for (const auto &key : kKeys) {
                if (step.what == key.name) {
                    SDL_Event press{};
                    press.type = SDL_EVENT_KEY_DOWN;
                    press.key.scancode = key.code;
                    SDL_PushEvent(&press);
                }
            }
        }
        if (testLog) {
            const std::string now = std::string(port.menu.IsOpen() ? "menu" : paused ? "stopped" : "running") + " speed " + std::to_string(port.menu.Speed()) + " slot " +
                                    std::to_string(port.menu.Slot()) + " note \"" + noteShown + "\"";
            if (now != logged || (port.frame % 60 == 0 && port.frame != loggedFrame)) {
                logged = now;
                loggedFrame = port.frame;
                fprintf(stderr, "window: pass %llu frame %llu at %.0f ms: %s\n", static_cast<unsigned long long>(pass), static_cast<unsigned long long>(port.frame),
                        static_cast<double>(SDL_GetTicksNS() - startedAt) / 1e6, now.c_str());
            }
        }
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_EVENT_QUIT: running = false; break;
            case SDL_EVENT_KEY_DOWN:
                if ((ev.key.scancode == SDL_SCANCODE_ESCAPE || ev.key.scancode == SDL_SCANCODE_F1) && !ev.key.repeat) {
                    toggleMenu();
                } else if (ev.key.scancode == SDL_SCANCODE_F5 || ev.key.scancode == SDL_SCANCODE_F9) {
                    // F5 keeps the game in the slot, F9 goes back to it; F6 / F7: the slot before, the next
                    if (!port.menu.IsOpen()) {
                        quickState(ev.key.scancode == SDL_SCANCODE_F5);
                    }
                } else if (ev.key.scancode == SDL_SCANCODE_F2 || ev.key.scancode == SDL_SCANCODE_F3) {
                    if (!port.menu.IsOpen()) {
                        resetPoint(ev.key.scancode == SDL_SCANCODE_F2);
                    }
                } else if (ev.key.scancode == SDL_SCANCODE_F4) {
                    paused = !paused;
                } else if (ev.key.scancode == SDL_SCANCODE_F8) {
                    paused = true;
                    stepFrame = true;
                } else if (ev.key.scancode == SDL_SCANCODE_MINUS || ev.key.scancode == SDL_SCANCODE_EQUALS) {
                    const int speed = port.menu.Speed();
                    setSpeed(ev.key.scancode == SDL_SCANCODE_MINUS ? (speed == 100 ? 50 : 25) : (speed == 25 ? 50 : 100));
                } else if (ev.key.scancode == SDL_SCANCODE_F6 || ev.key.scancode == SDL_SCANCODE_F7) {
                    port.menu.SetSlot(port.menu.Slot() + (ev.key.scancode == SDL_SCANCODE_F7 ? 1 : -1));
                    menuDirty = true;
                } else if (ev.key.scancode == SDL_SCANCODE_F11 ||
                           (ev.key.scancode == SDL_SCANCODE_RETURN && (ev.key.mod & SDL_KMOD_ALT))) {
                    const bool fs = SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN;
                    SDL_SetWindowFullscreen(window, !fs);
                }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (ButtonBound(ev.gbutton.button)) {
                    // (a button given to the game does only that)
                } else if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_BACK || ev.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_STICK) {
                    toggleMenu();
                    backToggled = backToggled || ev.gbutton.button == SDL_GAMEPAD_BUTTON_BACK;
                } else if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_STICK && !port.menu.IsOpen()) {
                    resetPoint(port.resetPoint.empty()); // (the first click sets it)
                }
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (SDL_Gamepad *gp = SDL_OpenGamepad(ev.gdevice.which)) {
                    gamepads.push_back(gp);
                }
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                for (auto it = gamepads.begin(); it != gamepads.end(); ++it) {
                    if (SDL_GetGamepadID(*it) == ev.gdevice.which) {
                        SDL_CloseGamepad(*it);
                        gamepads.erase(it);
                        break;
                    }
                }
                break;
            default: break;
            }
        }

        if (port.opt.moviePlay.empty()) {
            // Keyboard and the first gamepad are player 1; a second gamepad is player 2 (who has keys only if
            // some were chosen)
            uint16_t pads[2] = {0, 0};
            const bool *keys = SDL_GetKeyboardState(nullptr);
            if (!(SDL_GetModState() & SDL_KMOD_ALT)) {
                pads[0] |= ReadKeyboard(keys, 0);
                pads[1] |= ReadKeyboard(keys, 1);
            }
            for (size_t i = 0; i < gamepads.size() && i < 2; i++) {
                pads[i] |= ReadGamepad(gamepads[i], static_cast<int>(i));
            }
            // (the buttons themselves, on any gamepad, whatever they do in the game)
            bool both = false, back = false;
            for (SDL_Gamepad *gamepad : gamepads) {
                const bool held = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_BACK);
                back = back || held;
                both = both || (held && SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_START));
            }
            uint16_t pressed = pads[0] & ~previousPad;
            previousPad = pads[0];
            // Start and Select held together open and close the menu. Select alone does that here already, so
            // when its own press has just done it the menu is not switched a second time; either way the
            // second of the two to go down is not also a press in the menu (Start chooses a row there), and
            // neither reaches the game until let go.
            if (both && !bothHeld) {
                if (!backToggled) {
                    toggleMenu();
                }
                pressed = 0;
                heldAtClose = previousPad;
            }
            bothHeld = both;
            backToggled = backToggled && back;
            // The menu's buttons are the same whatever the game's are set to: the arrows, the d-pad and the
            // left stick (and the game's own directions); Space, Enter and A choose; Backspace and B go back
            unsigned menuNow = 0;
            menuNow |= (previousPad & kPadUp) || keys[SDL_SCANCODE_UP] ? recomp::kMenuUp : 0;
            menuNow |= (previousPad & kPadDown) || keys[SDL_SCANCODE_DOWN] ? recomp::kMenuDown : 0;
            menuNow |= (previousPad & kPadLeft) || keys[SDL_SCANCODE_LEFT] ? recomp::kMenuLeft : 0;
            menuNow |= (previousPad & kPadRight) || keys[SDL_SCANCODE_RIGHT] ? recomp::kMenuRight : 0;
            menuNow |= keys[SDL_SCANCODE_SPACE] || (keys[SDL_SCANCODE_RETURN] && !(SDL_GetModState() & SDL_KMOD_ALT)) ? recomp::kMenuConfirm : 0;
            menuNow |= keys[SDL_SCANCODE_BACKSPACE] ? recomp::kMenuBack : 0;
            for (SDL_Gamepad *gamepad : gamepads) {
                menuNow |= SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP) ? recomp::kMenuUp : 0;
                menuNow |= SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN) ? recomp::kMenuDown : 0;
                menuNow |= SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT) ? recomp::kMenuLeft : 0;
                menuNow |= SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) ? recomp::kMenuRight : 0;
                menuNow |= SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_SOUTH) ? recomp::kMenuConfirm : 0;
                menuNow |= SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_EAST) ? recomp::kMenuBack : 0;
            }
            unsigned menuPressed = menuNow & ~previousMenu;
            previousMenu = menuNow;
            if (both) {
                menuPressed = 0;
            }
            if (port.menu.IsOpen()) {
                if (menuPressed) {
                    port.MenuAct(port.menu.Press(menuPressed));
                    menuDirty = true;
                } else if (pressed) {
                    port.menu.Note(""); // (a press of something else still takes the note away)
                    menuDirty = true;
                }
                if (!port.menu.IsOpen()) {
                    heldAtClose = pads[0];
                }
                pads[0] = pads[1] = 0;
            }
            // (the look controls, for a game's free camera: runtime/game.h, Look)
            satrecomp::LookInput look;
            if (!port.menu.IsOpen()) {
                auto key = [&](SDL_Scancode code) {
                    return keys[code] && !KeyBound(code) ? 1.0f : 0.0f;
                };
                look.x = key(SDL_SCANCODE_L) - key(SDL_SCANCODE_J);
                look.y = key(SDL_SCANCODE_K) - key(SDL_SCANCODE_I);
                look.zoom = key(SDL_SCANCODE_U) - key(SDL_SCANCODE_O);
                for (SDL_Gamepad *gamepad : gamepads) {
                    auto axis = [&](SDL_GamepadAxis which) {
                        const float at = SDL_GetGamepadAxis(gamepad, which) / 32767.0f;
                        return std::fabs(at) < 0.2f ? 0.0f : at;
                    };
                    look.x = std::clamp(look.x + axis(SDL_GAMEPAD_AXIS_RIGHTX), -1.0f, 1.0f);
                    look.y = std::clamp(look.y + axis(SDL_GAMEPAD_AXIS_RIGHTY), -1.0f, 1.0f);
                }
            }
            satrecomp::Look() = look;
            heldAtClose &= pads[0];
            port.pad[0] = pads[0] & ~heldAtClose;
            port.pad[1] = pads[1];
        }

        if (audio.stream && silent != port.menu.muted) {
            silent = port.menu.muted;
            SDL_SetAudioStreamGain(audio.stream, silent ? 0.0f : 1.0f);
        }
        if (port.menu.IsOpen()) {
            // The game's picture as it was, with the menu over it
            if (menuDirty) {
                menuDirty = false;
                menuPicture = menuBase;
                port.menu.Draw(menuPicture.data(), static_cast<int>(menuWidth), static_cast<int>(menuHeight));
                if (port.gpu && !shaded) {
                    port.gpu->SetOverlay(menuPicture.data(), menuWidth, menuHeight);
                } else if (texture && texW == menuWidth && texH == menuHeight) {
                    SDL_UpdateTexture(texture, nullptr, menuPicture.data(), static_cast<int>(texW * sizeof(uint32_t)));
                }
            }
            if (shaded) {
                showShaded(menuPicture.data(), menuWidth, menuHeight, false);
            } else if (port.gpu) {
                port.gpu->Present(window);
            } else {
                showSoftware();
            }
            SDL_Delay(8);
            nextFrame = SDL_GetTicksNS();
            continue;
        }
        if (port.gpu) {
            port.gpu->ClearOverlay();
        }
        shownSerial = port.gpu ? shownSerial : 0; // (the software picture is put back after a menu)

        if (noteUntil && SDL_GetTicksNS() > noteUntil) {
            noteUntil = 0;
        }
        if (!noteUntil) { // (nothing just done to tell of: how things stand)
            showNote(paused ? "PAUSED  F8: A FRAME ON  F4: GO ON" : port.menu.Speed() != 100 ? "SPEED " + std::to_string(port.menu.Speed()) + "%" : "");
        }
        const bool runs = !paused || stepFrame;
        stepFrame = false;
        if (runs) {
            port.stepping = paused; // (a frame at a time: no run-ahead)
            port.RunFrame();
        } else {
            audio.pending.clear();
        }
        if (port.menu.Speed() != 100) {
            audio.pending.clear(); // (no sound in slow motion)
        }

        if (audio.stream && !audio.pending.empty()) {
            // Drop audio rather than fall behind when running unthrottled
            if (!port.opt.unthrottled || SDL_GetAudioStreamQueued(audio.stream) < 44100 * 4 / 5) {
                SDL_PutAudioStreamData(audio.stream, audio.pending.data(),
                                       static_cast<int>(audio.pending.size() * sizeof(int16_t)));
            }
        }
        audio.pending.clear();

        if (port.gpu) {
            // (a window, not full screen or maximized, takes the picture's shape again when that changes:
            // widescreen switched on or off in the menu)
            static float shownAspect = port.gpu->Aspect();
            if (shownAspect != port.gpu->Aspect()) {
                shownAspect = port.gpu->Aspect();
                if (!(SDL_GetWindowFlags(window) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED))) {
                    int windowW = 0, windowH = 0;
                    SDL_GetWindowSize(window, &windowW, &windowH);
                    SDL_SetWindowSize(window, static_cast<int>(static_cast<float>(windowH) * shownAspect + 0.5f), windowH);
                }
            }
            port.UpdateHud(); // (the note may have changed while the game stands still)
            if (shaded) {
                // (the picture at the Saturn's own size, with the readout, read back and drawn through the preset)
                port.gpu->SizeFor(0, 0);
                if (port.gpu->ReadShown(shadedPixels)) {
                    showShaded(shadedPixels.data(), port.gpu->OutputWidth(), port.gpu->OutputHeight(), true);
                }
            } else {
                port.gpu->Present(window);
            }
        } else {
            {
                std::lock_guard lock(port.fbMutex);
                static uint64_t hudSerial = 0;
                if ((port.fbSerial != shownSerial || hudSerial != HudShownSerial()) && port.fbWidth && port.fbHeight) {
                    shownSerial = port.fbSerial;
                    hudSerial = HudShownSerial();
                    if (!shaded && (!texture || texW != port.fbWidth || texH != port.fbHeight)) {
                        if (texture) {
                            SDL_DestroyTexture(texture);
                        }
                        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XBGR8888, SDL_TEXTUREACCESS_STREAMING, port.fbWidth, port.fbHeight);
                    }
                    texW = port.fbWidth;
                    texH = port.fbHeight;
                    softwareShown = port.fb;
                    DrawHud(softwareShown.data(), static_cast<int>(texW), static_cast<int>(texH));
                    if (!shaded) {
                        SDL_UpdateTexture(texture, nullptr, softwareShown.data(), static_cast<int>(texW * sizeof(uint32_t)));
                    }
                }
            }
            if (shaded) {
                if (!softwareShown.empty()) {
                    showShaded(softwareShown.data(), texW, texH, true);
                }
            } else {
                showSoftware();
            }
        }

        if (!port.opt.unthrottled) {
            nextFrame += static_cast<uint64_t>(kFrameNs * 100 / port.menu.Speed());
            const uint64_t now = SDL_GetTicksNS();
            if (now < nextFrame) {
                SDL_DelayPrecise(nextFrame - now);
            } else if (now - nextFrame > 100'000'000ull) {
                nextFrame = now; // fell far behind (paused, moved window): don't try to catch up
            }
        }
    }

    port.audioSink = nullptr;
    if (audio.stream) {
        SDL_DestroyAudioStream(audio.stream);
    }
    for (auto *gp : gamepads) {
        SDL_CloseGamepad(gp);
    }
    if (texture) {
        SDL_DestroyTexture(texture);
    }
    closeWindow();
    return 0;
}

} // namespace port
