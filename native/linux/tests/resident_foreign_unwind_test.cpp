// The game throws through its dynamic runtime while the adapter has a hidden
// static runtime. Exercise the real cleanup-only scope without catching or
// translating exceptions inside our runtime.
#include "../src/resident_hash_linux.cpp"
#include <cassert>
#include <thread>

namespace {
void (*ForeignThrow)();
bool (*ForeignCatch)(void (*)());
thread_local unsigned mode;
thread_local unsigned calls;
thread_local uint64_t outerSet;
thread_local uint64_t innerSet;
constexpr uint64_t kOuterHash = UINT64_C(0xfedcba9876543210);
constexpr uint64_t kInnerHash = UINT64_C(0x123456789abcdef0);
constexpr size_t kResult = size_t(UINT64_C(0xabcdef1234567890));

size_t Original(void*, uint64_t);

void InvokeInner()
{
    assert(ResidentScopedPrepare(&innerSet, kInnerHash, Original) == kResult);
}

void InvokeOuter()
{
    assert(ResidentScopedPrepare(&outerSet, kOuterHash, Original) == kResult);
}

size_t Original(void* set, uint64_t hash)
{
    ++calls;
    ResidentScope* const current = g_residentScope;
    assert(current && current->set == set);
    assert(IsResidentSet(reinterpret_cast<uintptr_t>(set)));
    if (set == &innerSet) {
        assert(hash == kInnerHash);
        if (mode) ForeignThrow();
    } else {
        assert(set == &outerSet && hash == kOuterHash);
        assert(!IsResidentSet(reinterpret_cast<uintptr_t>(&innerSet)));
        if (mode == 1) ForeignThrow();
        if (mode == 2) InvokeInner();
        if (mode == 3) {
            assert(ForeignCatch(InvokeInner));
            assert(g_residentScope == current);
            assert(IsResidentSet(reinterpret_cast<uintptr_t>(&outerSet)));
            assert(!IsResidentSet(reinterpret_cast<uintptr_t>(&innerSet)));
        }
        if (!mode) InvokeInner();
    }
    assert(g_residentScope == current);
    return kResult;
}

void CheckSequence()
{
    assert(g_residentScope == nullptr);
    for (unsigned i = 0; i < 128; ++i) {
        mode = 1;
        assert(ForeignCatch(InvokeOuter));
        assert(g_residentScope == nullptr);
        assert(ForeignCatch(InvokeInner));
        assert(g_residentScope == nullptr);
        mode = 2;
        assert(ForeignCatch(InvokeOuter));
        assert(g_residentScope == nullptr);
        mode = 3;
        InvokeOuter();
        assert(g_residentScope == nullptr);
        mode = 0;
        InvokeOuter();
        assert(g_residentScope == nullptr);
    }
    assert(calls == 1024);
}
}

int main(int argc, char** argv)
{
    assert(argc == 2);
    void* foreign = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
    assert(foreign);
    ForeignThrow = reinterpret_cast<void (*)()>(dlsym(foreign, "SliceTestForeignThrow"));
    ForeignCatch = reinterpret_cast<bool (*)(void (*)())>(dlsym(foreign, "SliceTestForeignCatch"));
    assert(ForeignThrow && ForeignCatch && ResolveResidentRuntime());
    CheckSequence();
    std::thread first(CheckSequence), second(CheckSequence);
    first.join();
    second.join();
    assert(g_residentScope == nullptr);
    puts("resident foreign unwind: 3072 original calls; nested exceptions, scope restoration, address reuse and three threads passed");
}
