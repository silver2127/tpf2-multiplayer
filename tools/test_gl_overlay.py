"""The OpenGL path of the Multiplayer panel (native/src/gl_overlay.h) against a real
OpenGL context on this PC, no game.

  1. the panel lands where it should: its top-left pixel at (x, y) measured from the
     TOP of the target (GL's origin is bottom-left, the blit flips), a pitch wider than
     the panel is honoured, and nothing outside the rectangle is touched
  2. every piece of GL state it touches comes back as the "game" left it: read and draw
     framebuffers, the 2D texture binding, a bound pixel-unpack buffer, the four unpack
     parameters, the scissor test and sRGB -- deliberately all set to odd values first
  3. a frame without an upload re-blits the last panel; a second upload replaces it

    python tools\\test_gl_overlay.py
"""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / '.local-test/tests/gl-overlay'
out.mkdir(parents=True, exist_ok=True)
code = r'''
#include <windows.h>
#include <cstdio>
#include <vector>
#include "GL_OVERLAY"
using namespace GlOverlay;
static int fails = 0;
#define CHECK(c, msg) do { if (c) printf("ok   %s\n", msg); else { printf("FAIL %s\n", msg); ++fails; } } while (0)

typedef void (WINAPI* ClearColorFn)(float, float, float, float);
typedef void (WINAPI* ClearFn)(GLbitfield);
typedef void (WINAPI* ReadPixelsFn)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
typedef void (WINAPI* GenBuffersFn)(GLsizei, GLuint*);
typedef void (WINAPI* BufferDataFn)(GLenum, ptrdiff_t, const void*, GLenum);

int main() {
    WNDCLASSW wc = {}; wc.style = CS_OWNDC; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"gltest";
    RegisterClassW(&wc);
    HWND wnd = CreateWindowW(L"gltest", L"gltest", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    HDC dc = GetDC(wnd);
    PIXELFORMATDESCRIPTOR pfd = { sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32 };
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    HGLRC rc = wglCreateContext(dc);
    if (!rc || !wglMakeCurrent(dc, rc)) { puts("SKIP: no OpenGL context on this machine"); return 0; }
    HMODULE gl = GetModuleHandleW(L"opengl32.dll");
    auto clearColor = (ClearColorFn)GetProcAddress(gl, "glClearColor");
    auto clear = (ClearFn)GetProcAddress(gl, "glClear");
    auto readPixels = (ReadPixelsFn)GetProcAddress(gl, "glReadPixels");
    auto genBuffers = (GenBuffersFn)WglProc("glGenBuffers");
    auto bufferData = (BufferDataFn)WglProc("glBufferData");

    State s;
    CHECK(Load(s), "every GL function resolves (3.0+)");
    if (!s.loaded) { printf("  why: %s\n", s.why); return 1; }
    const Fns& f = s.f;

    // the "window": a 200 x 100 target, cleared to a known colour
    const int FW = 200, FH = 100;
    GLuint target = 0, targetTex = 0;
    f.GenTextures(1, &targetTex); f.BindTexture(TEXTURE_2D, targetTex);
    f.TexImage2D(TEXTURE_2D, 0, RGBA8, FW, FH, 0, BGRA, UNSIGNED_BYTE, nullptr);
    f.GenFramebuffers(1, &target); f.BindFramebuffer(DRAW_FRAMEBUFFER, target);
    f.FramebufferTexture2D(DRAW_FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, targetTex, 0);
    clearColor(0.f, 0.f, 1.f, 1.f); clear(COLOR_BUFFER_BIT);    // BGRA bytes 255,0,0,255

    // the panel: 64 x 32, pitch 80 pixels, each pixel encodes its own (x, y)
    const int PW = 64, PH = 32, PITCH = 80 * 4;
    std::vector<unsigned char> px(PITCH * PH, 0xEE);
    for (int y = 0; y < PH; ++y) for (int x = 0; x < PW; ++x) {
        unsigned char* p = &px[y * PITCH + x * 4]; p[0] = (unsigned char)x; p[1] = (unsigned char)y; p[2] = 77; p[3] = 255; }
    CHECK(Ensure(s, 96, 48), "a 96 x 48 panel texture and its read framebuffer");

    // the "game's" state, deliberately odd
    GLuint gameRead = 0, gameDraw = 0, gameTex = 0, pbo = 0;
    f.GenFramebuffers(1, &gameRead); f.GenFramebuffers(1, &gameDraw); f.GenTextures(1, &gameTex);
    genBuffers(1, &pbo); f.BindBuffer(PIXEL_UNPACK_BUFFER, pbo); bufferData(PIXEL_UNPACK_BUFFER, 4096, nullptr, 0x88E0 /*STREAM_DRAW*/);
    f.BindFramebuffer(READ_FRAMEBUFFER, gameRead); f.BindFramebuffer(DRAW_FRAMEBUFFER, gameDraw); f.BindTexture(TEXTURE_2D, gameTex);
    f.PixelStorei(UNPACK_ALIGNMENT, 1); f.PixelStorei(UNPACK_ROW_LENGTH, 7); f.PixelStorei(UNPACK_SKIP_ROWS, 2); f.PixelStorei(UNPACK_SKIP_PIXELS, 3);
    f.Enable(SCISSOR_TEST); f.Enable(FRAMEBUFFER_SRGB);
    while (f.GetError() != GL_OK) {}

    const int X = 10, Y = 5;
    bool drew = Draw(s, px.data(), PITCH, PW, PH, true, X, Y, FH, target);
    CHECK(drew, "draw (upload + blit) succeeds");
    if (!drew) printf("  why: %s\n", s.why);

    Saved after; Save(f, after);
    CHECK(after.readFb == (GLint)gameRead && after.drawFb == (GLint)gameDraw, "read and draw framebuffers restored");
    CHECK(after.tex == (GLint)gameTex, "2D texture binding restored");
    CHECK(after.unpackBuf == (GLint)pbo, "the pixel-unpack buffer binding restored");
    CHECK(after.align == 1 && after.rowLen == 7 && after.skipRows == 2 && after.skipPixels == 3, "the four unpack parameters restored");
    CHECK(after.scissor && after.srgb, "scissor test and sRGB left enabled, as found");

    // read the target back (bottom row first, as GL returns it)
    std::vector<unsigned char> img(FW * FH * 4);
    f.BindBuffer(PIXEL_UNPACK_BUFFER, 0);
    f.BindFramebuffer(READ_FRAMEBUFFER, target);
    readPixels(0, 0, FW, FH, BGRA, UNSIGNED_BYTE, img.data());
    auto at = [&](int x, int yTop) { return &img[((FH - 1 - yTop) * FW + x) * 4]; };   // y from the top
    bool placed = true, outside = true;
    for (int yt = 0; yt < FH; ++yt) for (int x = 0; x < FW; ++x) {
        const unsigned char* p = at(x, yt);
        const bool inside = x >= X && x < X + PW && yt >= Y && yt < Y + PH;
        if (inside) { if (p[0] != x - X || p[1] != yt - Y || p[2] != 77) placed = false; }
        else if (!(p[0] == 255 && p[1] == 0 && p[2] == 0)) outside = false;
    }
    CHECK(placed, "every panel pixel lands at (x, y) from the top, the right way up, pitch honoured");
    CHECK(outside, "nothing outside the panel rectangle is touched");

    // a frame without an upload keeps the image; a second upload replaces it
    f.BindFramebuffer(DRAW_FRAMEBUFFER, target); f.Disable(SCISSOR_TEST); clear(COLOR_BUFFER_BIT);
    CHECK(Draw(s, nullptr, PITCH, PW, PH, false, X, Y, FH, target), "a frame without an upload re-blits");
    f.BindFramebuffer(READ_FRAMEBUFFER, target); readPixels(0, 0, FW, FH, BGRA, UNSIGNED_BYTE, img.data());
    CHECK(at(X + 5, Y + 3)[0] == 5 && at(X + 5, Y + 3)[1] == 3, "the last panel is still there");
    for (auto& b : px) if (b == 77) b = 99;
    CHECK(Draw(s, px.data(), PITCH, PW, PH, true, X, Y, FH, target), "a second upload");
    f.BindFramebuffer(READ_FRAMEBUFFER, target); readPixels(0, 0, FW, FH, BGRA, UNSIGNED_BYTE, img.data());
    CHECK(at(X + 1, Y + 1)[2] == 99, "the new panel replaced the old");
    CHECK(!Draw(s, px.data(), PITCH, 97, PH, true, X, Y, FH, target), "a panel wider than its texture is refused, not drawn");

    wglMakeCurrent(nullptr, nullptr); wglDeleteContext(rc); ReleaseDC(wnd, dc); DestroyWindow(wnd);
    if (fails) { printf("%d FAILED\n", fails); return 1; }
    puts("PASS: the OpenGL panel blit lands exactly, flips right, restores every state it touches, and re-blits without re-uploading");
    return 0;
}
'''
code = code.replace('GL_OVERLAY', (root / 'native/src/gl_overlay.h').as_posix())
(out / 'test.cpp').write_text(code)
vcvars = root / 'tools' / 'msvc_env.bat'
(out / 'build.cmd').write_text(f'@echo off\ncall "{vcvars}" || exit /b 1\n'
    'cl /nologo /EHsc /W4 test.cpp /Fe:test.exe opengl32.lib user32.lib gdi32.lib >build.log 2>&1\n'
    'if errorlevel 1 (type build.log & exit /b 1)\nexit /b 0\n')
subprocess.run(['cmd', '/d', '/c', str(out / 'build.cmd')], cwd=out, check=True, timeout=180)
subprocess.run([str(out / 'test.exe')], cwd=out, check=True, timeout=30)
