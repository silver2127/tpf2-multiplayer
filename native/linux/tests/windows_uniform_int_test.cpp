#include "windows_uniform_int_linux.h"
#include <cassert>
#include <climits>
#include <cstdio>
#include <initializer_list>
#include <vector>

struct Stream {
    std::vector<uint32_t> words;
    size_t used = 0;
    static uint32_t Next(void* context) {
        auto& self = *static_cast<Stream*>(context);
        assert(self.used < self.words.size());
        return self.words[self.used++];
    }
};

static void Check(int32_t lo, int32_t hi, std::initializer_list<uint32_t> words,
                  int32_t expected, size_t draws)
{
    Stream stream{words};
    assert(Tpf2mpWindowsUniformIntInclusive(lo, hi, &stream, Stream::Next) == expected);
    assert(stream.used == draws);
}

int main()
{
    // These witnesses were executed against the original Windows 35924
    // sampler at RVA 0x955010, including its actual MT engine advancement.
    // Native bucket division returns 0 and 108771 for these same raw words.
    Check(0, 9, {46662977}, 7, 1);
    Check(0, 9999998, {46662977}, 6662981, 1);
    Check(-10, -1, {46662977}, -3, 1);
    Check(-5, 4, {46662977}, 2, 1);

    // A singleton consumes no random word, including at both signed limits.
    Check(0, 0, {}, 0, 0);
    Check(INT_MIN, INT_MIN, {}, INT_MIN, 0);
    Check(INT_MAX, INT_MAX, {}, INT_MAX, 0);

    // Power-of-two widths accept UINT_MAX. The native sampler rejects it.
    Check(0, 1, {UINT32_MAX}, 1, 1);
    Check(0, INT_MAX, {UINT32_MAX}, INT_MAX, 1);
    Check(INT_MIN, -1, {UINT32_MAX}, -1, 1);
    Check(INT_MAX - 1, INT_MAX, {UINT32_MAX}, INT_MAX, 1);

    // Full signed width = 2^32, not zero. Preserve raw word ordering.
    Check(INT_MIN, INT_MAX, {0}, INT_MIN, 1);
    Check(INT_MIN, INT_MAX, {0x80000000}, 0, 1);
    Check(INT_MIN, INT_MAX, {UINT32_MAX}, INT_MAX, 1);

    // Tail rejections consume exactly those words, including multiple rejects
    // and the values on either side of the acceptance boundary.
    Check(0, 9, {4294967289u}, 9, 1);
    Check(0, 9, {4294967290u, UINT32_MAX, 5}, 5, 3);
    Check(0, 2, {UINT32_MAX, UINT32_MAX - 1}, 2, 2);
    Check(INT_MIN, INT_MAX - 1, {UINT32_MAX, 0}, INT_MIN, 2);
    Check(INT_MIN, 0, {UINT32_MAX, 0x80000001u, 0x80000000u}, 0, 3);

    Stream sequence{{46662977, 46662977, UINT32_MAX, 8}};
    assert(Tpf2mpWindowsUniformIntInclusive(0, 9, &sequence, Stream::Next) == 7);
    assert(Tpf2mpWindowsUniformIntInclusive(99, 99, &sequence, Stream::Next) == 99);
    assert(Tpf2mpWindowsUniformIntInclusive(0, 9999998, &sequence, Stream::Next) == 6662981);
    assert(Tpf2mpWindowsUniformIntInclusive(0, 9, &sequence, Stream::Next) == 8);
    assert(sequence.used == 4);
    puts("PASS: Windows integer witnesses, signed bounds, singleton/no-draw, full-width and rejection advancement");
}
