// save_zstd.inl -- part of tpf2_slice.dll: included by slice_hook.cpp in this order, one translation unit.
// SAVES COMPRESS ON ZSTD WORKER THREADS (build 35924; the stream logic is ../save_zstd.h)
//
// The Windows exe embeds zstd 1.5.2 and boost::iostreams' zstd_base exactly as the
// Linux one does; its six call sites into the compression API are redirected here to
// the stream logic shared with Linux, backed by zstd 1.5.7 built into this DLL with
// ZSTD_MULTITHREAD (third_party/zstd). The file stays one standard zstd frame that the
// game's own decoder reads. Before anything is patched a self-test compresses 8 MiB
// through the same code in boost's exact call pattern (with a flush in the middle)
// and decompresses it again; any difference leaves the game's compressor alone.
//
// save_threads=0 turns it off; save_threads=N (1..16) sets the workers (default 4,
// at most one less than the CPUs).

// ~zstd_base: ZSTD_freeCStream(cstream_) at 25fcd4c
static const uint8_t SAVEZSTD_DTOR[58] = {
    0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0x48,0x8b,0x09,0xe8,0xef,0x48,0x5a,0x00,0x48,0x8b,0x4b,0x08,0xe8,0x56,0x89,
    0x5a,0x00,0x48,0x8b,0x4b,0x10,0xba,0x18,0x00,0x00,0x00,0xe8,0x54,0x6d,0x5f,0x00,0x48,0x8b,0x4b,0x18,0xba,0x18,0x00,0x00,
    0x00,0x48,0x83,0xc4,0x20,0x5b,0xe9,0x41,0x6d,0x5f,
};
// deflate(action): compressStream 25fcfab, endStream 25fcfce (action 0), flushStream 25fcfd5 (action 1)
static const uint8_t SAVEZSTD_DEFLATE[195] = {
    0x48,0x8b,0xc4,0x41,0x56,0x48,0x83,0xec,0x60,0x48,0xc7,0x40,0xb8,0xfe,0xff,0xff,0xff,0x48,0x89,0x58,0x08,0x48,0x89,0x68,
    0x10,0x48,0x89,0x70,0x18,0x48,0x89,0x78,0x20,0x8b,0xf2,0x48,0x8b,0xf9,0x48,0x8b,0x19,0x4c,0x8b,0x41,0x10,0x48,0x8b,0x69,
    0x18,0x83,0x79,0x20,0x00,0x74,0x0e,0x49,0x83,0x78,0x08,0x00,0x75,0x07,0xb8,0x01,0x00,0x00,0x00,0xeb,0x63,0x48,0x8b,0xd5,
    0x48,0x8b,0xcb,0xe8,0xa0,0x1f,0x5a,0x00,0x4c,0x8b,0xf0,0x48,0x8b,0xc8,0xe8,0x95,0xe6,0x59,0x00,0x85,0xc0,0x75,0x7b,0x83,
    0xfe,0x02,0x74,0x42,0x48,0x8b,0xd5,0x48,0x8b,0xcb,0x85,0xf6,0x75,0x07,0xe8,0x8d,0x3a,0x5a,0x00,0xeb,0x05,0xe8,0x46,0x44,
    0x5a,0x00,0x48,0x8b,0xd8,0x48,0x8b,0xc8,0xe8,0x6b,0xe6,0x59,0x00,0x85,0xc0,0x75,0x3a,0x33,0xc9,0x85,0xf6,0x75,0x08,0x48,
    0x85,0xdb,0x8d,0x41,0x01,0x74,0x02,0x8b,0xc1,0x89,0x47,0x20,0x48,0x85,0xdb,0x0f,0x94,0xc1,0x8b,0xc1,0xeb,0x02,0x33,0xc0,
    0x4c,0x8d,0x5c,0x24,0x60,0x49,0x8b,0x5b,0x10,0x49,0x8b,0x6b,0x18,0x49,0x8b,0x73,0x20,0x49,0x8b,0x7b,0x28,0x49,0x8b,0xe3,
    0x41,0x5e,0xc3,
};
// reset(compress, realloc): initCStream(cstream_, level) at 25fd0a2
static const uint8_t SAVEZSTD_RESET[103] = {
    0x40,0x53,0x48,0x83,0xec,0x60,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48,0x8b,0x41,0x10,0x4c,0x8b,0x49,0x18,0x45,
    0x33,0xd2,0x4c,0x89,0x10,0x4c,0x89,0x50,0x08,0x4c,0x89,0x50,0x10,0x33,0xc0,0x49,0x89,0x01,0x49,0x89,0x41,0x08,0x49,0x89,
    0x41,0x10,0x89,0x41,0x20,0x8b,0x12,0x89,0x51,0x24,0x45,0x84,0xc0,0x74,0x0a,0x48,0x8b,0x09,0xe8,0x59,0x4b,0x5a,0x00,0xeb,
    0x09,0x48,0x8b,0x49,0x08,0xe8,0xae,0x89,0x5a,0x00,0x48,0x8b,0xd8,0x48,0x8b,0xc8,0xe8,0x93,0xe5,0x59,0x00,0x85,0xc0,0x75,
    0x06,0x48,0x83,0xc4,0x60,0x5b,0xc3,
};
// do_init(params, compress): initCStream(cstream_, level_) at 25fd1d3
static const uint8_t SAVEZSTD_DOINIT[104] = {
    0x45,0x84,0xc0,0x74,0x62,0x53,0x48,0x83,0xec,0x60,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48,0x8b,0x41,0x10,0x4c,
    0x8b,0x49,0x18,0x45,0x33,0xc0,0x4c,0x89,0x00,0x4c,0x89,0x40,0x08,0x4c,0x89,0x40,0x10,0x33,0xc0,0x49,0x89,0x01,0x49,0x89,
    0x41,0x08,0x49,0x89,0x41,0x10,0x89,0x41,0x20,0x84,0xd2,0x74,0x0d,0x8b,0x51,0x24,0x48,0x8b,0x09,0xe8,0x28,0x4a,0x5a,0x00,
    0xeb,0x09,0x48,0x8b,0x49,0x08,0xe8,0x7d,0x88,0x5a,0x00,0x48,0x8b,0xd8,0x48,0x8b,0xc8,0xe8,0x62,0xe4,0x59,0x00,0x85,0xc0,
    0x75,0x06,0x48,0x83,0xc4,0x60,0x5b,0xc3,
};
struct SaveZstdGuard { uintptr_t rva; const uint8_t* bytes; size_t size; };
static const SaveZstdGuard SAVEZSTD_GUARDS[] = {
    { 0x25fcd40, SAVEZSTD_DTOR, sizeof(SAVEZSTD_DTOR) },
    { 0x25fcf60, SAVEZSTD_DEFLATE, sizeof(SAVEZSTD_DEFLATE) },
    { 0x25fd060, SAVEZSTD_RESET, sizeof(SAVEZSTD_RESET) },
    { 0x25fd190, SAVEZSTD_DOINIT, sizeof(SAVEZSTD_DOINIT) },
};
// The embedded zstd 1.5.2 functions the sites call.
static const uintptr_t RVA_ZSTD_INIT = 0x2ba1c00, RVA_ZSTD_COMPRESS = 0x2b9ef50, RVA_ZSTD_FLUSH = 0x2ba1420,
                       RVA_ZSTD_END = 0x2ba0a60, RVA_ZSTD_FREE = 0x2ba1640;

static savezstd::Streams* g_saveStreams = nullptr;
static size_t SaveZstdInit(void* cs, int level) { return g_saveStreams->Init(cs, level); }
static size_t SaveZstdCompress(void* cs, savezstd::OutBuf* o, savezstd::InBuf* i) { return g_saveStreams->Compress(cs, o, i); }
static size_t SaveZstdFlush(void* cs, savezstd::OutBuf* o) { return g_saveStreams->Flush(cs, o); }
static size_t SaveZstdEnd(void* cs, savezstd::OutBuf* o) { return g_saveStreams->End(cs, o); }
static size_t SaveZstdFree(void* cs) { return g_saveStreams->Free(cs); }

// zstd 1.5.7, built in: the Api the shared stream logic calls.
static void* ZApiCreate() { return ZSTD_createCCtx(); }
static size_t ZApiFree(void* c) { return ZSTD_freeCCtx((ZSTD_CCtx*)c); }
static size_t ZApiSet(void* c, int p, int v) { return ZSTD_CCtx_setParameter((ZSTD_CCtx*)c, (ZSTD_cParameter)p, v); }
static size_t ZApiReset(void* c, int r) { return ZSTD_CCtx_reset((ZSTD_CCtx*)c, (ZSTD_ResetDirective)r); }
static size_t ZApiStream2(void* c, savezstd::OutBuf* o, savezstd::InBuf* i, int d)
{ return ZSTD_compressStream2((ZSTD_CCtx*)c, (ZSTD_outBuffer*)o, (ZSTD_inBuffer*)i, (ZSTD_EndDirective)d); }
static unsigned ZApiIsError(size_t r) { return ZSTD_isError(r); }
static_assert(sizeof(savezstd::InBuf) == sizeof(ZSTD_inBuffer) && sizeof(savezstd::OutBuf) == sizeof(ZSTD_outBuffer), "buffer layout");
static_assert((int)savezstd::kWorkers == (int)ZSTD_c_nbWorkers && (int)savezstd::kLevel == (int)ZSTD_c_compressionLevel &&
              (int)savezstd::kEnd == (int)ZSTD_e_end && (int)savezstd::kFlush == (int)ZSTD_e_flush &&
              (int)savezstd::kContinue == (int)ZSTD_e_continue &&
              (int)savezstd::kResetSession == (int)ZSTD_reset_session_only, "zstd enums");

// During the self-test nothing reaches the game's functions: every call goes to the
// built-in zstd, as it will for a real save.
static size_t SaveZstdNoInit(void*, int) { return 0; }
static size_t SaveZstdNoStream(void*, savezstd::OutBuf*, savezstd::InBuf*) { return 0; }
static size_t SaveZstdNoOut(void*, savezstd::OutBuf*) { return 0; }
static size_t SaveZstdNoFree(void*) { return 0; }

// 8 MiB of compressible, varied bytes in 64 KiB writes, a flush in the middle, then
// boost's close: an empty compressStream and endStream until the end reports 0.
static bool SaveZstdSelfTest(const savezstd::Api& api, int workers, char* why, size_t cap)
{
    savezstd::Embedded none;
    none.init = &SaveZstdNoInit; none.compress = &SaveZstdNoStream; none.flush = &SaveZstdNoOut;
    none.end = &SaveZstdNoOut; none.free = &SaveZstdNoFree;
    savezstd::Streams s(api, none, workers);
    std::vector<char> input(8u << 20), packed, buf(65536), back;
    uint32_t x = 2463534242u;
    for (size_t i = 0; i < input.size(); ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        input[i] = char("saveZSTD"[x & 7] + (i % 251 == 0));
    }
    int cs = 0;
    s.Init(&cs, 1);
    auto take = [&](const savezstd::OutBuf& o) { packed.insert(packed.end(), buf.data(), buf.data() + o.pos); };
    for (size_t at = 0; at < input.size();) {
        savezstd::InBuf in{ input.data() + at, (std::min)(size_t(65536), input.size() - at), 0 };
        while (in.pos < in.size) {
            savezstd::OutBuf o{ buf.data(), buf.size(), 0 };
            if (api.isError(s.Compress(&cs, &o, &in))) { snprintf(why, cap, "compressStream failed"); s.Free(&cs); return false; }
            take(o);
        }
        at += in.size;
        if (at == input.size() / 2)
            for (size_t left = 1; left;) {
                savezstd::OutBuf o{ buf.data(), buf.size(), 0 };
                left = s.Flush(&cs, &o);
                if (api.isError(left)) { snprintf(why, cap, "flushStream failed"); s.Free(&cs); return false; }
                take(o);
            }
    }
    for (size_t left = 1; left;) {
        savezstd::OutBuf o{ buf.data(), buf.size(), 0 };
        savezstd::InBuf empty{ nullptr, 0, 0 };
        if (api.isError(s.Compress(&cs, &o, &empty))) { snprintf(why, cap, "empty compressStream during the end failed"); s.Free(&cs); return false; }
        left = s.End(&cs, &o);
        if (api.isError(left)) { snprintf(why, cap, "endStream failed"); s.Free(&cs); return false; }
        take(o);
    }
    s.Free(&cs);
    if (s.Snapshot().fallbacks) { snprintf(why, cap, "no worker context"); return false; }
    back.resize(input.size() + 1);
    const size_t n = ZSTD_decompress(back.data(), back.size(), packed.data(), packed.size());
    if (ZSTD_isError(n) || n != input.size() || memcmp(back.data(), input.data(), n)) {
        snprintf(why, cap, "round trip differs (%s)", ZSTD_isError(n) ? ZSTD_getErrorName(n) : "bytes");
        return false;
    }
    snprintf(why, cap, "self-test %zu -> %zu bytes", input.size(), packed.size());
    return true;
}

static int SaveZstdFlagThreads()
{
    for (int i = 0; i < 2; i++) {
        const char* dir = i == 0 ? g_dllDir : g_dataDir;
        if (!dir[0]) continue;
        char p[MAX_PATH];
        snprintf(p, sizeof(p), "%stpf2_menu_flags.txt", dir);
        FILE* f = _fsopen(p, "r", _SH_DENYNO);
        if (!f) continue;
        char line[256]; int n = -1;
        while (fgets(line, sizeof(line), f))
            if (!strncmp(line, "save_threads=", 13)) n = atoi(line + 13);
        fclose(f);
        return n;
    }
    return -1;
}

// Point the `call rel32` at rva `site` (which must reach `callee`) at `to`, through a
// 12-byte stub in the near page. False, writing nothing, if it is not that call.
static bool SaveZstdRedirect(uintptr_t site, uintptr_t callee, void* to)
{
    const uintptr_t at = g_base + site;
    int32_t rel = 0; memcpy(&rel, (const void*)(at + 1), 4);
    if (*(const uint8_t*)at != 0xE8 || at + 5 + (int64_t)rel != g_base + callee) return false;
    uint8_t* stub = NearAlloc(16);
    if (!stub) return false;
    const uintptr_t target = (uintptr_t)to;
    stub[0] = 0x48; stub[1] = 0xB8; memcpy(stub + 2, &target, 8);   // mov rax, to
    stub[10] = 0xFF; stub[11] = 0xE0;                                  // jmp rax
    FlushInstructionCache(GetCurrentProcess(), stub, 12);
    const int64_t nrel = (int64_t)(uintptr_t)stub - (int64_t)(at + 5);
    if (nrel < INT32_MIN || nrel > INT32_MAX) return false;
    DWORD old = 0;
    if (!VirtualProtect((void*)(at + 1), 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    const int32_t r32 = (int32_t)nrel;
    memcpy((void*)(at + 1), &r32, 4);
    VirtualProtect((void*)(at + 1), 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 5);
    return true;
}

static void InstallSaveZstd()
{
    int workers = SaveZstdFlagThreads();
    if (workers == 0) { Log("[savezstd] OFF (save_threads=0 in tpf2_menu_flags.txt) -- saves compress on the saving thread\n"); return; }
    SYSTEM_INFO si; GetSystemInfo(&si);
    const int cpus = (int)si.dwNumberOfProcessors;
    if (workers < 0) workers = 4;
    if (workers > 16) workers = 16;
    if (cpus > 1 && workers > cpus - 1) workers = cpus - 1;
    if (workers < 1) workers = 1;
    for (const auto& g : SAVEZSTD_GUARDS)
        if (!BytesAre(g.rva, g.bytes, g.size, "savezstd")) return;
    savezstd::Api api;
    api.createCCtx = &ZApiCreate; api.freeCCtx = &ZApiFree; api.setParameter = &ZApiSet;
    api.reset = &ZApiReset; api.compressStream2 = &ZApiStream2; api.isError = &ZApiIsError;
    char why[160];
    if (!SaveZstdSelfTest(api, workers, why, sizeof(why))) {
        Log("[savezstd] OFF: %s -- saves compress on the saving thread\n", why);
        return;
    }
    savezstd::Embedded game;
    game.init = (size_t (*)(void*, int))(g_base + RVA_ZSTD_INIT);
    game.compress = (size_t (*)(void*, savezstd::OutBuf*, savezstd::InBuf*))(g_base + RVA_ZSTD_COMPRESS);
    game.flush = (size_t (*)(void*, savezstd::OutBuf*))(g_base + RVA_ZSTD_FLUSH);
    game.end = (size_t (*)(void*, savezstd::OutBuf*))(g_base + RVA_ZSTD_END);
    game.free = (size_t (*)(void*))(g_base + RVA_ZSTD_FREE);
    g_saveStreams = new (std::nothrow) savezstd::Streams(api, game, workers);   // lives as long as the process
    if (!g_saveStreams) return;
    // The free first and the inits last: a stream is only ever taken over once every
    // call that may reach it already comes here.
    struct Site { uintptr_t at, callee; void* to; };
    const Site sites[] = {
        { 0x25fcd4c, RVA_ZSTD_FREE, (void*)&SaveZstdFree },
        { 0x25fcfab, RVA_ZSTD_COMPRESS, (void*)&SaveZstdCompress },
        { 0x25fcfd5, RVA_ZSTD_FLUSH, (void*)&SaveZstdFlush },
        { 0x25fcfce, RVA_ZSTD_END, (void*)&SaveZstdEnd },
        { 0x25fd0a2, RVA_ZSTD_INIT, (void*)&SaveZstdInit },
        { 0x25fd1d3, RVA_ZSTD_INIT, (void*)&SaveZstdInit },
    };
    int done = 0;
    for (const auto& s : sites) {
        if (!SaveZstdRedirect(s.at, s.callee, s.to)) break;
        ++done;
    }
    if (done != 6) {
        Log("[savezstd] only %d of 6 call sites redirected -- no stream is taken over\n", done);
        return;
    }
    Log("[savezstd] installed: save compression runs on %d zstd worker threads (zstd %s built in, %s; save_threads=0 turns it off)\n",
        workers, ZSTD_versionString(), why);
}

static void SaveZstdLogAlive()
{
    if (!g_saveStreams) return;
    static uint64_t lastStreams = ~0ull;
    const auto s = g_saveStreams->Snapshot();
    if (s.streams == lastStreams) return;
    lastStreams = s.streams;
    Log("[savezstd] streams=%llu fallbacks=%llu input=%.1f MiB\n", (unsigned long long)s.streams,
        (unsigned long long)s.fallbacks, double(s.bytesIn) / 1048576.0);
}
