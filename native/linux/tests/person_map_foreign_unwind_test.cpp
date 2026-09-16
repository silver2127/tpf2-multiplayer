#include "../src/person_map_order_linux.cpp"
#include <cassert>
#include <cstdio>
#include <dlfcn.h>
#include <thread>

namespace {
void (*ForeignThrow)();
bool (*ForeignCatch)(void (*)());
void (*ForeignScope)(void (*)(), void (*)(void*), void*);
struct Case {
    alignas(8) unsigned char maps[kInfoMapCount * kNativeMapSize]{};
    alignas(8) unsigned char node[56]{};
    uint32_t key;
    bool throws;
};
thread_local Case outer, inner;
thread_local Case* current;
thread_local unsigned cleanups, calls, successes;

void Cleanup(void* data)
{
    ForgetMaps(reinterpret_cast<uintptr_t>(data));
    ++cleanups;
}

void* Original(void* map, const uint32_t* key)
{
    ++calls;
    assert(current && map == current->maps && key == &current->key);
    if (current->throws) ForeignThrow();
    const uintptr_t node = reinterpret_cast<uintptr_t>(current->node);
    const uintptr_t count = 1;
    std::memcpy(current->maps + 0x10, &node, 8);
    std::memcpy(current->maps + 0x18, &count, 8);
    std::memcpy(current->node + 8, key, 4);
    return current->node + 16;
}

void Insert()
{
    assert(MapInsert(current->maps, &current->key) == current->node + 16);
    ++successes;
    assert(FirstMapNode(reinterpret_cast<uintptr_t>(current->maps), 0) == reinterpret_cast<uintptr_t>(current->node));
    assert(NextMapNode(reinterpret_cast<uintptr_t>(current->node), 0) == 0);
}

void RunCurrent()
{
    BindMaps(reinterpret_cast<uintptr_t>(current->maps), reinterpret_cast<uintptr_t>(current));
    ForeignScope(Insert, Cleanup, current->maps);
}

void Nested()
{
    current = &inner;
    assert(ForeignCatch(RunCurrent));
    current = &outer;
    Insert();
}

void Reset(Case& value, uint32_t key, bool throws)
{
    value = {};
    value.key = key;
    value.throws = throws;
}

void CheckSequence()
{
    for (unsigned i = 0; i < 128; ++i) {
        Reset(outer, 20851, true);
        current = &outer;
        assert(ForeignCatch(RunCurrent));
        // Reuse the same object address after the game's exceptional cleanup.
        Reset(outer, 20852, false);
        RunCurrent();
        Reset(outer, 20851, false);
        Reset(inner, 20852, true);
        BindMaps(reinterpret_cast<uintptr_t>(outer.maps), reinterpret_cast<uintptr_t>(&outer));
        ForeignScope(Nested, Cleanup, outer.maps);
    }
    assert(calls == 512 && successes == 256 && cleanups == 512);
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
    g_mapOriginal[12] = reinterpret_cast<void*>(Original);
    CheckSequence();
    std::thread first(CheckSequence), second(CheckSequence);
    first.join();
    second.join();
    assert(g_mapOwners == nullptr);
    assert(!std::strstr(Tpf2mpPersonMapOrderStatus(), "ERROR"));
    puts("person map foreign unwind: 1536 insertions and owner cleanups; nested exceptions, address reuse and three threads passed");
}
