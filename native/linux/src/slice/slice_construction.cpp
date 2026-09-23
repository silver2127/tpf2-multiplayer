// Build 35924: docs/re/linux/SLICE_CONSTRUCTION.md (SC-*).
#include "slice_construction.h"
#include "slice_construction_checks.h"
#include "slice_proposal.h"
#include <cmath>
#include <vector>
#include <set>
#include <cstring>
#include <atomic>
#include <cstdio>
#include <fcntl.h>
#include <initializer_list>
#include <unistd.h>

namespace {
void Quoted(SliceRecord* out, const char* text, size_t len)
{
    SliceRecordAppend(out, "\"", 1);
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = text[i];
        if (c == '\\' || c == '"') SliceRecordPrintf(out, "\\%c", c);
        else if (c < 32 || c == 127) SliceRecordPrintf(out, "\\%03u", c);
        else SliceRecordAppend(out, text + i, 1);
    }
    SliceRecordAppend(out, "\"", 1);
}
bool Scalar(SliceRecord* out, uintptr_t value, bool key)
{
    uint8_t tag = 0xff;
    if (!SliceReadT(value + 0x30, &tag)) return false;
    if (tag == 2) {
        double number;
        if (!SliceReadT(value, &number) || !std::isfinite(number)) return false;
        SliceRecordPrintf(out, "%.14g", number);
    } else if (tag == 3) {
        std::string text;
        if (!SliceReadStdString(value, &text)) return false;
        Quoted(out, text.data(), text.size());
    } else if (!key && tag == 1) {
        uint8_t b;
        if (!SliceReadT(value, &b) || b > 1) return false;
        SliceRecordAppend(out, b ? "true" : "false", b ? 4 : 5);
    } else return false;
    return !out->failed;
}
bool Collect(uintptr_t, uintptr_t key, void* context)
{
    static_cast<std::vector<uintptr_t>*>(context)->push_back(key);
    return true;
}
// An explicit stack follows nested Param maps without consuming the C stack.
// Every map retains its libstdc++ count/link checks; ancestor aliases are cycles.
bool Params(uintptr_t table, SliceRecord* out, size_t* nodes)
{
    struct Frame { uintptr_t table; std::vector<uintptr_t> keys; size_t next = 0; };
    std::vector<Frame> stack;
    std::set<uintptr_t> active;
    size_t count = 0;
    auto push = [&](uintptr_t at) {
        if (!active.insert(at).second) return false;
        Frame f{at, {}};
        if (!SliceWalkStdMap(at, SIZE_MAX, Collect, &f.keys, nullptr)) return false;
        stack.push_back(std::move(f));
        SliceRecordAppend(out, "{", 1);
        return !out->failed;
    };
    if (!push(table)) return false;
    while (!stack.empty()) {
        Frame& f = stack.back();
        if (f.next == f.keys.size()) {
            SliceRecordAppend(out, "}", 1);
            active.erase(f.table); stack.pop_back(); continue;
        }
        if (f.next) SliceRecordAppend(out, ",", 1);
        uintptr_t key = f.keys[f.next++]; ++count;
        SliceRecordAppend(out, "[", 1);
        if (!Scalar(out, key, true)) return false;
        SliceRecordAppend(out, "]=", 2);
        uint8_t tag;
        if (!SliceReadT(key + 0x38 + 0x30, &tag)) return false;
        if (tag == 4) { if (!push(key + 0x38)) return false; }
        else if (!Scalar(out, key + 0x38, false)) return false;
        if (out->failed) return false;
    }
    if (nodes) *nodes = count;
    return !out->failed;
}
std::atomic<unsigned long long> placementSerial{0};
thread_local unsigned long long currentSerial = 0;
thread_local bool currentRoad = false;

thread_local SliceRecord pending{};
thread_local bool pendingPlacement = false;
std::atomic<unsigned long> dumpSeq{0};
void DumpRange(FILE* f, const char* name, uintptr_t address, size_t bytes)
{
    std::fprintf(f, "%s addr=%lx bytes=%zu\n", name, (unsigned long)address, bytes);
    for (size_t offset = 0; offset < bytes; offset += 32) {
        uint8_t row[32]; const size_t n = bytes - offset < 32 ? bytes - offset : 32;
        if (!SliceRead(address + offset, row, n)) { std::fprintf(f, "+%04zx unreadable\n", offset); break; }
        std::fprintf(f, "+%04zx", offset);
        for (size_t i = 0; i < n; ++i) std::fprintf(f, " %02x", row[i]);
        std::fputc('\n', f);
    }
}
void Dump(const SliceFactoryCall& c)
{
    if (!SliceDumpPropOn()) return;
    char instance[8], path[4096];
    if (!SliceInstance(instance, sizeof(instance))) return;
    const int n = std::snprintf(path, sizeof(path), "%sconstruction_%s_%d_%lu_%lx.txt", SliceDataDir(), instance,
                                getpid(), dumpSeq.fetch_add(1), (unsigned long)c.retRva);
    if (n < 0 || size_t(n) >= sizeof(path)) return;
    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return;
    FILE* f = fdopen(fd, "w");
    if (!f) { close(fd); return; }
    std::fprintf(f, "build=35924 caller=%lx tid=%d phase=factory-entry script=%d\n",
                 (unsigned long)c.retRva, c.tid, c.script);
    DumpRange(f, "Proposal", c.rdx, 0x3c0); DumpRange(f, "Context", c.rcx, 0x68);
    struct Span { const char* name; size_t offset, stride, cap; };
    const Span spans[] = {{"addedNodes",0,24,4096}, {"addedSegments",0x18,120,4096},
        {"removedNodes",0x30,24,4096}, {"removedSegments",0x48,120,4096},
        {"frozen",0x220,4,4096}, {"segmentTags",0x270,32,4096},
        {"toRemove",0x288,4,4096}, {"toAdd",0x2a0,0x8f0,64}};
    for (const auto& span : spans) {
        SliceVec v;
        if (SliceReadStdVector(c.rdx + span.offset, span.stride, span.cap, &v))
            DumpRange(f, span.name, v.begin, v.count * span.stride);
        else std::fprintf(f, "%s unreadable/oversize\n", span.name);
    }
    SliceRecord rec{};
    for (bool upgrade : {false, true}) {
        if (SliceConstructionRecord(c.rdx, upgrade, &rec)) {
            std::fwrite(rec.data, 1, rec.len, f); SliceRecordFree(&rec); break;
        }
        SliceRecordFree(&rec);
    }
    std::fclose(f);
    SliceLog("[construction] read-only entry dump %s\n", path);
}
bool Prepare(const SliceAddCall&, void*)
{
    return SliceInjectWrite(pending, SliceArmedLine::One) != SliceInjectResult::NotWritten;
}
void Landed(const SliceAddCall*, SliceOutcome outcome, void*)
{
    if (outcome == SliceOutcome::RanNatively)
        SliceLog("[construction] strict %s cancel did not complete\n", pendingPlacement ? "placement" : "upgrade");
    SliceRecordFree(&pending);
}
void OnProposal(const SliceFactoryCall& c, void*)
{
    if (c.retRva == 0xe34860 || c.retRva == 0xe4f6bd || c.retRva == 0xf229a5 ||
        c.retRva == 0xdd4e99 || c.retRva == 0x1971333) Dump(c);
    if (c.retRva == 0x1971333) {
        if (SliceMergeTemplateStreet(c.rdx) && SliceDumpPropOn()) Dump(c);
        return;
    }
    if (c.script || !SliceSessionLive()) return;
    const bool placement = c.retRva == 0xe34860;
    // Same .22 routes and records: station internal tracks are regenerated by
    // the shared Lua construction replay. Module edits carry the new params.
    if (!placement && c.retRva != 0xe4f6bd && c.retRva != 0xf229a5 && c.retRva != 0xdd4e99) return;
    SliceVec add{};
    if (c.retRva == 0xdd4e99 && SliceReadStdVector(c.rdx + 0x2a0, 0x8f0, SIZE_MAX, &add) && !add.count) return;
    SliceRecord rec{};
    SliceVec edges{};
    if (placement && !SliceReadStdVector(c.rdx + 0x18, 120, SIZE_MAX, &edges)) return;
    const bool road = placement && edges.count;
    currentSerial = placement ? ++placementSerial : 0;
    currentRoad = road;
    bool decoded = !road || SliceProposalBuildRoadRecord(c.rdx, true, false, &rec);
    if (road && decoded && rec.len && rec.data[rec.len-1] == '\n') {
        rec.data[--rec.len] = 0;
        SliceRecordPrintf(&rec, " ps=%llu\n", currentSerial);
    }
    if (!decoded || !SliceConstructionRecord(c.rdx, !placement, &rec)) {
        SliceRecordFree(&rec);
        SliceLog("[construction] proposal failed construction decode; strict action awaits the core block\n");
        return;
    }
    SliceRecordFree(&pending);
    pending = rec;
    pendingPlacement = placement;
    // AddModuleComp has an EMPTY completion function (SC-ADD-PAIRING).
    if (!SliceArmCancel(c, {placement ? "CONXP" : "CONUP", c.retRva == 0xf229a5 ? SliceDone::IfPresent : SliceDone::Required,
                           false, nullptr, Landed, nullptr, Prepare})) {
        SliceRecordFree(&pending);
        SliceLog("[construction] strict replay could not arm; action awaits the core block\n");
    }
}
}

bool SliceConstructionParams(uintptr_t table, SliceRecord* out, size_t* nodes)
{
    return Params(table, out, nodes);
}
bool SliceConstructionRecord(uintptr_t proposal, bool upgrade, SliceRecord* out)
{
    SliceVec add{}, remove{};
    // .22 StashConxpFromProposal serializes the first CE; its upgrade shape
    // accepts nonempty add/remove lists and sends their first entries.
    if (!SliceReadStdVector(proposal + 0x2a0, 0x8f0, SIZE_MAX, &add) || !add.count ||
        !SliceReadStdVector(proposal + 0x288, 4, SIZE_MAX, &remove) || (upgrade && !remove.count)) return false;
    int32_t old = 0;
    if (upgrade && (!SliceReadT(remove.begin, &old) || old <= 0)) return false;
    std::string name;
    if (!SliceReadStdString(add.begin, &name) || name.empty()) return false;
    const size_t len = name.size();
    // Filename is a whitespace-delimited wire token; refuse ambiguous paths.
    for (size_t i = 0; i < len; ++i) if (static_cast<unsigned char>(name[i]) <= 32 || name[i] == 127) return false;
    float transform[16];
    if (!SliceRead(add.begin + 0x738, transform, sizeof(transform))) return false;
    for (float f : transform) if (!std::isfinite(f)) return false;
    if (upgrade) SliceRecordPrintf(out, "CONUP %d %s t=", old, name.c_str());
    else SliceRecordPrintf(out, "CONXP %s t=", name.c_str());
    for (unsigned i = 0; i < 16; ++i) SliceRecordPrintf(out, "%s%.4f", i ? "," : "", transform[i]);
    if (!upgrade) SliceRecordPrintf(out, " ps=%llu rc=%d", currentSerial, int(currentRoad));
    SliceRecordAppend(out, " params=", 8);
    if (!SliceConstructionParams(add.begin + 0x448, out, nullptr)) return false;
    SliceRecordAppend(out, "\n", 1);
    return !out->failed;
}

SLICE_AREA(slice_construction, "slice-construction")
{
    for (const auto& p : slice_construction::kProbes) {
        uint8_t actual[16];
        if (!SliceRead(SliceAddr(p.rva), actual, p.size) || std::memcmp(actual, p.bytes, p.size)) {
            SliceLog("[construction] byte gate mismatch at %lx; capture disabled\n", (unsigned long)p.rva);
            return;
        }
    }
    for (const auto& p : slice_construction::kPlacementProbes) {
        uint8_t actual[16];
        if (!SliceRead(SliceAddr(p.rva), actual, p.size) || std::memcmp(actual, p.bytes, p.size)) {
            SliceLog("[construction] UI parameter-provenance byte mismatch at %lx; capture disabled\n", (unsigned long)p.rva);
            return;
        }
    }
    SliceOnFactory({"slice-construction", 0x15ee930, OnProposal, nullptr, nullptr, true, 20});
    SliceLog("[construction] CONXP+ROADC placement, CONUP module/bulldozer upgrades and Linux template weld enabled\n");
}
