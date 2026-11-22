// gl_overlay.h -- put the Multiplayer panel on screen when the game renders with OpenGL.
//
// Transport Fever 2 has a Renderer setting, Vulkan or OpenGL. The menu DLL's overlay
// only knew Vulkan (vkQueuePresentKHR), so on OpenGL the MULTIPLAYER button opened a
// panel that never drew -- and the per-frame work that lives in the present hook
// (lobby polling, the load stage, the dedicated tick) never ran either. Reported
// 2026-09-21: "my son's multiplayer button ... doesn't click".
//
// The panel is drawn on the CPU (GDI into a BGRA layer, composed opaque), the same
// image both renderers show. This header only moves it onto the default framebuffer:
// one texture + one read framebuffer, glTexSubImage2D when the panel changed, and a
// glBlitFramebuffer every frame (a blit bypasses the fragment pipeline: no shader,
// no blending, no depth -- only the scissor test and sRGB conversion apply, and both
// are switched off for it). Every piece of GL state it touches is read first and put
// back, so the game's next frame starts exactly where it left off.
//
// Needs a current context of GL 3.0 or newer (framebuffer objects and blit are core
// there). Windows resolves through opengl32/wgl; Linux uses SDL_GL_GetProcAddress.
#pragma once
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <cstddef>
#define WINAPI
#endif
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace GlOverlay {
typedef unsigned int GLenum; typedef unsigned int GLuint; typedef int GLint; typedef int GLsizei;
typedef unsigned char GLboolean; typedef unsigned int GLbitfield; typedef ptrdiff_t GLintptr;

enum : GLenum {
    TEXTURE_2D = 0x0DE1, RGBA8 = 0x8058, BGRA = 0x80E1, UNSIGNED_BYTE = 0x1401,
    TEXTURE_MIN_FILTER = 0x2801, TEXTURE_MAG_FILTER = 0x2800, NEAREST = 0x2600,
    TEXTURE_BINDING_2D = 0x8069,
    READ_FRAMEBUFFER = 0x8CA8, DRAW_FRAMEBUFFER = 0x8CA9, READ_FRAMEBUFFER_BINDING = 0x8CAA, DRAW_FRAMEBUFFER_BINDING = 0x8CA6,
    COLOR_ATTACHMENT0 = 0x8CE0, FRAMEBUFFER_COMPLETE = 0x8CD5, COLOR_BUFFER_BIT = 0x4000,
    SCISSOR_TEST = 0x0C11, FRAMEBUFFER_SRGB = 0x8DB9,
    UNPACK_ALIGNMENT = 0x0CF5, UNPACK_ROW_LENGTH = 0x0CF2, UNPACK_SKIP_ROWS = 0x0CF3, UNPACK_SKIP_PIXELS = 0x0CF4,
    PIXEL_UNPACK_BUFFER = 0x88EC, PIXEL_UNPACK_BUFFER_BINDING = 0x88EF,
    GL_OK = 0,
};

struct Fns {
    void   (WINAPI* GetIntegerv)(GLenum, GLint*);
    GLboolean (WINAPI* IsEnabled)(GLenum);
    void   (WINAPI* Enable)(GLenum);
    void   (WINAPI* Disable)(GLenum);
    void   (WINAPI* GenTextures)(GLsizei, GLuint*);
    void   (WINAPI* DeleteTextures)(GLsizei, const GLuint*);
    void   (WINAPI* BindTexture)(GLenum, GLuint);
    void   (WINAPI* TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
    void   (WINAPI* TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*);
    void   (WINAPI* TexParameteri)(GLenum, GLenum, GLint);
    void   (WINAPI* PixelStorei)(GLenum, GLint);
    GLenum (WINAPI* GetError)();
    // GL 3.0 core, through wglGetProcAddress
    void   (WINAPI* GenFramebuffers)(GLsizei, GLuint*);
    void   (WINAPI* DeleteFramebuffers)(GLsizei, const GLuint*);
    void   (WINAPI* BindFramebuffer)(GLenum, GLuint);
    void   (WINAPI* FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    GLenum (WINAPI* CheckFramebufferStatus)(GLenum);
    void   (WINAPI* BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
    void   (WINAPI* BindBuffer)(GLenum, GLuint);
};

struct State {
    Fns f{};
    bool loaded = false, failed = false;
    GLuint tex = 0, fbo = 0;
    int texW = 0, texH = 0;
    char why[160] = "";
};

// wglGetProcAddress returns 1, 2, 3 or -1 for "not found" on some drivers. Looked up at
// run time: the menu DLL does not link opengl32.lib (the game has it loaded already).
inline void* WglProc(const char* name) {
#ifdef _WIN32
    typedef PROC (WINAPI* GetProcFn)(LPCSTR);
    static GetProcFn get = nullptr;
    if (!get) { HMODULE gl = GetModuleHandleW(L"opengl32.dll"); if (gl) get = reinterpret_cast<GetProcFn>(GetProcAddress(gl, "wglGetProcAddress")); }
    if (!get) return nullptr;
    void* p = reinterpret_cast<void*>(get(name));
#else
    using GetProcFn = void* (*)(const char*);
    static auto get = reinterpret_cast<GetProcFn>(dlsym(RTLD_DEFAULT, "SDL_GL_GetProcAddress"));
    void* p = get ? get(name) : nullptr;
#endif
    const intptr_t v = reinterpret_cast<intptr_t>(p);
    return (v == 0 || v == 1 || v == 2 || v == 3 || v == -1) ? nullptr : p;
}

// Resolve every function once, with a context current. False (and `why`) if any is missing.
inline bool Load(State& s) {
    if (s.loaded) return true;
    if (s.failed) return false;
#ifdef _WIN32
    HMODULE gl = GetModuleHandleW(L"opengl32.dll");
    if (!gl) { strcpy_s(s.why, "opengl32.dll is not loaded"); s.failed = true; return false; }
    auto base = [&](const char* n) { return reinterpret_cast<void*>(GetProcAddress(gl, n)); };
#else
    auto base = [](const char* n) { return WglProc(n); };
#endif
    Fns& f = s.f;
    *reinterpret_cast<void**>(&f.GetIntegerv) = base("glGetIntegerv");
    *reinterpret_cast<void**>(&f.IsEnabled) = base("glIsEnabled");
    *reinterpret_cast<void**>(&f.Enable) = base("glEnable");
    *reinterpret_cast<void**>(&f.Disable) = base("glDisable");
    *reinterpret_cast<void**>(&f.GenTextures) = base("glGenTextures");
    *reinterpret_cast<void**>(&f.DeleteTextures) = base("glDeleteTextures");
    *reinterpret_cast<void**>(&f.BindTexture) = base("glBindTexture");
    *reinterpret_cast<void**>(&f.TexImage2D) = base("glTexImage2D");
    *reinterpret_cast<void**>(&f.TexSubImage2D) = base("glTexSubImage2D");
    *reinterpret_cast<void**>(&f.TexParameteri) = base("glTexParameteri");
    *reinterpret_cast<void**>(&f.PixelStorei) = base("glPixelStorei");
    *reinterpret_cast<void**>(&f.GetError) = base("glGetError");
    *reinterpret_cast<void**>(&f.GenFramebuffers) = WglProc("glGenFramebuffers");
    *reinterpret_cast<void**>(&f.DeleteFramebuffers) = WglProc("glDeleteFramebuffers");
    *reinterpret_cast<void**>(&f.BindFramebuffer) = WglProc("glBindFramebuffer");
    *reinterpret_cast<void**>(&f.FramebufferTexture2D) = WglProc("glFramebufferTexture2D");
    *reinterpret_cast<void**>(&f.CheckFramebufferStatus) = WglProc("glCheckFramebufferStatus");
    *reinterpret_cast<void**>(&f.BlitFramebuffer) = WglProc("glBlitFramebuffer");
    *reinterpret_cast<void**>(&f.BindBuffer) = WglProc("glBindBuffer");
    void* const* all = reinterpret_cast<void* const*>(&f);
    static const char* names[] = { "glGetIntegerv", "glIsEnabled", "glEnable", "glDisable", "glGenTextures", "glDeleteTextures",
        "glBindTexture", "glTexImage2D", "glTexSubImage2D", "glTexParameteri", "glPixelStorei", "glGetError",
        "glGenFramebuffers", "glDeleteFramebuffers", "glBindFramebuffer", "glFramebufferTexture2D",
        "glCheckFramebufferStatus", "glBlitFramebuffer", "glBindBuffer" };
    static_assert(sizeof(Fns) == sizeof(void*) * (sizeof(names) / sizeof(names[0])), "one name per function");
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (!all[i]) { snprintf(s.why, sizeof s.why, "%s is not available (OpenGL 3.0 or newer is needed)", names[i]); s.failed = true; return false; }
    s.loaded = true;
    return true;
}

// Everything Draw touches, read before and written back after.
struct Saved {
    GLint readFb = 0, drawFb = 0, tex = 0, unpackBuf = 0, align = 4, rowLen = 0, skipRows = 0, skipPixels = 0;
    GLboolean scissor = 0, srgb = 0;
};
inline void Save(const Fns& f, Saved& v) {
    f.GetIntegerv(READ_FRAMEBUFFER_BINDING, &v.readFb); f.GetIntegerv(DRAW_FRAMEBUFFER_BINDING, &v.drawFb);
    f.GetIntegerv(TEXTURE_BINDING_2D, &v.tex); f.GetIntegerv(PIXEL_UNPACK_BUFFER_BINDING, &v.unpackBuf);
    f.GetIntegerv(UNPACK_ALIGNMENT, &v.align); f.GetIntegerv(UNPACK_ROW_LENGTH, &v.rowLen);
    f.GetIntegerv(UNPACK_SKIP_ROWS, &v.skipRows); f.GetIntegerv(UNPACK_SKIP_PIXELS, &v.skipPixels);
    v.scissor = f.IsEnabled(SCISSOR_TEST); v.srgb = f.IsEnabled(FRAMEBUFFER_SRGB);
}
inline void Restore(const Fns& f, const Saved& v) {
    f.BindFramebuffer(READ_FRAMEBUFFER, GLuint(v.readFb)); f.BindFramebuffer(DRAW_FRAMEBUFFER, GLuint(v.drawFb));
    f.BindTexture(TEXTURE_2D, GLuint(v.tex)); f.BindBuffer(PIXEL_UNPACK_BUFFER, GLuint(v.unpackBuf));
    f.PixelStorei(UNPACK_ALIGNMENT, v.align); f.PixelStorei(UNPACK_ROW_LENGTH, v.rowLen);
    f.PixelStorei(UNPACK_SKIP_ROWS, v.skipRows); f.PixelStorei(UNPACK_SKIP_PIXELS, v.skipPixels);
    if (v.scissor) f.Enable(SCISSOR_TEST); else f.Disable(SCISSOR_TEST);
    if (v.srgb) f.Enable(FRAMEBUFFER_SRGB); else f.Disable(FRAMEBUFFER_SRGB);
}

// Size the texture (and its read framebuffer) for a panel of at most w x h.
inline bool Ensure(State& s, int w, int h) {
    if (s.tex && s.texW == w && s.texH == h) return true;
    const Fns& f = s.f;
    Saved v; Save(f, v);
    if (s.fbo) { f.DeleteFramebuffers(1, &s.fbo); s.fbo = 0; }
    if (s.tex) { f.DeleteTextures(1, &s.tex); s.tex = 0; }
    f.BindBuffer(PIXEL_UNPACK_BUFFER, 0);
    f.GenTextures(1, &s.tex);
    f.BindTexture(TEXTURE_2D, s.tex);
    f.TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, NEAREST);
    f.TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, NEAREST);
    f.TexImage2D(TEXTURE_2D, 0, RGBA8, w, h, 0, BGRA, UNSIGNED_BYTE, nullptr);
    f.GenFramebuffers(1, &s.fbo);
    f.BindFramebuffer(READ_FRAMEBUFFER, s.fbo);
    f.FramebufferTexture2D(READ_FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, s.tex, 0);
    const GLenum status = f.CheckFramebufferStatus(READ_FRAMEBUFFER);
    const GLenum err = f.GetError();
    Restore(f, v);
    if (status != FRAMEBUFFER_COMPLETE || err != GL_OK) {
        snprintf(s.why, sizeof s.why, "the panel framebuffer is not usable (status 0x%x, error 0x%x)", status, err);
        s.failed = true; return false;
    }
    s.texW = w; s.texH = h;
    return true;
}

// Upload w x h BGRA pixels (rows `pitch` bytes apart, top row first) when `upload`, then
// blit them to (x, y) of the default framebuffer, y measured from the TOP of a window
// `fbH` pixels tall. Returns false (and `why`) on a GL error; the state is restored.
// `dstFb` is 0 (the window) in the game; the offline test passes its own target.
inline bool Draw(State& s, const void* pixels, size_t pitch, int w, int h, bool upload, int x, int y, int fbH, GLuint dstFb = 0) {
    if (!s.loaded || s.failed || !s.tex || w <= 0 || h <= 0 || w > s.texW || h > s.texH) return false;
    const Fns& f = s.f;
    while (f.GetError() != GL_OK) {}   // start clean, so an error below is ours
    Saved v; Save(f, v);
    if (f.GetError() != GL_OK) { snprintf(s.why, sizeof(s.why), "reading the GL state failed"); s.failed = true; return false; }
    if (upload && pixels) {
        f.BindBuffer(PIXEL_UNPACK_BUFFER, 0);
        f.PixelStorei(UNPACK_ALIGNMENT, 4); f.PixelStorei(UNPACK_ROW_LENGTH, GLint(pitch / 4));
        f.PixelStorei(UNPACK_SKIP_ROWS, 0); f.PixelStorei(UNPACK_SKIP_PIXELS, 0);
        f.BindTexture(TEXTURE_2D, s.tex);
        f.TexSubImage2D(TEXTURE_2D, 0, 0, 0, w, h, BGRA, UNSIGNED_BYTE, pixels);
    }
    f.Disable(SCISSOR_TEST); f.Disable(FRAMEBUFFER_SRGB);
    f.BindFramebuffer(READ_FRAMEBUFFER, s.fbo);
    f.BindFramebuffer(DRAW_FRAMEBUFFER, dstFb);
    // texture row 0 is the panel's top; the window's GL origin is bottom-left: flip
    f.BlitFramebuffer(0, 0, w, h, x, fbH - y, x + w, fbH - y - h, COLOR_BUFFER_BIT, NEAREST);
    const GLenum err = f.GetError();
    Restore(f, v);
    if (err != GL_OK) { snprintf(s.why, sizeof s.why, "drawing the panel failed (GL error 0x%x)", err); s.failed = true; return false; }
    return true;
}
}  // namespace GlOverlay
