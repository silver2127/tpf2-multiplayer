#include "catalogue.h"
#include <cassert>
#include <iostream>
int main() {
    Result empty{};
    Shadow one(empty,0,"1954591986",L"C:/offline/mod");
    assert(one.added && one.result.size==1);
    assert(one.result.slots[0].items.first->id.equals("1954591986"));
    assert(one.result.slots[0].items.first->path.size==15);
    auto registeredPath=std::wstring((const wchar_t*)one.result.slots[0].items.first->path.data.ptr);
    assert(registeredPath+L"mod.lua"==L"C:/offline/mod/mod.lua");
    Shadow duplicate(one.result,0,"1954591986",L"C:/other");
    assert(!duplicate.added && duplicate.result.slots[0].items.last-duplicate.result.slots[0].items.first==1);
    Shadow replaced(one.result,0,"1954591986",L"C:/replacement",true);
    assert(replaced.added && replaced.result.slots[0].items.last-replaced.result.slots[0].items.first==1);
    assert(std::wstring((const wchar_t*)replaced.result.slots[0].items.first->path.data.ptr)==L"C:/replacement/");
    Shadow slashed(empty,0,"123",L"C:/mod/");
    assert(std::wstring((const wchar_t*)slashed.result.slots[0].items.first->path.data.ptr)==L"C:/mod/");
    Shadow backslashed(empty,0,"123",L"C:\\mod\\");
    assert(std::wstring((const wchar_t*)backslashed.result.slots[0].items.first->path.data.ptr)==L"C:\\mod\\");
    Shadow second(one.result,0,"999",L"C:/second");
    assert(second.added && second.result.slots[0].items.last-second.result.slots[0].items.first==2);
    Shadow otherBackend(one.result,1,"888",L"C:/third");
    assert(otherBackend.result.size==2);
    assert(one.result.size==1); // source remains untouched
    int removed=123; Result withRemoved=empty;
    withRemoved.removedFirst=&removed; withRemoved.removedLast=&removed;
    Shadow preserved(withRemoved,0,"123",L"C:/x");
    assert(preserved.result.removedFirst==&removed);
    bool failed=false;
    try { Shadow invalid(empty,2,"123",L"C:/x"); } catch(const std::runtime_error&) { failed=true; }
    assert(failed);
    std::cout<<"PASS: empty catalogue, duplicate precedence, append, both backends, source isolation, removed list, invalid backend\n";
}
