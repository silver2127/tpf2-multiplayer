#include "../alignment_batch.h"
#include "../memory_budget.h"
#include <map>
#include <vector>
#include <array>
#include <cassert>
#include <cstring>
#include <bits/stl_tree.h>
using namespace linux_alignment;
using Source=std::map<uint64_t,std::vector<std::array<float,4>>>;
static std::vector<Value> visited;
static size_t calls;
static const Link* Next(const Link* link) {
    return reinterpret_cast<const Link*>(std::_Rb_tree_increment(reinterpret_cast<const std::_Rb_tree_node_base*>(link)));
}
static void UpdateView(void* context,const Map* view) {
    assert(context==&calls);++calls;
    size_t n=0;
    for(const auto* it=view->head.left;it!=&view->head;it=Next(it)) {
        assert(n++<view->count);
        visited.push_back(reinterpret_cast<const Node*>(it)->value);
    }
    assert(n==view->count);
}
int main() {
    for(size_t count:{0u,1u,2u,511u,512u,513u,2049u}) {
        Source source;
        for(size_t i=count;i>0;--i)source[i*7].push_back({float(i),2,3,4});
        const auto original=source;
        std::vector<Value> expected;
        for(auto& p:source)expected.push_back({p.first,p.second.data(),p.second.data()+p.second.size(),p.second.data()+p.second.capacity()});
        auto* view=reinterpret_cast<const Map*>(&source);
        assert(sizeof(source)==sizeof(Map) && view->count==count);
        for(size_t batch:{0u,1u,7u,512u,65536u}) {
            calls=0;visited.clear();
            const size_t result=Run(&calls,view,batch,UpdateView,Next);
            assert(result==(batch && count>batch?count:0));
            assert(calls==(result?(count+batch-1)/batch:1));
            assert(visited.size()==expected.size());
            if(count)assert(!memcmp(visited.data(),expected.data(),count*sizeof(Value)));
            assert(source==original); // Keys, vector payloads and ownership survive.
        }
    }
    using linux_memory::TerrainHotMB;
    assert(TerrainHotMB(0,0)==256);
    assert(TerrainHotMB(16ull<<30,0)==546);
    assert(TerrainHotMB(31ull<<30,0)==1058);
    assert(TerrainHotMB(32ull<<30,0)==1092);
    assert(TerrainHotMB(256ull<<30,0)==4096);
    assert(TerrainHotMB(32ull<<30,1024)==1024);
    assert(TerrainHotMB(32ull<<30,1)==128);
    assert(TerrainHotMB(32ull<<30,20000)==16384);
}
