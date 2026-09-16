// Linux build 35924: docs/re/linux/SLICE_TIME.md. Clock callbacks decrement
// counters incremented AFTER Add, so they must never fire inside a cancelled Add.
#include "slice_core.h"
#include "slice_terrain_assets.h"
#include <atomic>
#include <cstring>
#include <initializer_list>
#include <pthread.h>
#include <sys/uio.h>
#include <unistd.h>

namespace slice_time {
constexpr uintptr_t kSpeed = 0x15eb600, kCalendar = 0x15eb690, kDate = 0x15eb7b0;
constexpr uintptr_t kStep = 0xf6c4b0, kButtons = 0xf6b88e, kToggle = 0xf6c28d;
constexpr uintptr_t kPicker = 0xf6bce5, kSlider = 0xf70410;
static void* g_stepTrampoline = nullptr;

// There are deliberately no C++ cleanups surrounding the game's DoStep: its
// exceptions must unwind through the game's runtime, not our static libgcc.
// A scope is active only while its precise frame/return-address pair is still
// on the guarded frame chain. That also invalidates TLS left by an exception.
struct Scope { uintptr_t frame, caller; };
static thread_local Scope t_step{};

__attribute__((noinline)) static bool InStep()
{
    if (!t_step.frame) return false;
    uintptr_t fp = uintptr_t(__builtin_frame_address(0));
    for (unsigned i = 0; i < 256 && fp && fp <= t_step.frame; ++i) {
        uintptr_t frame[2];
        if (!SliceRead(fp, frame, sizeof(frame))) break;
        if (fp == t_step.frame && frame[1] == t_step.caller) return true;
        if (frame[0] <= fp || frame[0] - fp > 1024 * 1024) break;
        fp = frame[0];
    }
    t_step = {};
    return false;
}

static bool WriteCounter(uintptr_t address, int value)
{
    iovec local{&value, sizeof(value)}, remote{reinterpret_cast<void*>(address), sizeof(value)};
    return process_vm_writev(getpid(), &local, 1, &remote, 1, 0) == ssize_t(sizeof(value));
}

struct Compensation { uintptr_t counter; uintptr_t buttons[4]; uint64_t due; bool ready; };
static Compensation g_compensations[64]{};
static pthread_mutex_t g_counterMutex = PTHREAD_MUTEX_INITIALIZER;

static void ReleaseSlot(int slot)
{
    if (slot < 0) return;
    pthread_mutex_lock(&g_counterMutex);
    g_compensations[slot] = {};
    pthread_mutex_unlock(&g_counterMutex);
}

static int ReserveCounter(const SliceAddCall& add, bool date, bool clamp)
{
    uintptr_t manager, invoker, pointer;
    if (!SliceStdFunctionParts(uintptr_t(add.done), &manager, &invoker) ||
        manager != SliceAddr(date ? 0xf6ae50 : 0xf6ae90) ||
        invoker != SliceAddr(date ? 0xf6afd0 : 0xf6af90) ||
        !SliceReadT(uintptr_t(add.done), &pointer) || !pointer) return -1;
    if (date && pointer < 0x478) return -1;
    const uintptr_t clock = date ? pointer - 0x478 : pointer;
    Compensation c{};
    c.counter = clock + 0x478;
    c.due = SliceNowMs() + (clamp ? 1000 : 0);
    int count;
    if (!SliceRead(clock + 0x4b0, c.buttons, sizeof(c.buttons)) ||
        !SliceReadT(c.counter, &count) || count < 0 || !WriteCounter(c.counter, count)) return -1;
    int slot = -1;
    pthread_mutex_lock(&g_counterMutex);
    for (int i = 0; i < 64; ++i) if (!g_compensations[i].counter) {
        g_compensations[i] = c;
        slot = i;
        break;
    }
    pthread_mutex_unlock(&g_counterMutex);
    return slot;
}

static void Compensate(uintptr_t clock)
{
    uintptr_t buttons[4];
    if (!SliceRead(clock + 0x4b0, buttons, sizeof(buttons))) return;
    const uint64_t now = SliceNowMs();
    pthread_mutex_lock(&g_counterMutex);
    for (auto& c : g_compensations) {
        if (c.counter != clock + 0x478) continue;
        if (std::memcmp(c.buttons, buttons, sizeof(buttons))) { c = {}; continue; }
        if (!c.ready || c.due > now) continue;
        int count;
        if (SliceReadT(c.counter, &count) && count > 0 && WriteCounter(c.counter, count - 1)) c = {};
    }
    pthread_mutex_unlock(&g_counterMutex);
}

__attribute__((noinline)) static void DoStep(void* clock, int64_t delta, int64_t now)
{
    SliceTerrainPollHeldTools();
    Compensate(uintptr_t(clock));
    const Scope previous = t_step;
    t_step = {uintptr_t(__builtin_frame_address(0)), uintptr_t(__builtin_return_address(0))};
    reinterpret_cast<void (*)(void*, int64_t, int64_t)>(g_stepTrampoline)(clock, delta, now);
    t_step = previous;
}

struct Pending { bool active, counter, date, clamp; int slot; SliceFactoryCall call; SliceRecord record; };
static thread_local Pending t_pending{};

static void Landed(const SliceAddCall*, SliceOutcome outcome, void*)
{
    const bool cancelled = outcome == SliceOutcome::CancelledFired || outcome == SliceOutcome::CancelledNotFired ||
        outcome == SliceOutcome::Blocked;
    if (t_pending.slot >= 0) {
        if (cancelled) {
            pthread_mutex_lock(&g_counterMutex);
            g_compensations[t_pending.slot].ready = true;
            pthread_mutex_unlock(&g_counterMutex);
        } else ReleaseSlot(t_pending.slot);
    }
    SliceRecordFree(&t_pending.record);
    t_pending = {};
    t_pending.slot = -1;
}

static bool PrepareTime(const SliceAddCall& add, void*)
{
    if (!t_pending.active || uintptr_t(add.cmd) != t_pending.call.rdi) return false;
    bool safe = SliceCancelAvailable() && SliceSessionLive() &&
        SliceCommandTag(uintptr_t(add.cmd)) == t_pending.call.factory->tag;
    if (safe && t_pending.counter) {
        t_pending.slot = ReserveCounter(add, t_pending.date, t_pending.clamp);
        safe = t_pending.slot >= 0;
    }
    // At this point the Command/tag and Never callback policy guarantee the
    // immediately following core decision can cancel. A failed write is blocked;
    // the reserved counter still needs compensation after the caller increments
    // it. Time records retain the existing protocol with no ARMED line.
    if (safe && !t_pending.clamp)
        safe = SliceInjectWrite(t_pending.record, SliceArmedLine::None) != SliceInjectResult::NotWritten;
    return safe;
}

static void OnFactory(const SliceFactoryCall& c, void*)
{
    if (c.script || !c.armable || !SliceSessionLive() || !SliceCancelAvailable()) return;
    const int32_t value = int32_t(c.rsi);
    const char* op;
    bool counter = false, date = false, clamp = false;
    switch (c.factory->rva) {
        case kSpeed:
            if ((c.retRva != kButtons && c.retRva != kToggle) || value < 0 || value > 64) return;
            op = "SPEEDBTN";
            counter = c.retRva == kButtons;
            if (counter) {
                if (!SliceHookInstalled(kStep)) return;
                clamp = InStep();
            }
            break;
        case kDate:
            if (c.retRva != kPicker || value < 1721426 || value > 5373484 || !SliceHookInstalled(kStep)) return;
            op = "SETDATE"; counter = date = true;
            break;
        case kCalendar:
            if (c.retRva != kSlider || value < 0 || value > 10000000) return;
            op = "CALSPEED";
            break;
        default: return;
    }
    char letter[8];
    if (!SliceInstance(letter, sizeof(letter))) return;
    SliceRecord rec{};
    if (!clamp) {
        if (c.factory->rva == kSpeed)
            SliceRecordPrintf(&rec, "%s %d %s\n", op, value, c.retRva == kToggle ? "toggle" : "button");
        else SliceRecordPrintf(&rec, "%s %d\n", op, value);
    }
    if (!rec.failed && SliceArmCancel(c, {clamp ? "clock clamp" : op, SliceDone::Never, true, nullptr, Landed, nullptr, PrepareTime})) {
        t_pending = {true, counter, date, clamp, -1, c, rec};
        return;
    }
    SliceRecordFree(&rec);
}
} // namespace slice_time

SLICE_AREA(slice_time_area, "slice-time")
{
    using namespace slice_time;
    static const uint8_t bytes[] = {0xf3,0x0f,0x1e,0xfa,0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,
        0x41,0x54,0x53,0x48,0x83,0xec,0x48,0x8b,0x97,0x78,0x04,0x00,0x00};
    SliceRegisterHook({"slice-time", "UI::Clock::DoStep", -1, kStep, bytes, sizeof(bytes), 14,
        reinterpret_cast<void*>(DoStep), &g_stepTrampoline, true});
    for (uintptr_t rva : {kSpeed, kCalendar, kDate})
        SliceOnFactory({"slice-time", rva, OnFactory, nullptr, nullptr, true, 0});
}
