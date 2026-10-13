// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// The video renderer (renderer.h) connected to the Ymir hardware model: Ymir's video chips tell a renderer
// what to draw through IVDPRenderer, and this passes that on. It also owns Ymir's own software renderer, as
// the reference the renderer's output can be compared with (SATRECOMP_VIDEO=check).
//
// This file and its .cpp are the only video code that knows Ymir.
#pragma once

#include <ymir/hw/vdp/renderer/vdp_renderer_base.hpp>
#include <ymir/hw/vdp/renderer/vdp_renderer_sw.hpp>

#include "renderer.h"

#include <memory>

namespace satrecomp::video {

class YmirAdapter final : public ymir::vdp::IVDPRenderer, private Reference {
public:
    YmirAdapter(ymir::vdp::VDPState &state, const ymir::vdp::config::VDP2DebugRender &debug,
                const ymir::vdp::config::VDP2AccessPatternsConfig &access);

    static ::util::ObjectResult<YmirAdapter> Create(ymir::vdp::VDPState &state, const ymir::vdp::config::VDP2DebugRender &debug,
                                                    const ymir::vdp::config::VDP2AccessPatternsConfig &access);

    Renderer &Core() {
        return m_core;
    }
    // Ymir's software renderer (for its frame callback and threading options)
    ymir::vdp::SoftwareVDPRenderer &Reference() {
        return *m_reference;
    }

    // Renderer::SetMode. Mode::Reference: every call goes to Ymir's renderer; Mode::Check: to both.
    void SetMode(Renderer::Mode mode, ymir::vdp::CBSoftwareFrameComplete onFrame);
    // For tests of a GPU backend: Ymir's renderer rasterises the VDP1's commands as well
    void SetGpuCheck(bool on) {
        m_gpuCheck = on;
        m_core.SetGpuCheck(on);
    }

    bool IsValid() const override;
    bool IsHardwareRenderer() const override;
    void Reset(bool hard) override;
    void PreSaveStateSync() override;
    void PostLoadStateSync() override;
    void SaveState(ymir::savestate::VDPSaveState::VDPRendererSaveState &state) override;
    bool ValidateState(const ymir::savestate::VDPSaveState::VDPRendererSaveState &state) const override;
    void LoadState(const ymir::savestate::VDPSaveState::VDPRendererSaveState &state) override;
    void VDP1WriteVRAM(uint32 address, uint8 value) override;
    void VDP1WriteVRAM(uint32 address, uint16 value) override;
    void VDP1SyncFB() override;
    void VDP1DebugSyncFB() override;
    void VDP1WriteFB(uint32 address, uint8 value) override;
    void VDP1WriteFB(uint32 address, uint16 value) override;
    void VDP1WriteReg(uint32 address, uint16 value) override;
    void VDP2WriteVRAM(uint32 address, uint8 value) override;
    void VDP2WriteVRAM(uint32 address, uint16 value) override;
    void VDP2WriteCRAM(uint32 address, uint8 value) override;
    void VDP2WriteCRAM(uint32 address, uint16 value) override;
    void VDP2WriteReg(uint32 address, uint16 value) override;
    void VDP1EraseFramebuffer(uint64 cycles = 0) override;
    void VDP1SwapFramebuffer() override;
    void VDP1BeginFrame() override;
    void VDP1ExecuteCommand(uint32 cmdAddress, ymir::vdp::VDP1Command::Control control) override;
    void VDP1EndFrame() override;
    void VDP2SetResolution(uint32 h, uint32 v, bool exclusive) override;
    void VDP2SetField(bool odd) override;
    void VDP2LatchTVMD() override;
    void VDP2BeginFrame() override;
    void VDP2RenderLine(uint32 y) override;
    void VDP2EndFrame() override;
    void UpdateEnabledLayers() override;
    void DumpExtraVDP1Framebuffers(std::ostream &out) const override;

protected:
    void UpdateEnhancements() override;

private:
    // video::Reference: Ymir's renderer's lines and picture
    void LayerLine(uint32 layer, uint32 width, uint32 *color, uint8 *priority) override;
    void SpriteLine(uint32 width, uint8 *special, uint8 *shadowOrWindow) override;
    const uint32 *Picture() override;

    // Whether Ymir's renderer draws the VDP1's side too
    bool ReferenceDrawsVdp1() const {
        return m_gpu ? m_gpuCheck : (!m_compose || m_check);
    }

    ymir::vdp::VDPState &m_state;
    std::unique_ptr<ymir::vdp::SoftwareVDPRenderer> m_reference;
    const ymir::vdp::config::VDP2AccessPatternsConfig &m_access;
    Renderer m_core;
    bool m_compose = false, m_check = false, m_gpu = false, m_gpuCheck = false;
};

} // namespace satrecomp::video
