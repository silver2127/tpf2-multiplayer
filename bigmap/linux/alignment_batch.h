// Build 35924 libstdc++ map view; see docs/linux/PORT.md.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
namespace linux_alignment {
struct Link { uint32_t color=0, pad=0; Link *parent=nullptr,*left=nullptr,*right=nullptr; };
struct Value { uint64_t key; void *begin,*end,*capacity; };
struct Node { Link link; Value value; };
struct Map { uint64_t comparator=0; Link head; size_t count=0; };
static_assert(sizeof(Link)==32 && offsetof(Node,value)==32 && sizeof(Node)==64);
static_assert(offsetof(Map,head)==8 && offsetof(Map,count)==40 && sizeof(Map)==48);
using Update=void(*)(void*,const Map*);
using Increment=const Link*(*)(const Link*);
// The original only iterates this view. It never balances, erases or owns it.
inline void Chain(Map& view,Node* nodes,size_t n) {
    view.count=n;
    view.head={0,0,n?&nodes[n-1].link:nullptr,n?&nodes[0].link:&view.head,n?&nodes[n-1].link:&view.head};
    for(size_t i=0;i<n;++i)
        nodes[i].link={1,0,i+1<n?&nodes[i+1].link:&view.head,i?&nodes[i-1].link:nullptr,nullptr};
}
// Allocate before publishing anything. OOM or an inconsistent iteration falls
// back to the single stock call, never after partially publishing the input.
inline size_t Run(void* self,const Map* map,size_t batch,Update update,Increment next) {
    const size_t total=map?map->count:0;
    if(!batch || total<=batch || total>(size_t(64)<<20)) {update(self,map);return 0;}
    std::unique_ptr<Value[]> values(new(std::nothrow) Value[total]);
    std::unique_ptr<Node[]> nodes(new(std::nothrow) Node[batch]);
    if(!values || !nodes) {update(self,map);return 0;}
    const Link* it=map->head.left;
    size_t n=0;
    for(;it && it!=&map->head && n<total;it=next(it))
        values[n++]=reinterpret_cast<const Node*>(it)->value;
    if(n!=total || it!=&map->head) {update(self,map);return 0;}
    for(size_t off=0;off<total;off+=batch) {
        const size_t count=total-off<batch?total-off:batch;
        for(size_t i=0;i<count;++i)nodes[i].value=values[off+i];
        Map view;Chain(view,nodes.get(),count);update(self,&view);
    }
    return total;
}
}
