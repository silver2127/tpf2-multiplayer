// Exercise game-runtime exceptions across the hidden static runtime used by
// the native loader. The foreign throw and catch both live in a dynamic DSO.
#include "../src/animal_rng_linux.cpp"
#include <cassert>
#include <dlfcn.h>
#include <thread>
#include <type_traits>

namespace {
void (*ForeignThrow)();
bool (*ForeignCatch)(void (*)());
thread_local unsigned mode;
thread_local unsigned workerCalls;
thread_local unsigned spawnCalls;
thread_local uint64_t fakeEngine;
constexpr uint32_t kTime = 0x44610000;
constexpr int32_t kBegin = 17;
constexpr int32_t kSpawnSeed = -33461;

void InvokeSpawn()
{
    AnimalSpawn(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, kSpawnSeed);
}

void InvokeWorker()
{
    unsigned char capture[0x30]{};
    std::memcpy(capture + 0x28, &kTime, sizeof(kTime));
    assert(AnimalWorker(nullptr, capture, kBegin, kBegin + 9) == &fakeEngine);
}

void SpawnOriginal(void*, void*, void*, void*, void*, void*, void*, int32_t seed)
{
    ++spawnCalls;
    assert(g_animalScope && g_animalScope->kind == AnimalKind::Spawn);
    assert(g_animalScope->seed == uint32_t(seed));
    g_animalScope->nativeEngine = &fakeEngine;
    std::mt19937 expected{uint32_t(seed)};
    assert(AnimalUnit(&fakeEngine) == WindowsAnimalUnit(expected));
    if (mode) ForeignThrow();
}

void* WorkerOriginal(void*, const void*, int32_t begin, int32_t)
{
    ++workerCalls;
    AnimalScope* const outer = g_animalScope;
    assert(outer && outer->kind == AnimalKind::Worker);
    assert(outer->seed == Tpf2mpWindowsTimeSeed(uint32_t(begin), kTime));
    outer->nativeEngine = &fakeEngine;
    std::mt19937 expected(outer->seed);
    assert(AnimalUnit(&fakeEngine) == WindowsAnimalUnit(expected));
    if (mode == 1) ForeignThrow();
    if (mode == 2) InvokeSpawn();
    if (mode == 3) {
        assert(ForeignCatch(InvokeSpawn));
        assert(g_animalScope == outer);
        // The nested engine used the same address. Its cleanup must restore
        // both the parent context and the parent's independent draw position.
        assert(AnimalUnit(&fakeEngine) == WindowsAnimalUnit(expected));
    }
    if (!mode) InvokeSpawn();
    assert(g_animalScope == outer);
    return &fakeEngine;
}

void CheckSequence()
{
    assert(g_animalScope == nullptr);
    for (unsigned i = 0; i < 64; ++i) {
        mode = 1;
        assert(ForeignCatch(InvokeWorker));
        assert(g_animalScope == nullptr);
        assert(ForeignCatch(InvokeSpawn));
        assert(g_animalScope == nullptr);
        mode = 2;
        assert(ForeignCatch(InvokeWorker));
        assert(g_animalScope == nullptr);
        mode = 3;
        InvokeWorker();
        assert(g_animalScope == nullptr);
        mode = 0;
        InvokeWorker();
        assert(g_animalScope == nullptr);
    }
    assert(workerCalls == 256 && spawnCalls == 256);
}
}

int main(int argc, char** argv)
{
    static_assert(std::is_trivially_destructible<AnimalScope>::value,
                  "Foreign frames must not enter a static-runtime cleanup personality");
    assert(argc == 2);
    void* foreign = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
    assert(foreign);
    ForeignThrow = reinterpret_cast<void (*)()>(dlsym(foreign, "SliceTestForeignThrow"));
    ForeignCatch = reinterpret_cast<bool (*)(void (*)())>(dlsym(foreign, "SliceTestForeignCatch"));
    assert(ForeignThrow && ForeignCatch);
    assert(ResolveAnimalRuntime());
    g_originalWorker = reinterpret_cast<void*>(WorkerOriginal);
    g_originalSpawn = reinterpret_cast<void*>(SpawnOriginal);
    g_animalReady = true;
    CheckSequence();
    std::thread first(CheckSequence), second(CheckSequence);
    first.join();
    second.join();
    assert(g_animalScope == nullptr);
    puts("animal foreign unwind: 768 worker calls, 768 spawn calls; nested cleanup, same-address reuse and three threads passed");
}
