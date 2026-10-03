// descriptor_recycle.h -- descriptor sets kept across pool resets while a
// dedicated server renders nothing (dedicated_render=0). Shared by the Linux
// menu (overlay_vk_linux.cpp, the dispatcher slots) and the Windows one
// (menu_hook.cpp, the hooked vkGetDeviceProcAddr); under Proton the Windows
// game reaches the same lavapipe.
//
// WHY. The game resets its descriptor pools every frame and allocates ~260 sets
// again. lavapipe backs each set with its own 4 KiB mapping of a memfd: an mmap
// per allocation, a munmap per set on reset. On the dedicated server that was
// 7,200 of each a second, a quarter of the main thread, and a TLB shootdown IPI
// to every CPU the process runs on for each munmap (2026-09-27). With the
// command buffers dropped at submit, no set is ever read by a draw, so a set
// handed back by a reset can be handed out again for the same layout.
//
// THE RULES.
//   - A reset moves the pool's sets to its per-layout spare lists instead of
//     calling the driver; an allocation takes from those lists first and asks the
//     driver only for what is missing.
//   - A pool that holds a set this code does not track (an allocation with a
//     pNext chain, an allocation that threw) gets real resets until one clears it.
//   - Every `realEvery`-th reset of a pool is real, which bounds what a pool keeps
//     when the game's per-frame mix of layouts shifts.
//   - A set is never handed out after its layout is destroyed: the spec forbids
//     updating it then, and lavapipe would write through the dead layout.
//   - A failed allocation returns the spares it took and leaves every output
//     VK_NULL_HANDLE, as the spec requires of the driver.
#pragma once
#include "../third_party/vk/vulkan_core.h"
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dsrecycle {

struct Real {
    PFN_vkAllocateDescriptorSets alloc = nullptr;
    PFN_vkFreeDescriptorSets free = nullptr;
    PFN_vkResetDescriptorPool reset = nullptr;
    PFN_vkDestroyDescriptorPool destroyPool = nullptr;
    PFN_vkDestroyDescriptorSetLayout destroyLayout = nullptr;
};

struct Stats { uint64_t reused = 0, allocated = 0, kept = 0, realResets = 0; };

class Recycler {
public:
    Recycler(Real real, uint32_t realEvery) : real_(real), realEvery_(realEvery ? realEvery : 1) {}

    VkResult Allocate(VkDevice dev, const VkDescriptorSetAllocateInfo* info, VkDescriptorSet* out)
    {
        if (!info || !out || !info->descriptorSetCount || !info->pSetLayouts) return real_.alloc(dev, info, out);
        std::lock_guard<std::mutex> lock(mu_);
        Pool* p = nullptr;
        try { p = &pools_[info->descriptorPool]; } catch (...) { return real_.alloc(dev, info, out); }
        if (info->pNext) { p->untracked = true; return real_.alloc(dev, info, out); }
        const uint32_t n = info->descriptorSetCount;
        try {
            missing_.clear(); layouts_.clear();
            missing_.reserve(n); layouts_.reserve(n); p->live.reserve(p->live.size() + n);
            for (uint32_t i = 0; i < n; ++i) {
                auto it = p->spare.find(info->pSetLayouts[i]);
                if (it != p->spare.end() && !it->second.empty()) { out[i] = it->second.back(); it->second.pop_back(); }
                else { out[i] = VK_NULL_HANDLE; missing_.push_back(i); layouts_.push_back(info->pSetLayouts[i]); }
            }
            if (!missing_.empty()) {
                fresh_.assign(missing_.size(), VK_NULL_HANDLE);
                VkDescriptorSetAllocateInfo ask = *info;
                ask.descriptorSetCount = uint32_t(missing_.size());
                ask.pSetLayouts = layouts_.data();
                const VkResult r = real_.alloc(dev, &ask, fresh_.data());
                if (r != VK_SUCCESS) {
                    for (uint32_t i = 0; i < n; ++i) {
                        if (out[i] != VK_NULL_HANDLE) p->spare[info->pSetLayouts[i]].push_back(out[i]);
                        out[i] = VK_NULL_HANDLE;
                    }
                    return r;
                }
                for (size_t k = 0; k < missing_.size(); ++k) out[missing_[k]] = fresh_[k];
                stats_.allocated += missing_.size();
            }
            stats_.reused += n - missing_.size();
            for (uint32_t i = 0; i < n; ++i) p->live.emplace_back(info->pSetLayouts[i], out[i]);
            return VK_SUCCESS;
        } catch (...) {
            // Whatever was handed out stays allocated in the pool; the next reset is real.
            p->untracked = true;
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }

    VkResult Free(VkDevice dev, VkDescriptorPool pool, uint32_t count, const VkDescriptorSet* sets)
    {
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = pools_.find(pool);
            if (it != pools_.end() && sets) {
                auto& live = it->second.live;
                for (uint32_t i = 0; i < count; ++i)
                    for (size_t k = 0; k < live.size(); ++k)
                        if (live[k].second == sets[i]) { live[k] = live.back(); live.pop_back(); break; }
            }
        }
        return real_.free(dev, pool, count, sets);
    }

    VkResult Reset(VkDevice dev, VkDescriptorPool pool, VkDescriptorPoolResetFlags flags)
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = pools_.find(pool);
        if (it == pools_.end()) return real_.reset(dev, pool, flags);
        Pool& p = it->second;
        if (p.untracked || flags || ++p.resets % realEvery_ == 0) {
            pools_.erase(it);
            ++stats_.realResets;
            return real_.reset(dev, pool, flags);
        }
        try {
            for (auto& ls : p.live) p.spare[ls.first].push_back(ls.second);
        } catch (...) {
            pools_.erase(it);
            ++stats_.realResets;
            return real_.reset(dev, pool, flags);
        }
        stats_.kept += p.live.size();
        p.live.clear();
        return VK_SUCCESS;
    }

    void DestroyPool(VkDevice dev, VkDescriptorPool pool, const VkAllocationCallbacks* a)
    {
        { std::lock_guard<std::mutex> lock(mu_); pools_.erase(pool); }
        real_.destroyPool(dev, pool, a);
    }

    void DestroyLayout(VkDevice dev, VkDescriptorSetLayout layout, const VkAllocationCallbacks* a)
    {
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (auto& entry : pools_) {
                Pool& p = entry.second;
                bool held = p.spare.erase(layout) != 0;
                for (size_t k = 0; k < p.live.size();)
                    if (p.live[k].first == layout) { p.live[k] = p.live.back(); p.live.pop_back(); held = true; }
                    else ++k;
                if (held) p.untracked = true;   // those sets are only reclaimed by a real reset
            }
        }
        real_.destroyLayout(dev, layout, a);
    }

    Stats Snapshot() { std::lock_guard<std::mutex> lock(mu_); return stats_; }

private:
    struct Pool {
        std::vector<std::pair<VkDescriptorSetLayout, VkDescriptorSet>> live;   // handed out since the last reset
        std::unordered_map<VkDescriptorSetLayout, std::vector<VkDescriptorSet>> spare;
        bool untracked = false;
        uint32_t resets = 0;
    };
    Real real_;
    uint32_t realEvery_;
    std::mutex mu_;
    std::unordered_map<VkDescriptorPool, Pool> pools_;
    std::vector<uint32_t> missing_;
    std::vector<VkDescriptorSetLayout> layouts_;
    std::vector<VkDescriptorSet> fresh_;
    Stats stats_;
};

} // namespace dsrecycle
