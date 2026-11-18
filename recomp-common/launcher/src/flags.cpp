// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of recomp-common: https://github.com/tmcleroy/recomp-common
#include "flags.h"

#include "countries.h"
#include "embedded.h"
#include "ui.h"

#include <SDL3/SDL.h>
#include <imgui.h>

namespace launcher::flags {

int find(std::string_view code) {
    if (code.size() != 2) {
        return -1;
    }
    char upper[3] = {static_cast<char>(code[0] & ~0x20), static_cast<char>(code[1] & ~0x20), 0};
    for (int n = 0; n < kFlagCount; n++) {
        if (kCountryFlags[n].code[0] == upper[0] && kCountryFlags[n].code[1] == upper[1]) {
            return n;
        }
    }
    return -1;
}

std::string known(std::string_view code) {
    const int n = find(code);
    return n < 0 ? std::string() : std::string(kCountryFlags[n].code);
}

namespace {

// The flags' picture as a texture, made the first time one is drawn (nullptr: none to be had)
SDL_Texture* texture(SDL_Renderer* renderer) {
    static SDL_Texture* made = nullptr;
    static bool tried = false;
    if (!tried && renderer) {
        tried = true;
#if SDL_VERSION_ATLEAST(3, 4, 0)
        const std::string_view png = blob("flags");
        SDL_Surface* surface = png.empty() ? nullptr : SDL_LoadPNG_IO(SDL_IOFromConstMem(png.data(), png.size()), true);
        if (surface) {
            made = SDL_CreateTextureFromSurface(renderer, surface);
            SDL_DestroySurface(surface);
            if (made) {
                SDL_SetTextureScaleMode(made, SDL_SCALEMODE_LINEAR);
            }
        }
#endif
    }
    return made;
}

} // namespace

bool draw(SDL_Renderer* renderer, std::string_view code, float height) {
    const int n = find(code);
    SDL_Texture* flags = n < 0 ? nullptr : texture(renderer);
    if (!flags) {
        return false;
    }
    const float h = ui::px(height), w = h * kFlagWidth / kFlagHeight;
    const float across = static_cast<float>(kFlagsAcross * kFlagWidth);
    const float down = static_cast<float>(((kFlagCount + kFlagsAcross - 1) / kFlagsAcross) * kFlagHeight);
    const float x = static_cast<float>((n % kFlagsAcross) * kFlagWidth), y = static_cast<float>((n / kFlagsAcross) * kFlagHeight);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImDrawList* list = ImGui::GetWindowDrawList();
    list->AddImage(ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(flags))), at, ImVec2(at.x + w, at.y + h), ImVec2(x / across, y / down),
                   ImVec2((x + kFlagWidth) / across, (y + kFlagHeight) / down));
    list->AddRect(at, ImVec2(at.x + w, at.y + h), IM_COL32(0, 0, 0, 60));  // (a white flag's edge on a light page)
    ImGui::Dummy(ImVec2(w, h));
    return true;
}

} // namespace launcher::flags
