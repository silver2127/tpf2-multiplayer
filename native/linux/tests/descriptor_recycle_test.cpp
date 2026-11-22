// descriptor_recycle.h against a fake driver that tracks which sets each
// pool really holds, and refuses once a pool's capacity is reached.
#define VK_NO_PROTOTYPES
#include "descriptor_recycle.h"
#include <cassert>
#include <map>
#include <set>

namespace {
struct FakePool { std::set<uint64_t> sets; uint32_t cap = 1000; };
std::map<uint64_t, FakePool> pools;
std::map<uint64_t, uint64_t> layoutOf;   // set -> layout it was allocated with
std::set<uint64_t> deadLayouts;
uint64_t nextSet = 1000;
int allocCalls = 0, resetCalls = 0, freeCalls = 0;

VkDescriptorPool P(uint64_t v) { return reinterpret_cast<VkDescriptorPool>(v); }
VkDescriptorSetLayout L(uint64_t v) { return reinterpret_cast<VkDescriptorSetLayout>(v); }
uint64_t U(const void* h) { return reinterpret_cast<uint64_t>(h); }

VkResult Alloc(VkDevice, const VkDescriptorSetAllocateInfo* i, VkDescriptorSet* out)
{
    ++allocCalls;
    auto& p = pools[U(i->descriptorPool)];
    if (p.sets.size() + i->descriptorSetCount > p.cap) {
        for (uint32_t k = 0; k < i->descriptorSetCount; ++k) out[k] = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_POOL_MEMORY;
    }
    for (uint32_t k = 0; k < i->descriptorSetCount; ++k) {
        assert(!deadLayouts.count(U(i->pSetLayouts[k])));
        const uint64_t s = nextSet++;
        p.sets.insert(s); layoutOf[s] = U(i->pSetLayouts[k]);
        out[k] = reinterpret_cast<VkDescriptorSet>(s);
    }
    return VK_SUCCESS;
}
VkResult Free(VkDevice, VkDescriptorPool pool, uint32_t n, const VkDescriptorSet* s)
{
    ++freeCalls;
    for (uint32_t k = 0; k < n; ++k) assert(pools[U(pool)].sets.erase(U(s[k])) == 1);
    return VK_SUCCESS;
}
VkResult Reset(VkDevice, VkDescriptorPool pool, VkDescriptorPoolResetFlags) { ++resetCalls; pools[U(pool)].sets.clear(); return VK_SUCCESS; }
void DestroyPool(VkDevice, VkDescriptorPool pool, const VkAllocationCallbacks*) { pools.erase(U(pool)); }
void DestroyLayout(VkDevice, VkDescriptorSetLayout l, const VkAllocationCallbacks*) { deadLayouts.insert(U(l)); }

VkResult Ask(dsrecycle::Recycler& r, uint64_t pool, std::vector<uint64_t> layouts, std::vector<VkDescriptorSet>* out,
             const void* pNext = nullptr)
{
    std::vector<VkDescriptorSetLayout> ls;
    for (auto l : layouts) ls.push_back(L(l));
    VkDescriptorSetAllocateInfo i{};
    i.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    i.pNext = pNext;
    i.descriptorPool = P(pool);
    i.descriptorSetCount = uint32_t(ls.size());
    i.pSetLayouts = ls.data();
    out->assign(ls.size(), reinterpret_cast<VkDescriptorSet>(uint64_t(0xdead)));
    return r.Allocate(nullptr, &i, out->data());
}
// Every handed-out set is really held by that pool, with the layout asked for.
void Valid(uint64_t pool, std::vector<uint64_t> layouts, const std::vector<VkDescriptorSet>& got)
{
    std::set<uint64_t> distinct;
    for (size_t k = 0; k < got.size(); ++k) {
        assert(pools[pool].sets.count(U(got[k])));
        assert(layoutOf[U(got[k])] == layouts[k]);
        distinct.insert(U(got[k]));
    }
    assert(distinct.size() == got.size());
}
} // namespace

int main()
{
    dsrecycle::Real real;
    real.alloc = &Alloc; real.free = &Free; real.reset = &Reset;
    real.destroyPool = &DestroyPool; real.destroyLayout = &DestroyLayout;
    dsrecycle::Recycler r(real, 4);
    std::vector<VkDescriptorSet> a, b;

    // frame 1: fresh; frame 2: the same mix comes back without the driver
    assert(Ask(r, 1, {10, 10, 11}, &a) == VK_SUCCESS); Valid(1, {10, 10, 11}, a);
    assert(allocCalls == 1);
    assert(r.Reset(nullptr, P(1), 0) == VK_SUCCESS && resetCalls == 0);
    assert(Ask(r, 1, {11, 10, 10}, &b) == VK_SUCCESS); Valid(1, {11, 10, 10}, b);
    assert(allocCalls == 1 && pools[1].sets.size() == 3);
    // a bigger frame asks the driver only for what is missing, in one call
    assert(r.Reset(nullptr, P(1), 0) == VK_SUCCESS);
    assert(Ask(r, 1, {10, 12, 10, 10}, &a) == VK_SUCCESS); Valid(1, {10, 12, 10, 10}, a);
    assert(allocCalls == 2 && pools[1].sets.size() == 5);
    assert(r.Reset(nullptr, P(1), 0) == VK_SUCCESS && resetCalls == 0);
    assert(Ask(r, 1, {12}, &a) == VK_SUCCESS); Valid(1, {12}, a);
    assert(allocCalls == 2);
    // the 4th reset of the pool is real, and everything starts over
    assert(r.Reset(nullptr, P(1), 0) == VK_SUCCESS && resetCalls == 1 && pools[1].sets.empty());
    assert(Ask(r, 1, {10}, &a) == VK_SUCCESS); Valid(1, {10}, a);
    assert(allocCalls == 3);

    // a refusal gives the spares back and leaves every output null
    pools[2].cap = 2;
    assert(Ask(r, 2, {20, 20}, &a) == VK_SUCCESS);
    assert(r.Reset(nullptr, P(2), 0) == VK_SUCCESS);
    assert(Ask(r, 2, {20, 21}, &b) == VK_ERROR_OUT_OF_POOL_MEMORY);
    assert(b[0] == VK_NULL_HANDLE && b[1] == VK_NULL_HANDLE);
    assert(Ask(r, 2, {20, 20}, &b) == VK_SUCCESS); Valid(2, {20, 20}, b);   // both spares still there

    // an individually freed set is not handed out again
    assert(r.Reset(nullptr, P(2), 0) == VK_SUCCESS);
    assert(Ask(r, 2, {20, 20}, &a) == VK_SUCCESS);
    assert(r.Free(nullptr, P(2), 1, &a[0]) == VK_SUCCESS && freeCalls == 1);
    assert(r.Reset(nullptr, P(2), 0) == VK_SUCCESS);
    assert(Ask(r, 3, {30}, &a) == VK_SUCCESS);
    assert(r.Free(nullptr, P(3), 1, &a[0]) == VK_SUCCESS);
    assert(r.Reset(nullptr, P(3), 0) == VK_SUCCESS);
    assert(Ask(r, 3, {30}, &b) == VK_SUCCESS); Valid(3, {30}, b);
    assert(b[0] != a[0]);

    // a pNext allocation is the driver's; that pool's next reset is real
    const int resetsBefore = resetCalls;
    VkDescriptorSetVariableDescriptorCountAllocateInfo variable{};
    variable.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
    assert(Ask(r, 4, {40}, &a) == VK_SUCCESS);
    assert(Ask(r, 4, {41}, &b, &variable) == VK_SUCCESS);
    assert(r.Reset(nullptr, P(4), 0) == VK_SUCCESS && resetCalls == resetsBefore + 1 && pools[4].sets.empty());
    assert(Ask(r, 4, {40}, &a) == VK_SUCCESS); Valid(4, {40}, a);
    assert(r.Reset(nullptr, P(4), 0) == VK_SUCCESS && resetCalls == resetsBefore + 1);   // tracked again

    // a destroyed layout's sets are never handed out again; the pool's next reset is real
    assert(Ask(r, 5, {50, 51}, &a) == VK_SUCCESS);
    assert(r.Reset(nullptr, P(5), 0) == VK_SUCCESS);
    const int allocs = allocCalls;
    r.DestroyLayout(nullptr, L(50), nullptr);
    assert(Ask(r, 5, {51}, &b) == VK_SUCCESS && allocCalls == allocs);   // 51 still recycled
    const int resets = resetCalls;
    assert(r.Reset(nullptr, P(5), 0) == VK_SUCCESS && resetCalls == resets + 1);
    assert(Ask(r, 5, {52}, &b) == VK_SUCCESS); Valid(5, {52}, b);

    // a reset with flags, and a destroyed pool, go to the driver
    assert(r.Reset(nullptr, P(5), 1) == VK_SUCCESS && resetCalls == resets + 2);
    assert(Ask(r, 6, {60}, &a) == VK_SUCCESS);
    r.DestroyPool(nullptr, P(6), nullptr);
    assert(!pools.count(6));
    assert(Ask(r, 6, {60}, &b) == VK_SUCCESS && allocCalls > allocs);   // a new pool with the same handle starts empty
    Valid(6, {60}, b);

    const auto st = r.Snapshot();
    assert(st.reused > 0 && st.allocated > 0 && st.realResets > 0);
    return 0;
}
