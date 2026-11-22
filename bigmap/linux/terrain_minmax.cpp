#include <emmintrin.h>
#include <cstdint>
#include <algorithm>
extern "C" {
__attribute__((visibility("hidden"))) void* bigmap_minmax_return=nullptr;
__attribute__((visibility("hidden"))) uint32_t BigmapMinMax(const uint16_t* p,const uint16_t* end) {
    // SSE2 signed min/max on sign-bit-flipped uint16 values: exact on all CPUs
    // supported by the x86-64 game, with no resolution or rounding changes.
    uint16_t lo=65535,hi=0;
    const auto bias=_mm_set1_epi16(short(-32768));
    auto mn=_mm_set1_epi16(32767),mx=_mm_set1_epi16(short(-32768));
    for(;end-p>=8;p+=8){auto v=_mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p)),bias);mn=_mm_min_epi16(mn,v);mx=_mm_max_epi16(mx,v);}
    alignas(16) uint16_t lows[8],highs[8];
    _mm_store_si128(reinterpret_cast<__m128i*>(lows),_mm_xor_si128(mn,bias));
    _mm_store_si128(reinterpret_cast<__m128i*>(highs),_mm_xor_si128(mx,bias));
    for(int i=0;i<8;++i){lo=std::min(lo,lows[i]);hi=std::max(hi,highs[i]);}
    for(;p<end;++p){lo=std::min(lo,*p);hi=std::max(hi,*p);}
    return uint32_t(lo)|(uint32_t(hi)<<16);
}
}
