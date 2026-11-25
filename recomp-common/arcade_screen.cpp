// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of recomp-common: https://github.com/tmcleroy/recomp-common
// arcade_screen.h: Switchres's exact-rate modes and the screen's refresh clock (Windows; elsewhere neither), and the
// frames synced to the screen with a frame delay.
#include "arcade_screen.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>

#ifdef _WIN32
#define SR_WIN32_STATIC
#include <windows.h>
#include <switchres_wrapper.h> // (downloaded at build time: switchres.cmake)
#endif

namespace recomp::sdl {

#ifdef _WIN32

namespace {
    bool switchresUp = false;   // (sr_init done)
    bool switchresMade = false; // (the desktop in a mode it made)

    // (Switchres's own lines, in the game's log)
    void SwitchresSays(const char *format, ...) {
        va_list args;
        va_start(args, format);
        fputs("switchres: ", stderr);
        vfprintf(stderr, format, args);
        va_end(args);
    }
} // namespace

bool SwitchresBuilt() {
    return true;
}

bool SwitchresMake(int screen, const char *monitor, int width, int height, double rate, MadeMode &made, std::string &error) {
    SwitchresRestore();
    sr_init();
    switchresUp = true;
    sr_set_log_level(2);
    sr_set_log_callback_info(reinterpret_cast<void *>(&SwitchresSays));
    sr_set_log_callback_error(reinterpret_cast<void *>(&SwitchresSays));
    sr_set_log_callback_debug(reinterpret_cast<void *>(&SwitchresSays));
    sr_set_disp(-1); // (the settings every screen it adds starts from)
    sr_set_monitor(monitor);
    // (the board's picture is never interlaced or doubled: a mode of its own lines, or none)
    sr_set_option("interlace", "0");
    sr_set_option("doublescan", "0");
    // (the main screen as the system says; another by its number in the system's order)
    char which[8] = "auto";
    if (screen > 0 && screen < 10) {
        snprintf(which, sizeof which, "%d", screen);
    }
    if (sr_init_disp(screen > 0 ? which : "auto", nullptr) < 0) {
        error = "Switchres could not open the screen";
        SwitchresRestore();
        return false;
    }
    sr_mode mode{};
    if (!sr_switch_to_mode(width, height, rate, 0, &mode)) {
        error = "Switchres could not make or switch to the mode";
        SwitchresRestore();
        return false;
    }
    switchresMade = true;
    made.width = mode.width;
    made.height = mode.height;
    made.vfreq = mode.vfreq;
    made.hfreq = mode.hfreq;
    made.exact = !mode.is_refresh_off && std::fabs(mode.vfreq - rate) < 0.01;
    return true;
}

bool SwitchresActive() {
    return switchresMade;
}

void SwitchresRestore() {
    if (switchresUp) {
        sr_deinit(); // (the desktop's mode, and the modes it changed, as they were)
    }
    switchresUp = switchresMade = false;
}

// The refresh clock: a thread that waits for each refresh of the window's screen
namespace {
    using D3dkmtHandle = UINT;
    struct OpenAdapterFromHdc {
        HDC hdc;
        D3dkmtHandle adapter;
        LUID luid;
        UINT source;
    };
    struct WaitForVerticalBlank {
        D3dkmtHandle adapter;
        D3dkmtHandle device;
        UINT source;
    };
    struct CloseAdapter {
        D3dkmtHandle adapter;
    };
    using OpenFn = LONG(APIENTRY *)(OpenAdapterFromHdc *);
    using WaitFn = LONG(APIENTRY *)(const WaitForVerticalBlank *);
    using CloseFn = LONG(APIENTRY *)(const CloseAdapter *);

    // (shared with the thread, which may be waiting on a refresh still when the clock is stopped: it lets go of it)
    struct Refreshes {
        std::atomic<bool> stop{false};
        std::mutex lock;
        uint64_t last = 0;
        double period = 0;
        int seen = 0;
    };
} // namespace

struct RefreshClock::Impl {
    std::shared_ptr<Refreshes> refreshes;
};

RefreshClock::~RefreshClock() {
    Stop();
}

void RefreshClock::Start(SDL_Window *window) {
    Stop();
    HMODULE gdi = GetModuleHandleA("gdi32.dll");
    if (!gdi) {
        gdi = LoadLibraryA("gdi32.dll");
    }
    const auto open = gdi ? reinterpret_cast<OpenFn>(reinterpret_cast<void *>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromHdc"))) : nullptr;
    const auto wait = gdi ? reinterpret_cast<WaitFn>(reinterpret_cast<void *>(GetProcAddress(gdi, "D3DKMTWaitForVerticalBlankEvent"))) : nullptr;
    const auto close = gdi ? reinterpret_cast<CloseFn>(reinterpret_cast<void *>(GetProcAddress(gdi, "D3DKMTCloseAdapter"))) : nullptr;
    HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!open || !wait || !close || !hwnd) {
        return;
    }
    MONITORINFOEXA info{};
    info.cbSize = sizeof info;
    if (!GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info)) {
        return;
    }
    HDC dc = CreateDCA(nullptr, info.szDevice, nullptr, nullptr);
    if (!dc) {
        return;
    }
    OpenAdapterFromHdc adapter{};
    adapter.hdc = dc;
    const LONG opened = open(&adapter);
    DeleteDC(dc);
    if (opened != 0) {
        fprintf(stderr, "sync to the screen: the screen's refreshes can't be followed (D3DKMT %ld)\n", opened);
        return;
    }
    m_impl = new Impl;
    m_impl->refreshes = std::make_shared<Refreshes>();
    std::thread([refreshes = m_impl->refreshes, adapter, wait, close] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        WaitForVerticalBlank each{adapter.adapter, 0, adapter.source};
        while (!refreshes->stop) {
            if (wait(&each) != 0) {
                break;
            }
            const uint64_t now = SDL_GetTicksNS();
            std::lock_guard<std::mutex> hold(refreshes->lock);
            if (refreshes->last) {
                const double between = static_cast<double>(now - refreshes->last);
                // (a refresh not waited for, the thread held up: not a period; the period an average of many)
                if (refreshes->period == 0) {
                    refreshes->period = between;
                } else if (between < refreshes->period * 1.5) {
                    refreshes->period += (between - refreshes->period) / 64;
                }
            }
            refreshes->last = now;
            refreshes->seen++;
        }
        CloseAdapter done{adapter.adapter};
        close(&done);
    }).detach();
}

void RefreshClock::Stop() {
    if (m_impl) {
        m_impl->refreshes->stop = true;
        delete m_impl;
        m_impl = nullptr;
    }
}

bool RefreshClock::Next(uint64_t when, uint64_t &next, double &period) const {
    if (!m_impl) {
        return false;
    }
    std::lock_guard<std::mutex> hold(m_impl->refreshes->lock);
    const Refreshes &r = *m_impl->refreshes;
    // (some refreshes seen, and the last not long ago: the thread is keeping up)
    if (r.seen < 30 || r.period <= 0 || when > r.last + static_cast<uint64_t>(r.period * 4)) {
        return false;
    }
    period = r.period;
    const double after = when > r.last ? static_cast<double>(when - r.last) : 0;
    next = r.last + static_cast<uint64_t>((std::floor(after / period) + 1) * period);
    return true;
}

#else

bool SwitchresBuilt() {
    return false;
}
bool SwitchresMake(int, const char *, int, int, double, MadeMode &, std::string &error) {
    error = "this build has no Switchres (Windows only)";
    return false;
}
bool SwitchresActive() {
    return false;
}
void SwitchresRestore() {}

struct RefreshClock::Impl {};
RefreshClock::~RefreshClock() {}
void RefreshClock::Start(SDL_Window *) {}
void RefreshClock::Stop() {}
bool RefreshClock::Next(uint64_t, uint64_t &, double &) const {
    return false;
}

#endif

// The arcade screen

void ArcadeScreen::SetFullscreen(SDL_Window *window, const ArcadeChoices &choices, bool full) {
    if (choices.mode != ArcadeMode::Off && full && choices.switchres && SwitchresBuilt()) {
        const char *monitor = kMonitorPresets[std::clamp(choices.monitor, 0, kMonitorPresetCount - 1)];
        const int columns = choices.mode == ArcadeMode::Super ? kSuperColumns : m_board.width;
        MadeMode made;
        std::string error;
        if (SwitchresMake(choices.screen, monitor, columns, m_board.lines, m_board.rate, made, error)) {
            fprintf(stderr, "full screen: switchres made %dx%d at %.3f Hz (%.3f kHz)%s for monitor %s\n", made.width, made.height,
                    made.vfreq, made.hfreq / 1000, made.exact ? "" : ", not the board's rate: the monitor's ranges allow no nearer",
                    monitor);
            SDL_PumpEvents(); // (the system's word that the desktop changed)
            SDL_SetWindowFullscreenMode(window, nullptr);
            return;
        }
        fprintf(stderr, "full screen: switchres: %s; a mode the system lists instead\n", error.c_str());
    } else if (SwitchresActive()) {
        SwitchresRestore(); // (the Arcade monitor page back to off, or the window out of full screen)
    }
    SetFullscreenMode(window, choices.mode, choices.screen, m_board);
}

void ArcadeScreen::Follow(SDL_Window *window, const ArcadeChoices &choices) {
    if (!m_started) {
        m_started = true;
        m_followed = choices;
        return;
    }
    const ArcadeChoices &was = m_followed;
    if (!window || (choices.screen == was.screen && choices.mode == was.mode && choices.switchres == was.switchres &&
                    choices.monitor == was.monitor)) {
        m_followed.boardPace = choices.boardPace;
        return;
    }
    const bool full = SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN;
    if (full) {
        SDL_SetWindowFullscreen(window, false);
        SDL_SyncWindow(window);
    }
    if (choices.screen != was.screen) {
        CentreOnScreen(window, choices.screen);
    }
    SetFullscreen(window, choices, full);
    if (full) {
        SDL_SetWindowFullscreen(window, true);
    }
    SDL_SyncWindow(window);
    m_followed = choices;
}

bool ArcadeScreen::OnArcade(SDL_Window *window) const {
    return window && (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) && (SDL_GetWindowFullscreenMode(window) || SwitchresActive());
}

bool ArcadeScreen::Paced(SDL_Window *window, const ArcadeChoices &choices, bool online) {
    const bool synced = OnArcade(window) && !choices.boardPace && !online;
    // (the screen's refreshes followed while synced, for the frame delay: started again when the window's size, so its
    // mode, changes)
    int w = 0, h = 0;
    if (window) {
        SDL_GetWindowSizeInPixels(window, &w, &h);
    }
    if (synced && (!m_clockOn || w != m_clockWidth || h != m_clockHeight)) {
        m_clock.Start(window);
        m_clockOn = true;
        m_clockWidth = w;
        m_clockHeight = h;
    } else if (!synced && m_clockOn) {
        m_clock.Stop();
        m_clockOn = false;
    }
    if (!synced) {
        m_syncLast = m_syncSince = 0;
        m_aimed = 0;
    }
    return synced;
}

void ArcadeScreen::WorkDone() {
    m_workEnd = SDL_GetTicksNS();
}

uint64_t ArcadeScreen::Shown() {
    // (paced by the screen: the picture just shown waited for its refresh)
    const double frameNs = 1e9 / m_board.rate;
    const uint64_t now = SDL_GetTicksNS();
    uint64_t refresh = 0;
    double period = 0;
    double delay = 0;
    // The frame delay (GroovyMAME's frame_delay, worked out by itself): each frame's controls read and its picture made
    // as late before the refresh it goes up at as it can be, instead of just after the one before, which takes most of
    // the wait for the refresh out of the controls' lag. Its work the longest of late (decaying), a margin for the
    // system's own (more after a refresh missed, less again in time)
    if (m_syncLast && m_clock.Next(now, refresh, period)) {
        // (this frame's work: from the end of the last delay to its picture handed over, never the wait for the screen in
        // showing it, which would make the work a whole refresh and the delay nothing. A frame held up, once, kept for a
        // second or two at most: the longest decays by 2% a frame)
        const double work = m_workEnd > m_syncLast ? std::min(static_cast<double>(m_workEnd - m_syncLast), period) : 0.0;
        m_work = std::max(work, m_work * 0.98);
        // (the refresh after the one aimed at already come: missed, and a frame stays up twice; a wider margin)
        if (m_aimed && refresh > m_aimed + static_cast<uint64_t>(period * 1.5)) {
            m_missed++;
            m_margin = std::min(m_margin + 500000.0, 6e6);
            m_calm = 0;
        } else if (++m_calm >= 600 && m_margin > 1.5e6) {
            m_margin = std::max(m_margin - 250000.0, 1.5e6);
            m_calm = 0;
        }
        // (the next frame for the next refresh; never the one aimed at last, where showing a picture did not wait for it:
        // one frame for each refresh)
        if (m_aimed && refresh < m_aimed + static_cast<uint64_t>(period * 0.5)) {
            refresh = m_aimed + static_cast<uint64_t>(period);
        }
        m_aimed = refresh;
        const double start = static_cast<double>(m_aimed) - m_work - m_margin;
        delay = std::clamp(start - static_cast<double>(now), 0.0, period);
        if (delay > 0) {
            SDL_DelayPrecise(static_cast<uint64_t>(delay));
        }
    } else {
        // (the refreshes not known: a frame never sooner than nine tenths of the board's after the last, in case the
        // system shows it without waiting)
        m_aimed = 0;
        const uint64_t soonest = m_syncLast + static_cast<uint64_t>(frameNs * 0.9);
        if (m_syncLast && now < soonest) {
            SDL_DelayPrecise(soonest - now);
        }
    }
    m_delaySum += delay;
    m_syncLast = SDL_GetTicksNS();
    // (the screen's refresh as measured, in the log every ten seconds or so: the mode's real rate; and the frame delay)
    if (!m_syncSince) {
        m_syncSince = m_syncLast;
        m_syncFrames = 0;
        m_delaySum = 0;
    } else if (++m_syncFrames == 600) {
        fprintf(stderr, "sync to the screen: %.3f frames a second (the board's: %.3f), frame delay %.1f ms%s (work %.1f ms, margin %.1f ms), %d refreshes missed\n",
                m_syncFrames * 1e9 / static_cast<double>(m_syncLast - m_syncSince), m_board.rate, m_delaySum / m_syncFrames / 1e6,
                period > 0 ? "" : " (the screen's refreshes not followed: none)", m_work / 1e6, m_margin / 1e6, m_missed);
        m_syncSince = m_syncLast;
        m_syncFrames = 0;
        m_delaySum = 0;
        m_missed = 0;
    }
    return m_syncLast;
}

void ArcadeScreen::BoardPace(bool arcade) {
    m_syncLast = m_syncSince = 0;
    m_aimed = 0;
    if (!arcade) {
        m_paceSince = 0;
        return;
    }
    // (in an arcade mode on the board's pace: the rate as measured, in the log every ten seconds or so)
    const uint64_t at = SDL_GetTicksNS();
    if (!m_paceSince) {
        m_paceSince = at;
        m_paceFrames = 0;
    } else if (++m_paceFrames == 600) {
        fprintf(stderr, "board's pace: %.3f frames a second (the board's: %.3f)\n", m_paceFrames * 1e9 / static_cast<double>(at - m_paceSince),
                m_board.rate);
        m_paceSince = at;
        m_paceFrames = 0;
    }
}

void ArcadeScreen::Menu() {
    m_syncLast = m_syncSince = m_paceSince = 0;
    m_aimed = 0;
}

void ArcadeScreen::Close() {
    m_clock.Stop();
    m_clockOn = false;
    SwitchresRestore();
}

} // namespace recomp::sdl
