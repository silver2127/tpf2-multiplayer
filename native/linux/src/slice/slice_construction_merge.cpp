// Existing .22 MergeTemplateStreet geometry, with Linux container ownership.
#include "slice_construction.h"
#include "slice_terrain_assets.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace {
constexpr size_t MaxRecords = SIZE_MAX, MaxOwnedBytes = SliceSanityBytes;
using Node = std::array<uint8_t, 24>;
using Edge = std::array<uint8_t, 120>;
using String = std::array<uint8_t, 32>;
template<class T> T Get(const void* at, size_t offset) { T value; std::memcpy(&value, static_cast<const uint8_t*>(at) + offset, sizeof(value)); return value; }
template<class T> void Put(void* at, size_t offset, T value) { std::memcpy(static_cast<uint8_t*>(at) + offset, &value, sizeof(value)); }
bool Writable(uintptr_t address, size_t bytes)
{
    if (!address || address + bytes < address) return false;
    FILE* f = std::fopen("/proc/self/maps", "re"); if (!f) return false;
    char line[512]; uintptr_t cursor = address;
    while (std::fgets(line, sizeof(line), f)) {
        unsigned long lo, hi; char mode[5]{};
        if (std::sscanf(line, "%lx-%lx %4s", &lo, &hi, mode) != 3) continue;
        if (lo <= cursor && cursor < hi) {
            if (mode[0] != 'r' || mode[1] != 'w') break;
            cursor = std::min(uintptr_t(hi), address + bytes);
            if (cursor == address + bytes) break;
        }
    }
    std::fclose(f); return cursor == address + bytes;
}
struct Edit { uintptr_t at; std::vector<uint8_t> before, after; };
struct Transaction {
    slice_terrain_assets::GameMemory memory;
    std::vector<Edit> edits;
    std::vector<void*> created, retired;
    std::map<uintptr_t, uintptr_t> owned;
    explicit Transaction(slice_terrain_assets::GameMemory m) : memory(m) {}
    ~Transaction() { for (void* p : created) memory.release(p); }
    bool Own(uintptr_t at, size_t bytes) {
        if (!at) return bytes == 0;
        if (bytes > MaxOwnedBytes || at + bytes < at) return false;
        const uintptr_t end = at + std::max(bytes, size_t(1));
        auto next = owned.lower_bound(at);
        if (next != owned.end() && next->first < end) return false;
        if (next != owned.begin() && std::prev(next)->second > at) return false;
        owned.emplace(at, end); return true;
    }
    bool Vector(uintptr_t at, size_t stride, size_t max, SliceVec* out) {
        uintptr_t cap;
        return SliceReadStdVector(at, stride, max, out) && SliceReadT(at + 16, &cap) &&
               Own(out->begin, cap - out->begin);
    }
    bool Write(uintptr_t at, const void* bytes, size_t count) {
        if (!count) return true;
        if (!Writable(at, count)) return false;
        Edit edit{at, std::vector<uint8_t>(count), std::vector<uint8_t>(count)};
        if (!SliceRead(at, edit.before.data(), count)) return false;
        std::memcpy(edit.after.data(), bytes, count); edits.push_back(std::move(edit)); return true;
    }
    template<class T> bool Scalar(uintptr_t at, T value) { return Write(at, &value, sizeof(value)); }
    void* Allocate(size_t bytes) {
        if (!memory.allocate || !memory.release) return nullptr;
        // Reserve bookkeeping before allocating with the foreign runtime.
        created.reserve(created.size() + 1);
        void* p = memory.allocate(bytes); if (p) created.push_back(p); return p;
    }
    bool Retire(uintptr_t at) {
        if (!at) return true;
        if (!memory.release || std::find(retired.begin(), retired.end(), reinterpret_cast<void*>(at)) != retired.end()) return false;
        retired.push_back(reinterpret_cast<void*>(at)); return true;
    }
    bool Commit() {
        // All ownership, allocation and writable-range checks precede any write.
        for (const auto& edit : edits) {
            std::vector<uint8_t> now(edit.before.size());
            if (!SliceRead(edit.at, now.data(), now.size()) || now != edit.before) return false;
        }
        for (const auto& edit : edits) std::memcpy(reinterpret_cast<void*>(edit.at), edit.after.data(), edit.after.size());
        created.clear();
        for (void* p : retired) memory.release(p);
        return true;
    }
};
bool ReadIndices(Transaction& tx, uintptr_t at, size_t nodes, SliceVec* span, std::vector<int32_t>* values)
{
    if (!tx.Vector(at, 4, MaxRecords, span)) return false;
    values->resize(span->count);
    if (!SliceRead(span->begin, values->data(), span->count * 4)) return false;
    for (int32_t index : *values) if (index < 0 || size_t(index) >= nodes) return false;
    return true;
}
bool StringOwnership(Transaction& tx, uintptr_t at, String* value)
{
    std::string text;
    if (!SliceReadStdString(at, &text) || !SliceRead(at, value->data(), 32)) return false;
    const uintptr_t ptr = Get<uintptr_t>(value->data(), 0);
    if (ptr == at + 16) return true;
    const size_t capacity = Get<size_t>(value->data(), 16);
    return capacity < MaxOwnedBytes && tx.Own(ptr, capacity + 1);
}
void EmptyString(String& value, uintptr_t at)
{
    value = {}; Put(value.data(), 0, at + 16);
}
}

// Adopt the unique boundary connector of a modular station. Linux vectors and
// SSO strings must be moved with their ownership intact when indices compact.
static bool MergeStationEndpoint(uintptr_t p, const slice_terrain_assets::GameMemory* supplied)
{
    Transaction tx(supplied ? *supplied : slice_terrain_assets::RuntimeMemory());
    SliceVec nv{},sv{},cv{},fv{},cfv{},tv{};
    if(!p || !tx.Own(p,0x3c0) || !tx.Vector(p,24,MaxRecords,&nv) || nv.count<4 ||
       !tx.Vector(p+0x18,120,MaxRecords,&sv) || sv.count<3 ||
       !tx.Vector(p+0x2a0,0x8f0,1,&cv) || cv.count!=1)return false;
    std::vector<Node> nodes(nv.count);std::vector<Edge> edges(sv.count);
    if(!SliceRead(nv.begin,nodes.data(),nodes.size()*24) || !SliceRead(sv.begin,edges.data(),edges.size()*120))return false;
    auto id=[&](size_t i){return Get<int32_t>(nodes[i].data(),20);};
    auto find=[&](int32_t v){for(size_t i=0;i<nodes.size();++i)if(id(i)==v)return i;return SIZE_MAX;};
    std::vector<size_t> degree(nv.count);std::vector<bool> owned(nv.count);
    std::vector<SliceVec> objects(sv.count);
    for(size_t i=0;i<nv.count;++i){
        for(size_t k=0;k<12;k+=4)if(!std::isfinite(Get<float>(nodes[i].data(),k)))return false;
        for(size_t j=0;j<i;++j)if(id(i)==id(j))return false;
    }
    for(size_t i=0;i<sv.count;++i){
        const auto& e=edges[i];
        if(e[0x74]>1 || !tx.Vector(sv.begin+i*120+0x30,8,SIZE_MAX,&objects[i]))return false;
        for(size_t k=0x10;k<0x28;k+=4)if(!std::isfinite(Get<float>(e.data(),k)))return false;
        if(e[0x74])for(size_t off:{size_t(8),size_t(12)}){
            size_t n=find(Get<int32_t>(e.data(),off));if(n!=SIZE_MAX){++degree[n];owned[n]=true;}
        }
    }
    size_t o=SIZE_MAX,a=0,x=0,t=0,u=0;
    for(size_t s=0;s<sv.count;++s)if(!edges[s][0x74]){
        int32_t v0=Get<int32_t>(edges[s].data(),8),v1=Get<int32_t>(edges[s].data(),12);
        if((v0<0)==(v1<0))continue;
        size_t xi=find(v0<0?v0:v1);if(xi==SIZE_MAX || owned[xi])continue;
        float dir[3];memcpy(dir,edges[s].data()+(v0<0?0x10:0x1c),12);
        if(v1<0)for(float& d:dir)d=-d;
        for(size_t ti=0;ti<nv.count;++ti)if(owned[ti]){
            float distance=0;
            for(size_t k=0;k<3;++k){float d=Get<float>(nodes[xi].data(),k*4)-Get<float>(nodes[ti].data(),k*4);distance+=d*d;}
            if(distance>0.0625f)continue;
            for(size_t j=0;j<sv.count;++j)if(edges[j][0x74] && Get<int32_t>(edges[j].data(),4)==Get<int32_t>(edges[s].data(),4)){
                int32_t j0=Get<int32_t>(edges[j].data(),8),j1=Get<int32_t>(edges[j].data(),12);
                size_t ui=j0==id(ti)?find(j1):j1==id(ti)?find(j0):SIZE_MAX;
                if(ui==SIZE_MAX || degree[ui]!=1)continue;
                float dot=0,dd=0,ll=0;
                for(size_t k=0;k<3;++k){float v=Get<float>(nodes[ui].data(),k*4)-Get<float>(nodes[ti].data(),k*4);dot+=v*dir[k];dd+=dir[k]*dir[k];ll+=v*v;}
                if(dot<=0 || dot*dot<0.99f*dd*ll || dd<1e-6f || ll<1e-6f)continue;
                if(o!=SIZE_MAX)return false;
                o=s;a=j;x=xi;t=ti;u=ui;
            }
        }
    }
    if(o==SIZE_MAX || objects[o].count)return false;
    int32_t before;size_t skip;uintptr_t head;
    if(!SliceReadT(cv.begin+0x790,&before) || before<0 || size_t(before)>sv.count || o>=size_t(before) || a<size_t(before) ||
       !SliceReadT(p+0x250,&skip) || skip || !SliceReadT(p+0x248,&head) || head)return false;
    std::vector<int32_t> frozen,ceFrozen;
    if(!ReadIndices(tx,p+0x220,nv.count,&fv,&frozen) || !ReadIndices(tx,cv.begin+0x778,nv.count,&cfv,&ceFrozen))return false;
    for(const auto* indices:{&frozen,&ceFrozen})for(int32_t i:*indices)if(size_t(i)==u)return false;
    // No surviving segment may reference the discarded dangling endpoint.
    for(size_t i=0;i<sv.count;++i)if(i!=a && (Get<int32_t>(edges[i].data(),8)==id(u) || Get<int32_t>(edges[i].data(),12)==id(u)))return false;
    if(!tx.Vector(p+0x270,32,MaxRecords,&tv) || (tv.count && tv.count!=sv.count))return false;
    std::vector<String> tags(tv.count);
    for(size_t i=0;i<tv.count;++i)if(!StringOwnership(tx,tv.begin+i*32,&tags[i]))return false;
    if(!tx.Retire(objects[o].begin))return false;
    if(tv.count){
        uintptr_t old=Get<uintptr_t>(tags[o].data(),0);
        if(old!=tv.begin+o*32+16 && !tx.Retire(old))return false;
        for(size_t i=o;i+1<tv.count;++i){
            tags[i]=tags[i+1];
            if(Get<uintptr_t>(tags[i].data(),0)==tv.begin+(i+1)*32+16)Put(tags[i].data(),0,tv.begin+i*32+16);
        }
        EmptyString(tags.back(),tv.begin+(tv.count-1)*32);
        if(!tx.Write(tv.begin,tags.data(),tags.size()*32) || !tx.Scalar(p+0x278,tv.begin+(tv.count-1)*32))return false;
    }
    std::vector<int32_t> remap(nv.count,-1);int32_t count=0;
    for(size_t i=0;i<nv.count;++i)if(i!=x && i!=u)remap[i]=count++;
    remap[x]=remap[t];
    memcpy(edges[a].data()+8,edges[o].data()+8,32);
    for(auto& edge:edges)for(size_t off:{size_t(8),size_t(12)})if(Get<int32_t>(edge.data(),off)==id(x))Put(edge.data(),off,id(t));
    for(auto* indices:{&frozen,&ceFrozen})for(int32_t& i:*indices)i=remap[i];
    size_t write=0;for(size_t i=0;i<nv.count;++i)if(i!=x && i!=u)nodes[write++]=nodes[i];
    for(size_t i=o;i+1<sv.count;++i)edges[i]=edges[i+1];
    edges.back()={};
    if(!tx.Write(nv.begin,nodes.data(),nodes.size()*24) || !tx.Write(sv.begin,edges.data(),edges.size()*120) ||
       !tx.Write(fv.begin,frozen.data(),frozen.size()*4) || !tx.Write(cfv.begin,ceFrozen.data(),ceFrozen.size()*4) ||
       !tx.Scalar(p+8,nv.begin+(nv.count-2)*24) || !tx.Scalar(p+0x20,sv.begin+(sv.count-1)*120) || !tx.Scalar(cv.begin+0x790,before-1) || !tx.Commit())return false;
    SliceLog("[station-weld] adopted connector %zu into segment %zu; frozen indices remapped\n",o,a);
    return true;
}

static bool MergeTemplateStreet(uintptr_t proposal, const slice_terrain_assets::GameMemory* supplied)
{
    Transaction tx(supplied ? *supplied : slice_terrain_assets::RuntimeMemory());
    SliceVec nv{}, sv{}, rv{}, cv{}, fv{}, cfv{}, tv{};
    if (!proposal || !tx.Own(proposal, 0x3c0) ||
        !tx.Vector(proposal, 24, MaxRecords, &nv) || nv.count < 2 ||
        !tx.Vector(proposal + 0x18, 120, MaxRecords, &sv) || !sv.count ||
        !tx.Vector(proposal + 0x48, 120, MaxRecords, &rv) ||
        !tx.Vector(proposal + 0x2a0, 0x8f0, 1, &cv) || cv.count != 1) return false;
    std::vector<Node> nodes(nv.count);
    std::vector<Edge> edges(sv.count), removed(rv.count);
    if (!SliceRead(nv.begin, nodes.data(), nodes.size() * 24) ||
        !SliceRead(sv.begin, edges.data(), edges.size() * 120) ||
        !SliceRead(rv.begin, removed.data(), removed.size() * 120)) return false;
    std::vector<SliceVec> objects(sv.count), removedObjects(rv.count);
    for (size_t i = 0; i < sv.count; ++i) {
        if (edges[i][0x74] > 1 || !tx.Vector(sv.begin + i * 120 + 0x30, 8, SIZE_MAX, &objects[i])) return false;
        for (size_t j = 0x10; j < 0x28; j += 4) if (!std::isfinite(Get<float>(edges[i].data(), j))) return false;
    }
    for (size_t i = 0; i < rv.count; ++i)
        if (!tx.Vector(rv.begin + i * 120 + 0x30, 8, SIZE_MAX, &removedObjects[i])) return false;
    auto nodeId = [&](size_t i) { return Get<int32_t>(nodes[i].data(), 0x14); };
    for (size_t i = 0; i < nv.count; ++i) {
        for (size_t axis = 0; axis < 12; axis += 4) if (!std::isfinite(Get<float>(nodes[i].data(), axis))) return false;
        if (nodes[i][0xc] > 1) return false;
        for (size_t j = 0; j < i; ++j) if (nodeId(i) == nodeId(j)) return false;
    }
    std::vector<bool> isTemplate(nv.count); size_t countTemplate = 0;
    for (const Edge& edge : edges) {
        if (!edge[0x74]) continue;
        const int32_t a = Get<int32_t>(edge.data(), 8), b = Get<int32_t>(edge.data(), 12);
        for (size_t i = 0; i < nv.count; ++i)
            if (!isTemplate[i] && nodeId(i) < 0 && (nodeId(i) == a || nodeId(i) == b)) { isTemplate[i] = true; ++countTemplate; }
    }
    if (!countTemplate || countTemplate == nv.count) return false;
    size_t split = MaxRecords, outer = MaxRecords; float distance = 225;
    for (size_t i = 0; i < nv.count; ++i) if (!isTemplate[i])
        for (size_t j = 0; j < nv.count; ++j) if (isTemplate[j]) {
            const float dx = Get<float>(nodes[i].data(), 0) - Get<float>(nodes[j].data(), 0);
            const float dy = Get<float>(nodes[i].data(), 4) - Get<float>(nodes[j].data(), 4);
            const float d = dx * dx + dy * dy;
            if (d < distance) { distance = d; split = i; outer = j; }
        }
    if (split == MaxRecords) return false;
    std::vector<int32_t> frozen, ceFrozen;
    if (!ReadIndices(tx, proposal + 0x220, nv.count, &fv, &frozen) ||
        !ReadIndices(tx, cv.begin + 0x778, nv.count, &cfv, &ceFrozen)) return false;
    int32_t segmentsBefore;
    if (!SliceReadT(cv.begin + 0x790, &segmentsBefore) || segmentsBefore < 0 || size_t(segmentsBefore) > sv.count) return false;
    // The index set is empty on the .22 supported endpoint and split shapes.
    size_t skipEdges; uintptr_t skipHead;
    if (!SliceReadT(proposal + 0x250, &skipEdges) || skipEdges ||
        !SliceReadT(proposal + 0x248, &skipHead) || skipHead) return false;
    const int32_t xid = nodeId(split);
    size_t newNodeCount = nv.count, newSegmentCount = sv.count;
    if (nv.count >= 3 && outer == nv.count - 2 && isTemplate[nv.count - 1]) {
        const size_t inner = outer;
        outer = nv.count - 1;
        const int32_t inId = nodeId(inner), outId = nodeId(outer);
        int apron = -1, ours = -1;
        for (size_t i = 0; i < sv.count; ++i) {
            const int32_t a = Get<int32_t>(edges[i].data(), 8), b = Get<int32_t>(edges[i].data(), 12);
            if (edges[i][0x74] && ((a == inId && b == outId) || (b == inId && a == outId))) {
                if (apron != -1) return false;
                apron = int(i);
            } else if (!edges[i][0x74] && ((a == xid && b >= 0) || (b == xid && a >= 0))) {
                if (ours != -1) return false;
                ours = int(i);
            }
        }
        if (apron != int(sv.count) - 1 || ours < 0 || ceFrozen.size() != 1 ||
            ceFrozen[0] != int(inner) || segmentsBefore != apron) return false;
        // Every reference to a removed node must belong to the apron itself.
        for (size_t i = 0; i < sv.count - 1; ++i) {
            const int32_t a = Get<int32_t>(edges[i].data(), 8), b = Get<int32_t>(edges[i].data(), 12);
            if (a == inId || a == outId || b == inId || b == outId) return false;
        }
        for (int32_t& index : frozen) {
            if (index == int(outer)) return false;
            if (index == int(inner)) index = int(split);
        }
        if (!tx.Retire(objects[ours].begin)) return false;
        // Move the apron object-vector along with its tail. Its old slot is
        // excluded from vector destruction, so clear it instead of aliasing it.
        std::memcpy(edges[ours].data() + 0x28, edges[apron].data() + 0x28, 80);
        std::memset(edges[apron].data() + 0x30, 0, 24);
        nodes[split][0xc] = 0;
        ceFrozen[0] = int(split);
        if (!tx.Scalar(cv.begin + 0x790, int32_t(ours))) return false;
        if (!tx.Vector(proposal + 0x270, 32, MaxRecords, &tv) || (tv.count && tv.count != sv.count)) return false;
        if (tv.count) {
            std::vector<String> tags(tv.count);
            for (size_t i = 0; i < tv.count; ++i) if (!StringOwnership(tx, tv.begin + i * 32, &tags[i])) return false;
            const uintptr_t old = Get<uintptr_t>(tags[ours].data(), 0);
            if (old != tv.begin + size_t(ours) * 32 + 16 && !tx.Retire(old)) return false;
            const uintptr_t from = tv.begin + size_t(apron) * 32, to = tv.begin + size_t(ours) * 32;
            tags[ours] = tags[apron];
            if (Get<uintptr_t>(tags[ours].data(), 0) == from + 16) Put(tags[ours].data(), 0, to + 16);
            EmptyString(tags[apron], from);
            if (!tx.Write(tv.begin, tags.data(), tags.size() * 32) || !tx.Scalar(proposal + 0x278, tv.begin + (tv.count - 1) * 32)) return false;
        }
        newNodeCount -= 2; --newSegmentCount;
    } else {
        if (outer != nv.count - 1) return false;
        for (int32_t index : frozen) if (index == int(outer)) return false;
        for (int32_t index : ceFrozen) if (index == int(outer)) return false;
        const int32_t tid = nodeId(outer);
        size_t repointed = 0;
        for (size_t i = 0; i < nv.count; ++i) if (!isTemplate[i]) nodes[i][0xc] = 0;
        for (Edge& edge : edges) {
            const int32_t a = Get<int32_t>(edge.data(), 8), b = Get<int32_t>(edge.data(), 12);
            if (a != tid && b != tid) continue;
            const int32_t otherId = a == tid ? b : a;
            size_t other = outer;
            for (size_t j = 0; j < nv.count; ++j) if (nodeId(j) == otherId) { other = j; break; }
            if (other == outer) return false;
            for (size_t j = 0; j < 12; j += 4) {
                const float from = Get<float>(nodes[split].data(), j), to = Get<float>(nodes[other].data(), j);
                const float tangent = a == tid ? to - from : from - to;
                if (!std::isfinite(tangent)) return false;
                Put(edge.data(), 0x10 + j, tangent); Put(edge.data(), 0x1c + j, tangent);
            }
            Put(edge.data(), a == tid ? 8 : 12, xid); ++repointed;
        }
        if (!repointed) return false;
        if (removed.size() == 1) {
            const auto& original = removed[0];
            for (size_t i = 0; i < sv.count; ++i) {
                Edge& edge = edges[i];
                if (edge[0x74] || (Get<int32_t>(edge.data(), 8) != xid && Get<int32_t>(edge.data(), 12) != xid)) continue;
                const SliceVec& source = removedObjects[0];
                const size_t bytes = source.count * 8;
                void* copy = bytes ? tx.Allocate(bytes) : nullptr;
                if ((bytes && (!copy || !SliceRead(source.begin, copy, bytes))) || !tx.Retire(objects[i].begin)) return false;
                std::memcpy(edge.data() + 0x28, original.data() + 0x28, 0x64 - 0x28);
                const uintptr_t begin = reinterpret_cast<uintptr_t>(copy);
                Put(edge.data(), 0x30, begin); Put(edge.data(), 0x38, begin + bytes); Put(edge.data(), 0x40, begin + bytes);
                edge[0x64] = 0; // effective .22 catenary value; padding is not data
                Put(edge.data(), 0x6c, Get<uint32_t>(original.data(), 0x6c));
            }
        }
        --newNodeCount;
    }
    if (!tx.Write(nv.begin, nodes.data(), nodes.size() * 24) || !tx.Write(sv.begin, edges.data(), edges.size() * 120) ||
        !tx.Write(fv.begin, frozen.data(), frozen.size() * 4) || !tx.Write(cfv.begin, ceFrozen.data(), ceFrozen.size() * 4) ||
        !tx.Scalar(proposal + 8, nv.begin + newNodeCount * 24) ||
        !tx.Scalar(proposal + 0x20, sv.begin + newSegmentCount * 120) || !tx.Commit()) return false;
    SliceLog("[merge] .22 %s weld: nodes %zu->%zu segments %zu->%zu\n", newSegmentCount < sv.count ? "endpoint" : "split",
             nv.count, newNodeCount, sv.count, newSegmentCount);
    return true;
}

bool SliceMergeTemplateStreet(uintptr_t proposal, const slice_terrain_assets::GameMemory* memory)
{
    try { return MergeStationEndpoint(proposal, memory) || MergeTemplateStreet(proposal, memory); }
    catch (...) {
        // Our STL can allocate; the only game calls are the verified nothrow
        // allocator and deallocator. No foreign exception is caught here.
        SliceLog("[merge] local allocation failed; proposal untouched\n");
        return false;
    }
}
