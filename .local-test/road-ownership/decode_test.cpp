
#include <cstdint>
#include <cstring>
#include <vector>
#include <cassert>
enum VecRead { VEC_EMPTY, VEC_OK, VEC_BAD };
static uint8_t records[240];
static uint64_t bytes=240;
static VecRead status=VEC_OK;
static VecRead ReadVecAnyEx(uint64_t, uint64_t* begin, uint64_t* span, const char*) {
    *begin=(uint64_t)records; *span=bytes; return status;
}
static void Log(const char*, ...) {}
struct Edge { int32_t node0, node1; float t0[3], t1[3]; int32_t btype, bidx; int32_t owner = -1; };
static int DecodeEdgesVec(uint64_t a2, std::vector<Edge>* out, const char* tag)
{
    out->clear();
    uint64_t begin = 0, span = 0;
    const VecRead r = ReadVecAnyEx(a2 + 0x18, &begin, &span, tag);
    if (r == VEC_EMPTY) return 0;
    if (r != VEC_OK) return -1;
    if (span % 120 != 0) {
        Log("[slice] %s: edge vector span %llu is not whole 120-byte records\n", tag, (unsigned long long)span);
        return -1;
    }
    const int n = (int)(span / 120);
    out->resize((size_t)n);
    const uint8_t* b = (const uint8_t*)begin;
    for (int i = 0; i < n; i++) {
        Edge& e = (*out)[(size_t)i];
        memcpy(&e.node0, b + (size_t)i * 120 + 0x08, 4);
        memcpy(&e.node1, b + (size_t)i * 120 + 0x0c, 4);
        memcpy(e.t0,     b + (size_t)i * 120 + 0x10, 12);
        memcpy(e.t1,     b + (size_t)i * 120 + 0x1c, 12);
        memcpy(&e.btype, b + (size_t)i * 120 + 0x28, 4);
        memcpy(&e.bidx,  b + (size_t)i * 120 + 0x2c, 4);
        // Optional PlayerOwned: paired ownership-tool capture, 2026-09-18.
        // The replaced records have flag 0; otherwise identical new records
        // have flag 1 and the local player at +0x70. Never read an absent optional.
        const uint8_t owned = b[(size_t)i * 120 + 0x74];
        if (owned > 1) return -1;
        if (owned) {
            memcpy(&e.owner, b + (size_t)i * 120 + 0x70, 4);
            if (e.owner < 0) return -1;
        }
    }
    return n;
}


static const uint8_t fixture[]={255,255,255,255,0,0,0,0,119,174,1,0,120,174,1,0,4,155,146,66,147,230,242,65,146,170,32,63,105,176,158,66,0,0,0,0,235,29,10,62,0,0,0,0,255,255,255,255,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,15,0,0,0,0,1,0,0,0,0,0,0,2,0,0,0,2,0,0,0,255,255,255,255,0,1,0,0,225,177,0,0,1,0,12,0,81,15,0,0,1,0,0,0};

int main() {
    memcpy(records, fixture, 120); memcpy(records+120, fixture, 120);
    records[120+0x74]=0; // absence must ignore the stale player payload
    std::vector<Edge> edges;
    assert(DecodeEdgesVec(0,&edges,"test")==2);
    assert(edges[0].owner==3921 && edges[1].owner==-1);
    assert(edges[0].node0==110199 && edges[0].node1==110200);
    records[0x74]=0;
    assert(DecodeEdgesVec(0,&edges,"test")==2 && edges[0].owner==-1);
    records[0x74]=2; assert(DecodeEdgesVec(0,&edges,"test")==-1);
    records[0x74]=1; memset(records+0x70,255,4);
    assert(DecodeEdgesVec(0,&edges,"test")==-1);
    bytes=119; assert(DecodeEdgesVec(0,&edges,"test")==-1);
    status=VEC_EMPTY; assert(DecodeEdgesVec(0,&edges,"test")==0 && edges.empty());
}
