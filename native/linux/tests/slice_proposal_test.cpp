// Off-game wire goldens and rejection tests use real guarded readers and the
// real libstdc++ vector/string ABI. Only hook registration/cancellation is faked.
#include "slice/slice_proposal.h"
#include "slice/slice_core_internal.h"
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>
#include <string>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); } } while (0)

static SliceFactoryHandler g_handler{};
static SliceArm g_arm{};
static bool g_armed = false;
static bool g_cancelAvailable = true;
static bool g_acceptArm = true;
bool SliceOnFactory(const SliceFactoryHandler& h) { g_handler = h; return true; }
bool SliceCancelAvailable() { return g_cancelAvailable; }
bool SliceArmCancel(const SliceFactoryCall&, const SliceArm& arm)
{
    CHECK(!g_armed);
    CHECK(arm.done == SliceDone::Required && !arm.honourArmed);
    if (!g_acceptArm) return false;
    g_arm = arm;
    g_armed = true;
    return true;
}
const char* SliceOutcomeName(SliceOutcome) { return "test outcome"; }
extern const SliceAreaEntry __start_tpf2mp_slice_areas[];
extern const SliceAreaEntry __stop_tpf2mp_slice_areas[];

template<size_t N> using Bytes = std::array<unsigned char, N>;
template<typename T, size_t N> void Put(Bytes<N>& bytes, size_t off, T value)
{ CHECK(off + sizeof(value) <= N); memcpy(bytes.data() + off, &value, sizeof(value)); }
template<typename T, size_t N> void VectorAt(Bytes<N>& bytes, size_t off, std::vector<T>& vec)
{
    static_assert(sizeof(vec) == 24, "test requires the game's libstdc++ vector ABI");
    memcpy(bytes.data() + off, &vec, 24);
}
template<size_t N> uintptr_t Address(Bytes<N>& b) { return reinterpret_cast<uintptr_t>(b.data()); }

static Bytes<0x18> Node(int id, float x, float y, float z)
{
    Bytes<0x18> n{};
    Put(n, 0, x); Put(n, 4, y); Put(n, 8, z); Put(n, 0x14, id);
    return n;
}
static Bytes<0x78> Edge(int id, int a, int b, int kind = 0)
{
    Bytes<0x78> e{};
    Put(e, 0, id); Put(e, 8, a); Put(e, 12, b);
    Put(e, 0x10, 10.0f); Put(e, 0x20, 10.0f);
    Put(e, 0x28, 1); Put(e, 0x2c, 7);
    Put(e, 0x48, kind); Put(e, 0x4c, 25); Put(e, 0x54, 2); Put(e, 0x60, 9);
    e[0x50] = 1; e[0x51] = 0xff; e[0x52] = 0xff; e[0x53] = 0xff;
    e[0x64] = kind ? 1 : 0xff; // an inactive component is deliberately garbage
    e[0x65] = 0xff;
    return e;
}

struct Fixture {
    Bytes<0x3c0> p{};
    std::vector<Bytes<0x18>> nodes, removedNodes;
    std::vector<Bytes<0x78>> edges, removedEdges;
    std::vector<int32_t> frozen, removedCon;
    uintptr_t bind()
    {
        VectorAt(p, 0, nodes); VectorAt(p, 0x18, edges);
        VectorAt(p, 0x30, removedNodes); VectorAt(p, 0x48, removedEdges);
        VectorAt(p, 0x220, frozen); VectorAt(p, 0x288, removedCon);
        return Address(p);
    }
};

static void RoadGoldens()
{
    Fixture f;
    f.nodes = {Node(-1, 1, 2, 3), Node(-2, 4, 5, 6)};
    f.edges = {Edge(-3, -1, -2)};
    f.frozen = {1};
    SliceRecord r{};
    CHECK(SliceProposalBuildRoadRecord(f.bind(), false, false, &r));
    CHECK(std::string(r.data) == "STREETP 1 2\nROADE 2 0 25 1 0 1 0 0"
          " -1 1.0000 2.0000 3.0000 -2 4.0000 5.0000 6.0000"
          " -1 -2 10.0000 0.0000 0.0000 0.0000 10.0000 0.0000 1 7\n");
    SliceRecordFree(&r);
    f.nodes.clear(); f.frozen.clear();
    f.edges = {Edge(-3, 101, 102, 1)};
    CHECK(SliceProposalBuildRoadRecord(f.bind(), false, false, &r));
    CHECK(std::string(r.data).find("ROADE 0 1 -1 9 1 1 0 0 101 102") == 0);
    SliceRecordFree(&r);
    CHECK(!SliceProposalBuildRoadRecord(f.bind(), false, true, &r));
    f.removedEdges = {Edge(500, 101, 102, 1)};
    CHECK(SliceProposalBuildRoadRecord(f.bind(), false, true, &r));
    CHECK(std::string(r.data).find("ROADE 0 1 -1 9 1 1 0 1 101 102") == 0);
    SliceRecordFree(&r);
    CHECK(SliceProposalBuildRoadRecord(f.bind(), true, false, &r));
    CHECK(std::string(r.data).find("ROADC 0 1 -1 9 1 1 1 101 102") == 0);
    CHECK(!strstr(r.data, "STREETP"));
    SliceRecordFree(&r);
}

static void CrossingAndBridgeGoldens()
{
    Fixture f;
    f.nodes = {Node(-1, 1, 2, 3), Node(-2, 4, 5, 6)};
    f.edges = {Edge(-3, -1, -2), Edge(-4, 201, 202, 1),
               Edge(-5, 301, -1), Edge(-6, -1, 302)};
    f.removedEdges = {Edge(501, 202, 201, 1), Edge(600, 301, 302)};
    SliceRecord r{};
    // Opposite-network bridge span: retain its geometry and its reversed
    // in-place removal, omit the split parent's removal. Lua identifies br.
    CHECK(SliceProposalBuildRoadRecord(f.bind(), false, false, &r));
    CHECK(strstr(r.data, "ROADE 2 0 25 1 0 4 0 1"));
    CHECK(strstr(r.data, " 202 201 10.0000 0.0000 0.0000 0.0000 10.0000 0.0000"));
    CHECK(!strstr(r.data, " 301 302 "));
    SliceRecordFree(&r);
    // Construction's existing ROADC contract carries every removal, including
    // the split parent that ROADE leaves to Lua's crossing classification.
    // Linux bool padding has no effect on the captured geometry.
    f.nodes[0][0x0d] = 0x7f;
    CHECK(SliceProposalBuildRoadRecord(f.bind(), true, false, &r));
    CHECK(strstr(r.data, "ROADC 2 0 25 1 0 4 2") == r.data);
    CHECK(strstr(r.data, " 202 201 10.0000 0.0000 0.0000 0.0000 10.0000 0.0000"));
    CHECK(strstr(r.data, " 301 302 10.0000 0.0000 0.0000 0.0000 10.0000 0.0000"));
    CHECK(!strstr(r.data, "STREETP") && !strstr(r.data, "ARMED"));
    CHECK(std::string(r.data).find(" 1 7 1 7 1 7 1 7\n") == r.len - 17);
    SliceRecordFree(&r);
    // Same-network companion with its own street type is also retained (bs).
    Put(f.edges[1], 0x48, 0); f.edges[1][0x64] = 0xff;
    Put(f.edges[1], 0x4c, 12);
    Put(f.removedEdges[0], 0x48, 0);
    CHECK(SliceProposalBuildRoadRecord(f.bind(), false, false, &r));
    CHECK(strstr(r.data, "ROADE 2 0 25 1 0 4 0 1"));
    SliceRecordFree(&r);
    // Level crossing: track is the drawn network, street halves remain on the
    // wire so the Lua can classify them against the standing street parent.
    f.edges = {Edge(-3, -1, -2, 1), Edge(-4, 301, -1), Edge(-5, -1, 302)};
    f.removedEdges = {Edge(600, 301, 302)};
    CHECK(SliceProposalBuildRoadRecord(f.bind(), false, false, &r));
    CHECK(strstr(r.data, "ROADE 2 1 -1 9 1 3 0 0"));
    CHECK(strstr(r.data, " 301 -1 "));
    CHECK(strstr(r.data, " -1 302 "));
    SliceRecordFree(&r);
}

static void RoadRejections()
{
    Fixture f;
    f.nodes = {Node(-1, 1, 2, 3), Node(-2, 4, 5, 6)};
    f.edges = {Edge(-3, -1, -2)};
    SliceRecord r{};
    f.frozen = {2};
    CHECK(!SliceProposalBuildRoadRecord(f.bind(), false, false, &r));
    f.frozen.clear();
    Put(f.nodes[0], 0, std::numeric_limits<float>::quiet_NaN());
    CHECK(!SliceProposalBuildRoadRecord(f.bind(), false, false, &r));
    Put(f.nodes[0], 0, 1.0f);
    Put(f.edges[0], 8, -99);
    CHECK(!SliceProposalBuildRoadRecord(f.bind(), false, false, &r));
    Put(f.edges[0], 8, -1);
    Put(f.edges[0], 0x48, 2);
    CHECK(!SliceProposalBuildRoadRecord(f.bind(), false, false, &r));
    Put(f.edges[0], 0x48, 0);
    f.edges[0][0x50] = 2;
    CHECK(!SliceProposalBuildRoadRecord(f.bind(), false, false, &r));
    f.edges[0][0x50] = 1;
    f.edges.push_back(Edge(-4, -1, -2, 1));
    CHECK(SliceProposalBuildRoadRecord(f.bind(), false, false, &r));
    SliceRecordFree(&r);
    f.edges.resize(1);
    f.nodes[1] = f.nodes[0];
    CHECK(!SliceProposalBuildRoadRecord(f.bind(), false, false, &r));
    f.nodes.clear(); f.edges = {Edge(-3, 101, 102)};
    f.edges.resize(600, f.edges[0]); // exceeds the Windows decoder's old 512 cap
    CHECK(SliceProposalBuildRoadRecord(f.bind(), false, false, &r));
    CHECK(strstr(r.data, "ROADE 0 0 25 1 0 600 0 0"));
    SliceRecordFree(&r);
    f.edges.resize(2049, f.edges[0]);
    CHECK(SliceProposalBuildRoadRecord(f.bind(), false, false, &r));
    CHECK(strstr(r.data, " 2049 0 0"));
    SliceRecordFree(&r);
    f.edges.resize(1); f.bind();
    Put(f.p, 0x20, reinterpret_cast<uintptr_t>(f.edges.data()) + 1); // fractional stride
    CHECK(!SliceProposalBuildRoadRecord(Address(f.p), false, false, &r));
    CHECK(!SliceProposalBuildRoadRecord(1, false, false, &r));
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    auto* blocked = mmap(nullptr, page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(blocked != MAP_FAILED);
    CHECK(!SliceProposalBuildRoadRecord(reinterpret_cast<uintptr_t>(blocked), false, false, &r));
    CHECK(munmap(blocked, page) == 0);
    CHECK(!r.len);
    SliceRecordFree(&r);
}

static void StopGoldens()
{
    Fixture f;
    f.removedEdges = {Edge(500, 101, 102)};
    std::vector<Bytes<0x100>> objects(1);
    std::vector<int32_t> removedObjects{123};
    auto& o = objects[0];
    Put(o, 4, 2); Put(o, 0x10, 17); Put(o, 0x44, 1.0f); Put(o, 0x48, 2.0f); Put(o, 0x4c, 3.0f);
    Put(o, 0xf8, 4); o[0xd1] = 1;
    auto* name = new (o.data() + 0xd8) std::string("Station\nEast");
    VectorAt(f.p, 0xe8, objects); VectorAt(f.p, 0xd0, removedObjects);
    SliceRecord r{};
    CHECK(SliceProposalBuildStopRecord(f.bind(), &r));
    CHECK(std::string(r.data) == "STOPX 500 2 17 1.0000 2.0000 3.0000 1 0 4 name=Station East\n");
    SliceRecordFree(&r);
    *name = std::string(10000, 'x'); // exercise the heap-string layout too
    CHECK(SliceProposalBuildStopRecord(f.bind(), &r));
    CHECK(strstr(r.data, name->c_str()));
    SliceRecordFree(&r);
    removedObjects.resize(64, 123); VectorAt(f.p, 0xd0, removedObjects);
    CHECK(!SliceProposalBuildStopRecord(f.bind(), &r));
    removedObjects.resize(1); VectorAt(f.p, 0xd0, removedObjects);
    Put(o, 4, 1); // category unsupported by the shared STOPX replay
    CHECK(!SliceProposalBuildStopRecord(f.bind(), &r));
    name->~basic_string();
    SliceRecordFree(&r);
}

static void DemolitionGoldens()
{
    Fixture f;
    f.removedEdges = {Edge(500, 101, 102, 1)};
    f.removedNodes = {Node(101, 1, 2, 3)};
    SliceRecord r{};
    CHECK(SliceProposalBuildBulldozeRecord(f.bind(), &r) == SliceProposalKind::EdgeDemolish);
    CHECK(std::string(r.data) == "EDEMO 1 1 101 102 1 101 1.0000 2.0000 3.0000\n");
    SliceRecordFree(&r);
    f.removedCon = {77, 88};
    CHECK(SliceProposalBuildBulldozeRecord(f.bind(), &r) == SliceProposalKind::ConstructionDemolish);
    CHECK(std::string(r.data) == "CDEMO 2 77 88\n");
    SliceRecordFree(&r);
    std::vector<Bytes<0x8f0>> additions(1);
    VectorAt(f.p, 0x2a0, additions);
    CHECK(SliceProposalBuildBulldozeRecord(f.bind(), &r) == SliceProposalKind::None);
    additions.clear(); VectorAt(f.p, 0x2a0, additions); f.removedCon.clear();
    using ObjectPair = std::array<int32_t, 2>;
    std::vector<ObjectPair> before, after;
    for (int i = 1; i <= 32; ++i) before.push_back({i, 2});
    after = before; after.pop_back();
    VectorAt(f.removedEdges[0], 0x30, before);
    CHECK(SliceProposalBuildBulldozeRecord(f.bind(), &r) == SliceProposalKind::None);
    f.edges = {f.removedEdges[0]};
    VectorAt(f.edges[0], 0x30, after);
    CHECK(SliceProposalBuildBulldozeRecord(f.bind(), &r) == SliceProposalKind::StopDelete);
    CHECK(std::string(r.data) == "STOPXDEL 32 500\n");
    SliceRecordFree(&r);
    after[0] = {99, 2}; VectorAt(f.edges[0], 0x30, after);
    CHECK(SliceProposalBuildBulldozeRecord(f.bind(), &r) == SliceProposalKind::None);
    after = before; after.pop_back(); VectorAt(f.edges[0], 0x30, after);
    Put(f.edges[0], 0x10, 20.0f);
    CHECK(SliceProposalBuildBulldozeRecord(f.bind(), &r) == SliceProposalKind::None);
    SliceRecordFree(&r);
}

static std::string ReadFile(const std::string& path)
{
    std::ifstream f(path);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

static void FinishArm(SliceOutcome outcome)
{
    CHECK(g_armed);
    if (outcome == SliceOutcome::CancelledFired || outcome == SliceOutcome::CancelledNotFired) {
        CHECK(g_arm.prepareCancel);
        SliceAddCall add{};
        if (!g_arm.prepareCancel(add, g_arm.ctx)) outcome = SliceOutcome::RanNatively;
    }
    g_arm.landed(nullptr, outcome, g_arm.ctx);
    g_armed = false;
}

static ssize_t NoSpaceWrite(int, const void*, size_t)
{
    errno = ENOSPC;
    return -1;
}

static int g_shortWrites;
static ssize_t ShortWrite(int fd, const void* data, size_t len)
{
    ++g_shortWrites;
    return write(fd, data, g_shortWrites == 1 ? len / 2 : len);
}

static void CaptureOutcomes(const std::string& dir)
{
    for (auto* a = __start_tpf2mp_slice_areas; a != __stop_tpf2mp_slice_areas; ++a) a->registerFn();
    CHECK(g_handler.onEntry && !g_handler.onReturn && g_handler.factoryRva == 0x15ee930);
    std::ofstream(dir + "tpf2_instance.txt") << "A\npid=" << getpid() << "\n";
    std::ofstream(dir + "lockstep_status_A.txt") << "t=10 peer=10\n";
    SliceSessionCacheReset();
    CHECK(SliceSessionLive());
    const std::string inject = dir + "lockstep_inject_A.txt";
    Fixture f;
    f.edges = {Edge(-1, 101, 102)};
    SliceFactoryCall call{};
    call.factory = SliceFactoryByRva(0x15ee930);
    call.retRva = 0xe86452; call.rdx = f.bind(); call.rdi = 0x12345; call.armable = true;
    g_handler.onEntry(call, nullptr);
    CHECK(g_armed && ReadFile(inject).empty()); // record is staged until Add decides
    // Commit must happen before completion/cancellation, never from landed.
    SliceAddCall add{};
    CHECK(g_arm.prepareCancel && g_arm.prepareCancel(add, g_arm.ctx));
    const std::string committed = ReadFile(inject);
    CHECK(committed.find("ARMED 1\nSTREETP 1 2\nROADE") == 0);
    g_arm.landed(nullptr, SliceOutcome::CancelledFired, g_arm.ctx); g_armed = false;
    CHECK(ReadFile(inject) == committed);
    CHECK(ReadFile(inject).find("ARMED 1\nSTREETP 1 2\nROADE") == 0);
    std::ofstream(inject, std::ios::trunc).close();
    g_handler.onEntry(call, nullptr);
    CHECK(g_armed);
    FinishArm(SliceOutcome::RanNatively);
    CHECK(ReadFile(inject).empty());
    for (auto outcome : {SliceOutcome::Mismatch, SliceOutcome::Superseded, SliceOutcome::Blocked}) {
        std::ofstream(inject, std::ios::trunc).close();
        g_handler.onEntry(call, nullptr);
        CHECK(g_armed);
        FinishArm(outcome);
        CHECK(ReadFile(inject).empty());
    }
    call.script = true;
    g_handler.onEntry(call, nullptr);
    CHECK(!g_armed && ReadFile(inject).empty());
    call.script = false;
    g_acceptArm = false;
    g_handler.onEntry(call, nullptr);
    CHECK(!g_armed && ReadFile(inject).empty());
    g_acceptArm = true;
    // The file can become unwritable after the factory captured successfully.
    // A pre-cancel write veto leaves no record; the core blocks the action.
    g_handler.onEntry(call, nullptr);
    CHECK(g_armed);
    SliceInjectSetWriteFn(NoSpaceWrite);
    CHECK(!g_arm.prepareCancel(add, g_arm.ctx));
    CHECK(ReadFile(inject).empty());
    FinishArm(SliceOutcome::Blocked);
    CHECK(ReadFile(inject).empty());
    SliceInjectSetWriteFn(nullptr);
    // Once part of an ARMED record reached disk, honour its replay commitment.
    g_handler.onEntry(call, nullptr);
    CHECK(g_armed);
    g_shortWrites = 0; SliceInjectSetWriteFn(ShortWrite);
    CHECK(g_arm.prepareCancel(add, g_arm.ctx));
    CHECK(ReadFile(inject).find("ARMED 1\nSTREETP") == 0);
    const int committedWrites = g_shortWrites;
    g_arm.landed(nullptr, SliceOutcome::CancelledFired, g_arm.ctx); g_armed = false;
    CHECK(g_shortWrites == committedWrites);
    SliceInjectSetWriteFn(nullptr);
    std::ofstream(inject, std::ios::trunc).close();
    call.script = false; call.retRva = 0xdd4e99;
    f.edges.clear(); f.removedCon = {77}; call.rdx = f.bind();
    g_handler.onEntry(call, nullptr);
    CHECK(g_armed);
    FinishArm(SliceOutcome::RanNatively);
    CHECK(ReadFile(inject).empty());
    std::ofstream(inject, std::ios::trunc).close();
    g_handler.onEntry(call, nullptr);
    CHECK(g_armed);
    FinishArm(SliceOutcome::CancelledFired);
    CHECK(ReadFile(inject) == "ARMED 1\nCDEMO 1 77\n");
    std::ofstream(inject, std::ios::trunc).close();
    g_cancelAvailable = false;
    g_handler.onEntry(call, nullptr);
    CHECK(!g_armed && ReadFile(inject).empty());
    std::ofstream(inject, std::ios::trunc).close();
    call.retRva = 0xeaaf79;
    g_handler.onEntry(call, nullptr);
    CHECK(!g_armed && ReadFile(inject).empty());
    std::ofstream(inject, std::ios::trunc).close();
    call.retRva = 0xdd4e99;
    std::vector<Bytes<0x8f0>> additions(1);
    VectorAt(f.p, 0x2a0, additions); call.rdx = f.bind();
    g_handler.onEntry(call, nullptr);
    CHECK(!g_armed && ReadFile(inject).empty());
    // Construction owns ROADC + CONXP as one cancelled record. The street area
    // never publishes an orphan ROADC from the same factory call.
    call.retRva = 0xe34860;
    f.edges = {Edge(-1, 101, 102)}; call.rdx = f.bind();
    g_cancelAvailable = true;
    g_handler.onEntry(call, nullptr);
    CHECK(!g_armed && ReadFile(inject).empty());
    // Explicit solo status releases the session latch and leaves tools alone.
    std::ofstream(dir + "lockstep_status_A.txt") <<
        "t=10  peer=?  skew=?  desyncs=0  late=0  applylag=0.0/0 of 0  queued=0  mp=1";
    SliceSessionCacheReset();
    CHECK(!SliceSessionLive());
    call.retRva = 0xe86452;
    g_handler.onEntry(call, nullptr);
    CHECK(!g_armed && ReadFile(inject).empty());
}

int main()
{
    char dirTemplate[] = "/tmp/tpf2mp-proposal-test-XXXXXX";
    char* tmp = mkdtemp(dirTemplate);
    CHECK(tmp);
    const std::string dir = std::string(tmp) + '/';
    SliceCoreEnv env{};
    env.dataDir = dir.c_str(); env.rootDir = "";
    CHECK(SliceCoreOpen(env) == SliceOpenResult::Ok);
    CHECK(SliceReadInit());
    RoadGoldens(); CrossingAndBridgeGoldens(); RoadRejections(); StopGoldens(); DemolitionGoldens(); CaptureOutcomes(dir);
    for (const char* name : {"tpf2_slice.log", "tpf2_slice.lock", "tpf2_instance.txt",
                             "lockstep_status_A.txt", "lockstep_inject_A.txt"})
        CHECK(unlink((dir + name).c_str()) == 0);
    CHECK(rmdir(tmp) == 0);
    puts("slice proposal: wire goldens, guarded rejection, and cancellation outcomes passed");
}
