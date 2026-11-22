// windows_ucrt_math_linux.cpp -- see windows_ucrt_math_linux.h.
// Plain C++17; also builds with MSVC for the exhaustive check against
// ucrtbase.dll. No FMA contraction may be applied to this file.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("fp-contract=off")
#endif
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
#include "windows_ucrt_math_linux.h"
#include <cmath>
#include <cstring>
#if __has_include(<bit>)
#include <bit>
#endif
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace {

// std::bit_cast where the library has it: MSVC 14.44 /O2 folds a memcpy of a
// truncated 64-bit loop counter into a float CONVERSION (found by the
// exhaustive check), so the Windows check build must not rely on memcpy.
#if defined(__cpp_lib_bit_cast)
inline uint32_t FBits(float f) { return std::bit_cast<uint32_t>(f); }
inline float BitsF(uint32_t u) { return std::bit_cast<float>(u); }
inline uint64_t DBits(double d) { return std::bit_cast<uint64_t>(d); }
inline double BitsD(uint64_t u) { return std::bit_cast<double>(u); }
#else
inline uint32_t FBits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
inline float BitsF(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
inline uint64_t DBits(double d) { uint64_t u; std::memcpy(&u, &d, 8); return u; }
inline double BitsD(uint64_t u) { double d; std::memcpy(&d, &u, 8); return d; }
#endif

inline void Mul64(uint64_t a, uint64_t b, uint64_t* lo, uint64_t* hi)
{
#if defined(_MSC_VER)
    *lo = _umul128(a, b, hi);
#else
    const unsigned __int128 p = (unsigned __int128)a * b;
    *lo = (uint64_t)p;
    *hi = (uint64_t)(p >> 64);
#endif
}
inline int Bsr64(uint64_t v)
{
#if defined(_MSC_VER)
    unsigned long i;
    _BitScanReverse64(&i, v);
    return (int)i;
#else
    return 63 - __builtin_clzll(v);
#endif
}

// ---- constants (bit patterns read from ucrtbase.dll) -------------------------
const double kC6 = 0x1.5555555555555p-3;          // 1/6 (small-argument sin)
// sin(r) = r + r^3*((S1 + S2 r^2) + r^4 (S3 + S4 r^2))
const double kS1 = -0x1.5555555555555p-3;
const double kS2 = 0x1.1111111111111p-7;
const double kS3 = -0x1.a01a01a01a01ap-13;
const double kS4 = 0x1.71de3a556c734p-19;
// cos(r) = (1 - r^2/2) + r^4*((K1 + K2 r^2) + r^4 (K3 + K4 r^2))
const double kK1 = 0x1.5555555555555p-5;
const double kK2 = -0x1.6c16c16c16c16p-10;
const double kK3 = 0x1.a01a01a01a019p-16;
const double kK4 = -0x1.27e4fb7789f5cp-22;
const double kTwoByPi = 0x1.45f306dc9c883p-1;
const double kPiBy2_1 = 0x1.921fb54400000p+0;
const double kPiBy2_1Tail = 0x1.0b4611a626331p-34;
const double kPiBy2_2 = 0x1.0b4611a600000p-34;
const double kPiBy2_2Tail = 0x1.3198a2e037073p-69;
const double kPiBy2 = 0x1.921fb54442d18p+0;
const double kPi = 0x1.921fb54442d18p+1;
const double kPiBy2Tail = 0x1.1a62633145c07p-54;
// tan(r) = r + r^3 * (T3 r^2 + T2) / ((T1 r^2 + T0b) r^2 + T0)
const double kThird = 0x1.5555555555555p-2;
const double kT3 = -0x1.19dba6efd6aadp-6;
const double kT2 = 0x1.8a8b0da56cb17p-2;
const double kT1 = 0x1.2e29003c692d9p-6;
const double kT0b = -0x1.07266d7b3511bp-1;
const double kT0 = 0x1.27e84a3e73a2ep+0;
const double kTwo23 = 0x1.0p+23;

#include "windows_ucrt_math_tables_linux.inc"

inline uint64_t Load64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }

// Payne-Hanek reduction of a positive double x >= 2^19, as inlined in sinf
// (ucrtbase 0xac490) and tanf (0xad568) and in 0xab710 (cosf). Returns the
// quadrant; *head is the reduced fraction f (x*2/pi minus the nearest integer,
// in [-1/2, 1/2]) as a double built from the top 53 product bits, *tail the next
// bits as a second double (used by cosf only).
int ReduceBits(double x, double* head, double* tail)
{
    const uint64_t ux = DBits(x);
    const int64_t e = (int64_t)(ux >> 52) - 0x3ff;
    const int64_t off = 0x86 - (e >> 3);
    const uint64_t m = (ux & 0x000fffffffffffffULL) | (1ULL << 52);
    uint64_t lo, hi;
    Mul64(Load64(kTwoByPiBits + off), m, &lo, &hi);
    uint64_t r8 = lo;
    const uint64_t carry = hi;
    Mul64(Load64(kTwoByPiBits + off + 8), m, &lo, &hi);
    uint64_t r9 = lo + carry;
    uint64_t r10 = hi + (r9 < lo ? 1 : 0);
    r10 += Load64(kTwoByPiBits + off + 16) * m;
    const int e7 = (int)(e & 7);
    const int cl = 54 - e7;
    uint64_t q = r10 >> cl;
    const uint64_t roundBit = (r10 >> (cl - 1)) & 1;
    uint64_t sign = 0;
    if (roundBit) { r10 = ~r10; r9 = ~r9; r8 = ~r8; sign = 1ULL << 63; }
    q = (q + roundBit) & 3;
    const int keep = e7 + 10;
    r10 = (r10 << keep) >> keep;
    uint64_t ex = (uint64_t)(int64_t)(keep - 64);
    if (r10 == 0) { r10 = r9; r9 = r8; r8 = 0; ex -= 64; }
    const int msb = Bsr64(r10);
    ex += (uint64_t)msb;
    const int c = msb - 52;
    if (c > 0) {
        const uint64_t saved = r10;
        r10 >>= c;
        r9 >>= c;
        r9 |= saved << (64 - c);
    } else if (c < 0) {
        const int n = -c;
        const uint64_t a = r9;
        r10 <<= n;
        r9 <<= n;
        r10 |= a >> (64 - n);
        r9 |= r8 >> (64 - n);
    }
    ex += 0x3ff;
    r10 &= ~(1ULL << 52);
    r10 |= sign;
    r10 |= ex << 52;
    *head = BitsD(r10);
    if (tail) {
        // 0xab86e: the next bits of r9, normalised (x86 shift counts are mod 64).
        const int p2 = r9 ? Bsr64(r9) : 0;
        const uint64_t sh = (uint64_t)(64 - p2);
        uint64_t t = r9 << (sh & 63);
        t >>= 12;
        const uint64_t tex = ex - (sh + 52);
        t |= sign;
        t |= tex << 52;
        *tail = BitsD(t);
    }
    return (int)q;
}

// sin/cos kernels of the AVX2+FMA paths (Horner with fused multiply-adds).
inline double SinFma(double r)
{
    const double x2 = r * r;
    double p = std::fma(x2, kS4, kS3);
    p = std::fma(p, x2, kS2);
    p = std::fma(p, x2, kS1);
    const double x3 = r * x2;
    return std::fma(p, x3, r);
}
// cosf's kernel: 1 - r^2/2 rounded before the fused tail.
inline double CosFmaCosf(double r)
{
    const double x2 = r * r;
    const double head = 1.0 - x2 * 0.5;
    double p = std::fma(x2, kK4, kK3);
    p = std::fma(p, x2, kK2);
    p = std::fma(p, x2, kK1);
    const double x4 = x2 * x2;
    return std::fma(p, x4, head);
}
// sinf's cosine kernel: 1 - r^2/2 is itself fused.
inline double CosFmaSinf(double r)
{
    const double x2 = r * r;
    const double head = std::fma(x2, -0.5, 1.0);
    double p = std::fma(x2, kK4, kK3);
    p = std::fma(p, x2, kK2);
    p = std::fma(p, x2, kK1);
    const double x4 = x2 * x2;
    return std::fma(p, x4, head);
}

// Cody-Waite single step with fused reduction (both sinf and cosf AVX paths).
inline double ReduceFma(double xa, int* region)
{
    const double t = std::fma(kTwoByPi, xa, 0.5);
    const int n = (int)t;                              // cvttpd2dq
    *region = n & 3;
    const double dn = (double)n;
    const double head = std::fma(-dn, kPiBy2_1, xa);
    const double tail = dn * kPiBy2_1Tail;
    return head - tail;
}

inline float NanResult(float x)
{
    // UCRT error path: NaN in -> quiet NaN (same payload); +-inf -> default NaN.
    const uint32_t u = FBits(x);
    if ((u & 0x7fffffff) > 0x7f800000) return BitsF(u | 0x00400000);
    return BitsF(0xffc00000u);
}

const double kPiBy2Hi = 0x1.921fb50000000p+0;          // ucrtbase 0x10bbb0
const double kPiBy2Lo = 0x1.110b460000000p-26;          // 0x10bbc0
const double kPiBy2Tail2 = 0x1.1a62633145c06p-54;       // 0x10bbd0

} // namespace

// ---- sinf, AVX2+FMA path (ucrtbase 0xac36d) ---------------------------------
float Tpf2mpUcrtSinf(float xf)
{
    const uint32_t ax = FBits(xf) & 0x7fffffff;
    if (ax >= 0x7f800000) return NanResult(xf);
    const double xd = xf;
    if (ax <= 0x3f490fdb) {
        if (ax < 0x3c000000) {
            if (ax < 0x39000000) return xf;
            const double x3 = (xd * xd) * xd;
            return (float)std::fma(-x3, kC6, xd);
        }
        return (float)SinFma(xd);
    }
    const double xa = BitsD(DBits(xd) & 0x7fffffffffffffffULL);
    double r;
    int region;
    if (ax < 0x4b800456) {
        r = ReduceFma(xa, &region);
    } else {
        double f;
        region = ReduceBits(xa, &f, nullptr);
        r = f * kPiBy2;
    }
    double v = (region & 1) ? CosFmaSinf(r) : SinFma(r);
    uint64_t flip = 0;
    if (region < 2) flip ^= 0x8000000000000000ULL;
    if (!(DBits(xd) >> 63)) flip ^= 0x8000000000000000ULL;
    v = BitsD(DBits(v) ^ flip);
    return (float)v;
}

// ---- cosf, AVX2+FMA path (ucrtbase 0xa7f7d) ---------------------------------
float Tpf2mpUcrtCosf(float xf)
{
    const uint32_t ax = FBits(xf) & 0x7fffffff;
    if (ax >= 0x7f800000) return NanResult(xf);
    const double xd = xf;
    if (ax <= 0x3f490fdb) {
        if (ax < 0x3c000000) {
            if (ax < 0x39000000) return 1.0f;
            return (float)std::fma(-(xd * 0.5), xd, 1.0);
        }
        return (float)CosFmaCosf(xd);
    }
    const double xa = BitsD(DBits(xd) & 0x7fffffffffffffffULL);
    double r;
    int region;
    if (ax < 0x4f490fdb) {
        r = ReduceFma(xa, &region);
    } else {
        // 0xab710: double-double product of the fraction with pi/2.
        double f, t;
        region = ReduceBits(xa, &f, &t);
        const double fh = BitsD(DBits(f) & 0xfffffffff8000000ULL);
        const double fl = f - fh;
        const double big = f * kPiBy2;
        double lo = fh * kPiBy2Hi;
        lo = lo - big;
        lo = std::fma(fl, kPiBy2Hi, lo);
        lo = std::fma(fh, kPiBy2Lo, lo);
        lo = std::fma(fl, kPiBy2Lo, lo);
        double tl = t * kPiBy2;
        tl = std::fma(f, kPiBy2Tail2, tl);
        lo = lo + tl;
        r = big + lo;
    }
    double v = (region & 1) ? SinFma(r) : CosFmaCosf(r);
    if (((region + 1) >> 1) & 1) v = BitsD(DBits(v) ^ 0x8000000000000000ULL);
    return (float)v;
}

void Tpf2mpUcrtSincosf(float x, float* s, float* c)
{
    *s = Tpf2mpUcrtSinf(x);
    *c = Tpf2mpUcrtCosf(x);
}

namespace {
inline double TanRational(double r)
{
    const double x2 = r * r;
    double num = kT3 * x2;
    num = num + kT2;
    double den = kT1 * x2;
    den = den + kT0b;
    den = den * x2;
    den = den + kT0;
    num = num / den;
    double t = x2 * r;
    t = t * num;
    return r + t;
}
} // namespace

float Tpf2mpUcrtTanf(float xf)
{
    const uint32_t ax = FBits(xf) & 0x7fffffff;
    if (ax >= 0x7f800000) return NanResult(xf);
    const double x5 = xf;
    if (ax <= 0x3f490fdb) {
        if (ax < 0x39000000) {
            if (ax < 0x32000000) return xf;
            double t = x5 * x5;
            t = t * x5;
            t = t * kThird;
            return (float)(t + x5);
        }
        return (float)TanRational(x5);
    }
    const uint64_t abits = DBits(x5) & 0x7fffffffffffffffULL;
    const double xa = BitsD(abits);
    double r;
    int region;
    if ((int64_t)abits < (int64_t)DBits(kTwo23)) {
        const double t = xa * kTwoByPi + 0.5;
        const int n = (int)t;
        region = n & 3;
        const double dn = (double)n;
        const double head = xa - dn * kPiBy2_1;
        const double tail = dn * kPiBy2_1Tail;
        r = head - tail;
    } else {
        double f;
        region = ReduceBits(xa, &f, nullptr);
        r = f * kPiBy2;
    }
    double v = TanRational(r);
    if (region & 1) v = -1.0 / v;
    v = BitsD(DBits(v) ^ (DBits(x5) & 0x8000000000000000ULL));
    return (float)v;
}

// ---- acosf, AVX2+FMA path (ucrtbase 0x738ac) --------------------------------
float Tpf2mpUcrtAcosf(float x)
{
    const uint32_t ux = FBits(x);
    const uint32_t ax = ux & 0x7fffffff;
    const unsigned e = (ux >> 23) & 0xff;
    if (ax > 0x7f800000) return BitsF(ux | 0x00400000);
    if (e < 0x65) return BitsF(0x3fc90fdbu);                  // pi/2
    if (e >= 0x7f) {
        if (x == 1.0f) return 0.0f;
        if (x == -1.0f) return BitsF(0x40490fdbu);            // pi
        return BitsF(0xffc00000u);
    }
    const float a = BitsF(ax);
    const bool big = e >= 0x7e;                               // |x| >= 0.5
    float r, s = 0.0f;
    if (!big) {
        r = a * a;
    } else {
        r = (1.0f - a) * 0.5f;
        s = std::sqrt(r);
    }
    float p = std::fma(-0x1.039cd6p-8f, r, -0x1.b67fc2p-7f);
    p = std::fma(p, r, -0x1.cf17bap-5f);
    p = std::fma(p, r, 0x1.7929b8p-3f);
    const float num = p * r;
    const float den = std::fma(-0x1.ac3e1ap-1f, r, 0x1.1adf4ap+0f);
    const float u = num / den;
    if (!big) {
        const double d = (double)(u * x);
        const double t = kPiBy2Tail - d;
        const double w = (double)x - t;
        return (float)(kPiBy2 - w);
    }
    if (ux >> 31) {
        const double d1 = (double)(s * u) - kPiBy2Tail;
        const double v = d1 + (double)s;
        return (float)(kPi - (v + v));
    }
    const float c = BitsF(FBits(s) & 0xffff0000u);
    const float sc = c + s;
    const float corr = std::fma(-c, c, r) / sc;
    float t3 = corr + corr;
    const float s2 = s + s;
    t3 = std::fma(u, s2, t3);
    const float c2 = c + c;
    return t3 + c2;
}

// ---- atan2f, SSE2 path (ucrtbase 0x519c0) ------------------------------------
namespace {

const double kAtanC3 = 0x1.5555555550877p-2;
const double kAtanP3 = 0x1.5555555555538p-2;
const double kAtanP5 = 0x1.99999999643a3p-3;
const double kAtanP7 = 0x1.2492482bd6be1p-3;
const double kAtanSmall = 0x1.a36e2eb1c432dp-14;      // 0.0001 (ucrtbase 0x102910)
} // namespace


float Tpf2mpUcrtAtan2f(float yf, float xf)
{
    const double xd = xf, yd = yf;
    const uint64_t xb = DBits(xd), yb = DBits(yd);
    const uint64_t xa = xb & 0x7fffffffffffffffULL, ya = yb & 0x7fffffffffffffffULL;
    const uint64_t inf = 0x7ff0000000000000ULL;
    const bool yneg = (yb >> 63) != 0, xneg = (xb >> 63) != 0;
    const int ediff = (int)((yb >> 52) & 0x7ff) - (int)((xb >> 52) & 0x7ff);
    if (xa > inf) return BitsF(FBits(xf) | 0x00400000);
    if (ya > inf) return BitsF(FBits(yf) | 0x00400000);
    if (ya == 0) {
        if (xneg) return yneg ? BitsF(0xc0490fdbu) : BitsF(0x40490fdbu);
        return yf;
    }
    if (xa == 0) {
        if (yneg) return BitsF(0xbfc90fdbu);
        // +y over +-0 continues and leaves through ediff > 26.
    }
    if (ediff > 26) return yneg ? BitsF(0xbfc90fdbu) : BitsF(0x3fc90fdbu);
    if (ediff < -13 && !xneg) {
        if (ediff < -150) return yneg ? BitsF(0x80000000u) : 0.0f;
        if (ediff < -126) return (float)std::ldexp(0x1.0p100 * yd / xd, -100);
        return (float)(yd / xd);
    }
    if (ediff < -26) {   // x < 0 here
        return yneg ? BitsF(0xc0490fdbu) : BitsF(0x40490fdbu);
    }
    if (ya == inf) {
        if (xa == inf) {
            if (!xneg) return yneg ? BitsF(0xbf490fdbu) : BitsF(0x3f490fdbu);
            return yneg ? BitsF(0xc016cbe4u) : BitsF(0x4016cbe4u);
        }
    }
    double den = BitsD(xa), num = BitsD(ya);
    bool swapped = false;
    if (num > den) { const double t = den; den = num; num = t; swapped = true; }
    double u = num / den;
    double res;
    if (u > 0.0625) {
        const int k = (int)(u * 256.0 + 0.5);
        const double dk = (double)k;
        const double a = dk * num;
        const double b = dk * den;
        const double den256 = den * 256.0;
        double v = num * 256.0 - b;
        v = v / (a + den256);
        res = v + kAtanTable[k - 16];
        double c = v * v;
        c = c * v;
        c = c * kAtanC3;
        res = res - c;
    } else if (kAtanSmall > u) {
        res = u;
    } else {
        double u2 = u * u;
        double p = kAtanP5 - u2 * kAtanP7;
        p = p * u2;
        const double u3 = u2 * u;
        double q = kAtanP3 - p;
        q = q * u3;
        res = u - q;
    }
    if (swapped) res = kPiBy2 - res;
    if (xneg) res = kPi - res;
    if (yneg) res = BitsD(DBits(res) ^ 0x8000000000000000ULL);
    return (float)res;
}
