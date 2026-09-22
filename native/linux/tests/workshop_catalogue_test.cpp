#include "../src/plugin/workshop_catalogue_linux.h"
#include <cassert>
#include <cstdio>
using namespace workshop;
int main() {
    int8_t controls[32];memset(controls,-128,sizeof(controls));controls[0]=0;controls[15]=-1;
    const std::string id="123",path="/original/",other="777";
    Item items[]={{{id.c_str(),id.size(),{}},{path.c_str(),path.size(),{}}},
                  {{other.c_str(),other.size(),{}},{path.c_str(),path.size(),{}}}};
    Backend slots[16]{};slots[0].items={items,items+2,items+2};
    Result source{controls,slots,1,15,0,0};
    std::vector<std::pair<std::string,std::string>> rows={{id,"/替換 folder"}};
    for(int i=1000;i<6000;++i)rows.emplace_back(std::to_string(i),"/mods/"+std::to_string(i));
    Shadow shadow(source,0,rows);
    const auto& result=shadow.result;
    assert(result.size==1 && result.controls[15]==-1 && source.size==1);
    const auto& list=result.slots[0].items;
    assert(list.last-list.first==5002);
    assert(list.first[0].id.equals("777") && list.first[0].path.equals(path));
    assert(list.first[1].id.equals("123") && list.first[1].path.equals("/替換 folder/"));
    for(int i=0;i<5000;++i)assert(list.first[i+2].path.equals("/mods/"+std::to_string(i+1000)+"/"));
    assert(items[0].path.equals(path)); // never mutate/destroy borrowed engine strings
    bool rejected=false;try{Shadow invalid(source,-1,rows);}catch(const std::runtime_error&){rejected=true;}assert(rejected);
    source.removedFirst=items;source.removedLast=items+1;
    Shadow preserve(source,1,{});assert(preserve.result.removedFirst==items && preserve.result.removedLast==items+1);
    puts("PASS: native Workshop result replacement, UTF-8 paths, 5000 rows, borrowed ownership and backend validation");
}
