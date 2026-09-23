// Game-runtime exceptions unwind its owner; the actual adapter writer and
// destructor entry must not introduce a static-runtime exception personality.
#include "../src/target_order_linux.cpp"
#include "../src/network_person_order_linux.cpp"
#include <cassert>
#include <dlfcn.h>
#include <thread>

namespace {
void (*ForeignThrow)();
bool (*ForeignCatch)(void (*)());
void (*ForeignScope)(void (*)(), void (*)(void*), void*);
struct TargetFixture {
    uint64_t map[7]{};
    uint64_t outerNode[9]{};
    uint64_t node[3]{};
    uint32_t id = 20840;
    int32_t target = 20835;
    unsigned mode = 0;
};
thread_local TargetFixture targetOuter, targetInner;
thread_local TargetFixture* activeTarget;
thread_local unsigned targetWrites, targetClears, networkClears;

bool HasTarget(uintptr_t address)
{
    bool found = false;
    pthread_mutex_lock(&g_targetMutex);
    for (auto* owner = g_targetOwners; owner; owner = owner->next)
        if (owner->map == address) found = true;
    pthread_mutex_unlock(&g_targetMutex);
    return found;
}
void* Lookup(void* address, const uint32_t* key)
{
    auto* fixture = static_cast<TargetFixture*>(address);
    assert(*key == uint32_t(fixture->target));
    return fixture->outerNode;
}
void InsertOriginal(const uint32_t* id, const int32_t* target, void* address)
{
    ++targetWrites;
    auto* fixture = static_cast<TargetFixture*>(address);
    assert(id == &fixture->id && target == &fixture->target);
    if (fixture->mode == 1) ForeignThrow();
    fixture->outerNode[4] = reinterpret_cast<uintptr_t>(fixture->node);
    fixture->outerNode[5] = 1;
    fixture->node[1] = *id;
}
void EraseOriginal(const uint32_t*, const int32_t*, void* address)
{
    ++targetWrites;
    auto* fixture = static_cast<TargetFixture*>(address);
    if (fixture->mode == 2) ForeignThrow();
    fixture->outerNode[4] = fixture->outerNode[5] = 0;
}
void ClearOriginal(void* address)
{
    assert(!HasTarget(reinterpret_cast<uintptr_t>(address)));
    ++targetClears;
}
void TargetCleanup(void* address) { TargetClearOwner(address); }
void TargetBody()
{
    auto* fixture = activeTarget;
    TargetInsert(&fixture->id, &fixture->target, fixture);
    assert(FirstTargetNode(reinterpret_cast<uintptr_t>(fixture->outerNode + 2)) == reinterpret_cast<uintptr_t>(fixture->node));
    assert(NextTargetNode(reinterpret_cast<uintptr_t>(fixture->node)) == 0);
    TargetErase(&fixture->id, &fixture->target, fixture);
}
void RunTarget()
{
    BindTargetOwner(reinterpret_cast<uintptr_t>(activeTarget));
    ForeignScope(TargetBody, TargetCleanup, activeTarget);
}
void NestedTargetBody()
{
    activeTarget = &targetInner;
    assert(ForeignCatch(RunTarget));
    activeTarget = &targetOuter;
    assert(HasTarget(reinterpret_cast<uintptr_t>(&targetOuter)));
    TargetBody();
}

struct NetworkFixture {
    uint64_t set[7]{};
    uint64_t node[3]{};
    uint32_t id = 20840;
    uintptr_t batch[3]{};
    uintptr_t batches[3]{};
    bool throws = false;
};
thread_local NetworkFixture networkOuter, networkInner;
thread_local NetworkFixture* activeNetwork;
bool HasNetwork(uintptr_t address)
{
    bool found = false;
    pthread_mutex_lock(&g_networkMutex);
    for (auto* record = g_networkRecords; record; record = record->next)
        if (record->nativeSet == address) found = true;
    pthread_mutex_unlock(&g_networkMutex);
    return found;
}
void NetworkOriginalDestructor(void* address)
{
    assert(!HasNetwork(reinterpret_cast<uintptr_t>(address)));
    ++networkClears;
}
void NetworkCleanup(void* address)
{
    // Execute the real register-saving destructor shim, ending at its original
    // callback. The dynamic game owner calls this during its active unwind.
    reinterpret_cast<void (*)(void*)>(NetworkStub2)(address);
}
void ResetNetwork(NetworkFixture& fixture, bool throws)
{
    fixture = {};
    fixture.throws = throws;
    fixture.set[2] = reinterpret_cast<uintptr_t>(fixture.node);
    fixture.set[3] = 1;
    fixture.node[1] = fixture.id;
    fixture.batch[0] = reinterpret_cast<uintptr_t>(&fixture.id);
    fixture.batch[1] = fixture.batch[2] = fixture.batch[0] + 4;
    fixture.batches[0] = reinterpret_cast<uintptr_t>(fixture.batch);
    fixture.batches[1] = fixture.batches[2] = fixture.batches[0] + sizeof(fixture.batch);
}
void NetworkBody()
{
    auto* fixture = activeNetwork;
    assert(FirstNetworkNode(reinterpret_cast<uintptr_t>(fixture->set), reinterpret_cast<uintptr_t>(fixture->batches)) == reinterpret_cast<uintptr_t>(fixture->node));
    if (fixture->throws) ForeignThrow();
}
void RunNetwork() { ForeignScope(NetworkBody, NetworkCleanup, activeNetwork->set); }
void NestedNetworkBody()
{
    NetworkBody();
    activeNetwork = &networkInner;
    assert(ForeignCatch(RunNetwork));
    activeNetwork = &networkOuter;
    assert(HasNetwork(reinterpret_cast<uintptr_t>(networkOuter.set)));
}
void CheckSequence()
{
    for (unsigned i = 0; i < 64; ++i) {
        for (unsigned mode : {1u, 2u, 0u}) {
            targetOuter = {};
            targetOuter.mode = mode;
            activeTarget = &targetOuter;
            assert(ForeignCatch(RunTarget) == (mode != 0));
            assert(!HasTarget(reinterpret_cast<uintptr_t>(&targetOuter)));
        }
        targetOuter = {}; targetInner = {}; targetInner.mode = 1;
        BindTargetOwner(reinterpret_cast<uintptr_t>(&targetOuter));
        activeTarget = &targetOuter;
        ForeignScope(NestedTargetBody, TargetCleanup, &targetOuter);
        assert(!HasTarget(reinterpret_cast<uintptr_t>(&targetOuter)));
        for (bool throws : {true, false}) {
            ResetNetwork(networkOuter, throws);
            activeNetwork = &networkOuter;
            assert(ForeignCatch(RunNetwork) == throws);
            assert(!HasNetwork(reinterpret_cast<uintptr_t>(networkOuter.set)));
        }
        ResetNetwork(networkOuter, false); ResetNetwork(networkInner, true);
        activeNetwork = &networkOuter;
        ForeignScope(NestedNetworkBody, NetworkCleanup, networkOuter.set);
        assert(!HasNetwork(reinterpret_cast<uintptr_t>(networkOuter.set)));
    }
    assert(targetWrites == 512 && targetClears == 320 && networkClears == 256);
}
}
int main(int argc, char** argv)
{
    assert(argc == 2);
    void* game = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
    assert(game);
    ForeignThrow = reinterpret_cast<void (*)()>(dlsym(game, "PersonMapForeignThrow"));
    ForeignCatch = reinterpret_cast<bool (*)(void (*)())>(dlsym(game, "PersonMapForeignCatch"));
    ForeignScope = reinterpret_cast<decltype(ForeignScope)>(dlsym(game, "PersonMapForeignScope"));
    assert(ForeignThrow && ForeignCatch && ForeignScope);
    g_targetOriginal[1] = reinterpret_cast<void*>(ClearOriginal);
    g_targetOriginal[2] = reinterpret_cast<void*>(InsertOriginal);
    g_targetOriginal[3] = reinterpret_cast<void*>(EraseOriginal);
    g_targetLookup = Lookup;
    g_targetReady = true;
    g_networkOriginal[2] = reinterpret_cast<void*>(NetworkOriginalDestructor);
    g_networkReady.store(true, std::memory_order_release);
    CheckSequence();
    std::thread first(CheckSequence), second(CheckSequence);
    first.join(); second.join();
    assert(!g_targetOwners && !g_networkRecords);
    assert(!std::strstr(Tpf2mpTargetOrderStatus(), "ERROR"));
    assert(!std::strstr(Tpf2mpNetworkPersonOrderStatus(), "ERROR"));
    puts("ordering foreign unwind: 1536 target writes, 960 target owner cleanups, 768 network shim cleanups; nested exceptions/reuse/three threads passed");
}
