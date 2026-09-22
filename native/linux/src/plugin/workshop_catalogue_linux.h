#pragma once
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
#include <stdexcept>
#include <algorithm>

namespace workshop {
// Build 35924 RefreshModList 31ae800 reads UTF-8 pointer/length at item+0/+8
// and +20/+28, advancing 40 hex bytes at 31af043. These are borrowed views,
// never destructed as game strings. The source SSO pointers stay in the game.
struct Text {
    const char* pointer=nullptr;
    uint64_t size=0;
    uint64_t storage[2]{};
    bool equals(const std::string& s) const {return size==s.size() && (!size || !memcmp(pointer,s.data(),size));}
};
struct Item {Text id,path;};
struct Items {const Item *first=nullptr,*last=nullptr,*end=nullptr;};
struct Backend {int32_t key=0,pad=0;Items items;};
struct Result {
    const int8_t* controls=nullptr;
    const Backend* slots=nullptr;
    uint64_t size=0,capacity=0,growth=0,reserved=0;
    const void *removedFirst=nullptr,*removedLast=nullptr,*removedEnd=nullptr;
};
struct ModKey {Text name;int32_t version,pad;};
static_assert(sizeof(Text)==32 && sizeof(Item)==64 && sizeof(Backend)==32 && sizeof(Result)==72 && sizeof(ModKey)==40);

class Shadow {
    int8_t controls[32];
    Backend slots[16]{};
    std::vector<Item> entries[2];
    bool present[2]{};
    std::deque<std::string> owned;
    Text text(std::string s) {
        owned.push_back(std::move(s));
        const auto& v=owned.back();
        return {v.c_str(),v.size(),{v.size(),0}};
    }
public:
    Result result;
    Shadow(const Result& source,int steam,const std::vector<std::pair<std::string,std::string>>& rows):result(source) {
        if(steam<0 || steam>1 || source.capacity>4095 ||
           (source.capacity && (!source.controls || !source.slots)))throw std::runtime_error("Workshop result layout");
        for(uint64_t i=0;i<source.capacity;++i) {
            if(source.controls[i]<0)continue;
            const auto& b=source.slots[i];
            if(b.key<0 || b.key>1 || present[b.key])throw std::runtime_error("Workshop backend");
            present[b.key]=true;
            const auto first=uintptr_t(b.items.first),last=uintptr_t(b.items.last);
            if(last<first || (last-first)%sizeof(Item) || (last && !first))throw std::runtime_error("Workshop item span");
            for(auto p=b.items.first;p!=b.items.last;++p)entries[b.key].push_back(*p);
        }
        for(const auto& row:rows) {
            auto& v=entries[steam];
            v.erase(std::remove_if(v.begin(),v.end(),[&](const Item& i){return i.id.equals(row.first);}),v.end());
            std::string path=row.second;
            if(path.empty() || path.back()!='/')path+='/';
            v.push_back({text(row.first),text(std::move(path))});present[steam]=true;
        }
        memset(controls,-128,sizeof(controls));controls[15]=-1;
        unsigned count=0;
        for(int backend=0;backend<2;++backend)if(present[backend]) {
            const auto& v=entries[backend];const auto* end=v.empty()?v.data():v.data()+v.size();
            controls[count]=0;slots[count].key=backend;slots[count].items={v.data(),end,end};++count;
        }
        result.controls=controls;result.slots=slots;result.size=count;result.capacity=15;
    }
    Shadow(const Shadow&)=delete;
    Shadow& operator=(const Shadow&)=delete;
};
}
