// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp

#include "shading.h"

#ifdef __ANDROID__
#include <dlfcn.h>

#include <filesystem>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <type_traits>
#include <vector>

#define LIBRA_RUNTIME_OPENGL
#include "librashader.h"

namespace port {

namespace {
    // librashader's functions, from the library beside the program (opened once, the first time a shader is asked for)
    struct Library {
        PFN_libra_instance_abi_version abi_version;
        PFN_libra_preset_create preset_create;
        PFN_libra_preset_free preset_free;
        PFN_libra_gl_filter_chain_create chain_create;
        PFN_libra_gl_filter_chain_frame chain_frame;
        PFN_libra_gl_filter_chain_free chain_free;
        PFN_libra_error_write error_write;
        PFN_libra_error_free_string error_free_string;
        PFN_libra_error_free error_free;
    };
    const Library *TheLibrary(std::string &error) {
        static Library library;
        static std::string why;
        static const bool opened = [] {
#if defined(_WIN32)
            const char *name = "librashader.dll";
#elif defined(__APPLE__)
            const char *name = "librashader.dylib";
#else
            const char *name = "librashader.so";
#endif
            // (beside the program; in a macOS app, its Frameworks folder; else wherever the system finds it)
            std::vector<std::string> paths;
#ifdef __ANDROID__
            // (on Android, beside the game's own library: the app's folder of them)
            Dl_info self;
            if (dladdr(reinterpret_cast<void *>(&Shading::Available), &self) && self.dli_fname) {
                paths.push_back((std::filesystem::path(self.dli_fname).parent_path() / name).string());
            }
#else
            if (const char *base = SDL_GetBasePath()) {
                paths.push_back(std::string(base) + name);
                paths.push_back(std::string(base) + "../Frameworks/" + name);
            }
#endif
            paths.push_back(name);
            SDL_SharedObject *object = nullptr;
            std::string failed; // (why the first place, beside the game, did not do)
            for (const std::string &path : paths) {
                if ((object = SDL_LoadObject(path.c_str()))) {
                    break;
                }
                if (failed.empty()) {
                    failed = SDL_GetError();
                }
            }
            if (!object) {
                why = std::string("no ") + name + " beside the game (" + failed + ")";
                return false;
            }
            bool all = true;
            auto take = [&](auto &function, const char *symbol) {
                function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(SDL_LoadFunction(object, symbol));
                all = all && function != nullptr;
            };
            take(library.abi_version, "libra_instance_abi_version");
            take(library.preset_create, "libra_preset_create");
            take(library.preset_free, "libra_preset_free");
            take(library.chain_create, "libra_gl_filter_chain_create");
            take(library.chain_frame, "libra_gl_filter_chain_frame");
            take(library.chain_free, "libra_gl_filter_chain_free");
            take(library.error_write, "libra_error_write");
            take(library.error_free_string, "libra_error_free_string");
            take(library.error_free, "libra_error_free");
            if (!all || library.abi_version() != LIBRASHADER_CURRENT_ABI) {
                why = std::string(name) + " is not the version the game was made for (or has no OpenGL)";
                SDL_UnloadObject(object);
                return false;
            }
            return true;
        }();
        error = why;
        return opened ? &library : nullptr;
    }
    // A librashader error's message (and the error freed); "" for none
    std::string Message(const Library &library, libra_error_t error) {
        if (!error) {
            return "";
        }
        char *text = nullptr;
        std::string message = library.error_write(error, &text) == 0 && text ? text : "librashader failed";
        if (text) {
            library.error_free_string(&text);
        }
        library.error_free(&error);
        return message;
    }

    // The few OpenGL functions drawn with here (librashader finds its own the same way: SDL_GL_GetProcAddress)
#ifdef _WIN32
#define GL_CALL __stdcall
#else
#define GL_CALL
#endif
    using GLuint = unsigned int;
    using GLenum = unsigned int;
    using GLint = int;
    using GLsizei = int;
    enum : GLenum {
        GL_TEXTURE_2D = 0x0DE1, GL_RGBA = 0x1908, GL_RGBA8 = 0x8058, GL_UNSIGNED_BYTE = 0x1401,
        GL_TEXTURE_MAG_FILTER = 0x2800, GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803,
        GL_NEAREST = 0x2600, GL_LINEAR = 0x2601, GL_CLAMP_TO_EDGE = 0x812F, GL_TEXTURE_SWIZZLE_A = 0x8E45, GL_ONE = 1,
        GL_TEXTURE_MAX_LEVEL = 0x813D,
        GL_UNPACK_ALIGNMENT = 0x0CF5, GL_UNPACK_ROW_LENGTH = 0x0CF2, GL_SCISSOR_TEST = 0x0C11,
        GL_READ_FRAMEBUFFER = 0x8CA8, GL_DRAW_FRAMEBUFFER = 0x8CA9, GL_COLOR_ATTACHMENT0 = 0x8CE0, GL_COLOR_BUFFER_BIT = 0x4000,
        GL_PACK_ALIGNMENT = 0x0D05,
        GL_VERTEX_SHADER = 0x8B31, GL_FRAGMENT_SHADER = 0x8B30, GL_BLEND = 0x0BE2, GL_ONE_MINUS_SRC_ALPHA = 0x0303,
        GL_TRIANGLES = 0x0004, GL_TEXTURE0 = 0x84C0, GL_FRAMEBUFFER = 0x8D40,
    };
    struct Gl {
        void(GL_CALL *GenTextures)(GLsizei, GLuint *);
        void(GL_CALL *DeleteTextures)(GLsizei, const GLuint *);
        void(GL_CALL *BindTexture)(GLenum, GLuint);
        void(GL_CALL *TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
        void(GL_CALL *TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
        void(GL_CALL *TexParameteri)(GLenum, GLenum, GLint);
        void(GL_CALL *PixelStorei)(GLenum, GLint);
        void(GL_CALL *GenFramebuffers)(GLsizei, GLuint *);
        void(GL_CALL *DeleteFramebuffers)(GLsizei, const GLuint *);
        void(GL_CALL *BindFramebuffer)(GLenum, GLuint);
        void(GL_CALL *FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
        void(GL_CALL *BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, unsigned, GLenum);
        void(GL_CALL *ClearColor)(float, float, float, float);
        void(GL_CALL *Clear)(unsigned);
        void(GL_CALL *Disable)(GLenum);
        void(GL_CALL *ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
        // (the overlay's: a texture drawn over the window, blended)
        GLuint(GL_CALL *CreateShader)(GLenum);
        void(GL_CALL *ShaderSource)(GLuint, GLsizei, const char *const *, const GLint *);
        void(GL_CALL *CompileShader)(GLuint);
        void(GL_CALL *DeleteShader)(GLuint);
        GLuint(GL_CALL *CreateProgram)();
        void(GL_CALL *AttachShader)(GLuint, GLuint);
        void(GL_CALL *LinkProgram)(GLuint);
        void(GL_CALL *DeleteProgram)(GLuint);
        void(GL_CALL *UseProgram)(GLuint);
        GLint(GL_CALL *GetUniformLocation)(GLuint, const char *);
        void(GL_CALL *Uniform1i)(GLint, GLint);
        void(GL_CALL *GenVertexArrays)(GLsizei, GLuint *);
        void(GL_CALL *DeleteVertexArrays)(GLsizei, const GLuint *);
        void(GL_CALL *BindVertexArray)(GLuint);
        void(GL_CALL *DrawArrays)(GLenum, GLint, GLsizei);
        void(GL_CALL *Enable)(GLenum);
        void(GL_CALL *BlendFunc)(GLenum, GLenum);
        void(GL_CALL *ActiveTexture)(GLenum);
        void(GL_CALL *Viewport)(GLint, GLint, GLsizei, GLsizei);
        bool Load() {
            bool all = true;
            auto take = [&](auto &function, const char *symbol) {
                function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(SDL_GL_GetProcAddress(symbol));
                all = all && function != nullptr;
            };
            take(GenTextures, "glGenTextures");
            take(DeleteTextures, "glDeleteTextures");
            take(BindTexture, "glBindTexture");
            take(TexImage2D, "glTexImage2D");
            take(TexSubImage2D, "glTexSubImage2D");
            take(TexParameteri, "glTexParameteri");
            take(PixelStorei, "glPixelStorei");
            take(GenFramebuffers, "glGenFramebuffers");
            take(DeleteFramebuffers, "glDeleteFramebuffers");
            take(BindFramebuffer, "glBindFramebuffer");
            take(FramebufferTexture2D, "glFramebufferTexture2D");
            take(BlitFramebuffer, "glBlitFramebuffer");
            take(ClearColor, "glClearColor");
            take(Clear, "glClear");
            take(Disable, "glDisable");
            take(ReadPixels, "glReadPixels");
            take(CreateShader, "glCreateShader");
            take(ShaderSource, "glShaderSource");
            take(CompileShader, "glCompileShader");
            take(DeleteShader, "glDeleteShader");
            take(CreateProgram, "glCreateProgram");
            take(AttachShader, "glAttachShader");
            take(LinkProgram, "glLinkProgram");
            take(DeleteProgram, "glDeleteProgram");
            take(UseProgram, "glUseProgram");
            take(GetUniformLocation, "glGetUniformLocation");
            take(Uniform1i, "glUniform1i");
            take(GenVertexArrays, "glGenVertexArrays");
            take(DeleteVertexArrays, "glDeleteVertexArrays");
            take(BindVertexArray, "glBindVertexArray");
            take(DrawArrays, "glDrawArrays");
            take(Enable, "glEnable");
            take(BlendFunc, "glBlendFunc");
            take(ActiveTexture, "glActiveTexture");
            take(Viewport, "glViewport");
            return all;
        }
    } gl;

    // A texture of `width` x `height` with a framebuffer to read it or draw into it
    void MakeTexture(GLuint &texture, GLuint &frame, int width, int height, GLenum filter) {
        if (texture) {
            gl.DeleteTextures(1, &texture);
            gl.DeleteFramebuffers(1, &frame);
        }
        gl.GenTextures(1, &texture);
        gl.BindTexture(GL_TEXTURE_2D, texture);
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // (one level, complete for a sampler that would blend between levels: librashader's are)
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        // (the picture's fourth byte is spare, not how opaque it is)
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ONE);
        gl.BindTexture(GL_TEXTURE_2D, 0);
        gl.GenFramebuffers(1, &frame);
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, frame);
        gl.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    }
} // namespace

namespace {
    libra_gl_filter_chain_t *Chain(void *&chain) {
        return reinterpret_cast<libra_gl_filter_chain_t *>(&chain);
    }
} // namespace

bool Shading::Available(std::string &error) {
    return TheLibrary(error) != nullptr;
}

Shading::~Shading() {
    Close();
}

bool Shading::Open(SDL_Window *window, std::string &error) {
    if (!Available(error)) {
        return false;
    }
    m_window = window;
#ifdef __ANDROID__
    // (OpenGL ES 3, which librashader draws with too: 3.2 where there is one, else 3.0)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    m_context = SDL_GL_CreateContext(window);
    if (!m_context) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        m_context = SDL_GL_CreateContext(window);
    }
#else
    // (OpenGL 3.3's core, the least librashader takes: on macOS that gives its newest, 4.1)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    m_context = SDL_GL_CreateContext(window);
#endif
    if (!m_context) {
        error = SDL_GetError();
        return false;
    }
    if (!gl.Load()) {
        error = "the window's OpenGL is too old for shaders (3.3, or OpenGL ES 3, is needed)";
        Close();
        return false;
    }
    SDL_GL_SetSwapInterval(0); // (frames are paced by the port, to the Saturn's rate)
    return true;
}

void Shading::Close() {
    if (m_context) {
        SDL_GL_MakeCurrent(m_window, m_context);
        std::string error;
        if (m_chain) {
            TheLibrary(error)->chain_free(Chain(m_chain));
        }
        if (m_overlayProgram) {
            gl.DeleteProgram(m_overlayProgram);
            gl.DeleteVertexArrays(1, &m_overlayArray);
        }
        for (GLuint *texture : {&m_picture, &m_drawn, &m_readout, &m_overlay}) {
            if (*texture) {
                gl.DeleteTextures(1, texture);
            }
        }
        for (GLuint *frame : {&m_pictureFrame, &m_drawnFrame, &m_readoutFrame}) {
            if (*frame) {
                gl.DeleteFramebuffers(1, frame);
            }
        }
        SDL_GL_DestroyContext(m_context);
    }
    m_chain = nullptr;
    m_context = nullptr;
    m_preset.clear();
    m_picture = m_drawn = m_readout = m_pictureFrame = m_drawnFrame = m_readoutFrame = 0;
    m_overlay = m_overlayProgram = m_overlayArray = 0;
    m_overlayWidth = m_overlayHeight = 0;
    m_width = m_height = m_drawnWidth = m_drawnHeight = 0;
}

bool Shading::Load(const std::string &preset, std::string &error) {
    const Library *library = TheLibrary(error);
    if (!library || !m_context) {
        return false;
    }
    SDL_GL_MakeCurrent(m_window, m_context);
    libra_shader_preset_t made = nullptr;
    if ((error = Message(*library, library->preset_create(preset.c_str(), &made))) != "") {
        return false;
    }
    filter_chain_gl_opt_t options = {};
    options.version = LIBRASHADER_CURRENT_VERSION;
#ifdef __ANDROID__
    options.glsl_version = 0; // (the context's own: OpenGL ES 3's)
#else
    options.glsl_version = 410;
#endif
    options.disable_cache = true; // (the cache needs OpenGL 4.5, which macOS has not)
    libra_gl_filter_chain_t chain = nullptr;
    auto loader = [](const char *name) { return reinterpret_cast<const void *>(SDL_GL_GetProcAddress(name)); };
    // (the preset is used up by this, made into the chain or not)
    if ((error = Message(*library, library->chain_create(&made, loader, &options, &chain))) != "") {
        return false;
    }
    if (m_chain) {
        library->chain_free(Chain(m_chain));
    }
    m_chain = chain;
    m_preset = preset;
    m_frame = 0;
    return true;
}

void Shading::Size(int &width, int &height) const {
    SDL_GetWindowSizeInPixels(m_window, &width, &height);
}

void Shading::Show(const uint32_t *pixels, int width, int height, const SDL_FRect &where, bool shade, const Zoomed *zoomed) {
    std::string error;
    const Library *library = TheLibrary(error);
    if (!m_context || !library) {
        return;
    }
    SDL_GL_MakeCurrent(m_window, m_context);
    if (!m_picture || width != m_width || height != m_height) {
        MakeTexture(m_picture, m_pictureFrame, width, height, GL_LINEAR);
        MakeTexture(m_readout, m_readoutFrame, width, height, GL_NEAREST);
        m_width = width;
        m_height = height;
    }
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, width);
    gl.BindTexture(GL_TEXTURE_2D, m_picture);
    gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (zoomed && !zoomed->keep.empty()) {
        gl.BindTexture(GL_TEXTURE_2D, m_readout);
        gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels + static_cast<size_t>(width) * height);
    }
    gl.BindTexture(GL_TEXTURE_2D, 0);

    int windowWidth = 0, windowHeight = 0;
    Size(windowWidth, windowHeight);
    gl.Disable(GL_SCISSOR_TEST);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    gl.ClearColor(0, 0, 0, 1);
    gl.Clear(GL_COLOR_BUFFER_BIT);
    // (the window's rows count up from its foot, and the picture's first row is its top)
    const int left = static_cast<int>(std::lround(where.x)), right = static_cast<int>(std::lround(where.x + where.w));
    const int top = windowHeight - static_cast<int>(std::lround(where.y)), foot = windowHeight - static_cast<int>(std::lround(where.y + where.h));
    if (shade && m_chain) {
        // The preset draws the picture into a texture of `where`'s size, which then goes to the window
        const int drawnWidth = right - left, drawnHeight = top - foot;
        if (drawnWidth <= 0 || drawnHeight <= 0) {
            SDL_GL_SwapWindow(m_window);
            return;
        }
        if (!m_drawn || drawnWidth != m_drawnWidth || drawnHeight != m_drawnHeight) {
            MakeTexture(m_drawn, m_drawnFrame, drawnWidth, drawnHeight, GL_NEAREST);
            m_drawnWidth = drawnWidth;
            m_drawnHeight = drawnHeight;
        }
        const libra_image_gl_t in = {m_picture, GL_RGBA8, static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
        const libra_image_gl_t out = {m_drawn, GL_RGBA8, static_cast<uint32_t>(drawnWidth), static_cast<uint32_t>(drawnHeight)};
        const libra_viewport_t viewport = {0, 0, static_cast<uint32_t>(drawnWidth), static_cast<uint32_t>(drawnHeight)};
        frame_gl_opt_t options = {};
        options.version = LIBRASHADER_CURRENT_VERSION;
        options.frame_direction = 1;
        options.total_subframes = options.current_subframe = 1;
        options.frames_per_second = 59.8261f; // (the Saturn's, NTSC)
        options.frametime_delta = 17;
        options.brightness_nits = 200;
        const std::string failed = Message(*library, library->chain_frame(Chain(m_chain), m_frame++, in, out, &viewport, nullptr, &options));
        // (librashader leaves its own framebuffers and scissor as they were in its last pass)
        gl.Disable(GL_SCISSOR_TEST);
        gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        if (failed.empty()) {
            gl.BindFramebuffer(GL_READ_FRAMEBUFFER, m_drawnFrame);
            // (librashader's picture has its first row at the texture's start too, as it was given it)
            if (!zoomed) {
                gl.BlitFramebuffer(0, 0, drawnWidth, drawnHeight, left, top, right, foot, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            } else {
                // Of what the preset drew, the part `view` (only what is on the picture: past its edges is black),
                // enlarged to fill `where`
                const SDL_FRect &view = zoomed->view;
                const float x0 = std::max(view.x, 0.0f), y0 = std::max(view.y, 0.0f);
                const float x1 = std::min(view.x + view.w, static_cast<float>(width)), y1 = std::min(view.y + view.h, static_cast<float>(height));
                if (x1 > x0 && y1 > y0) {
                    const float acrossFrom = static_cast<float>(drawnWidth) / width, downFrom = static_cast<float>(drawnHeight) / height;
                    const float across = where.w / view.w, down = where.h / view.h;
                    gl.BlitFramebuffer(static_cast<int>(std::lround(x0 * acrossFrom)), static_cast<int>(std::lround(y0 * downFrom)),
                                       static_cast<int>(std::lround(x1 * acrossFrom)), static_cast<int>(std::lround(y1 * downFrom)),
                                       static_cast<int>(std::lround(where.x + (x0 - view.x) * across)),
                                       windowHeight - static_cast<int>(std::lround(where.y + (y0 - view.y) * down)),
                                       static_cast<int>(std::lround(where.x + (x1 - view.x) * across)),
                                       windowHeight - static_cast<int>(std::lround(where.y + (y1 - view.y) * down)), GL_COLOR_BUFFER_BIT, GL_LINEAR);
                }
                // (the readout's panels where they are and at their size, not through the preset)
                gl.BindFramebuffer(GL_READ_FRAMEBUFFER, m_readoutFrame);
                const float across = where.w / width, down = where.h / height;
                for (const SDL_Rect &keep : zoomed->keep) {
                    gl.BlitFramebuffer(keep.x, keep.y, keep.x + keep.w, keep.y + keep.h, static_cast<int>(std::lround(where.x + keep.x * across)),
                                       windowHeight - static_cast<int>(std::lround(where.y + keep.y * down)),
                                       static_cast<int>(std::lround(where.x + (keep.x + keep.w) * across)),
                                       windowHeight - static_cast<int>(std::lround(where.y + (keep.y + keep.h) * down)), GL_COLOR_BUFFER_BIT, GL_NEAREST);
                }
            }
        } else if (m_frame == 1) {
            fprintf(stderr, "shader: %s\n", failed.c_str());
        }
    } else {
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, m_pictureFrame);
        gl.BlitFramebuffer(0, 0, width, height, left, top, right, foot, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    DrawOverlay(windowWidth, windowHeight);
    if (keepLast) {
        Keep();
    }
    SDL_GL_SwapWindow(m_window);
}

void Shading::SetOverlay(const uint32_t *pixels, int width, int height) {
    if (!m_context) {
        return;
    }
    SDL_GL_MakeCurrent(m_window, m_context);
    if (!pixels) {
        if (m_overlay) {
            gl.DeleteTextures(1, &m_overlay);
        }
        m_overlay = 0;
        m_overlayWidth = m_overlayHeight = 0;
        return;
    }
    if (!m_overlay || width != m_overlayWidth || height != m_overlayHeight) {
        if (m_overlay) {
            gl.DeleteTextures(1, &m_overlay);
        }
        gl.GenTextures(1, &m_overlay);
        gl.BindTexture(GL_TEXTURE_2D, m_overlay);
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        m_overlayWidth = width;
        m_overlayHeight = height;
    }
    gl.BindTexture(GL_TEXTURE_2D, m_overlay);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, width);
    gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    gl.BindTexture(GL_TEXTURE_2D, 0);
}

void Shading::DrawOverlay(int windowWidth, int windowHeight) {
    if (!m_overlay) {
        return;
    }
    if (!m_overlayProgram) {
        // The whole window from three corners, the texture's first row at its top
#ifdef __ANDROID__
        const char *version = "#version 300 es\nprecision mediump float;\n";
#else
        const char *version = "#version 330 core\n";
#endif
        const char *vertex = "out vec2 uv;\n"
                             "void main() {\n"
                             "    vec2 at = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
                             "    uv = vec2(at.x, 1.0 - at.y);\n"
                             "    gl_Position = vec4(at * 2.0 - 1.0, 0.0, 1.0);\n"
                             "}\n";
        const char *fragment = "in vec2 uv;\nuniform sampler2D overlay;\nout vec4 colour;\n"
                               "void main() { colour = texture(overlay, uv); }\n";
        auto compile = [&](GLenum kind, const char *body) {
            const GLuint shader = gl.CreateShader(kind);
            const char *parts[] = {version, body};
            gl.ShaderSource(shader, 2, parts, nullptr);
            gl.CompileShader(shader);
            return shader;
        };
        const GLuint v = compile(GL_VERTEX_SHADER, vertex), f = compile(GL_FRAGMENT_SHADER, fragment);
        m_overlayProgram = gl.CreateProgram();
        gl.AttachShader(m_overlayProgram, v);
        gl.AttachShader(m_overlayProgram, f);
        gl.LinkProgram(m_overlayProgram);
        gl.DeleteShader(v);
        gl.DeleteShader(f);
        gl.GenVertexArrays(1, &m_overlayArray);
    }
    // (premultiplied: present.h, SetOverlay)
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.Viewport(0, 0, windowWidth, windowHeight);
    gl.UseProgram(m_overlayProgram);
    gl.BindVertexArray(m_overlayArray);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, m_overlay);
    gl.Uniform1i(gl.GetUniformLocation(m_overlayProgram, "overlay"), 0);
    gl.Enable(GL_BLEND);
    gl.BlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);
    gl.Disable(GL_BLEND);
    gl.BindTexture(GL_TEXTURE_2D, 0);
    gl.BindVertexArray(0);
    gl.UseProgram(0);
}

void Shading::Keep() {
    int width = 0, height = 0;
    Size(width, height);
    std::vector<uint32_t> rows(static_cast<size_t>(width) * height);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
    gl.ReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rows.data());
    m_last.resize(rows.size());
    for (int y = 0; y < height; y++) {
        std::copy_n(&rows[static_cast<size_t>(height - 1 - y) * width], width, &m_last[static_cast<size_t>(y) * width]);
    }
    m_lastWidth = width;
    m_lastHeight = height;
}

bool Shading::ReadBack(std::vector<uint32_t> &pixels, int &width, int &height) {
    pixels = m_last;
    width = m_lastWidth;
    height = m_lastHeight;
    return !m_last.empty();
}

} // namespace port
