// game_image.h -- where the game's executable is mapped, and whether it is the
// build every address in native/linux was taken from.
//
// The Linux counterpart of the PE header checks in slice_hook.cpp and
// plugin/host.cpp: the GNU build-id stands in for TimeDateStamp + SizeOfImage.
// Anything that patches code refuses to run when it differs.
// Header-only so each library's build stays single-file.
#pragma once
#include <elf.h>
#include <link.h>
#include <cstdint>
#include <cstring>

// GNU build-id of the Steam Linux build 35924 (depot build 16719842):
// 3a0e156390b0e6f1e372051c24802c8493ae454a.
static const unsigned char kTpf2BuildId35924[20] = {
    0x3a, 0x0e, 0x15, 0x63, 0x90, 0xb0, 0xe6, 0xf1, 0xe3, 0x72,
    0x05, 0x1c, 0x24, 0x80, 0x2c, 0x84, 0x93, 0xae, 0x45, 0x4a,
};

struct Tpf2GameImage {
    uintptr_t base = 0;     // load address; add an RVA to get a live address
    bool buildOk = false;   // build-id is build 35924's
};

// dl_iterate_phdr reports the main executable first.
static inline int Tpf2GameImageCb(dl_phdr_info* info, size_t, void* data)
{
    auto* img = static_cast<Tpf2GameImage*>(data);
    img->base = info->dlpi_addr;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_NOTE) continue;
        const char* p = (const char*)(info->dlpi_addr + ph.p_vaddr);
        const char* end = p + ph.p_memsz;
        while (p + sizeof(ElfW(Nhdr)) <= end) {
            const auto* nh = (const ElfW(Nhdr)*)p;
            const char* name = p + sizeof(*nh);
            const unsigned char* desc = (const unsigned char*)name + ((nh->n_namesz + 3) & ~3u);
            if (nh->n_type == NT_GNU_BUILD_ID && nh->n_namesz == 4 && memcmp(name, "GNU", 4) == 0
                && nh->n_descsz == sizeof(kTpf2BuildId35924))
                img->buildOk = memcmp(desc, kTpf2BuildId35924, sizeof(kTpf2BuildId35924)) == 0;
            p = (const char*)desc + ((nh->n_descsz + 3) & ~3u);
        }
    }
    return 1;
}

static inline Tpf2GameImage Tpf2mpGameImage()
{
    Tpf2GameImage img;
    dl_iterate_phdr(Tpf2GameImageCb, &img);
    return img;
}
