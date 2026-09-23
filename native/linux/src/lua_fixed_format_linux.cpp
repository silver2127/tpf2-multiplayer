#include "lua_fixed_format_linux.h"

#include <cstdint>
#include <cstring>
#include <cfenv>
#include <limits>

bool Tpf2mpLuaFixedPrecision(const char* format, int* precision)
{
    if (!format || !precision || *format++ != '%') return false;
    // Lua's scanformat limits both width and precision to two digits and its
    // flags to five characters. Reject stars, lengths, literals and additional
    // conversions so a different vararg ABI can never reach the double path.
    unsigned flags = 0;
    while (*format && std::strchr("-+ #0", *format)) {
        if (++flags > 5) return false;
        ++format;
    }
    unsigned width = 0;
    while (*format >= '0' && *format <= '9') {
        if (++width > 2) return false;
        ++format;
    }
    int digits = 6;
    if (*format == '.') {
        ++format;
        digits = 0;
        unsigned count = 0;
        while (*format >= '0' && *format <= '9') {
            if (++count > 2) return false;
            digits = digits * 10 + (*format++ - '0');
        }
    }
    if ((*format != 'f' && *format != 'F') || format[1] != '\0') return false;
    *precision = digits;
    return true;
}

bool Tpf2mpWindowsFixedTieCorrection(char* output, int length, double value, int precision)
{
    if (!output || length <= 0 || precision < 0 || precision > 99 ||
        std::fegetround() != FE_TONEAREST) return false;
    static_assert(sizeof(double) == sizeof(uint64_t), "binary64 required");
    static_assert(std::numeric_limits<double>::is_iec559 &&
                  std::numeric_limits<double>::digits == 53, "IEEE binary64 required");
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    const unsigned exponent = unsigned((bits >> 52) & 0x7ff);
    uint64_t mantissa = bits & UINT64_C(0x000fffffffffffff);
    if (exponent == 0x7ff) return false; // infinities and NaNs, payload intact
    int power = -1074;
    if (exponent) {
        mantissa |= UINT64_C(0x0010000000000000);
        power = int(exponent) - 1023 - 52;
    }
    if (!mantissa) return false; // both signed zeros

    // abs(value) = mantissa * 2^power. Multiplying by 10^precision adds
    // precision factors of two and only odd factors of five. The result is
    // exactly an integer + 1/2 iff precisely one factor of two remains in the
    // denominator. No rounded multiply, epsilon, locale or fenv is involved.
    const int zeros = __builtin_ctzll(mantissa);
    if (power + zeros + precision != -1) return false;

    // At a tie the scaled magnitude is odd/2. Since 5^precision == 1 (mod 4),
    // the lower integer is even iff the odd mantissa == 1 (mod 4). Only those
    // ties differ: the other half already round away from zero under glibc.
    if (((mantissa >> zeros) & 3) != 1) return false;

    // The final fixed-decimal digit is then 0/2/4/6/8, so increasing it by one
    // requires no carry, padding change or extra buffer space. Skip a trailing
    // decimal point (%#.0f) or spaces (%-8.1f). Do not nudge the input double:
    // at high precision one binary ULP spans MANY printed decimal places.
    for (int i = length - 1; i >= 0; --i) {
        if (output[i] < '0' || output[i] > '9') continue;
        if ((output[i] - '0') % 2 != 0) return false;
        ++output[i];
        return true;
    }
    return false;
}
