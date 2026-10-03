// save_zstd.h -- the game's zstd compression streams handed to a multithreaded
// zstd (build 35924; docs/DEDICATED_SERVER.md, "The autosave"). Shared by the
// Linux slice (linux/src/slice/save_zstd_linux.cpp: the system libzstd, dlopen'd)
// and the Windows one (slice/save_zstd.inl: zstd 1.5.7 built in, third_party/zstd).
//
// WHY. A save is one boost::iostreams zstd_compressor fed by the serializer on the
// simulation thread. Measured on the dedicated server (2026-09-27, a 1.2 GB world,
// 513 MB on disk): the thread spends 5.4 s at 100%, and 48% of it is the game's
// own embedded zstd 1.5.2 compressing at level 1 -- on one core, with six engine
// workers idle because the session is held. The whole session waits for it.
//
// HOW. boost's zstd_base (the only caller of the embedded compression API) calls
// ZSTD_initCStream, ZSTD_compressStream, ZSTD_flushStream, ZSTD_endStream and
// ZSTD_freeCStream from six call sites, in both builds. Those calls are pointed
// here. A stream that is initialised gets a ZSTD_CCtx with nbWorkers set; the
// calls map onto ZSTD_compressStream2 with continue / flush / end. With workers,
// continue only copies the input into a job and returns: the serializer runs on
// while the workers compress. The output is one standard zstd frame, which the
// game's decoder reads (loading is untouched). The embedded stream is still
// initialised and freed as before, so a stream this code does not take keeps
// working exactly as the game built it.
//
// BOOST'S END LOOP. zstd_base::deflate(finish) calls compressStream with no input
// and then endStream, again and again until endStream reports 0. The game's
// single-threaded zstd takes the empty compressStream in the middle of an end;
// a multithreaded context refuses any `continue` once an end has begun
// (stage_wrong), boost throws, and the save closes without its last block --
// the first build of this did exactly that on the server (2026-09-27 18:18, a
// 511 MB save that stopped short of its end). So while a flush or end is
// pending, an empty compressStream is answered here without the library, and
// one that brings input first finishes the pending directive.
//
// OFF: save_threads=0 in tpf2_menu_flags.txt, no usable zstd, or any byte guard
// failing. save_threads=N (1..16) sets the workers; the default is 4, at most one
// less than the CPUs.
#pragma once
#include <cstddef>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace savezstd {

struct InBuf { const void* src; size_t size; size_t pos; };    // ZSTD_inBuffer
struct OutBuf { void* dst; size_t size; size_t pos; };         // ZSTD_outBuffer

// The stable zstd entry points used (all >= 1.4.0).
struct Api {
    void* (*createCCtx)() = nullptr;
    size_t (*freeCCtx)(void*) = nullptr;
    size_t (*setParameter)(void*, int, int) = nullptr;
    size_t (*reset)(void*, int) = nullptr;
    size_t (*compressStream2)(void*, OutBuf*, InBuf*, int) = nullptr;
    unsigned (*isError)(size_t) = nullptr;
    bool Complete() const { return createCCtx && freeCCtx && setParameter && reset && compressStream2 && isError; }
};
enum { kLevel = 100, kWorkers = 400 };                  // ZSTD_cParameter
enum { kResetSession = 1 };                             // ZSTD_reset_session_only
enum { kContinue = 0, kFlush = 1, kEnd = 2 };           // ZSTD_EndDirective

// The game's own functions, as the six call sites reached them.
struct Embedded {
    size_t (*init)(void* cs, int level) = nullptr;
    size_t (*compress)(void* cs, OutBuf* out, InBuf* in) = nullptr;
    size_t (*flush)(void* cs, OutBuf* out) = nullptr;
    size_t (*end)(void* cs, OutBuf* out) = nullptr;
    size_t (*free)(void* cs) = nullptr;
};

struct Stats { uint64_t streams = 0, fallbacks = 0, bytesIn = 0; };

class Streams {
public:
    Streams(const Api& api, const Embedded& game, int workers) : api_(api), game_(game), workers_(workers) {}

    // The embedded stream is initialised first, as the game did; its result is
    // what boost checks when this stream stays the game's.
    size_t Init(void* cs, int level)
    {
        const size_t r = game_.init(cs, level);
        std::lock_guard<std::mutex> lock(mu_);
        Ctx& c = map_[cs];
        void*& ctx = c.cctx;
        c.pending = kNone;
        bool ok = true;
        if (!ctx) ok = (ctx = api_.createCCtx()) != nullptr &&
                       !api_.isError(api_.setParameter(ctx, kWorkers, workers_));
        ok = ok && !api_.isError(api_.reset(ctx, kResetSession)) &&
             !api_.isError(api_.setParameter(ctx, kLevel, level));
        if (!ok) {
            if (ctx) api_.freeCCtx(ctx);
            map_.erase(cs);
            ++stats_.fallbacks;
        } else ++stats_.streams;
        last_ = {nullptr, nullptr};
        return r;
    }
    size_t Compress(void* cs, OutBuf* out, InBuf* in)
    {
        Ctx* c = Find(cs);
        if (!c) return game_.compress(cs, out, in);
        void* ctx = c->cctx;
        const bool input = in && in->pos < in->size;
        if (c->pending != kNone) {
            if (!input) return 0;                    // boost's empty call inside an end or flush
            InBuf none{nullptr, 0, 0};
            const size_t left = api_.compressStream2(ctx, out, &none, c->pending);
            if (api_.isError(left) || left) return left;   // still draining: the input waits
            c->pending = kNone;
        }
        const size_t before = in ? in->pos : 0;
        const size_t r = api_.compressStream2(ctx, out, in, kContinue);
        if (in) bytesIn_.fetch_add(in->pos - before, std::memory_order_relaxed);
        return r;
    }
    size_t Flush(void* cs, OutBuf* out)
    {
        Ctx* c = Find(cs);
        if (!c) return game_.flush(cs, out);
        return Drain(c, out, c->pending == kEnd ? kEnd : kFlush);
    }
    size_t End(void* cs, OutBuf* out)
    {
        Ctx* c = Find(cs);
        if (!c) return game_.end(cs, out);
        return Drain(c, out, kEnd);
    }
    size_t Free(void* cs)
    {
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = map_.find(cs);
            if (it != map_.end()) { api_.freeCCtx(it->second.cctx); map_.erase(it); }
            last_ = {nullptr, nullptr};
        }
        return game_.free(cs);
    }
    Stats Snapshot()
    {
        std::lock_guard<std::mutex> lock(mu_);
        Stats s = stats_; s.bytesIn = bytesIn_.load(std::memory_order_relaxed); return s;
    }

private:
    enum { kNone = -1 };
    struct Ctx { void* cctx = nullptr; int pending = kNone; };   // pending: kFlush / kEnd until it reports 0
    size_t Drain(Ctx* c, OutBuf* out, int directive)
    {
        InBuf none{nullptr, 0, 0};
        const size_t left = api_.compressStream2(c->cctx, out, &none, directive);
        c->pending = api_.isError(left) || !left ? int(kNone) : directive;
        return left;
    }
    // Compress runs ~20,000 times a save on one thread: the last stream found is
    // kept (streams are only added and removed under the lock, which clears it).
    // A Ctx is only touched by the thread that owns its stream.
    Ctx* Find(void* cs)
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (last_.first == cs) return last_.second;
        auto it = map_.find(cs);
        Ctx* c = it == map_.end() ? nullptr : &it->second;
        last_ = {cs, c};
        return c;
    }
    Api api_;
    Embedded game_;
    int workers_;
    std::mutex mu_;
    std::unordered_map<void*, Ctx> map_;     // node-based: a Ctx* stays valid until its erase
    std::pair<void*, Ctx*> last_{nullptr, nullptr};
    Stats stats_;
    std::atomic<uint64_t> bytesIn_{0};
};

} // namespace savezstd
