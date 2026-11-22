// Manual check against the REAL build-35924 ELF (not a CTest: it needs the
// game file).  test_family_canon_elf /path/to/TransportFever2
//
// Maps the ELF's PT_LOADs at a fresh base, applies its R_X86_64_RELATIVE
// relocations (so the family vtables hold real function addresses), installs
// the order canon and the town trace as boot.cpp does, then builds an engine
// whose 63 families use the binary's OWN 63 family vtables -- every
// ComponentGroupFamily<...> the game instantiates -- each with an unsorted node
// list, and runs one per-iteration family sort. Before the getter inventory
// the sort recognised 1 of the 28 node lists (the live server's
// `lists=1 ... unknown/refused=61`); it must now sort all 28 and refuse none.
// Nothing in the mapped image is executed.
#include "../src/order_canon_linux.cpp"
#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cassert>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

static std::string g_logged;
static void CaptureLog(const char* fmt, ...)
{
    char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    g_logged += b;
}

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s GAME_ELF\n", argv[0]); return 2; }
    const int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror("open"); return 2; }
    Elf64_Ehdr eh; assert(pread(fd, &eh, sizeof eh, 0) == sizeof eh);
    std::vector<Elf64_Phdr> ph(eh.e_phnum);
    assert(pread(fd, ph.data(), sizeof(Elf64_Phdr) * eh.e_phnum, eh.e_phoff) == ssize_t(sizeof(Elf64_Phdr) * eh.e_phnum));
    uint64_t hi = 0; const Elf64_Phdr* dyn = nullptr;
    for (auto& p : ph) {
        if (p.p_type == PT_LOAD && p.p_vaddr + p.p_memsz > hi) hi = p.p_vaddr + p.p_memsz;
        if (p.p_type == PT_DYNAMIC) dyn = &p;
    }
    hi = (hi + 0xfff) & ~0xfffull;
    auto* image = static_cast<uint8_t*>(mmap(nullptr, hi, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(image != MAP_FAILED && dyn);
    for (auto& p : ph) if (p.p_type == PT_LOAD) assert(pread(fd, image + p.p_vaddr, p.p_filesz, p.p_offset) == ssize_t(p.p_filesz));
    const uintptr_t base = reinterpret_cast<uintptr_t>(image);
    // RELATIVE relocations: *(base+off) = base+addend.
    uint64_t rela = 0, relasz = 0;
    for (auto* d = reinterpret_cast<Elf64_Dyn*>(image + dyn->p_vaddr); d->d_tag != DT_NULL; ++d) {
        if (d->d_tag == DT_RELA) rela = d->d_un.d_ptr;
        if (d->d_tag == DT_RELASZ) relasz = d->d_un.d_val;
    }
    assert(rela && relasz);
    std::vector<std::pair<uint64_t, uint64_t>> relative;
    for (auto* r = reinterpret_cast<Elf64_Rela*>(image + rela); r < reinterpret_cast<Elf64_Rela*>(image + rela + relasz); ++r)
        if (ELF64_R_TYPE(r->r_info) == R_X86_64_RELATIVE) {
            const uint64_t v = base + r->r_addend;
            memcpy(image + r->r_offset, &v, 8);
            relative.emplace_back(r->r_offset, r->r_addend);
        }
    const char* id = "3a0e156390b0e6f1e372051c24802c8493ae454a";
    // Install as boot.cpp does: the canon verifies all 63 getters in this image.
    Tpf2mpOrderCanonSetLog(CaptureLog);
    unsetenv("TPF2MP_ORDER_CANON");
    const bool canon = Tpf2mpInstallOrderCanon(base, id);
    printf("order canon: %s  %s\n", canon ? "OK  " : "FAIL", Tpf2mpOrderCanonStatus());
    assert(canon);
    char tracePath[] = "/tmp/family_canon_elf_traceXXXXXX";
    const int tfd = mkstemp(tracePath); assert(tfd >= 0); close(tfd);
    setenv("TPF2MP_TOWN_TRACE", "1", 1);
    const bool trace = Tpf2mpInstallTownTrace(base, id, tracePath);
    printf("town trace:  %s  %s\n", trace ? "OK  " : "FAIL", Tpf2mpTownTraceStatus());
    assert(trace);
    // The Develop call now reaches the wrapper through a near stub.
    int32_t rel; memcpy(&rel, image + 0x1747918, 4);
    const uint8_t* stub = image + 0x174791c + rel;
    uintptr_t target; memcpy(&target, stub + 2, 8);
    assert(image[0x1747917] == 0xe8 && target == reinterpret_cast<uintptr_t>(&Tpf2mpTownTraceDevelop));
    unlink(tracePath);

    // The binary's own family vtables: a vptr whose slot 2 is an inventoried getter.
    struct Fam { uintptr_t vptr; bool list; };
    std::vector<Fam> fams;
    for (auto& [off, addend] : relative) {
        (void)addend;
        const uintptr_t vptr = base + off + 8;               // [off] = typeinfo, [off+8] = slot 0
        if (off + 32 > hi) continue;
        uintptr_t slot2; memcpy(&slot2, reinterpret_cast<void*>(vptr + 16), 8);
        uintptr_t ti; memcpy(&ti, image + off, 8);
        if (ti < base || ti >= base + hi) continue;
        uintptr_t name; memcpy(&name, reinterpret_cast<void*>(ti + 8), 8);
        if (name < base || name >= base + hi || strncmp(reinterpret_cast<const char*>(name), "N3ecs", 5) ||
            !strstr(reinterpret_cast<const char*>(name), "Family") || !strcmp(reinterpret_cast<const char*>(name), "N3ecs7IFamilyE"))
            continue;
        // Classified by the getter's own code, independently of the inventory.
        static const unsigned char getList[] = {0xf3,0x0f,0x1e,0xfa,0x48,0x8d,0x47,0x08,0xc3};
        static const unsigned char noList[] = {0xf3,0x0f,0x1e,0xfa,0x31,0xc0,0xc3};
        if (slot2 < base || slot2 + 16 > base + hi) continue;
        const bool isList = !memcmp(reinterpret_cast<void*>(slot2), getList, sizeof(getList));
        const bool isNone = !memcmp(reinterpret_cast<void*>(slot2), noList, sizeof(noList));
        if (!isList && !isNone) continue;
        fams.push_back({vptr, isList});
    }
    size_t nl = 0, nn = 0, legacy = 0;
    for (auto& f : fams) {
        (f.list ? nl : nn)++;
        uintptr_t slot2; memcpy(&slot2, reinterpret_cast<void*>(f.vptr + 16), 8);
        if (slot2 == base + 0xa914c0) ++legacy;
    }
    printf("family vtables in the image: %zu with a node list, %zu without; the single address the old canon knew covers %zu\n",
           nl, nn, legacy);
    assert(nl == 28 && nn == 35 && legacy == 1);
    size_t inventoried = 0;
    for (auto& f : fams) { uintptr_t g; memcpy(&g, reinterpret_cast<void*>(f.vptr + 16), 8); inventoried += FamilyGetter(g) == (f.list ? FG_LIST : FG_NONE); }
    printf("of those, the canon's getter inventory recognises %zu\n", inventoried);

    // An engine holding all 63, each list three nodes out of order (width 1).
    struct Obj { uintptr_t words[9]; std::vector<int32_t> nodes; int8_t ctrl[8]; int32_t slots[16]; };
    std::vector<Obj> objs(fams.size());
    std::vector<std::array<uintptr_t, 3>> chain(fams.size());
    for (size_t i = 0; i < fams.size(); ++i) {
        auto& o = objs[i];
        o.nodes = {int32_t(900 + i), 0, int32_t(100 + i), 1, int32_t(500 + i), 2};
        const int8_t ctrl[] = {0, 1, 2, -128, -128, -128, -128, -128};
        memcpy(o.ctrl, ctrl, sizeof(ctrl));
        const int32_t slots[] = {int32_t(900 + i), 0, int32_t(100 + i), 1, int32_t(500 + i), 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        memcpy(o.slots, slots, sizeof(slots));
        const uintptr_t nb = reinterpret_cast<uintptr_t>(o.nodes.data()), ne = nb + o.nodes.size() * 4;
        const uintptr_t w[9] = {fams[i].vptr, base + 0x59ac260, nb, ne, ne,
                                reinterpret_cast<uintptr_t>(o.ctrl), reinterpret_cast<uintptr_t>(o.slots), 3, 7};
        memcpy(o.words, w, sizeof(w));
        chain[i] = {i + 1 < fams.size() ? reinterpret_cast<uintptr_t>(chain[i + 1].data()) : 0, 0,
                    reinterpret_cast<uintptr_t>(o.words)};
    }
    for (size_t i = 0; i + 1 < fams.size(); ++i) chain[i][0] = reinterpret_cast<uintptr_t>(chain[i + 1].data());
    std::array<uintptr_t, 64> engine{};
    engine[0x170 / 8] = reinterpret_cast<uintptr_t>(chain[0].data());
    engine[0x178 / 8] = fams.size();
    g_logged.clear();
    CanonFamilies(reinterpret_cast<uintptr_t>(engine.data()));
    printf("%s", g_logged.c_str());
    size_t sorted = 0, untouched = 0;
    for (size_t i = 0; i < fams.size(); ++i) {
        const auto& n = objs[i].nodes;
        const bool s = n[0] == int32_t(100 + i) && n[2] == int32_t(500 + i) && n[4] == int32_t(900 + i);
        const bool u = n[0] == int32_t(900 + i) && n[2] == int32_t(100 + i) && n[4] == int32_t(500 + i);
        assert(fams[i].list ? s : u);
        sorted += s; untouched += u;
    }
    printf("one iteration: %zu node lists put in entity order, %zu no-list families left alone\n", sorted, untouched);
    assert(sorted == 28 && untouched == 35);
    assert(g_logged.find("families=63/63 lists=28 reordered=28") != std::string::npos);
    assert(g_logged.find("unknown/refused=0") != std::string::npos);
    printf("PASS: the real image's 63 families: 28 node lists sorted, 0 refused (was 1 sorted, 61 refused)\n");
    return 0;
}
