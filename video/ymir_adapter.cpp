// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
#include "ymir_adapter.h"

#include <algorithm>

namespace satrecomp::video {

using namespace ymir;
using namespace ymir::vdp;

namespace {
    Memory MemoryOf(VDPState &state) {
        Memory memory;
        memory.vram1 = state.mem1.VRAM.data();
        memory.fb[0] = state.mem1.FBRAM[0].data();
        memory.fb[1] = state.mem1.FBRAM[1].data();
        memory.vram2 = state.mem2.VRAM.data();
        memory.cram = state.mem2.CRAM.data();
        return memory;
    }
} // namespace

YmirAdapter::YmirAdapter(VDPState &state, const config::VDP2DebugRender &debug, const config::VDP2AccessPatternsConfig &access)
    : IVDPRenderer(VDPRendererType::External)
    , m_state(state)
    , m_reference(std::make_unique<SoftwareVDPRenderer>(state, debug, access))
    , m_access(access)
    , m_core(MemoryOf(state)) {}

util::ObjectResult<YmirAdapter> YmirAdapter::Create(VDPState &state, const config::VDP2DebugRender &debug,
                                                    const config::VDP2AccessPatternsConfig &access) {
    return std::make_unique<YmirAdapter>(state, debug, access);
}

void YmirAdapter::SetMode(Renderer::Mode mode, CBSoftwareFrameComplete onFrame) {
    using Mode = Renderer::Mode;
    m_compose = mode != Mode::Reference;
    m_check = mode == Mode::Check;
    m_gpu = mode == Mode::Gpu;
    m_core.SetMode(mode, [onFrame](uint32 *pixels, uint32 width, uint32 height) { onFrame(pixels, width, height); },
                   m_check ? static_cast<video::Reference *>(this) : nullptr);
    if (!m_compose) {
        m_reference->SwCallbacks.FrameComplete = onFrame;
        return;
    }
    // (in a check the reference draws here and now: its framebuffer and layers are read as they are)
    m_reference->EnableThreadedVDP1(false);
    m_reference->EnableThreadedVDP2(false);
    m_reference->SwCallbacks.FrameComplete = {};
}

// --- the reference's output, for the renderer's comparisons ---

void YmirAdapter::LayerLine(uint32 layer, uint32 width, uint32 *color, uint8 *priority) {
    const auto &pixels = m_reference->m_layerOutputs[0][layer].pixels;
    for (uint32 x = 0; x < width; x++) {
        color[x] = pixels.color[x].u32;
        priority[x] = pixels.priority[x];
    }
}
void YmirAdapter::SpriteLine(uint32 width, uint8 *special, uint8 *shadowOrWindow) {
    const auto &attrs = m_reference->m_spriteLayerAttrs[0];
    for (uint32 x = 0; x < width; x++) {
        // (SpriteData::Special and video::SpriteSpecial have the same values)
        special[x] = static_cast<uint8>(attrs.specialType[x]);
        shadowOrWindow[x] = attrs.shadowOrWindow[x];
    }
}
const uint32 *YmirAdapter::Picture() {
    return m_reference->m_framebuffer.data();
}
static_assert(static_cast<int>(SpriteData::Special::Normal) == static_cast<int>(SpriteSpecial::Normal) &&
              static_cast<int>(SpriteData::Special::Shadow) == static_cast<int>(SpriteSpecial::Shadow) &&
              static_cast<int>(SpriteData::Special::Transparent) == static_cast<int>(SpriteSpecial::Transparent));

// --- IVDPRenderer ---

bool YmirAdapter::IsValid() const {
    return m_reference->IsValid();
}
bool YmirAdapter::IsHardwareRenderer() const {
    return false;
}
void YmirAdapter::Reset(bool hard) {
    m_reference->Callbacks = Callbacks;
    m_reference->Reset(hard);
    m_core.Reset(hard);
}
void YmirAdapter::UpdateEnhancements() {
    m_reference->ConfigureEnhancements(m_enhancements);
}
void YmirAdapter::PreSaveStateSync() {
    m_reference->PreSaveStateSync();
}
void YmirAdapter::PostLoadStateSync() {
    m_reference->PostLoadStateSync();
}
void YmirAdapter::SaveState(savestate::VDPSaveState::VDPRendererSaveState &state) {
    m_reference->SaveState(state);
}
bool YmirAdapter::ValidateState(const savestate::VDPSaveState::VDPRendererSaveState &state) const {
    return m_reference->ValidateState(state);
}
void YmirAdapter::LoadState(const savestate::VDPSaveState::VDPRendererSaveState &state) {
    m_reference->LoadState(state);
}
void YmirAdapter::VDP1WriteVRAM(uint32 address, uint8 value) {
    m_core.Vdp1VramWritten();
    m_reference->VDP1WriteVRAM(address, value);
}
void YmirAdapter::VDP1WriteVRAM(uint32 address, uint16 value) {
    m_core.Vdp1VramWritten();
    m_reference->VDP1WriteVRAM(address, value);
}
void YmirAdapter::VDP1SyncFB() {
    m_reference->VDP1SyncFB();
}
void YmirAdapter::VDP1DebugSyncFB() {
    m_reference->VDP1DebugSyncFB();
}
void YmirAdapter::VDP1WriteFB(uint32 address, uint8 value) {
    m_reference->VDP1WriteFB(address, value);
}
void YmirAdapter::VDP1WriteFB(uint32 address, uint16 value) {
    m_reference->VDP1WriteFB(address, value);
}
void YmirAdapter::VDP1WriteReg(uint32 address, uint16 value) {
    m_core.WriteVdp1Reg(address, value);
    m_reference->VDP1WriteReg(address, value);
}
void YmirAdapter::VDP2WriteVRAM(uint32 address, uint8 value) {
    m_reference->VDP2WriteVRAM(address, value);
}
void YmirAdapter::VDP2WriteVRAM(uint32 address, uint16 value) {
    m_reference->VDP2WriteVRAM(address, value);
}
void YmirAdapter::VDP2WriteCRAM(uint32 address, uint8 value) {
    m_reference->VDP2WriteCRAM(address, value);
}
void YmirAdapter::VDP2WriteCRAM(uint32 address, uint16 value) {
    m_reference->VDP2WriteCRAM(address, value);
}
void YmirAdapter::VDP2WriteReg(uint32 address, uint16 value) {
    m_core.WriteVdp2Reg(address, value);
    m_reference->VDP2WriteReg(address, value);
}

void YmirAdapter::VDP1EraseFramebuffer(uint64 cycles) {
    m_core.EraseFramebuffer(cycles);
    if (ReferenceDrawsVdp1()) {
        m_reference->VDP1EraseFramebuffer(cycles);
    }
}
void YmirAdapter::VDP1SwapFramebuffer() {
    m_core.SwapFramebuffer();
    if (ReferenceDrawsVdp1()) {
        m_reference->VDP1SwapFramebuffer(); // (calls the hardware model back itself)
    } else {
        Callbacks.VDP1FramebufferSwap();
    }
}
void YmirAdapter::VDP1BeginFrame() {
    // (the hardware model sets the callbacks on this object; the reference must call the same ones)
    m_reference->Callbacks = Callbacks;
    m_core.BeginDrawing();
    if (ReferenceDrawsVdp1()) {
        m_reference->VDP1BeginFrame();
    }
}
void YmirAdapter::VDP1ExecuteCommand(uint32 cmdAddress, VDP1Command::Control control) {
    m_core.DrawCommand(cmdAddress);
    if (ReferenceDrawsVdp1()) {
        m_reference->VDP1ExecuteCommand(cmdAddress, control);
    }
}
void YmirAdapter::VDP1EndFrame() {
    m_core.EndDrawing();
    if (ReferenceDrawsVdp1()) {
        m_reference->VDP1EndFrame();
    } else {
        Callbacks.VDP1DrawFinished();
    }
}

void YmirAdapter::VDP2SetResolution(uint32 h, uint32 v, bool exclusive) {
    m_reference->VDP2SetResolution(h, v, exclusive);
    m_core.SetResolution(h, v, exclusive);
}
void YmirAdapter::VDP2SetField(bool odd) {
    m_reference->VDP2SetField(odd);
    m_core.SetField(odd);
}
void YmirAdapter::VDP2LatchTVMD() {
    m_reference->VDP2LatchTVMD();
    m_core.LatchDisplayMode();
}
void YmirAdapter::VDP2BeginFrame() {
    m_reference->Callbacks = Callbacks;
    m_reference->VDP2BeginFrame();
    m_core.BeginFrame();
}
void YmirAdapter::VDP2RenderLine(uint32 y) {
    if (!m_compose || m_check) {
        m_reference->VDP2RenderLine(y);
    } else if (m_state.regs2.displayEnabledLatch) {
        // What the hardware model expects of its renderer on every line: the VRAM access cycle's effects worked
        // out, the rotation parameters' "written" flags taken, the rotation screens' map addresses
        VDP2Regs &regs2 = m_state.regs2;
        m_state.state2.CalcAccessPatterns(regs2, m_access);
        m_state.state2.CalcVCellScrollDelay(regs2);
        if (regs2.bgEnabled[4] || regs2.bgEnabled[5]) {
            for (ymir::vdp::RotationParams &params : regs2.rotParams) {
                params.readXst = params.readYst = params.readKAst = false;
            }
        }
        m_state.state2.UpdateRotationPageBaseAddresses(regs2);
    }
    m_core.RenderLine(y);
}
void YmirAdapter::VDP2EndFrame() {
    if (!m_compose || m_check) {
        m_reference->VDP2EndFrame();
    } else {
        Callbacks.VDP2DrawFinished();
    }
    m_core.EndFrame();
}
void YmirAdapter::UpdateEnabledLayers() {
    m_reference->UpdateEnabledLayers();
}
void YmirAdapter::DumpExtraVDP1Framebuffers(std::ostream &out) const {
    m_reference->DumpExtraVDP1Framebuffers(out);
}

} // namespace satrecomp::video
