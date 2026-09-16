// Exercise the real compositor/draw path with a tiny mapped image and Vulkan
// call counters. No GPU or running game is needed.
#include "../src/overlay_vk_linux.cpp"
#include <cassert>
static uint8_t pixels[16]{}, output[24]{};
static bool dirty = false;
static HoverState hover;
static int copies = 0, submits = 0, flushes = 0;
namespace layer {
int Width() { return 2; }
int Height() { return 2; }
const uint8_t* Pixels() { return pixels; }
}
namespace panel {
bool Frame(int, int, int* x, int* y, int* w, int* h, bool* changed) {
    *x = *y = 0; *w = *h = 2; *changed = dirty; return true;
}
void MaxSize(int, int, int* w, int* h) { *w = *h = 2; }
bool Hover(int* x, int* y, int* w, int* h, bool* pressed) {
    *x = hover.x; *y = hover.y; *w = hover.w; *h = hover.h; *pressed = hover.pressed;
    return hover.w != 0;
}
}
int main() {
    g_panelPtr = output; g_panelPitch = 12; g_imgW = g_imgH = 2;
    g_imagesBuilt = true; g_scImgCount = 1; g_scExtent = {2,2};
    g_panelImg = (VkImage)1; g_scImages[0] = (VkImage)2;
    pResetCB = [](VkCommandBuffer, VkCommandBufferResetFlags) { return VK_SUCCESS; };
    pBeginCB = [](VkCommandBuffer, const VkCommandBufferBeginInfo*) { return VK_SUCCESS; };
    pEndCB = [](VkCommandBuffer) { return VK_SUCCESS; };
    pCmdBarrier = [](VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags,
        uint32_t, const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t, const VkImageMemoryBarrier*) {};
    pCmdCopyImage = [](VkCommandBuffer, VkImage src, VkImageLayout, VkImage dst, VkImageLayout,
        uint32_t, const VkImageCopy*) { assert(src == g_panelImg && dst == g_scImages[0]); ++copies; };
    pResetFences = [](VkDevice, uint32_t, const VkFence*) { return VK_SUCCESS; };
    pSubmit = [](VkQueue, uint32_t, const VkSubmitInfo*, VkFence) { ++submits; return VK_SUCCESS; };
    pWaitFences = [](VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t) { return VK_SUCCESS; };
    pFlush = [](VkDevice, uint32_t, const VkMappedMemoryRange*) { ++flushes; return VK_SUCCESS; };
    DrawPanel(VK_NULL_HANDLE,0);
    assert(output[0]==40 && output[1]==25 && output[2]==5 && output[3]==255);
    assert(output[8]==0 && output[12]==40); // honor row pitch
    DrawPanel(VK_NULL_HANDLE,0);
    assert(copies==2 && submits==2 && flushes==1); // one upload, no readback
    pixels[0]=10; pixels[1]=20; pixels[2]=30; pixels[3]=255; dirty=true;
    DrawPanel(VK_NULL_HANDLE,0); dirty=false;
    assert(output[0]==10 && output[1]==20 && output[2]==30 && flushes==2);
    hover={1,1,1,1,false}; DrawPanel(VK_NULL_HANDLE,0);
    const int light=output[16]; assert(light>40 && flushes==3);
    hover.pressed=true; DrawPanel(VK_NULL_HANDLE,0); assert(output[16]>light && flushes==4);
    hover={}; DrawPanel(VK_NULL_HANDLE,0); assert(output[16]==40 && flushes==5);
    g_rgbaOrder=true; g_composed=false; DrawPanel(VK_NULL_HANDLE,0);
    assert(output[0]==30 && output[2]==10 && output[16]==5 && output[18]==40);
    assert(copies==7 && submits==7 && flushes==6);
}
