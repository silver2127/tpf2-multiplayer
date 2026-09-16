// Drive the real menu detours against mock game functions at their verified
// RVAs, so the test covers insertion order and exactly one connected button.
#include "../src/menu_linux.cpp"
#include <sys/mman.h>
#include <cassert>
#include <vector>

namespace panel {
void Init(const char*, const char*, const char*, Tpf2mpLogFn) {}
void Open() {}
void OnMenuPage(int) {}
}
bool OverlayInstall(uintptr_t, Tpf2mpLogFn) { return false; }
bool MenuGame_Install(uintptr_t, Tpf2mpLogFn) { return false; }
void MenuGame_AutoEnableMod(Tpf2mpLogFn) {}
bool InstallHook(uintptr_t, void*, int, void**) { return false; }
int PrologueSteal(const unsigned char*, int) { return 0; }

static std::vector<uintptr_t> added;
static int created = 0, connected = 0;
static constexpr uintptr_t kButton = 100, kList = 200;

static void MockString(GStr* out, const char* s)
{
    assert(strlen(s) < sizeof(out->buf));
    out->p = out->buf;
    out->len = strlen(s);
    memcpy(out->buf, s, out->len + 1);
}
static void* MockButton(GStr* label, GStr* a, GStr* b)
{
    assert(strcmp(label->p, "Multiplayer") == 0 && a->len == 0 && b->len == 0);
    ++created;
    return (void*)kButton;
}
static void* MockConnect(void* conn, void* button, GFunc* fn)
{
    assert(button == (void*)kButton && fn->manager == (void*)&MpManager && fn->invoker == (void*)&MpInvoke);
    ++connected;
    fn->manager = nullptr;   // the game's connect moves it
    return conn;
}
static void MockConnectionDtor(void*) {}
static void MockStyle(void* button, GStr* style)
{
    assert(button == (void*)kButton && strcmp(style->p, "multiplayer") == 0);
}
static void MockFlag(void* button, int bit, bool on)
{
    assert(button == (void*)kButton && bit == 4 && !on);
}
static void* MockListAdd(void* list, void* button, GStr*)
{
    assert(list == (void*)kList);
    added.push_back((uintptr_t)button);
    return button;
}
static uintptr_t MockBuild(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t)
{
    GStr item;
    MockString(&item, "list-item");
    for (uintptr_t i = 1; i <= 8; ++i)
        assert(ListAddDetour((void*)kList, (void*)i, &item) == (void*)i);
    return 123;
}
static void Jump(uintptr_t rva, void* fn)
{
    // endbr64; jmp [rip+0]; address -- no register or stack changes.
    const unsigned char code[] = {0xf3,0x0f,0x1e,0xfa,0xff,0x25,0,0,0,0};
    memcpy((void*)(g_base + rva), code, sizeof(code));
    memcpy((void*)(g_base + rva + sizeof(code)), &fn, sizeof(fn));
}
static void CheckSlotFile(const char* path, const char* content, int want)
{
    FILE* f = fopen(path, "w");
    assert(f);
    assert(fputs(content, f) >= 0);
    assert(fclose(f) == 0);
    assert(ReadMenuSlot(path) == want);
}

int main()
{
    char path[] = "/tmp/tpf2mp-menu-slot-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    CheckSlotFile(path, "scale=1.25\nautoload=1\n", 0);
    CheckSlotFile(path, "slot=3\r\n", 3);
    CheckSlotFile(path, "slot=7\n", 7);
    CheckSlotFile(path, "slot=0\n", 0);
    CheckSlotFile(path, "slot=5\nslot=8\nslot=-1\nslot=oops\nslot=999999999999999999999999\n", 5);
    CheckSlotFile(path, "slot=1\nslot=6\n", 6);
    CheckSlotFile(path, "slot= 4\n slot=4\n#slot=4\n", 0);
    unlink(path);
    assert(ReadMenuSlot(path) == 0);

    const size_t imageSize = 0x31a0000;
    void* image = mmap(nullptr, imageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(image != MAP_FAILED);
    g_base = (uintptr_t)image;
    Jump(RVA_TR, (void*)&MockString);
    Jump(RVA_STR_FROM_CSTR, (void*)&MockString);
    Jump(RVA_BUTTON_CREATE, (void*)&MockButton);
    Jump(RVA_CONNECT_CLICK, (void*)&MockConnect);
    Jump(RVA_CONNECTION_DTOR, (void*)&MockConnectionDtor);
    Jump(RVA_ADD_STYLE, (void*)&MockStyle);
    Jump(RVA_SET_FLAG, (void*)&MockFlag);
    assert(mprotect(image, imageSize, PROT_READ | PROT_EXEC) == 0);
    g_listAddTramp = (void*)&MockListAdd;
    g_mainBuildTramp = (void*)&MockBuild;
    for (int slot = 0; slot <= 7; ++slot) {
        g_flagSlot = slot;
        added.clear();
        created = connected = 0;
        assert(MainBuildDetour(0, 0, 0, 0, 0, 0) == 123);
        assert(created == 1 && connected == 1 && added.size() == 9);
        for (int i = 0; i < 9; ++i)
            assert(added[i] == (i == slot ? kButton : (uintptr_t)(i < slot ? i + 1 : i)));
        assert(!t_inMainBuild && t_mainListAdds == 8);
        ListAddDetour((void*)kList, (void*)9, nullptr);
        assert(added.size() == 10 && added.back() == 9 && created == 1);
    }
    munmap(image, imageSize);
    puts("menu slot: flags and real detour insertion order passed");
}
