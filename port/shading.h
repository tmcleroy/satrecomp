// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
//
// Shaders: the picture drawn through a RetroArch shader preset (a .slangp: CRT, scanlines, xBRZ, ScaleFX and the
// rest), by librashader (https://github.com/SnowflakePowered/librashader), through OpenGL. librashader is a library
// beside the program (librashader.dylib, .so, .dll), opened when a shader is first asked for: without it, or with
// a preset it cannot make, there is no shader and the picture goes to the window as it always did (present.h).
// librashader needs the window's own OpenGL, which SDL's GPU API keeps to itself, so a window that shows a shader
// is an OpenGL window (SDL_WINDOW_OPENGL) with nothing else drawing to it. On Android, OpenGL ES 3.
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>
#include <vector>

namespace port {

// Of the picture, the part shown larger (none yet in this toolkit: the parameter is cps2recomp's camera's, kept so
// the two copies stay alike). The picture given is then twice as tall: the game's own above, and below it the same
// with the readout over it. `view`, in the pixels of the one above, fills `where`; `keep`, rectangles of the one
// below, are shown where they are and at their size all the same.
struct Zoomed {
    SDL_FRect view;
    std::vector<SDL_Rect> keep;
};

class Shading {
public:
    ~Shading();
    // True if librashader is there to be used (it is opened the first time this is asked); false with the reason
    static bool Available(std::string &error);
    // Takes the window's OpenGL (an OpenGL window, SDL_WINDOW_OPENGL); false with the reason
    bool Open(SDL_Window *window, std::string &error);
    void Close();
    // The preset to draw with (a .slangp file); false with the reason, and the one before is kept
    bool Load(const std::string &preset, std::string &error);
    const std::string &Preset() const {
        return m_preset;
    }
    // The window's size in pixels
    void Size(int &width, int &height) const;
    // Shows a picture (`width` x `height` pixels, each red, green, blue and a spare byte in memory) in the
    // rectangle `where` of the window, the rest black: through the preset, or (`shade` false: the menu, which
    // is drawn larger than the game's picture) only fitted, smoothly. Zoomed (present.h, Zoomed: `pixels` two
    // pictures, one above the other), the preset draws the whole of the one above, and of what it drew the part
    // `view` fills `where`, as a camera would close in on a monitor; the readout's panels over it, as they are
    void Show(const uint32_t *pixels, int width, int height, const SDL_FRect &where, bool shade, const Zoomed *zoomed = nullptr);
    // What is drawn over the whole window after the picture (present.h, Presenter::SetOverlay); nullptr: nothing
    void SetOverlay(const uint32_t *pixels, int width, int height);
    // For the checks (SATRECOMP_SHADER_SHOT): each picture shown kept, and the last one, its first row its top
    bool keepLast = false;
    bool ReadBack(std::vector<uint32_t> &pixels, int &width, int &height);

private:
    SDL_Window *m_window = nullptr;
    SDL_GLContext m_context = nullptr;
    void *m_chain = nullptr; // (librashader's libra_gl_filter_chain_t)
    std::string m_preset;
    // (the picture's texture, and the one the preset draws into, of `where`'s size; zoomed, the picture with the
    // readout over it, for its panels; a framebuffer for each)
    uint32_t m_picture = 0, m_drawn = 0, m_readout = 0, m_pictureFrame = 0, m_drawnFrame = 0, m_readoutFrame = 0;
    int m_width = 0, m_height = 0, m_drawnWidth = 0, m_drawnHeight = 0;
    size_t m_frame = 0;
    std::vector<uint32_t> m_last;
    int m_lastWidth = 0, m_lastHeight = 0;
    void Keep();
    // (the overlay: its texture, and the program that blends it over the window)
    uint32_t m_overlay = 0, m_overlayProgram = 0, m_overlayArray = 0;
    int m_overlayWidth = 0, m_overlayHeight = 0;
    void DrawOverlay(int windowWidth, int windowHeight);
};

} // namespace port
