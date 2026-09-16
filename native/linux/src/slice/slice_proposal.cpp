// Linux BuildProposal capture, read before the factory moves its by-value inputs.
// Only PROVEN build-35924 fields from SLICE_PROPOSAL.md are read. No Windows STL
// layout, game allocator, or unverified construction/template mutation is used.
#include "slice_proposal.h"
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {
constexpr uintptr_t kBuildProposal = 0x15ee930;
constexpr uintptr_t kStreetBuilder = 0xe86452;
constexpr uintptr_t kTrackModifier = 0xed6da9;
constexpr uintptr_t kStopBuilder = 0xeaaf79;
constexpr uintptr_t kBulldozer = 0xdd4e99;
constexpr size_t kMaxNodes = 4096;
constexpr size_t kMaxEdges = 2048;
constexpr size_t kMaxObjects = 128;
// Preserve the shipped Windows replacement decision in mixed-platform lobbies:
// the threshold is bytes, not objects (SLICE_PROPOSAL.md section 6).
constexpr size_t kStopReplaceRefusalBytes = 0x100;

struct Snapshot {
    unsigned char* data = nullptr;
    size_t count = 0;
    uintptr_t begin = 0;
    ~Snapshot() { free(data); }
    Snapshot() = default;
    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;
};

bool ReadVector(uintptr_t address, size_t stride, size_t maxCount, Snapshot* out)
{
    SliceVec v{};
    if (!SliceReadStdVector(address, stride, maxCount, &v)) return false;
    out->count = v.count;
    out->begin = v.begin;
    if (!v.count) return true;
    out->data = static_cast<unsigned char*>(malloc(v.count * stride));
    return out->data && SliceRead(v.begin, out->data, v.count * stride);
}

template <typename T> T Field(const unsigned char* data, size_t off)
{
    T value{};
    memcpy(&value, data + off, sizeof(value));
    return value;
}

bool Finite3(const unsigned char* record, size_t off)
{
    return std::isfinite(Field<float>(record, off)) &&
           std::isfinite(Field<float>(record, off + 4)) &&
           std::isfinite(Field<float>(record, off + 8));
}

bool ValidNodes(const Snapshot& nodes, bool existing)
{
    for (size_t i = 0; i < nodes.count; ++i) {
        const auto* p = nodes.data + i * 0x18;
        if (!Finite3(p, 0) || (existing && Field<int32_t>(p, 0x14) < 0)) return false;
        // Duplicate placeholders make topology ambiguous on the Lua reader.
        for (size_t j = 0; j < i; ++j)
            if (Field<int32_t>(nodes.data + j * 0x18, 0x14) == Field<int32_t>(p, 0x14)) return false;
    }
    return true;
}

bool KnownEndpoint(int32_t entity, const Snapshot& nodes)
{
    if (entity >= 0) return true;
    for (size_t i = 0; i < nodes.count; ++i)
        if (Field<int32_t>(nodes.data + i * 0x18, 0x14) == entity) return true;
    return false;
}

bool ValidEdges(const Snapshot& edges, const Snapshot& nodes, bool existing)
{
    for (size_t i = 0; i < edges.count; ++i) {
        const auto* p = edges.data + i * 0x78;
        const auto kind = Field<int32_t>(p, 0x48);
        const auto baseType = Field<int32_t>(p, 0x28);
        const auto n0 = Field<int32_t>(p, 8), n1 = Field<int32_t>(p, 12);
        if ((kind != 0 && kind != 1) || baseType < 0 || baseType > 2 ||
            (baseType && Field<int32_t>(p, 0x2c) < 0) ||
            !Finite3(p, 0x10) || !Finite3(p, 0x1c) || n0 == n1 ||
            !KnownEndpoint(n0, nodes) || !KnownEndpoint(n1, nodes) ||
            (existing && (n0 < 0 || n1 < 0 || Field<int32_t>(p, 0) < 0))) return false;
        // Only the active type's fields are meaningful; adjacent bool padding
        // and the inactive street/track component can contain arbitrary bytes.
        if (kind == 0) {
            if (Field<int32_t>(p, 0x4c) < 0 || p[0x50] > 1) return false;
        } else if (Field<int32_t>(p, 0x60) < 0 || p[0x64] > 1) return false;
    }
    return true;
}

void AppendNodes(SliceRecord* out, const Snapshot& nodes)
{
    for (size_t i = 0; i < nodes.count; ++i) {
        const auto* p = nodes.data + i * 0x18;
        SliceRecordPrintf(out, " %d %.4f %.4f %.4f", Field<int32_t>(p, 0x14),
                          Field<float>(p, 0), Field<float>(p, 4), Field<float>(p, 8));
    }
}

void AppendEdge(SliceRecord* out, const unsigned char* p)
{
    SliceRecordPrintf(out, " %d %d %.4f %.4f %.4f %.4f %.4f %.4f",
                      Field<int32_t>(p, 8), Field<int32_t>(p, 12),
                      Field<float>(p, 0x10), Field<float>(p, 0x14), Field<float>(p, 0x18),
                      Field<float>(p, 0x1c), Field<float>(p, 0x20), Field<float>(p, 0x24));
}

void AppendEdges(SliceRecord* out, const Snapshot& edges)
{
    for (size_t i = 0; i < edges.count; ++i) AppendEdge(out, edges.data + i * 0x78);
}

bool SameEndpoints(const unsigned char* a, const unsigned char* b)
{
    const auto a0 = Field<int32_t>(a, 8), a1 = Field<int32_t>(a, 12);
    const auto b0 = Field<int32_t>(b, 8), b1 = Field<int32_t>(b, 12);
    return (a0 == b0 && a1 == b1) || (a0 == b1 && a1 == b0);
}

bool ValidateFrozenNodes(uintptr_t proposal, size_t nodeCount)
{
    Snapshot frozen;
    if (!ReadVector(proposal + 0x220, 4, kMaxNodes, &frozen)) return false;
    for (size_t i = 0; i < frozen.count; ++i) {
        const int32_t index = Field<int32_t>(frozen.data, i * 4);
        if (index < 0 || static_cast<size_t>(index) >= nodeCount) return false;
    }
    return true;
}

bool ReadObjects(uintptr_t segment, Snapshot* objects)
{
    if (!ReadVector(segment + 0x30, 8, kMaxObjects, objects)) return false;
    for (size_t i = 0; i < objects->count; ++i) {
        const auto id = Field<int32_t>(objects->data, i * 8);
        const auto type = Field<int32_t>(objects->data, i * 8 + 4);
        if (id <= 0 || type < 0 || type > 2) return false;
        for (size_t j = 0; j < i; ++j)
            if (Field<int32_t>(objects->data, j * 8) == id) return false;
    }
    return true;
}

bool StopDelete(const Snapshot& removed, const Snapshot& added, SliceRecord* out)
{
    if (removed.count != 1 || added.count != 1) return false;
    const auto* beforeEdge = removed.data;
    const auto* afterEdge = added.data;
    // Require the edge itself to survive with identical geometry. A replacement
    // that edits topology is not represented by STOPXDEL.
    if (Field<int32_t>(beforeEdge, 0) < 0 ||
        memcmp(beforeEdge + 8, afterEdge + 8, 0x28) ||
        Field<int32_t>(beforeEdge, 0x48) != Field<int32_t>(afterEdge, 0x48)) return false;
    Snapshot before, after;
    if (!ReadObjects(removed.begin, &before) || !ReadObjects(added.begin, &after) ||
        before.count != after.count + 1) return false;
    int32_t gone = -1;
    size_t missing = 0;
    for (size_t i = 0; i < before.count; ++i) {
        bool kept = false;
        for (size_t j = 0; j < after.count; ++j)
            if (!memcmp(before.data + i * 8, after.data + j * 8, 8)) kept = true;
        if (!kept) { gone = Field<int32_t>(before.data, i * 8); ++missing; }
    }
    if (missing != 1 || gone <= 0) return false;
    SliceRecordPrintf(out, "STOPXDEL %d %d\n", gone, Field<int32_t>(beforeEdge, 0));
    return !out->failed;
}

const char* KindName(SliceProposalKind kind)
{
    switch (kind) {
        case SliceProposalKind::Road: return "ROADE";
        case SliceProposalKind::ConstructionRoad: return "ROADC";
        case SliceProposalKind::Stop: return "STOPX";
        case SliceProposalKind::StopDelete: return "STOPXDEL";
        case SliceProposalKind::EdgeDemolish: return "EDEMO";
        case SliceProposalKind::ConstructionDemolish: return "CDEMO";
        default: return "proposal";
    }
}

struct Pending {
    SliceRecord record;
    SliceProposalKind kind;
    SliceInjectResult shipped;
};

// Called by slice-core after checking the Command pointer/tag and completion
// callback, before firing that callback or skipping Add. A failed write leaves
// the command and its callback untouched; the central multiplayer barrier
// blocks the action because no replay was committed.
bool PrepareCancel(const SliceAddCall&, void* ctx)
{
    auto* pending = static_cast<Pending*>(ctx);
    pending->shipped = SliceInjectWrite(pending->record, SliceArmedLine::One);
    if (pending->shipped == SliceInjectResult::NotWritten) {
        SliceLog("[proposal] %s: capture write failed; cancellation refused\n", KindName(pending->kind));
        return false;
    }
    // Some record bytes on disk can already be replayed. Preserve the cancel
    // on a partial append, as required by SliceInjectWrite's contract.
    return true;
}

void Landed(const SliceAddCall*, SliceOutcome outcome, void* ctx)
{
    auto* pending = static_cast<Pending*>(ctx);
    const bool cancelled = outcome == SliceOutcome::CancelledFired ||
                           outcome == SliceOutcome::CancelledNotFired;
    if (cancelled) {
        const auto result = pending->shipped;
        SliceLog("[proposal] %s: %s, capture %s\n", KindName(pending->kind), SliceOutcomeName(outcome),
                 result == SliceInjectResult::Written ? "written" :
                 result == SliceInjectResult::Partial ? "PARTIAL" : "NOT WRITTEN");
    }
    SliceRecordFree(&pending->record);
    free(pending);
}

void Submit(const SliceFactoryCall& call, SliceProposalKind kind, SliceRecord* record)
{
    if (record->failed || !record->len) return;
    if (SliceSessionLive() && call.armable && SliceCancelAvailable()) {
        auto* pending = static_cast<Pending*>(calloc(1, sizeof(Pending)));
        if (pending) {
            pending->record = *record;
            pending->kind = kind;
            pending->shipped = SliceInjectResult::NotWritten;
            SliceArm arm{KindName(kind), SliceDone::Required, false, nullptr, Landed, pending};
            arm.prepareCancel = PrepareCancel;
            if (SliceArmCancel(call, arm)) { *record = {}; return; }
            free(pending); // the caller still owns the record after a refused arm
        }
    }
    SliceLog("[proposal] %s: capture not armed; multiplayer action will be blocked\n", KindName(kind));
}

void OnBuildProposal(const SliceFactoryCall& call, void*)
{
    if (call.script || !call.rdx || !SliceSessionLive()) return;
    SliceRecord record{};
    SliceProposalKind kind = SliceProposalKind::None;
    if (call.retRva == kStreetBuilder || call.retRva == kTrackModifier) {
        if (SliceProposalBuildRoadRecord(call.rdx, false, call.retRva == kTrackModifier, &record))
            kind = SliceProposalKind::Road;
    } else if (call.retRva == kStopBuilder) {
        if (SliceProposalBuildStopRecord(call.rdx, &record)) kind = SliceProposalKind::Stop;
    } else if (call.retRva == kBulldozer) {
        SliceVec constructions{};
        if (SliceReadStdVector(call.rdx + 0x2a0, 0x8f0, 256, &constructions) && constructions.count)
            return; // CONUP (module removal) is owned by slice-construction.
        kind = SliceProposalBuildBulldozeRecord(call.rdx, &record);
    } else {
        // Terrain, script carriers and construction upgrade routing belong to
        // their areas; unknown UI property editors are intentionally not roads.
        return;
    }
    if (kind == SliceProposalKind::None) {
        SliceLog("[proposal] caller_rva=%lx: unsupported, empty, or unreadable proposal; multiplayer action will be blocked\n",
                 static_cast<unsigned long>(call.retRva));
    } else Submit(call, kind, &record);
    SliceRecordFree(&record);
}
} // namespace

bool SliceProposalBuildRoadRecord(uintptr_t proposal, bool construction, bool upgrade, SliceRecord* out)
{
    if (!proposal || !out || out->failed) return false;
    Snapshot nodes, edges, removed;
    if (!ReadVector(proposal, 0x18, kMaxNodes, &nodes) ||
        !ReadVector(proposal + 0x18, 0x78, kMaxEdges, &edges) || !edges.count ||
        !ReadVector(proposal + 0x48, 0x78, kMaxEdges, &removed) ||
        !ValidNodes(nodes, false) || !ValidEdges(edges, nodes, false) ||
        !ValidEdges(removed, nodes, true) || !ValidateFrozenNodes(proposal, nodes.count) ||
        (upgrade && (removed.count < edges.count))) return false;
    const auto* first = edges.data;
    const auto kind = Field<int32_t>(first, 0x48);
    // Preserve mixed-network geometry, as the Windows capture does. The Lua
    // reader identifies level-crossing split halves and unchanged bridge spans
    // against the originator's standing network; release 0.4.22 carries spans
    // as br/bs companions with their original kind and street/track properties.
    const int32_t streetType = kind == 0 ? Field<int32_t>(first, 0x4c) : -1;
    const int32_t trackType = kind == 1 ? Field<int32_t>(first, 0x60) : 1;
    const unsigned catenary = kind == 1 ? first[0x64] : 0;
    bool shipRemoval[kMaxEdges]{};
    size_t removalCount = 0;
    for (size_t i = 0; i < removed.count; ++i) {
        bool ship = construction || upgrade;
        // Release 0.4.22 also ships removals replaced between the same pair of
        // existing nodes (not split parents): a span above the newly drawn road
        // must be refreshed with its supports, not duplicated on replay.
        if (!ship) for (size_t j = 0; j < edges.count; ++j) {
            if (SameEndpoints(removed.data + i * 0x78, edges.data + j * 0x78)) { ship = true; break; }
        }
        shipRemoval[i] = ship;
        if (ship) ++removalCount;
    }
    if (!construction && kind == 0)
        SliceRecordPrintf(out, "STREETP %u %d\n", first[0x50], Field<int32_t>(first, 0x54));
    if (construction)
        SliceRecordPrintf(out, "ROADC %zu %d %d %d %u %zu %zu", nodes.count, kind,
                          streetType, trackType, catenary, edges.count, removalCount);
    else
        SliceRecordPrintf(out, "ROADE %zu %d %d %d %u %zu 0 %zu", nodes.count, kind,
                          streetType, trackType, catenary, edges.count, removalCount);
    AppendNodes(out, nodes);
    AppendEdges(out, edges);
    for (size_t i = 0; i < removed.count; ++i)
        if (shipRemoval[i]) AppendEdge(out, removed.data + i * 0x78);
    for (size_t i = 0; i < edges.count; ++i)
        SliceRecordPrintf(out, " %d %d", Field<int32_t>(edges.data + i * 0x78, 0x28),
                          Field<int32_t>(edges.data + i * 0x78, 0x2c));
    SliceRecordAppend(out, "\n", 1);
    return !out->failed;
}

bool SliceProposalBuildStopRecord(uintptr_t proposal, SliceRecord* out)
{
    if (!proposal || !out || out->failed) return false;
    Snapshot edges, objects, removed;
    if (!ReadVector(proposal + 0x48, 0x78, 1, &edges) || edges.count != 1 ||
        !ReadVector(proposal + 0xe8, 0x100, 1, &objects) || objects.count != 1 ||
        !ReadVector(proposal + 0xd0, 4, kMaxObjects, &removed)) return false;
    if (removed.count) SliceLog("[proposal] STOPX replacement removes %zu object(s), Windows refusal threshold %zu bytes\n",
                               removed.count, kStopReplaceRefusalBytes);
    if (removed.count * 4 >= kStopReplaceRefusalBytes) return false;
    const int32_t edge = Field<int32_t>(edges.data, 0);
    const auto* object = objects.data;
    const int32_t kind = Field<int32_t>(object, 4), model = Field<int32_t>(object, 0x10);
    const int32_t player = Field<int32_t>(object, 0xf8);
    if (edge < 0 || (kind != 0 && kind != 2) || model <= 0 || player < 0 ||
        !Finite3(object, 0x44) || object[0xd0] > 1 || object[0xd1] > 1) return false;
    char name[4097];
    size_t nameLen = 0;
    // A libstdc++ short string's pointer refers to the original game's object,
    // not the snapshot. Never reinterpret a copied string as a C++ object.
    if (!SliceReadStdString(objects.begin + 0xd8, name, sizeof(name), &nameLen, sizeof(name) - 1)) return false;
    for (size_t i = 0; i < nameLen; ++i) {
        if (!name[i]) return false;
        if (name[i] == '\r' || name[i] == '\n') name[i] = ' ';
    }
    SliceRecordPrintf(out, "STOPX %d %d %d %.4f %.4f %.4f %u %u %d name=%s\n",
                      edge, kind, model, Field<float>(object, 0x44), Field<float>(object, 0x48),
                      Field<float>(object, 0x4c), object[0xd1], object[0xd0], player, name);
    return !out->failed;
}

SliceProposalKind SliceProposalBuildBulldozeRecord(uintptr_t proposal, SliceRecord* out)
{
    if (!proposal || !out || out->failed) return SliceProposalKind::None;
    Snapshot addedCon, removedCon, added, removed, nodes;
    if (!ReadVector(proposal + 0x2a0, 0x8f0, 256, &addedCon) ||
        !ReadVector(proposal + 0x288, 4, 256, &removedCon) || addedCon.count ||
        !ReadVector(proposal + 0x18, 0x78, kMaxEdges, &added) ||
        !ReadVector(proposal + 0x48, 0x78, kMaxEdges, &removed) ||
        !ReadVector(proposal + 0x30, 0x18, kMaxNodes, &nodes)) return SliceProposalKind::None;
    if (removedCon.count) {
        for (size_t i = 0; i < removedCon.count; ++i)
            if (Field<int32_t>(removedCon.data, i * 4) <= 0) return SliceProposalKind::None;
        SliceRecordPrintf(out, "CDEMO %zu", removedCon.count);
        for (size_t i = 0; i < removedCon.count; ++i)
            SliceRecordPrintf(out, " %d", Field<int32_t>(removedCon.data, i * 4));
        SliceRecordAppend(out, "\n", 1);
        return out->failed ? SliceProposalKind::None : SliceProposalKind::ConstructionDemolish;
    }
    if (added.count && removed.count)
        return StopDelete(removed, added, out) ? SliceProposalKind::StopDelete : SliceProposalKind::None;
    if (added.count || !removed.count || !ValidNodes(nodes, true) ||
        !ValidEdges(removed, nodes, true)) return SliceProposalKind::None;
    // Replay cannot safely bulldoze an edge that still carries a stop/signal;
    // the Lua executor rejects it because the engine asserts on live objects.
    for (size_t i = 0; i < removed.count; ++i) {
        Snapshot objects;
        if (!ReadObjects(removed.begin + i * 0x78, &objects) || objects.count) return SliceProposalKind::None;
    }
    SliceRecordPrintf(out, "EDEMO %zu %zu", removed.count, nodes.count);
    for (size_t i = 0; i < removed.count; ++i) {
        const auto* edge = removed.data + i * 0x78;
        SliceRecordPrintf(out, " %d %d %d", Field<int32_t>(edge, 8), Field<int32_t>(edge, 12),
                          Field<int32_t>(edge, 0x48));
    }
    AppendNodes(out, nodes);
    SliceRecordAppend(out, "\n", 1);
    return out->failed ? SliceProposalKind::None : SliceProposalKind::EdgeDemolish;
}

SLICE_AREA(slice_proposal, "slice-proposal")
{
    SliceOnFactory({"slice-proposal", kBuildProposal, OnBuildProposal, nullptr, nullptr, true, 100});
    SliceLog("[proposal] cancelled ROADE/STREETP, STOPX/STOPXDEL, EDEMO/CDEMO registered; "
             "construction owns atomic ROADC+CONXP capture\n");
}
