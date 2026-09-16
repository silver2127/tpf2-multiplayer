// Linux build 35924: see docs/re/linux/SLICE_VEHICLES.md for every layout/RVA.
#include "slice_core.h"
#include <cmath>
#include <climits>
#include <initializer_list>

namespace slice_vehicles {
constexpr uintptr_t kBuy = 0x15ef3b0, kSell = 0x15ecf00, kReplace = 0x15ef8c0;
constexpr uintptr_t kDepot = 0x15ec220, kReverse = 0x15ebe00;

static bool IntVector(SliceRecord* rec, uintptr_t address, bool requireOne = false)
{
    SliceVec v{};
    if (!SliceReadStdVector(address, 4, 256, &v) || (requireOne && !v.count)) return false;
    SliceRecordPrintf(rec, " %zu", v.count);
    for (size_t i = 0; i < v.count; ++i) {
        int32_t value;
        if (!SliceReadT(v.begin + i * 4, &value)) return false;
        SliceRecordPrintf(rec, " %d", value);
    }
    return !rec->failed;
}

// libstdc++ stores 64-bit words; Lua expects signed 32-bit words, low half first.
// Normalize the start offset and erase unused tail bits, including garbage left
// in vector<bool>'s allocation. Limit matches the 256 load slots on the wire.
static bool AutoLoad(SliceRecord* rec, uintptr_t address)
{
    struct Bits { uintptr_t begin; uint32_t first, pad0; uintptr_t end; uint32_t last, pad1; uintptr_t cap; } b{};
    if (!SliceReadT(address, &b) || b.first >= 64 || b.last >= 64) return false;
    if (!b.begin) {
        if (b.end || b.cap || b.first || b.last) return false;
        SliceRecordPrintf(rec, " 0");
        return !rec->failed;
    }
    if (b.end < b.begin || b.cap < b.end || (b.end - b.begin) % 8 || (b.cap - b.begin) % 8 ||
        b.end - b.begin > 32 || (b.end == b.cap && b.last)) return false;
    const int64_t n = int64_t((b.end - b.begin) * 8) + b.last - b.first;
    if (n < 0 || n > 256) return false;
    uint64_t source[5]{};
    const size_t sourceWords = n ? (b.first + size_t(n) + 63) / 64 : 0;
    if (sourceWords && !SliceRead(b.begin, source, sourceWords * sizeof(source[0]))) return false;
    const size_t words = (size_t(n) + 31) / 32;
    SliceRecordPrintf(rec, " %zu", words);
    for (size_t w = 0; w < words; ++w) {
        uint32_t value = 0;
        for (size_t bit = 0; bit < 32 && w * 32 + bit < size_t(n); ++bit) {
            const size_t index = b.first + w * 32 + bit;
            value |= uint32_t((source[index / 64] >> (index % 64)) & 1) << bit;
        }
        SliceRecordPrintf(rec, " %d", int32_t(value));
    }
    return !rec->failed;
}

static bool Config(SliceRecord* rec, uintptr_t address)
{
    SliceVec parts{};
    if (!SliceReadStdVector(address, 0x88, 64, &parts) || !parts.count) return false;
    SliceRecordPrintf(rec, " %zu", parts.count);
    for (size_t i = 0; i < parts.count; ++i) {
        const uintptr_t p = parts.begin + i * 0x88;
        int32_t model;
        float color[3];
        if (!SliceReadT(p, &model) || !SliceRead(p + 0x20, color, sizeof(color)) ||
            !std::isfinite(color[0]) || !std::isfinite(color[1]) || !std::isfinite(color[2])) return false;
        SliceRecordPrintf(rec, " %d", model);
        if (!IntVector(rec, p + 8)) return false;
        SliceRecordPrintf(rec, " %.4f %.4f %.4f", color[0], color[1], color[2]);
        if (!AutoLoad(rec, p + 0x60)) return false;
    }
    return IntVector(rec, address + 0x18);
}

struct BuyPending { bool active, shipped; SliceFactoryCall call; SliceRecord record; };
static thread_local BuyPending t_buy{};

static void BuyLanded(const SliceAddCall*, SliceOutcome outcome, void*)
{
    SliceRecordFree(&t_buy.record);
    t_buy.active = false;
    if (outcome == SliceOutcome::Mismatch || outcome == SliceOutcome::Superseded)
        SliceLog("[slice-vehicles] pending VBUY dropped: %s\n", SliceOutcomeName(outcome));
}

static bool CloneLine(const SliceAddCall& add, int32_t* line)
{
    uintptr_t manager, invoker, lambda, impl;
    int32_t result;
    return SliceStdFunctionParts(uintptr_t(add.done), &manager, &invoker) &&
        manager == SliceAddr(0x126eb30) && invoker == SliceAddr(0x1272350) &&
        SliceReadT(uintptr_t(add.done), &lambda) && lambda &&
        SliceReadT(lambda + 0x30, line) && SliceReadT(uintptr_t(add.cmd), &impl) &&
        SliceReadT(impl + 0x38, &result) && result == -1;
}

// Write the buy and clone assignment together, immediately before a verified
// cancel. Strict mode leaves failed capture to the core's blocking policy.
static bool PrepareBuy(const SliceAddCall& add, void*)
{
    if (!t_buy.active || uintptr_t(add.cmd) != t_buy.call.rdi) return false;
    const bool safe = SliceCancelAvailable() && SliceSessionLive() && SliceCommandTag(uintptr_t(add.cmd)) == 13;
    int32_t line = -1;
    const bool known = safe && CloneLine(add, &line);
    if (!known) return false;
    if (line >= 0) SliceRecordPrintf(&t_buy.record, "VBUYLINE %d\n", line);
    const auto result = SliceInjectWrite(t_buy.record, SliceArmedLine::One);
    t_buy.shipped = result != SliceInjectResult::NotWritten;
    return t_buy.shipped;
}

static void OnFactory(const SliceFactoryCall& c, void*)
{
    if (c.script || !c.armable || !SliceSessionLive()) return;
    // The missing-resources replacement deliberately has different billing. Its
    // bool has no wire field, so replaying it as VREPL would change the price.
    if (c.factory->rva == kReplace && (c.r8 & 0xff)) return;
    SliceRecord rec{};
    bool valid = true;
    const char* what = "vehicle";
    const int32_t entity = int32_t(c.rdx);
    SliceDone done = SliceDone::Never;
    switch (c.factory->rva) {
        case kBuy:
            what = "VBUY";
            SliceRecordPrintf(&rec, "VBUY %d", int32_t(c.rcx));
            valid = Config(&rec, c.r8);
            break;
        case kReplace:
            what = "VREPL";
            done = SliceDone::Required;
            SliceRecordPrintf(&rec, "VREPL %d", entity);
            valid = Config(&rec, c.rcx);
            break;
        case kSell:
            what = "VSELL";
            SliceRecordPrintf(&rec, "VSELL");
            valid = IntVector(&rec, c.rdx, true);
            break;
        case kDepot:
            what = "VDEPOT";
            SliceRecordPrintf(&rec, "VDEPOT %d %d", entity, int(c.rcx & 1));
            break;
        case kReverse:
            what = "VREV";
            SliceRecordPrintf(&rec, "VREV %d", entity);
            break;
        default: return;
    }
    SliceRecordPrintf(&rec, "\n");
    if (!valid || rec.failed) {
        SliceLog("[slice-vehicles] %s payload unreadable or unsupported; capture refused (strict=%d)\n", what, int(SliceSessionLive()));
        SliceRecordFree(&rec);
        return;
    }
    if (c.factory->rva == kBuy) {
        if (SliceCancelAvailable() && SliceArmCancel(c, {what, SliceDone::Never, true, nullptr, BuyLanded, nullptr, PrepareBuy})) {
            t_buy = {true, false, c, rec};
            return;
        }
        SliceRecordFree(&rec);
        return;
    }
    SliceShipAndArm(c, {what, done, false, nullptr, nullptr, nullptr}, rec);
    SliceRecordFree(&rec);
}
} // namespace slice_vehicles

SLICE_AREA(slice_vehicles_area, "slice-vehicles")
{
    using namespace slice_vehicles;
    for (uintptr_t rva : {kBuy, kSell, kReplace, kDepot, kReverse})
        SliceOnFactory({"slice-vehicles", rva, OnFactory, nullptr, nullptr, true, 0});
}
