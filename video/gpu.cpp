// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
#include "gpu.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace satrecomp::video {

namespace {
#include "shaders.inc"

    struct Vertex {
        float position[2];
        float ab[4], cd[4];
        float clip[4], clipOut[4];
        uint32_t tex[4];
        uint32_t gouraud[4];
    };

    struct ComposeUniforms {
        float size[4];
        int32_t sprite[4];
        int32_t priLow[4], priHigh[4];
        int32_t misc[4];
        float colorOffset[7][4];
        int32_t floor[4];       // lines of a field, dots a line, flags, priority
        int32_t floorMore[4];   // colour offset, more character number bits, more palette bits, access
        uint32_t floorPlanes[16];
    };

    constexpr uint32_t kVramWidth = 1024, kVramHeight = 512;
} // namespace

SDL_GPUShader *Gpu::Shader(SDL_GPUShaderStage stage, const unsigned char *spirv, size_t spirvSize, const char *msl,
                           uint32_t samplers, uint32_t uniforms) {
    SDL_GPUShaderCreateInfo info{};
    info.stage = stage;
    info.num_samplers = samplers;
    info.num_uniform_buffers = uniforms;
    if (SDL_GetGPUShaderFormats(m_device) & SDL_GPU_SHADERFORMAT_MSL) {
        info.format = SDL_GPU_SHADERFORMAT_MSL;
        info.code = reinterpret_cast<const Uint8 *>(msl);
        info.code_size = strlen(msl);
        info.entrypoint = "main0";
    } else {
        info.format = SDL_GPU_SHADERFORMAT_SPIRV;
        info.code = spirv;
        info.code_size = spirvSize;
        info.entrypoint = "main";
    }
    SDL_GPUShader *shader = SDL_CreateGPUShader(m_device, &info);
    if (!shader) {
        fprintf(stderr, "video: shader: %s\n", SDL_GetError());
    }
    return shader;
}

SDL_GPUTexture *Gpu::Texture(SDL_GPUTextureFormat format, uint32_t width, uint32_t height, bool target) {
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | (target ? SDL_GPU_TEXTUREUSAGE_COLOR_TARGET : 0);
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    SDL_GPUTexture *texture = SDL_CreateGPUTexture(m_device, &info);
    if (!texture) {
        fprintf(stderr, "video: texture %ux%u: %s\n", width, height, SDL_GetError());
    }
    return texture;
}

void Gpu::Release(SDL_GPUTexture *&texture) {
    if (texture) {
        SDL_ReleaseGPUTexture(m_device, texture);
        texture = nullptr;
    }
}

#define SATRECOMP_SHADER(stage, name, samplers, uniforms) \
    Shader(stage, k_##name##_spirv, sizeof(k_##name##_spirv), k_##name##_msl, samplers, uniforms)

bool Gpu::Init(SDL_GPUDevice *device) {
    m_device = device;
    SDL_GPUShader *quadVertex = SATRECOMP_SHADER(SDL_GPU_SHADERSTAGE_VERTEX, quad_vert, 0, 1);
    SDL_GPUShader *quadFragment = SATRECOMP_SHADER(SDL_GPU_SHADERSTAGE_FRAGMENT, quad_frag, 1, 1);
    SDL_GPUShader *screenVertex = SATRECOMP_SHADER(SDL_GPU_SHADERSTAGE_VERTEX, screen_vert, 0, 0);
    SDL_GPUShader *composeFragment = SATRECOMP_SHADER(SDL_GPU_SHADERSTAGE_FRAGMENT, compose_frag, 11, 1);
    SDL_GPUShader *hudFragment = SATRECOMP_SHADER(SDL_GPU_SHADERSTAGE_FRAGMENT, hud_frag, 1, 0);
    if (!quadVertex || !quadFragment || !screenVertex || !composeFragment) {
        return false;
    }
    if (hudFragment) {
        // The readout: a picture blended over the output by its alpha
        SDL_GPUColorTargetDescription target{};
        target.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        target.blend_state.enable_blend = true;
        target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = screenVertex;
        info.fragment_shader = hudFragment;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.target_info.color_target_descriptions = &target;
        info.target_info.num_color_targets = 1;
        m_hudPipeline = SDL_CreateGPUGraphicsPipeline(m_device, &info);
        SDL_ReleaseGPUShader(m_device, hudFragment);
        SDL_GPUSamplerCreateInfo sampler{};
        sampler.min_filter = sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
        sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        m_linear = SDL_CreateGPUSampler(m_device, &sampler);
    }

    {
        SDL_GPUVertexBufferDescription buffer{};
        buffer.slot = 0;
        buffer.pitch = sizeof(Vertex);
        buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        const SDL_GPUVertexAttribute attributes[] = {
            {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Vertex, position)},
            {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(Vertex, ab)},
            {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(Vertex, cd)},
            {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(Vertex, clip)},
            {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(Vertex, clipOut)},
            {5, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT4, offsetof(Vertex, tex)},
            {6, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT4, offsetof(Vertex, gouraud)},
        };
        SDL_GPUColorTargetDescription target{};
        target.format = SDL_GPU_TEXTUREFORMAT_R16_UINT;
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = quadVertex;
        info.fragment_shader = quadFragment;
        info.vertex_input_state.vertex_buffer_descriptions = &buffer;
        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_attributes = attributes;
        info.vertex_input_state.num_vertex_attributes = static_cast<Uint32>(SDL_arraysize(attributes));
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.target_info.color_target_descriptions = &target;
        info.target_info.num_color_targets = 1;
        m_quadPipeline = SDL_CreateGPUGraphicsPipeline(m_device, &info);
    }
    {
        SDL_GPUColorTargetDescription target{};
        target.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = screenVertex;
        info.fragment_shader = composeFragment;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.target_info.color_target_descriptions = &target;
        info.target_info.num_color_targets = 1;
        m_composePipeline = SDL_CreateGPUGraphicsPipeline(m_device, &info);
    }
    SDL_ReleaseGPUShader(m_device, quadVertex);
    SDL_ReleaseGPUShader(m_device, quadFragment);
    SDL_ReleaseGPUShader(m_device, screenVertex);
    SDL_ReleaseGPUShader(m_device, composeFragment);
    if (!m_quadPipeline || !m_composePipeline) {
        fprintf(stderr, "video: pipeline: %s\n", SDL_GetError());
        return false;
    }

    SDL_GPUSamplerCreateInfo sampler{};
    sampler.min_filter = sampler.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    m_nearest = SDL_CreateGPUSampler(m_device, &sampler);

    m_vram = Texture(SDL_GPU_TEXTUREFORMAT_R8_UINT, kVramWidth, kVramHeight, false);
    m_colors = Texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 2048, 1, false);
    m_back = Texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 1, 512, false);
    m_vram2 = Texture(SDL_GPU_TEXTUREFORMAT_R8_UINT, kVramWidth, kVramHeight, false);
    m_floorLines = Texture(SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT, 2, 512, false);
    return m_nearest && m_vram && m_colors && m_back && m_vram2 && m_floorLines;
}

void Gpu::Shutdown() {
    if (!m_device) {
        return;
    }
    SDL_WaitForGPUIdle(m_device);
    for (auto *&texture : m_sprites) {
        Release(texture);
    }
    for (auto *&texture : m_keptSprites) {
        Release(texture);
    }
    for (auto *&texture : m_layers) {
        Release(texture);
    }
    Release(m_vram);
    Release(m_colors);
    Release(m_back);
    Release(m_window);
    Release(m_vram2);
    Release(m_floorLines);
    Release(m_output);
    Release(m_hud);
    Release(m_plain);
    if (m_hudPipeline) {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_hudPipeline);
        m_hudPipeline = nullptr;
    }
    if (m_linear) {
        SDL_ReleaseGPUSampler(m_device, m_linear);
        m_linear = nullptr;
    }
    if (m_vertices) {
        SDL_ReleaseGPUBuffer(m_device, m_vertices);
    }
    if (m_transfer) {
        SDL_ReleaseGPUTransferBuffer(m_device, m_transfer);
    }
    if (m_download) {
        SDL_ReleaseGPUTransferBuffer(m_device, m_download);
    }
    if (m_nearest) {
        SDL_ReleaseGPUSampler(m_device, m_nearest);
    }
    if (m_quadPipeline) {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_quadPipeline);
    }
    if (m_composePipeline) {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_composePipeline);
    }
    m_device = nullptr;
}

void Gpu::SetOutputSize(uint32_t width, uint32_t height) {
    m_wantedWidth = std::max<uint32_t>(width, 16);
    m_wantedHeight = std::max<uint32_t>(height, 16);
}

void Gpu::CopySprites(SDL_GPUTexture *const from[2], SDL_GPUTexture *const to[2]) {
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(m_device);
    SDL_GPUCopyPass *pass = SDL_BeginGPUCopyPass(cmd);
    for (int i = 0; i < 2; i++) {
        SDL_GPUTextureLocation source{}, destination{};
        source.texture = from[i];
        destination.texture = to[i];
        SDL_CopyGPUTextureToTexture(pass, &source, &destination, m_spriteWidth * m_scale, m_spriteHeight * m_scale, 1, false);
    }
    SDL_EndGPUCopyPass(pass);
    SDL_SubmitGPUCommandBuffer(cmd);
}

void Gpu::KeepSprites() {
    if (!m_device || !m_sprites[0]) {
        return;
    }
    if (!m_keptSprites[0] || m_keptWidth != m_spriteWidth || m_keptHeight != m_spriteHeight || m_keptScale != m_scale) {
        for (auto *&texture : m_keptSprites) {
            Release(texture);
            texture = Texture(SDL_GPU_TEXTUREFORMAT_R16_UINT, m_spriteWidth * m_scale, m_spriteHeight * m_scale, true);
        }
        if (!m_keptSprites[0] || !m_keptSprites[1]) {
            return;
        }
    }
    m_keptWidth = m_spriteWidth;
    m_keptHeight = m_spriteHeight;
    m_keptScale = m_scale;
    CopySprites(m_sprites, m_keptSprites);
}

void Gpu::PutSpritesBack() {
    // (not when the frames run ahead changed the framebuffers' size: those are new, and what is drawn next
    // fills them)
    if (!m_device || !m_sprites[0] || !m_keptSprites[0] || m_keptWidth != m_spriteWidth || m_keptHeight != m_spriteHeight ||
        m_keptScale != m_scale) {
        return;
    }
    CopySprites(m_keptSprites, m_sprites);
}

// Textures and buffers of the sizes this frame needs
bool Gpu::Prepare(const Frame &frame) {
    // The VDP1's targets: enough resolution for the output (twice over, so edges can be smoothed), at most 8x
    uint32_t scale = m_fixedScale;
    if (scale == 0) {
        scale = (m_wantedHeight * 2 + frame.spriteHeight - 1) / frame.spriteHeight;
    }
    scale = std::clamp<uint32_t>(scale, 1, 8);
    if (!m_sprites[0] || frame.spriteWidth != m_spriteWidth || frame.spriteHeight != m_spriteHeight || scale != m_scale) {
        m_spriteWidth = frame.spriteWidth;
        m_spriteHeight = frame.spriteHeight;
        m_scale = scale;
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(m_device);
        for (auto *&texture : m_sprites) {
            Release(texture);
            texture = Texture(SDL_GPU_TEXTUREFORMAT_R16_UINT, m_spriteWidth * scale, m_spriteHeight * scale, true);
            if (!texture) {
                return false;
            }
            SDL_GPUColorTargetInfo target{};
            target.texture = texture;
            target.load_op = SDL_GPU_LOADOP_CLEAR;
            target.store_op = SDL_GPU_STOREOP_STORE;
            SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, &target, 1, nullptr));
        }
        SDL_SubmitGPUCommandBuffer(cmd);
    }
    if (!m_layers[0] || frame.width != m_layerWidth || frame.height != m_layerHeight) {
        m_layerWidth = frame.width;
        m_layerHeight = frame.height;
        for (auto *&texture : m_layers) {
            Release(texture);
            texture = Texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, m_layerWidth, m_layerHeight, false);
        }
        Release(m_window);
        m_window = Texture(SDL_GPU_TEXTUREFORMAT_R8_UNORM, m_layerWidth, m_layerHeight, false);
        if (!m_layers[4] || !m_window) {
            return false;
        }
    }
    if (!m_output || m_outputWidth != m_wantedWidth || m_outputHeight != m_wantedHeight) {
        Release(m_output);
        m_outputWidth = m_wantedWidth;
        m_outputHeight = m_wantedHeight;
        m_output = Texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, m_outputWidth, m_outputHeight, true);
        if (!m_output) {
            return false;
        }
    }
    return true;
}

void Gpu::Draw(const Frame &frame) {
    if (!m_device || !frame.width || !frame.height || !Prepare(frame)) {
        return;
    }
    m_hudApplied = false;
    if (frame.resetSprites) {
        SDL_GPUCommandBuffer *clear = SDL_AcquireGPUCommandBuffer(m_device);
        for (SDL_GPUTexture *texture : m_sprites) {
            SDL_GPUColorTargetInfo target{};
            target.texture = texture;
            target.load_op = SDL_GPU_LOADOP_CLEAR;
            target.store_op = SDL_GPU_STOREOP_STORE;
            SDL_EndGPURenderPass(SDL_BeginGPURenderPass(clear, &target, 1, nullptr));
        }
        SDL_SubmitGPUCommandBuffer(clear);
    }
    const uint32_t layerBytes = frame.width * frame.height * 4;
    const uint32_t vertexBytes = static_cast<uint32_t>(frame.quads.size() * 6 * sizeof(Vertex));
    const uint32_t vramBytes = kVramWidth * kVramHeight;
    const uint32_t backRows = std::min<uint32_t>(frame.height, 512);

    // Everything to upload goes through one transfer buffer: [vertices][VRAM snapshots][layers][window][back][colours]
    uint32_t offset = 0;
    auto reserve = [&](uint32_t size) {
        const uint32_t at = offset;
        offset += (size + 15) & ~15u;
        return at;
    };
    const uint32_t vertexAt = reserve(vertexBytes);
    std::vector<uint32_t> vramAt;
    for (size_t i = 0; i < frame.vram.size(); i++) {
        vramAt.push_back(reserve(vramBytes));
    }
    uint32_t layerAt[5];
    for (uint32_t &at : layerAt) {
        at = reserve(layerBytes);
    }
    const uint32_t windowAt = reserve(frame.width * frame.height);
    const uint32_t backAt = reserve(512 * 4);
    const uint32_t colorsAt = reserve(2048 * 4);
    const uint32_t vram2At = reserve(vramBytes);
    const uint32_t floorAt = reserve(512 * 8 * sizeof(float));
    if (offset > m_transferCapacity) {
        if (m_transfer) {
            SDL_ReleaseGPUTransferBuffer(m_device, m_transfer);
        }
        m_transferCapacity = offset + offset / 2;
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = m_transferCapacity;
        m_transfer = SDL_CreateGPUTransferBuffer(m_device, &info);
    }
    if (vertexBytes > m_vertexCapacity) {
        if (m_vertices) {
            SDL_ReleaseGPUBuffer(m_device, m_vertices);
        }
        m_vertexCapacity = std::max<uint32_t>(vertexBytes * 2, 1024 * 6 * sizeof(Vertex));
        SDL_GPUBufferCreateInfo info{};
        info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        info.size = m_vertexCapacity;
        m_vertices = SDL_CreateGPUBuffer(m_device, &info);
    }
    auto *bytes = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(m_device, m_transfer, true));
    if (!bytes) {
        return;
    }

    // Each quad as the rectangle around it (two triangles), no further than its clipping rectangle
    auto *vertex = reinterpret_cast<Vertex *>(bytes + vertexAt);
    for (const Quad &quad : frame.quads) {
        float x0 = quad.corner[0][0], x1 = x0, y0 = quad.corner[0][1], y1 = y0;
        for (int i = 1; i < 4; i++) {
            x0 = std::min(x0, quad.corner[i][0]);
            x1 = std::max(x1, quad.corner[i][0]);
            y0 = std::min(y0, quad.corner[i][1]);
            y1 = std::max(y1, quad.corner[i][1]);
        }
        x0 = std::max(x0, quad.clip[0]);
        y0 = std::max(y0, quad.clip[1]);
        x1 = std::max(x0, std::min(x1, quad.clip[2]));
        y1 = std::max(y0, std::min(y1, quad.clip[3]));
        Vertex v{};
        memcpy(v.ab, quad.corner[0], sizeof(float) * 4);
        memcpy(v.cd, quad.corner[2], sizeof(float) * 4);
        memcpy(v.clip, quad.clip, sizeof(v.clip));
        memcpy(v.clipOut, quad.clipOut, sizeof(v.clipOut));
        v.tex[0] = quad.texture;
        v.tex[1] = quad.width | (static_cast<uint32_t>(quad.height) << 10) | (static_cast<uint32_t>(quad.lines) << 18);
        v.tex[2] = quad.mode | (static_cast<uint32_t>(quad.flags) << 16);
        v.tex[3] = quad.color;
        for (int i = 0; i < 4; i++) {
            v.gouraud[i] = quad.gouraud[i];
        }
        const float rect[6][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y0}, {x1, y1}, {x0, y1}};
        for (const auto &corner : rect) {
            v.position[0] = corner[0];
            v.position[1] = corner[1];
            *vertex++ = v;
        }
    }
    for (size_t i = 0; i < frame.vram.size(); i++) {
        memcpy(bytes + vramAt[i], frame.vram[i].data(), std::min<size_t>(vramBytes, frame.vram[i].size()));
    }
    for (int i = 0; i < 5; i++) {
        memcpy(bytes + layerAt[i], frame.layers[i], layerBytes);
    }
    for (uint32_t i = 0; i < frame.width * frame.height; i++) {
        bytes[windowAt + i] = frame.spriteWindow[i] ? 255 : 0;
    }
    memcpy(bytes + backAt, frame.back, backRows * 4);
    memcpy(bytes + colorsAt, frame.colors, 2048 * 4);
    memcpy(bytes + vram2At, frame.floor.vram, vramBytes);
    memcpy(bytes + floorAt, frame.floor.lines, 512 * 8 * sizeof(float));
    SDL_UnmapGPUTransferBuffer(m_device, m_transfer);

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(m_device);
    auto uploadTexture = [&](SDL_GPUCopyPass *pass, SDL_GPUTexture *texture, uint32_t at, uint32_t width, uint32_t height) {
        SDL_GPUTextureTransferInfo source{};
        source.transfer_buffer = m_transfer;
        source.offset = at;
        source.pixels_per_row = width;
        source.rows_per_layer = height;
        SDL_GPUTextureRegion region{};
        region.texture = texture;
        region.w = width;
        region.h = height;
        region.d = 1;
        SDL_UploadToGPUTexture(pass, &source, &region, true);
    };
    {
        SDL_GPUCopyPass *pass = SDL_BeginGPUCopyPass(cmd);
        if (vertexBytes) {
            SDL_GPUTransferBufferLocation source{m_transfer, vertexAt};
            SDL_GPUBufferRegion region{m_vertices, 0, vertexBytes};
            SDL_UploadToGPUBuffer(pass, &source, &region, true);
        }
        for (int i = 0; i < 5; i++) {
            uploadTexture(pass, m_layers[i], layerAt[i], frame.width, frame.height);
        }
        uploadTexture(pass, m_window, windowAt, frame.width, frame.height);
        uploadTexture(pass, m_back, backAt, 1, 512);
        uploadTexture(pass, m_colors, colorsAt, 2048, 1);
        uploadTexture(pass, m_vram2, vram2At, kVramWidth, kVramHeight);
        uploadTexture(pass, m_floorLines, floorAt, 2, 512);
        SDL_EndGPUCopyPass(pass);
    }

    // The VDP1's work, in the order it happened
    const float vertexUniforms[4] = {static_cast<float>(m_spriteWidth), static_cast<float>(m_spriteHeight), 0, 0};
    const float fragmentUniforms[4] = {static_cast<float>(m_scale), 0, 0, 0};
    for (const Vdp1Event &event : frame.events) {
        if (event.kind == Vdp1Event::Kind::Swap) {
            std::swap(m_sprites[0], m_sprites[1]);
            continue;
        }
        const bool erase = event.kind == Vdp1Event::Kind::Erase;
        if (!erase) {
            SDL_GPUCopyPass *pass = SDL_BeginGPUCopyPass(cmd);
            uploadTexture(pass, m_vram, vramAt[event.vram], kVramWidth, kVramHeight);
            SDL_EndGPUCopyPass(pass);
        }
        if (!event.quadCount) {
            continue;
        }
        SDL_GPUColorTargetInfo target{};
        target.texture = m_sprites[erase ? 0 : 1];
        target.load_op = SDL_GPU_LOADOP_LOAD;
        target.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(pass, m_quadPipeline);
        const SDL_GPUBufferBinding binding{m_vertices, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
        const SDL_GPUTextureSamplerBinding sampler{m_vram, m_nearest};
        SDL_BindGPUFragmentSamplers(pass, 0, &sampler, 1);
        SDL_PushGPUVertexUniformData(cmd, 0, vertexUniforms, sizeof(vertexUniforms));
        SDL_PushGPUFragmentUniformData(cmd, 0, fragmentUniforms, sizeof(fragmentUniforms));
        SDL_DrawGPUPrimitives(pass, event.quadCount * 6, 1, event.firstQuad * 6, 0);
        SDL_EndGPURenderPass(pass);
    }

    // The picture
    {
        ComposeUniforms uniforms{};
        uniforms.size[0] = static_cast<float>(frame.width);
        uniforms.size[1] = static_cast<float>(frame.height);
        uniforms.size[2] = static_cast<float>(m_spriteWidth * m_scale);
        uniforms.size[3] = static_cast<float>(m_spriteHeight * m_scale);
        uniforms.sprite[0] = static_cast<int32_t>(frame.spriteType);
        uniforms.sprite[1] = frame.spriteMixed;
        uniforms.sprite[2] = (frame.spriteWindowUsed ? 1 : 0) | (frame.spriteWindowEnabled ? 2 : 0) |
                             (frame.spriteWindowInverted ? 4 : 0) | (frame.spritesEnabled ? 8 : 0);
        uniforms.sprite[3] = static_cast<int32_t>(frame.spriteColorOffset);
        for (int i = 0; i < 4; i++) {
            uniforms.priLow[i] = frame.spritePriorities[i];
            uniforms.priHigh[i] = frame.spritePriorities[i + 4];
        }
        for (int i = 0; i < 7; i++) {
            uniforms.misc[0] |= frame.shadow[i] ? 1 << i : 0;
        }
        for (int i = 0; i < 7; i++) {
            for (int c = 0; c < 3; c++) {
                uniforms.colorOffset[i][c] = frame.colorOffset[i][c] / 255.0f;
            }
        }
        const Frame::Floor &floor = frame.floor;
        uniforms.floor[0] = static_cast<int32_t>(floor.lineCount);
        uniforms.floor[1] = static_cast<int32_t>(floor.dots);
        uniforms.floor[2] = (floor.fourCell ? 1 : 0) | (floor.twoWord ? 2 : 0) | (floor.extended ? 4 : 0) | (floor.colors256 ? 8 : 0) |
                            (floor.transparency ? 16 : 0) | (floor.priorityPerCharacter ? 32 : 0) | (floor.specialPriority ? 64 : 0) |
                            (floor.over << 8) | (floor.pageShiftH << 12) | (floor.pageShiftV << 13);
        uniforms.floor[3] = floor.priority;
        uniforms.floorMore[0] = static_cast<int32_t>(floor.colorOffset);
        uniforms.floorMore[1] = static_cast<int32_t>(floor.moreNumber);
        uniforms.floorMore[2] = static_cast<int32_t>(floor.morePalette);
        uniforms.floorMore[3] = floor.access;
        memcpy(uniforms.floorPlanes, floor.planes, sizeof(uniforms.floorPlanes));
        // (several samples a pixel when the VDP1's target has more pixels than the output shows)
        const uint32_t samples = std::clamp<uint32_t>((m_spriteHeight * m_scale + m_outputHeight / 2) / m_outputHeight, 1, 4);
        uniforms.misc[1] = static_cast<int32_t>(samples);
        uniforms.misc[2] = static_cast<int32_t>(frame.blankSides);

        SDL_GPUColorTargetInfo target{};
        target.texture = m_output;
        target.load_op = SDL_GPU_LOADOP_DONT_CARE;
        target.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(pass, m_composePipeline);
        const SDL_GPUTextureSamplerBinding samplers[11] = {
            {m_sprites[0], m_nearest}, {m_colors, m_nearest},    {m_layers[0], m_nearest},  {m_layers[1], m_nearest},
            {m_layers[2], m_nearest},  {m_layers[3], m_nearest}, {m_layers[4], m_nearest},  {m_window, m_nearest},
            {m_back, m_nearest},       {m_vram2, m_nearest},     {m_floorLines, m_nearest},
        };
        SDL_BindGPUFragmentSamplers(pass, 0, samplers, 11);
        SDL_PushGPUFragmentUniformData(cmd, 0, &uniforms, sizeof(uniforms));
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    }
    SDL_SubmitGPUCommandBuffer(cmd);
}

bool Gpu::Download(SDL_GPUTexture *texture, uint32_t width, uint32_t height, uint32_t bytesPerPixel, void *out) {
    const uint32_t size = width * height * bytesPerPixel;
    if (size > m_downloadCapacity) {
        if (m_download) {
            SDL_ReleaseGPUTransferBuffer(m_device, m_download);
        }
        m_downloadCapacity = size;
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        info.size = size;
        m_download = SDL_CreateGPUTransferBuffer(m_device, &info);
    }
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(m_device);
    SDL_GPUCopyPass *pass = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion region{};
    region.texture = texture;
    region.w = width;
    region.h = height;
    region.d = 1;
    SDL_GPUTextureTransferInfo destination{};
    destination.transfer_buffer = m_download;
    destination.pixels_per_row = width;
    destination.rows_per_layer = height;
    SDL_DownloadFromGPUTexture(pass, &region, &destination);
    SDL_EndGPUCopyPass(pass);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (!fence) {
        return false;
    }
    SDL_WaitForGPUFences(m_device, true, &fence, 1);
    SDL_ReleaseGPUFence(m_device, fence);
    const void *data = SDL_MapGPUTransferBuffer(m_device, m_download, false);
    if (!data) {
        return false;
    }
    memcpy(out, data, size);
    SDL_UnmapGPUTransferBuffer(m_device, m_download);
    return true;
}

bool Gpu::Read(std::vector<uint32_t> &pixels) {
    if (!m_output) {
        return false;
    }
    pixels.resize(static_cast<size_t>(m_outputWidth) * m_outputHeight);
    return Download(m_output, m_outputWidth, m_outputHeight, 4, pixels.data());
}

bool Gpu::ReadSprites(std::vector<uint16_t> &words, uint32_t &width, uint32_t &height) {
    if (!m_sprites[0]) {
        return false;
    }
    width = m_spriteWidth * m_scale;
    height = m_spriteHeight * m_scale;
    words.resize(static_cast<size_t>(width) * height);
    return Download(m_sprites[0], width, height, 2, words.data());
}

void Gpu::SetOverlay(const uint32_t *pixels, uint32_t width, uint32_t height) {
    if (!m_device || !width || !height) {
        return;
    }
    if (!m_overlay || width != m_overlayWidth || height != m_overlayHeight) {
        Release(m_overlay);
        m_overlay = Texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, width, height, false);
        m_overlayWidth = width;
        m_overlayHeight = height;
        if (!m_overlay) {
            return;
        }
    }
    const uint32_t size = width * height * 4;
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    info.size = size;
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(m_device, &info);
    if (!transfer) {
        return;
    }
    if (void *bytes = SDL_MapGPUTransferBuffer(m_device, transfer, false)) {
        memcpy(bytes, pixels, size);
        SDL_UnmapGPUTransferBuffer(m_device, transfer);
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(m_device);
        SDL_GPUCopyPass *pass = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureTransferInfo source{};
        source.transfer_buffer = transfer;
        source.pixels_per_row = width;
        source.rows_per_layer = height;
        SDL_GPUTextureRegion region{};
        region.texture = m_overlay;
        region.w = width;
        region.h = height;
        region.d = 1;
        SDL_UploadToGPUTexture(pass, &source, &region, false);
        SDL_EndGPUCopyPass(pass);
        SDL_SubmitGPUCommandBuffer(cmd);
        m_overlayShown = true;
    }
    SDL_ReleaseGPUTransferBuffer(m_device, transfer);
}

void Gpu::SetHudLayer(const uint32_t *pixels, uint32_t width, uint32_t height) {
    if (!m_device || !m_hudPipeline || !width || !height) {
        return;
    }
    if (!m_hud || width != m_hudWidth || height != m_hudHeight) {
        Release(m_hud);
        m_hud = Texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, width, height, false);
        m_hudWidth = width;
        m_hudHeight = height;
        if (!m_hud) {
            m_hudShown = false;
            return;
        }
    }
    const uint32_t size = width * height * 4;
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    info.size = size;
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(m_device, &info);
    if (!transfer) {
        return;
    }
    if (void *bytes = SDL_MapGPUTransferBuffer(m_device, transfer, false)) {
        memcpy(bytes, pixels, size);
        SDL_UnmapGPUTransferBuffer(m_device, transfer);
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(m_device);
        SDL_GPUCopyPass *pass = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureTransferInfo source{};
        source.transfer_buffer = transfer;
        source.pixels_per_row = width;
        source.rows_per_layer = height;
        SDL_GPUTextureRegion region{};
        region.texture = m_hud;
        region.w = width;
        region.h = height;
        region.d = 1;
        SDL_UploadToGPUTexture(pass, &source, &region, false);
        SDL_EndGPUCopyPass(pass);
        SDL_SubmitGPUCommandBuffer(cmd);
        m_hudShown = true;
        m_hudChanged = true;
    }
    SDL_ReleaseGPUTransferBuffer(m_device, transfer);
}

// Blends the readout into the output: once for each picture drawn, and again when the readout changes
// while the picture stays (the game stopped, a note)
void Gpu::ApplyHud() {
    if (!m_output || !m_hudPipeline || (m_hudApplied && !m_hudChanged) || (!m_hudApplied && !m_hudShown)) {
        m_hudChanged = false;
        return;
    }
    const bool again = m_hudApplied; // (the readout that is on the picture is not the one to show any more)
    m_hudApplied = true;
    m_hudChanged = false;
    if (!m_plain || m_plainWidth != m_outputWidth || m_plainHeight != m_outputHeight) {
        if (again) {
            return; // (no picture without the readout was kept: the next one drawn will be right)
        }
        Release(m_plain);
        m_plain = Texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, m_outputWidth, m_outputHeight, true);
        m_plainWidth = m_outputWidth;
        m_plainHeight = m_outputHeight;
        if (!m_plain) {
            return;
        }
    }
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(m_device);
    {
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureLocation from{}, to{};
        from.texture = again ? m_plain : m_output;
        to.texture = again ? m_output : m_plain;
        SDL_CopyGPUTextureToTexture(copy, &from, &to, m_outputWidth, m_outputHeight, 1, false);
        SDL_EndGPUCopyPass(copy);
    }
    if (m_hudShown && m_hud) {
        SDL_GPUColorTargetInfo target{};
        target.texture = m_output;
        target.load_op = SDL_GPU_LOADOP_LOAD;
        target.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(pass, m_hudPipeline);
        const SDL_GPUTextureSamplerBinding sampler{m_hud, m_linear};
        SDL_BindGPUFragmentSamplers(pass, 0, &sampler, 1);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    }
    SDL_SubmitGPUCommandBuffer(cmd);
}

bool Gpu::ReadShown(std::vector<uint32_t> &pixels) {
    ApplyHud();
    return Read(pixels);
}

void Gpu::Present(SDL_Window *window) {
    if (!(m_overlayShown && m_overlay)) {
        ApplyHud();
    }
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(m_device);
    SDL_GPUTexture *swapchain = nullptr;
    Uint32 width = 0, height = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swapchain, &width, &height) || !swapchain) {
        SDL_SubmitGPUCommandBuffer(cmd);
        return;
    }
    // As large as fits at the picture's shape (4:3, or wider), black around it
    uint32_t w = width, h = static_cast<uint32_t>(width / m_aspect + 0.5f);
    if (h > height) {
        h = height;
        w = static_cast<uint32_t>(height * m_aspect + 0.5f);
    }
    SetOutputSize(w, h);
    {
        SDL_GPUColorTargetInfo target{};
        target.texture = swapchain;
        target.clear_color = {0, 0, 0, 1};
        target.load_op = SDL_GPU_LOADOP_CLEAR;
        target.store_op = SDL_GPU_STOREOP_STORE;
        SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, &target, 1, nullptr));
    }
    if (m_output) {
        const bool overlay = m_overlayShown && m_overlay;
        SDL_GPUBlitInfo blit{};
        blit.source.texture = overlay ? m_overlay : m_output;
        blit.source.w = overlay ? m_overlayWidth : m_outputWidth;
        blit.source.h = overlay ? m_overlayHeight : m_outputHeight;
        blit.destination.texture = swapchain;
        blit.destination.x = (width - w) / 2;
        blit.destination.y = (height - h) / 2;
        blit.destination.w = w;
        blit.destination.h = h;
        blit.load_op = SDL_GPU_LOADOP_LOAD;
        blit.filter = SDL_GPU_FILTER_LINEAR;
        SDL_BlitGPUTexture(cmd, &blit);
    }
    SDL_SubmitGPUCommandBuffer(cmd);
}

} // namespace satrecomp::video
