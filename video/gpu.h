// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// The picture drawn by the GPU (SDL_GPU: Metal, Vulkan, Direct3D 12) from a frame as the renderer hands it
// over (frame.h): the VDP1's quads into a target several times the original's resolution, then that and the
// VDP2's layers put together by priority.
#pragma once

#include "frame.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <vector>

namespace satrecomp::video {

class Gpu {
public:
    bool Init(SDL_GPUDevice *device);
    void Shutdown();

    // The size in pixels the picture will be shown at; the VDP1's target is sized to match
    void SetOutputSize(uint32_t width, uint32_t height);
    // How many times the original's resolution the VDP1's quads are drawn at (0: as the output size needs)
    // The picture's shape, width / height (4:3 unless widescreen)
    void SetAspect(float aspect) {
        m_aspect = aspect;
    }
    float Aspect() const {
        return m_aspect;
    }
    void SetScale(uint32_t scale) {
        m_fixedScale = scale;
    }

    // Does a frame's VDP1 work and makes the picture, in Output()
    void Draw(const Frame &frame);
    // Run-ahead (port/main.cpp): the VDP1's two framebuffers as drawn here are kept before frames that will
    // be taken back, and put back after them (so the next frame draws on what it would have without them).
    // The picture made meanwhile stays: it is the one shown.
    void KeepSprites();
    void PutSpritesBack();
    SDL_GPUTexture *Output() const {
        return m_output;
    }
    uint32_t OutputWidth() const {
        return m_outputWidth;
    }
    uint32_t OutputHeight() const {
        return m_outputHeight;
    }
    // The picture's pixels (RGBA), for screenshots and tests
    bool Read(std::vector<uint32_t> &pixels);
    // The VDP1 framebuffer on display as drawn here (its 16-bit values), for tests
    bool ReadSprites(std::vector<uint16_t> &words, uint32_t &width, uint32_t &height);
    // Shows the picture in a window, as large as fits at 4:3
    void Present(SDL_Window *window);
    // A picture to show in place of the one drawn (the game's with a menu over it, while the game stands
    // still): 32-bit pixels, red in the low byte. ClearOverlay: the drawn picture again.
    void SetOverlay(const uint32_t *pixels, uint32_t width, uint32_t height);
    void ClearOverlay() {
        m_overlayShown = false;
    }
    // A picture with alpha shown over the drawn one, stretched to it (the readout, port/hud.h): 32-bit pixels,
    // red in the low byte. It is blended into the output when the output is shown (Present) or read with it
    // (ReadShown), so Read gives the game's picture alone.
    void SetHudLayer(const uint32_t *pixels, uint32_t width, uint32_t height);
    void ClearHudLayer() {
        m_hudChanged = m_hudChanged || m_hudShown;
        m_hudShown = false;
    }
    bool ReadShown(std::vector<uint32_t> &pixels);

private:
    void ApplyHud();
    SDL_GPUGraphicsPipeline *m_hudPipeline = nullptr;
    SDL_GPUSampler *m_linear = nullptr;
    SDL_GPUTexture *m_hud = nullptr;
    uint32_t m_hudWidth = 0, m_hudHeight = 0;
    bool m_hudShown = false, m_hudApplied = false; // (applied: to the output as it is now)
    // The output as it was before the readout went onto it, kept while a readout shows: when the readout
    // changes and the picture does not (the game stands still: a note says it is stopped), the picture is
    // put back and the new readout goes on
    SDL_GPUTexture *m_plain = nullptr;
    uint32_t m_plainWidth = 0, m_plainHeight = 0;
    bool m_hudChanged = false;
    SDL_GPUDevice *m_device = nullptr;
    SDL_GPUGraphicsPipeline *m_quadPipeline = nullptr, *m_composePipeline = nullptr;
    SDL_GPUSampler *m_nearest = nullptr;
    SDL_GPUTexture *m_sprites[2] = {}; // the VDP1's two framebuffers: [0] on display, [1] drawn to
    SDL_GPUTexture *m_keptSprites[2] = {}; // (run-ahead's copy of them, and the size it is of)
    uint32_t m_keptWidth = 0, m_keptHeight = 0, m_keptScale = 0;
    void CopySprites(SDL_GPUTexture *const from[2], SDL_GPUTexture *const to[2]);
    SDL_GPUTexture *m_vram = nullptr, *m_colors = nullptr, *m_back = nullptr, *m_window = nullptr;
    SDL_GPUTexture *m_vram2 = nullptr, *m_floorLines = nullptr; // for the rotation screen drawn here
    SDL_GPUTexture *m_layers[5] = {};
    SDL_GPUTexture *m_output = nullptr;
    SDL_GPUBuffer *m_vertices = nullptr;
    SDL_GPUTransferBuffer *m_transfer = nullptr, *m_download = nullptr;
    uint32_t m_vertexCapacity = 0, m_transferCapacity = 0, m_downloadCapacity = 0;
    uint32_t m_spriteWidth = 0, m_spriteHeight = 0, m_scale = 0, m_fixedScale = 0;
    uint32_t m_layerWidth = 0, m_layerHeight = 0;
    uint32_t m_outputWidth = 0, m_outputHeight = 0, m_wantedWidth = 640, m_wantedHeight = 480;
    float m_aspect = 4.0f / 3.0f;
    SDL_GPUTexture *m_overlay = nullptr;
    uint32_t m_overlayWidth = 0, m_overlayHeight = 0;
    bool m_overlayShown = false;

    SDL_GPUShader *Shader(SDL_GPUShaderStage stage, const unsigned char *spirv, size_t spirvSize, const char *msl,
                          uint32_t samplers, uint32_t uniforms);
    SDL_GPUTexture *Texture(SDL_GPUTextureFormat format, uint32_t width, uint32_t height, bool target);
    void Release(SDL_GPUTexture *&texture);
    bool Prepare(const Frame &frame);
    bool Download(SDL_GPUTexture *texture, uint32_t width, uint32_t height, uint32_t bytesPerPixel, void *out);
};

} // namespace satrecomp::video
