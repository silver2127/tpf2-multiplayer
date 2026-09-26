// overlay_vk_linux.cpp -- the Multiplayer panel drawn into the game's own Vulkan
// frames (menu_hook.cpp, "Vulkan in-frame overlay").
//
// HOW IT GETS IN. Windows hooks the loader's vkGetDeviceProcAddr. On Linux the
// loader is whatever the Steam runtime or the driver ships -- 1.2, 1.3 and 1.4
// on this one machine, with prologues that cannot all be hooked -- but the game
// is fixed. It resolves every device function into vulkan.hpp's global
// dispatcher (vk::DispatchLoaderDynamic, .bss 0x5b3d740) in init(VkDevice),
// 0x35167d0, and each Vulkan call reads its function pointer from there. init is
// called from exactly one place in the binary, VulkanRenderContext's
// constructor at 0x35096e9, so that call is pointed at InitDeviceDetour, which
// lets init run and then swaps three slots for ours:
//   +0x550 vkCreateSwapchainKHR    the format, extent and usage
//   +0x7a0 vkGetDeviceQueue        which family each queue belongs to
//   +0xad0 vkQueuePresentKHR       draw the panel, then present
// +0x798 is vkGetDeviceProcAddr, used for every other function we call. Each
// slot is compared with a fresh vkGetDeviceProcAddr lookup before anything is
// swapped: a wrong offset leaves the game untouched and says so.
//
// DRAWING uses a cached opaque panel, with no game-frame readback or blur.
#define VK_NO_PROTOTYPES
#include "../../third_party/vk/vulkan_core.h"
#include "near_alloc.h"
#include "panel.h"
#include "panel_layer.h"
#include "dedicated_linux.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <vector>

static const uintptr_t RVA_INIT_DEVICE_DISPATCH = 0x35167d0;   // vk::DispatchLoaderDynamic::init(vk::Device)
static const uintptr_t RVA_INIT_DEVICE_CALL     = 0x35096e9;   // its only call, in VulkanRenderContext's constructor
static const size_t    SLOT_CREATE_SWAPCHAIN     = 0x550;
static const size_t    SLOT_GET_DEVICE_PROC_ADDR = 0x798;
static const size_t    SLOT_GET_DEVICE_QUEUE     = 0x7a0;
static const size_t    SLOT_QUEUE_PRESENT        = 0xad0;
// 0x351814a stores the lookup of vkQueueSubmit here (build 35924).
static const size_t    SLOT_QUEUE_SUBMIT         = 0xae0;

static Tpf2mpLogFn g_log = nullptr;
static uintptr_t   g_base = 0;

static PFN_vkGetDeviceProcAddr   g_gdpa = nullptr;
static PFN_vkQueuePresentKHR     g_realPresent = nullptr;
static PFN_vkGetDeviceQueue      g_origGetQueue = nullptr;
static PFN_vkCreateSwapchainKHR  g_origCreateSc = nullptr;
static PFN_vkQueueSubmit g_origSubmit = nullptr;
static PFN_vkAcquireNextImageKHR g_origAcquire = nullptr;
static PFN_vkAcquireNextImage2KHR g_origAcquire2 = nullptr;
static bool g_noWsi = false;
static VkSwapchainKHR g_nullSc = VK_NULL_HANDLE;
static uint32_t g_nullCount = 0, g_nullNext = 0;
static bool g_noRender = false;
static VkDevice   g_dev = VK_NULL_HANDLE;
static VkFormat   g_scFormat = VK_FORMAT_UNDEFINED;
static VkExtent2D g_scExtent = { 0, 0 };
static VkImageUsageFlags g_scUsage = 0;
static bool       g_rgbaOrder = false;   // swapchain bytes are R,G,B,A (the layer is B,G,R,A)
static std::atomic<uint64_t> g_presentCount{0};

// queue -> family, from the game's vkGetDeviceQueue calls
struct QueueFamily { VkQueue q; uint32_t fam; };
static QueueFamily g_queues[16];
static std::atomic<int> g_queueCount{0};

// resolved device functions
static PFN_vkGetSwapchainImagesKHR pGetImages = nullptr;
static PFN_vkCreateCommandPool  pCreatePool = nullptr;
static PFN_vkAllocateCommandBuffers pAllocCB = nullptr;
static PFN_vkBeginCommandBuffer pBeginCB = nullptr;
static PFN_vkEndCommandBuffer   pEndCB = nullptr;
static PFN_vkQueueSubmit        pSubmit = nullptr;
static PFN_vkCreateFence        pCreateFence = nullptr;
static PFN_vkWaitForFences      pWaitFences = nullptr;
static PFN_vkResetFences        pResetFences = nullptr;
static PFN_vkResetCommandBuffer pResetCB = nullptr;
static PFN_vkCreateImage        pCreateImage = nullptr;
static PFN_vkGetImageMemoryRequirements pImgMemReq = nullptr;
static PFN_vkAllocateMemory     pAllocMem = nullptr;
static PFN_vkBindImageMemory    pBindImgMem = nullptr;
static PFN_vkMapMemory          pMapMem = nullptr;
static PFN_vkGetImageSubresourceLayout pImgSubLayout = nullptr;
static PFN_vkCmdCopyImage       pCmdCopyImage = nullptr;
static PFN_vkCmdPipelineBarrier pCmdBarrier = nullptr;
static PFN_vkFlushMappedMemoryRanges pFlush = nullptr;


// render resources, rebuilt for each swapchain
static bool g_rInit = false, g_rFail = false;
static VkSwapchainKHR g_theSc = VK_NULL_HANDLE;
static VkCommandPool g_pool = VK_NULL_HANDLE;
static VkFence       g_fence = VK_NULL_HANDLE;
static VkImage       g_scImages[8] = {};
static VkCommandBuffer g_cmd[8] = {};
static uint32_t      g_scImgCount = 0;

// Cached opaque panel copied onto the frame.
static VkImage g_panelImg = VK_NULL_HANDLE;
static VkDeviceMemory g_panelMem = VK_NULL_HANDLE;
static void* g_panelPtr = nullptr;
static size_t g_panelPitch = 0;
static bool g_composed = false;
static int g_imgW = 0, g_imgH = 0;
static bool g_imagesBuilt = false, g_imagesFail = false;

template <class T> static T rget(const char* n) { return (T)g_gdpa(g_dev, n); }

static bool FamilyOf(VkQueue q, uint32_t* fam)
{
    const int n = g_queueCount.load();
    for (int i = 0; i < n; i++) if (g_queues[i].q == q) { *fam = g_queues[i].fam; return true; }
    return false;
}

static bool InitRender(VkSwapchainKHR sc, VkQueue q)
{
    if (g_rFail) return false;
    uint32_t fam = 0;
    if (!g_dev || g_scFormat == VK_FORMAT_UNDEFINED || !FamilyOf(q, &fam)) return false;
    pGetImages   = rget<PFN_vkGetSwapchainImagesKHR>("vkGetSwapchainImagesKHR");
    pCreatePool  = rget<PFN_vkCreateCommandPool>("vkCreateCommandPool");
    pAllocCB     = rget<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers");
    pBeginCB     = rget<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer");
    pEndCB       = rget<PFN_vkEndCommandBuffer>("vkEndCommandBuffer");
    pSubmit      = rget<PFN_vkQueueSubmit>("vkQueueSubmit");
    pCreateFence = rget<PFN_vkCreateFence>("vkCreateFence");
    pWaitFences  = rget<PFN_vkWaitForFences>("vkWaitForFences");
    pResetFences = rget<PFN_vkResetFences>("vkResetFences");
    pResetCB     = rget<PFN_vkResetCommandBuffer>("vkResetCommandBuffer");
    pCreateImage = rget<PFN_vkCreateImage>("vkCreateImage");
    pImgMemReq   = rget<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements");
    pAllocMem    = rget<PFN_vkAllocateMemory>("vkAllocateMemory");
    pBindImgMem  = rget<PFN_vkBindImageMemory>("vkBindImageMemory");
    pMapMem      = rget<PFN_vkMapMemory>("vkMapMemory");
    pImgSubLayout= rget<PFN_vkGetImageSubresourceLayout>("vkGetImageSubresourceLayout");
    pCmdCopyImage= rget<PFN_vkCmdCopyImage>("vkCmdCopyImage");
    pCmdBarrier  = rget<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    pFlush       = rget<PFN_vkFlushMappedMemoryRanges>("vkFlushMappedMemoryRanges");

    const bool need[] = { !!pGetImages, !!pCreatePool, !!pAllocCB, !!pBeginCB, !!pEndCB, !!pSubmit, !!pCreateFence,
                          !!pWaitFences, !!pResetFences, !!pResetCB, !!pCreateImage, !!pImgMemReq, !!pAllocMem,
                          !!pBindImgMem, !!pMapMem, !!pImgSubLayout, !!pCmdCopyImage, !!pCmdBarrier };
    for (size_t i = 0; i < sizeof(need) / sizeof(need[0]); i++)
        if (!need[i]) { g_rFail = true; g_log("[overlay] vk: device function %zu did not resolve -- no panel\n", i); return false; }

    uint32_t n = 0;
    pGetImages(g_dev, sc, &n, nullptr);
    if (n == 0 || n > 8) { g_rFail = true; g_log("[overlay] vk: swapchain has %u images -- no panel\n", n); return false; }
    pGetImages(g_dev, sc, &n, g_scImages);
    g_scImgCount = n;
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = fam;
    if (pCreatePool(g_dev, &pci, nullptr, &g_pool) != VK_SUCCESS) { g_rFail = true; return false; }
    VkCommandBufferAllocateInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbi.commandPool = g_pool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = n;
    if (pAllocCB(g_dev, &cbi, g_cmd) != VK_SUCCESS) { g_rFail = true; return false; }
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (pCreateFence(g_dev, &fci, nullptr, &g_fence) != VK_SUCCESS) { g_rFail = true; return false; }
    g_theSc = sc;
    g_rInit = true;
    g_log("[overlay] vk: render resources ready (%u images %ux%u format %d family %u)\n",
          n, g_scExtent.width, g_scExtent.height, (int)g_scFormat, fam);
    return true;
}

static void Barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags srcA, VkAccessFlags dstA)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = from; b.newLayout = to; b.srcAccessMask = srcA; b.dstAccessMask = dstA;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 1;
    pCmdBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

static bool SubmitAndWait(VkQueue q, VkCommandBuffer cb, uint32_t waitCount=0, const VkSemaphore* waits=nullptr)
{
    std::vector<VkPipelineStageFlags> stages(waitCount,VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount=waitCount;si.pWaitSemaphores=waits;si.pWaitDstStageMask=stages.data();
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    if(pResetFences(g_dev,1,&g_fence)!=VK_SUCCESS || pSubmit(q,1,&si,g_fence)!=VK_SUCCESS)return false;
    // Never reset/reuse a pending command buffer after an ignored 100 ms
    // timeout: software rendering can legitimately take longer than that.
    return pWaitFences(g_dev,1,&g_fence,VK_TRUE,UINT64_MAX)==VK_SUCCESS;
}

// A host-visible linear image of the panel's largest size, mapped, in `layout`.
static bool MakeHostImage(VkQueue q, VkImageUsageFlags usage, VkImageLayout layout, VkAccessFlags access,
                          VkImage* img, VkDeviceMemory* mem, void** ptr, size_t* pitch)
{
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = g_scFormat;
    ici.extent = { (uint32_t)g_imgW, (uint32_t)g_imgH, 1 };
    ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_LINEAR; ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ici.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    if (pCreateImage(g_dev, &ici, nullptr, img) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    pImgMemReq(g_dev, *img, &mr);
    // The first allowed memory type that maps is host-visible (no physical device needed).
    for (uint32_t ti = 0; ti < 32 && !*ptr; ti++) {
        if (!(mr.memoryTypeBits & (1u << ti))) continue;
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = mr.size; mai.memoryTypeIndex = ti;
        VkDeviceMemory m = VK_NULL_HANDLE;
        if (pAllocMem(g_dev, &mai, nullptr, &m) != VK_SUCCESS) continue;
        void* p = nullptr;
        if (pMapMem(g_dev, m, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS && p) { *mem = m; *ptr = p; }
        // (a type that allocates but does not map leaks its allocation; a one-shot)
    }
    if (!*ptr) return false;
    pBindImgMem(g_dev, *img, *mem, 0);
    VkImageSubresource sub = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
    VkSubresourceLayout sl;
    pImgSubLayout(g_dev, *img, &sub, &sl);
    *pitch = (size_t)sl.rowPitch;
    VkCommandBuffer cb = g_cmd[0];
    pResetCB(cb, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    pBeginCB(cb, &bi);
    Barrier(cb, *img, VK_IMAGE_LAYOUT_PREINITIALIZED, layout, VK_ACCESS_HOST_WRITE_BIT, access);
    pEndCB(cb);
    if(!SubmitAndWait(q, cb))return false;
    return true;
}

static bool BuildImages(VkQueue q)
{
    if (g_imagesBuilt) return true;
    if (g_imagesFail) return false;
    if (g_scUsage && !(g_scUsage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        g_imagesFail = true;
        g_log("[overlay] vk: the swapchain has no TRANSFER_DST (usage 0x%x) -- no panel\n", g_scUsage);
        return false;
    }
    panel::MaxSize((int)g_scExtent.width, (int)g_scExtent.height, &g_imgW, &g_imgH);
    g_panelImg = VK_NULL_HANDLE; g_panelMem = VK_NULL_HANDLE; g_panelPtr = nullptr;
    g_composed = false;
    if (!MakeHostImage(q, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                       &g_panelImg, &g_panelMem, &g_panelPtr, &g_panelPitch)) {
        g_imagesFail = true;
        g_log("[overlay] vk: no host-visible image memory -- no panel\n");
        return false;
    }
    g_imagesBuilt = true;
    g_log("[overlay] vk: panel image ready %dx%d (pitch %zu)\n", g_imgW, g_imgH, g_panelPitch);
    return true;
}

// ---- Cached opaque compositing ------------------------------------------------
struct HoverState { int x = 0, y = 0, w = 0, h = 0; bool pressed = false; };
static HoverState g_lastHover;
static int g_composedW = 0, g_composedH = 0;

// The panel is composed in ordinary memory and then copied row by row into the
// mapped image. That mapping can be device memory (NVIDIA hands out VRAM behind
// the BAR, uncached): reading it back byte by byte took ~2 s for a 3456x2160
// title screen, longer than the 500 ms refresh, so the game froze.
static std::vector<unsigned char> g_composeBuf;

static void Compose(int w, int h, const HoverState& hover)
{
    const size_t rowBytes = (size_t)w * 4;
    g_composeBuf.resize(rowBytes * h);
    unsigned char* const buf = g_composeBuf.data();
    for (int y = 0; y < h; ++y) {
        auto* d = buf + (size_t)y * rowBytes;
        for (int x = 0; x < w; ++x, d += 4) {
            d[0] = g_rgbaOrder ? 5 : 40; d[1] = 25;
            d[2] = g_rgbaOrder ? 40 : 5; d[3] = 255;
            if (x >= hover.x && x < hover.x + hover.w && y >= hover.y && y < hover.y + hover.h) {
                const int fill = hover.pressed ? 100 : 50;
                for (int c = 0; c < 3; ++c) d[c] += (255 - d[c]) * fill / 255;
            }
        }
    }
    // the layer is B,G,R,A; an R,G,B,A swapchain takes it swapped
    const int i0 = g_rgbaOrder ? 2 : 0, i2 = g_rgbaOrder ? 0 : 2;
    const unsigned char* lp = layer::Pixels();
    if (layer::Width() == w && layer::Height() == h) {
        for (int y = 0; y < h; y++) {
            unsigned char* d = buf + (size_t)y * rowBytes; const unsigned char* l = lp + (size_t)y * w * 4;
            for (int x = 0; x < w; x++, d += 4, l += 4) {
                const int a = l[3];
                if (!a) continue;
                d[i0] = (unsigned char)((d[i0] * (255 - a) + l[0] * a) / 255);
                d[1]  = (unsigned char)((d[1]  * (255 - a) + l[1] * a) / 255);
                d[i2] = (unsigned char)((d[i2] * (255 - a) + l[2] * a) / 255);
            }
        }
    }
    for (int y = 0; y < h; ++y)
        memcpy(static_cast<unsigned char*>(g_panelPtr) + (size_t)y * g_panelPitch, buf + (size_t)y * rowBytes, rowBytes);
}

static bool DrawPanel(VkQueue q, uint32_t imgIndex, uint32_t waitCount=0, const VkSemaphore* waits=nullptr)
{
    if (imgIndex >= g_scImgCount) return false;
    int px, py, pw, ph;
    bool changed = false;
    if (!panel::Frame((int)g_scExtent.width, (int)g_scExtent.height, &px, &py, &pw, &ph, &changed)) return false;
    if (!BuildImages(q)) return false;
    if (pw > g_imgW) pw = g_imgW;
    if (ph > g_imgH) ph = g_imgH;
    if (px < 0 || py < 0 || px + pw > (int)g_scExtent.width || py + ph > (int)g_scExtent.height) return false;

    VkImageCopy region = {};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; region.srcSubresource.layerCount = 1;
    region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; region.dstSubresource.layerCount = 1;
    region.extent = { (uint32_t)pw, (uint32_t)ph, 1 };
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkCommandBuffer cb = g_cmd[imgIndex];

    HoverState hover;
    panel::Hover(&hover.x, &hover.y, &hover.w, &hover.h, &hover.pressed);
    if (!g_composed || changed || pw != g_composedW || ph != g_composedH ||
        hover.x != g_lastHover.x || hover.y != g_lastHover.y ||
        hover.w != g_lastHover.w || hover.h != g_lastHover.h || hover.pressed != g_lastHover.pressed) {
        Compose(pw, ph, hover);
        if (pFlush) { VkMappedMemoryRange r = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE }; r.memory = g_panelMem; r.size = VK_WHOLE_SIZE; pFlush(g_dev, 1, &r); }
        g_composed = true; g_composedW = pw; g_composedH = ph; g_lastHover = hover;
    }

    // Copy the cached panel onto this frame.
    pResetCB(cb, 0);
    pBeginCB(cb, &bi);
    Barrier(cb, g_scImages[imgIndex], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    region.srcOffset = { 0, 0, 0 }; region.dstOffset = { px, py, 0 };
    pCmdCopyImage(cb, g_panelImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_scImages[imgIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    Barrier(cb, g_scImages[imgIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    pEndCB(cb);
    return SubmitAndWait(q,cb,waitCount,waits);
}

// ---- the three swapped dispatcher slots --------------------------------------------
static VkResult NullSubmit(VkQueue queue, VkSemaphore signal, VkFence fence,
                           uint32_t count, const VkSemaphore* waits)
{
    if (!queue || !g_origSubmit) return VK_ERROR_INITIALIZATION_FAILED;
    try {
        std::vector<VkPipelineStageFlags> stages(count,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
        VkSubmitInfo info{};info.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;
        info.waitSemaphoreCount=count;info.pWaitSemaphores=waits;info.pWaitDstStageMask=stages.data();
        info.signalSemaphoreCount=signal?1:0;info.pSignalSemaphores=signal?&signal:nullptr;
        return g_origSubmit(queue,1,&info,fence);
    } catch (...) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
}
static VkResult NullAcquire(VkSemaphore signal,VkFence fence,uint32_t* index)
{
    if(!index || !g_nullCount || !g_queueCount.load())return VK_ERROR_INITIALIZATION_FAILED;
    const auto result=NullSubmit(g_queues[0].q,signal,fence,0,nullptr);
    if(result==VK_SUCCESS)*index=g_nullNext++%g_nullCount;
    return result;
}
static VkResult MyAcquire(VkDevice dev,VkSwapchainKHR sc,uint64_t timeout,VkSemaphore signal,VkFence fence,uint32_t* index)
{
    return g_noWsi && sc==g_nullSc?NullAcquire(signal,fence,index):g_origAcquire(dev,sc,timeout,signal,fence,index);
}
static VkResult MyAcquire2(VkDevice dev,const VkAcquireNextImageInfoKHR* info,uint32_t* index)
{
    return g_noWsi && info && info->swapchain==g_nullSc?NullAcquire(info->semaphore,info->fence,index):g_origAcquire2(dev,info,index);
}
static VkResult MyPresent(VkQueue q, const VkPresentInfoKHR* pi)
{
    bool copied=false;
    const uint64_t n = ++g_presentCount;
    if (n == 1) {
        g_log("[overlay] first present (swapchains %u)\n", pi ? pi->swapchainCount : 0);
        panel::InstallInput();
    }
    if (!g_noRender && pi && pi->swapchainCount >= 1 && panel::Visible()) {
        const VkSwapchainKHR sc = pi->pSwapchains[0];
        if ((!g_rInit || sc != g_theSc) && !g_rFail) InitRender(sc, q);
        if (g_rInit && sc == g_theSc) copied=DrawPanel(q,pi->pImageIndices[0],pi->waitSemaphoreCount,pi->pWaitSemaphores);
    }
    if (dedicated::Get().enabled) {
        timespec ts{}; clock_gettime(CLOCK_MONOTONIC, &ts);
        const uint64_t now = uint64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
        static uint64_t logged = 0, next = 0;
        if (now - logged >= 5000000000ULL) {
            logged = now;
            g_log("[dedicated] present heartbeat frames=%llu render=%d\n", (unsigned long long)n, !g_noRender);
        }
        const uint64_t period = 1000000000ULL / unsigned(dedicated::Get().fps);
        if (!next || now > next + period * 4) next = now;
        next += period;
        const timespec deadline{time_t(next / 1000000000), long(next % 1000000000)};
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr) == EINTR) {}
    }
    if(copied){
        // The copy waited for the game's completed frame and its fence has
        // completed. Binary semaphores must not be waited a second time.
        VkPresentInfoKHR ready=*pi;ready.waitSemaphoreCount=0;ready.pWaitSemaphores=nullptr;
        return g_realPresent(q,&ready);
    }
    if(g_noWsi && pi && pi->swapchainCount==1 && pi->pSwapchains[0]==g_nullSc){
        const auto result=NullSubmit(q,VK_NULL_HANDLE,VK_NULL_HANDLE,pi->waitSemaphoreCount,pi->pWaitSemaphores);
        if(pi->pResults)pi->pResults[0]=result;
        return result;
    }
    return g_realPresent(q, pi);
}

static VkResult MySubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence)
{
    if (!count || !submits) return g_origSubmit(queue, count, submits, fence);
    // Preserve the real fence, waits and signals. Only GPU command buffers are
    // omitted, matching Windows dedicated_render=0.
    try {
        std::vector<VkSubmitInfo> copy(submits, submits + count);
        for (auto& item : copy) { item.commandBufferCount = 0; item.pCommandBuffers = nullptr; }
        return g_origSubmit(queue, count, copy.data(), fence);
    } catch (...) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
}
static VkResult MyQueryResults(VkDevice, VkQueryPool, uint32_t, uint32_t,
                              size_t size, void* data, VkDeviceSize, VkQueryResultFlags)
{
    // The discarded command buffers never write query results; a real WAIT
    // would block forever. The game's render profiler receives zero timings.
    if (data && size) memset(data, 0, size);
    return VK_SUCCESS;
}

static void** FindDeviceSlot(void** slots, PFN_vkVoidFunction value)
{
    if (!value) return nullptr;
    void** found = nullptr;
    // The verified QueueSubmit store bounds the range. Resolve pointers and
    // require uniqueness before changing any slot.
    for (size_t i = 0; i <= SLOT_QUEUE_SUBMIT / sizeof(void*); ++i) {
        if (slots[i] != reinterpret_cast<void*>(value)) continue;
        if (found) return nullptr;
        found = &slots[i];
    }
    return found;
}

static void MyGetDeviceQueue(VkDevice dev, uint32_t fam, uint32_t idx, VkQueue* pq)
{
    g_origGetQueue(dev, fam, idx, pq);
    if (!pq || !*pq) return;
    const int n = g_queueCount.load();
    for (int i = 0; i < n; i++) if (g_queues[i].q == *pq) return;
    if (n < 16) { g_queues[n] = { *pq, fam }; g_queueCount = n + 1; }
}

static VkResult MyCreateSwapchain(VkDevice dev, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks* a, VkSwapchainKHR* sc)
{
    const VkResult r = g_origCreateSc(dev, ci, a, sc);
    if (r == VK_SUCCESS && ci) {
        if(g_noWsi && sc){
            const auto images=reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(g_gdpa(dev,"vkGetSwapchainImagesKHR"));
            uint32_t count=0;
            if(images && images(dev,*sc,&count,nullptr)==VK_SUCCESS && count){
                g_nullSc=*sc;g_nullCount=count;g_nullNext=0;
            }
        }
        g_scFormat = ci->imageFormat;
        g_scExtent = ci->imageExtent;
        g_scUsage = ci->imageUsage;
        g_rgbaOrder = ci->imageFormat == VK_FORMAT_R8G8B8A8_UNORM || ci->imageFormat == VK_FORMAT_R8G8B8A8_SRGB;
        // rebuilt on the next present against the new format and size (the old
        // resources are not freed: a rare event, and freeing under an in-flight
        // present is the riskier bug -- menu_hook.cpp, myCreateSwapchain)
        g_rInit = false; g_rFail = false; g_imagesBuilt = false; g_imagesFail = false;
        g_log("[overlay] swapchain format %d %ux%u usage 0x%x\n", (int)ci->imageFormat,
              ci->imageExtent.width, ci->imageExtent.height, ci->imageUsage);
    }
    return r;
}

using InitDeviceFn = void (*)(void* dispatcher, VkDevice dev);

static void InitDeviceDetour(void* dispatcher, VkDevice dev)
{
    ((InitDeviceFn)(g_base + RVA_INIT_DEVICE_DISPATCH))(dispatcher, dev);
    void** slot = (void**)dispatcher;
    const auto gdpa = (PFN_vkGetDeviceProcAddr)slot[SLOT_GET_DEVICE_PROC_ADDR / 8];
    const bool ok = gdpa
        && slot[SLOT_QUEUE_PRESENT / 8]    == (void*)gdpa(dev, "vkQueuePresentKHR")
        && slot[SLOT_CREATE_SWAPCHAIN / 8] == (void*)gdpa(dev, "vkCreateSwapchainKHR")
        && slot[SLOT_GET_DEVICE_QUEUE / 8] == (void*)gdpa(dev, "vkGetDeviceQueue")
        && slot[SLOT_QUEUE_PRESENT / 8] && slot[SLOT_CREATE_SWAPCHAIN / 8] && slot[SLOT_GET_DEVICE_QUEUE / 8];
    if (!ok) {
        g_log("[overlay] the dispatcher's slots do not match vkGetDeviceProcAddr -- no panel (the game is untouched)\n");
        return;
    }
    g_gdpa = gdpa;
    g_dev = dev;
    g_realPresent  = (PFN_vkQueuePresentKHR)slot[SLOT_QUEUE_PRESENT / 8];
    g_origCreateSc = (PFN_vkCreateSwapchainKHR)slot[SLOT_CREATE_SWAPCHAIN / 8];
    g_origGetQueue = (PFN_vkGetDeviceQueue)slot[SLOT_GET_DEVICE_QUEUE / 8];
    g_queueCount = 0;
    g_rInit = false; g_rFail = false; g_imagesBuilt = false; g_imagesFail = false;
    slot[SLOT_GET_DEVICE_QUEUE / 8] = (void*)&MyGetDeviceQueue;
    slot[SLOT_CREATE_SWAPCHAIN / 8] = (void*)&MyCreateSwapchain;
    slot[SLOT_QUEUE_PRESENT / 8]    = (void*)&MyPresent;
    g_noRender = false;
    g_noWsi = false;
    if (dedicated::Get().enabled && !dedicated::Get().render) {
        const auto submit = gdpa(dev, "vkQueueSubmit");
        const auto query = gdpa(dev, "vkGetQueryPoolResults");
        void** submitSlot = FindDeviceSlot(slot, submit);
        void** querySlot = FindDeviceSlot(slot, query);
        if (submitSlot && querySlot) {
            g_origSubmit = reinterpret_cast<PFN_vkQueueSubmit>(submit);
            *submitSlot = reinterpret_cast<void*>(&MySubmit);
            *querySlot = reinterpret_cast<void*>(&MyQueryResults);
            g_noRender = true;
            if(dedicated::Get().noWsi){
                const auto acquire=gdpa(dev,"vkAcquireNextImageKHR");
                const auto acquire2=gdpa(dev,"vkAcquireNextImage2KHR");
                auto first=FindDeviceSlot(slot,acquire),second=FindDeviceSlot(slot,acquire2);
                if(first){
                    g_origAcquire=reinterpret_cast<PFN_vkAcquireNextImageKHR>(acquire);
                    *first=reinterpret_cast<void*>(&MyAcquire);
                    if(second){g_origAcquire2=reinterpret_cast<PFN_vkAcquireNextImage2KHR>(acquire2);*second=reinterpret_cast<void*>(&MyAcquire2);}
                    g_noWsi=true;
                    g_log("[dedicated] experimental dedicated_nowsi=1: acquisition and presentation use empty synchronization submits\n");
                }
            }
            g_log("[dedicated] render submissions and query waits disabled; Vulkan synchronization preserved\n");
        } else g_log("[dedicated] render suppression unavailable: Vulkan dispatcher slots are not unique\n");
    }
    g_log("[overlay] device %p: present, swapchain creation and queue lookup routed through the panel\n", (void*)dev);
}

bool OverlayInstall(uintptr_t gameBase, Tpf2mpLogFn log)
{
    g_log = log;
    g_base = gameBase;
    if (!Tpf2mpRedirectCall(gameBase + RVA_INIT_DEVICE_CALL, gameBase + RVA_INIT_DEVICE_DISPATCH, (void*)&InitDeviceDetour)) {
        log("[overlay] the dispatcher setup call at %lx is not the one build 35924 has (or no stub fits near it) -- no panel\n",
            (unsigned long)RVA_INIT_DEVICE_CALL);
        return false;
    }
    log("[overlay] dispatcher setup at %lx redirected: the panel draws into the game's frames\n", (unsigned long)RVA_INIT_DEVICE_CALL);
    return true;
}
