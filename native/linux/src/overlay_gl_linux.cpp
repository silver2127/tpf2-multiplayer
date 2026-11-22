// Linux build 35924: SDL_GL_SwapWindow call, docs/re/linux/DEV_2B466E3.md.
#include "../../src/gl_overlay.h"
#include "near_alloc.h"
#include "panel.h"
#include "panel_layer.h"
#include <vector>

namespace {
using Swap = void (*)(void*);
Swap original;
Tpf2mpLogFn logger;
void Present(void* window) {
    using Size = void (*)(void*, int*, int*);
    using Context = void* (*)();
    static auto size = reinterpret_cast<Size>(dlsym(RTLD_DEFAULT, "SDL_GL_GetDrawableSize"));
    static auto context = reinterpret_cast<Context>(dlsym(RTLD_DEFAULT, "SDL_GL_GetCurrentContext"));
    static thread_local void* lastContext = nullptr;
    static thread_local GlOverlay::State state;
    static thread_local std::vector<unsigned char> pixels;
    panel::InstallInput();
    if (size && context && panel::Visible()) {
        void* current = context();
        if (current != lastContext) { state = GlOverlay::State(); lastContext = current; }
        int sw=0, sh=0, x=0, y=0, w=0, h=0; bool changed=false;
        size(window, &sw, &sh);
        if (current && panel::Frame(sw, sh, &x, &y, &w, &h, &changed) &&
            GlOverlay::Load(state) && GlOverlay::Ensure(state, w, h)) {
            const auto* layerPixels = layer::Pixels();
            pixels.resize(size_t(w)*h*4);
            int hx=0,hy=0,hw=0,hh=0; bool pressed=false;
            panel::Hover(&hx,&hy,&hw,&hh,&pressed);
            for (int py=0; py<h; ++py) for (int px=0; px<w; ++px) {
                size_t i=(size_t(py)*w+px)*4;
                const int alpha=layerPixels[i+3];
                for (int c=0; c<3; ++c) {
                    int bg=c==0?40:c==1?25:5;
                    if(px>=hx && px<hx+hw && py>=hy && py<hy+hh) bg+=(255-bg)*(pressed?100:50)/255;
                    pixels[i+c]=(bg*(255-alpha)+layerPixels[i+c]*alpha)/255;
                }
                pixels[i+3]=255;
            }
            if (!GlOverlay::Draw(state,pixels.data(),size_t(w)*4,w,h,true,x,y,sh))
                logger("[overlay-gl] disabled: %s\n",state.why);
        }
    }
    original(window);
}
}
bool OverlayGlInstall(uintptr_t base, Tpf2mpLogFn log) {
    logger=log;
    original=reinterpret_cast<Swap>(base+0x6ddb70);
    // Verify the PLT jump's relocation slot as well as the call instruction.
    static const unsigned char plt[]={0xff,0x25,0xc2,0xa0,0x36,0x05};
    if(memcmp(reinterpret_cast<void*>(base+0x6ddb70),plt,sizeof plt) ||
       !Tpf2mpRedirectCall(base+0x3496531,base+0x6ddb70,reinterpret_cast<void*>(&Present))) {
        log("[overlay-gl] swap call check failed; overlay off\n"); return false;
    }
    log("[overlay-gl] verified SDL_GL_SwapWindow call routed through panel\n");
    return true;
}
