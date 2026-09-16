#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <stdexcept>

// Read-only views of the release MSVC ABI. No engine allocation is freed here.
struct Text {
    union { char small[16]; const void* ptr; } data{};
    uint64_t size = 0, capacity = 15;
    const char* chars() const { return capacity > 15 ? (const char*)data.ptr : data.small; }
    bool equals(const std::string& s) const {
        return size == s.size() && !memcmp(chars(), s.data(), s.size());
    }
};
struct Item { Text id, path; }; // path is an MSVC wstring, SSO capacity 7
struct Items { const Item *first = nullptr, *last = nullptr, *end = nullptr; };
struct Backend { int32_t key = 0, padding = 0; Items items; };
struct Result {
    const int8_t* controls;
    const Backend* slots;
    uint64_t size, capacity, growth, reserved;
    const void *removedFirst, *removedLast, *removedEnd;
};
static_assert(sizeof(Text) == 32 && sizeof(Item) == 64);
static_assert(sizeof(Backend) == 32 && sizeof(Result) == 72);

// RefreshModList only iterates this flat map. Hash lookup is deliberately not
// supported by the shadow result. Do not pass it to other engine functions.
class Shadow {
    int8_t controls[32];
    Backend slots[16]{};
    std::vector<Item> entries[2];
    bool present[2]{};
    std::string id;
    std::wstring path;
public:
    Result result;
    bool added = false;
    Shadow(const Result& source, int steam, std::string itemId, std::wstring folder, bool replaceTestItem = false)
        : id(std::move(itemId)), path(std::move(folder)), result(source) {
        // GetModDir callers concatenate resource names directly. Workshop
        // discovery paths must end in a separator, even though the catalogue's
        // mod.lua existence check itself joins paths correctly.
        if (!path.empty() && path.back()!=L'/' && path.back()!=L'\\') path+=L'/';
        if (steam < 0 || steam > 1 || source.capacity > 4095 ||
            (source.capacity && (!source.controls || !source.slots)))
            throw std::runtime_error("unsupported Workshop result layout");
        for (uint64_t i=0; i<source.capacity; ++i) {
            if (source.controls[i] < 0) continue;
            const auto& b = source.slots[i];
            if (b.key < 0 || b.key > 1 || present[b.key])
                throw std::runtime_error("unsupported Workshop backend");
            present[b.key] = true;
            auto first = (uintptr_t)b.items.first, last = (uintptr_t)b.items.last;
            if (last < first || (last-first)%sizeof(Item) || (last-first)/sizeof(Item)>100000)
                throw std::runtime_error("invalid Workshop item span");
            if (first != last) for (auto p=b.items.first; p!=b.items.last; ++p) {
                if (replaceTestItem && b.key==steam && p->id.equals(id)) continue;
                entries[b.key].push_back(*p);
            }
        }
        bool exists = false;
        for (const auto& item : entries[steam]) if (item.id.equals(id)) exists = true;
        if (!exists) {
            Item item;
            item.id.data.ptr = id.c_str(); item.id.size = id.size();
            item.id.capacity = id.size() > 15 ? id.size() : 16;
            item.path.data.ptr = path.c_str(); item.path.size = path.size();
            item.path.capacity = path.size() > 7 ? path.size() : 8;
            entries[steam].push_back(item); added = true; present[steam] = true;
        }
        memset(controls, -128, sizeof(controls)); controls[15] = -1;
        unsigned count = 0;
        for (int backend=0; backend<2; ++backend) if (present[backend]) {
            auto& v = entries[backend];
            controls[count] = 0; slots[count].key = backend;
            slots[count].items = {v.data(), v.empty() ? v.data() : v.data()+v.size(),
                                 v.empty() ? v.data() : v.data()+v.size()};
            ++count;
        }
        result.controls = controls; result.slots = slots;
        result.size = count; result.capacity = 15;
    }
    Shadow(const Shadow&) = delete;
    Shadow& operator=(const Shadow&) = delete;
};
