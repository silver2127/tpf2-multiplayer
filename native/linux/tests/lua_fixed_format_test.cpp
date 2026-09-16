#include "lua_fixed_format_linux.h"

#include <algorithm>
#include <cassert>
#include <cfenv>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

static std::string Format(const char* format, double value)
{
    char out[2048];
    const int n = std::snprintf(out, sizeof(out), format, value);
    assert(n >= 0 && size_t(n) < sizeof(out));
    int p;
    if (Tpf2mpLuaFixedPrecision(format, &p))
        Tpf2mpWindowsFixedTieCorrection(out, n, value, p);
    return out;
}

static std::string Hash(const std::string& bytes)
{
    uint64_t a = 2166136261U % 2147483647U, b = 2166136261U % 2147483629U;
    for (unsigned char c : bytes) {
        a = (a * 48271 + c) % 2147483647;
        b = (b * 40692 + c) % 2147483629;
    }
    char out[32];
    std::snprintf(out, sizeof(out), "%010llu-%010llu",
                  (unsigned long long)a, (unsigned long long)b);
    return out;
}

static std::string SortedHash(std::vector<std::string> lines)
{
    std::sort(lines.begin(), lines.end());
    std::string joined;
    for (const auto& line : lines) {
        if (!joined.empty()) joined += '|';
        joined += line;
    }
    return Hash(joined);
}

static void Geometry(const char* path)
{
    // Captured from the common 718-edge save on both platforms. Only four
    // shared nodes differ in the hash text. Their exact raw float32 positions
    // were independently recovered from the identical compressed save bytes.
    struct Node { const char* nativeText; double x, y, z; };
    const Node nodes[] = {
        {"-2976.0,1630.4,69.2", -2976.03857421875, 1630.3798828125, 69.25},
        {"1156.9,2520.6,8.2", 1156.86669921875, 2520.5546875, 8.25},
        {"174.9,-55.4,7.2", 174.8911590576172, -55.411277770996094, 7.25},
        {"2972.3,2352.9,2.2", 2972.25390625, 2352.911865234375, 2.25}
    };
    std::ifstream file(path);
    assert(file.good());
    std::string line;
    std::getline(file, line); // dump timestamp
    std::vector<std::string> original, corrected, heights;
    unsigned corrections = 0;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        assert(!line.empty());
        original.push_back(line);
        std::string edge;
        std::istringstream parts(line);
        std::string point;
        while (std::getline(parts, point, '>')) {
            for (const auto& n : nodes) {
                if (point == n.nativeText) {
                    // Lua string.format invokes the C formatter once for each
                    // conversion, even for its three-component node format.
                    point = Format("%.1f", n.x) + ',' + Format("%.1f", n.y) + ',' + Format("%.1f", n.z);
                    ++corrections;
                    break;
                }
            }
            if (!edge.empty()) edge += '>';
            edge += point;
        }
        corrected.push_back(edge);
        const size_t split = edge.find('>');
        heights.push_back(edge.substr(0, split).substr(edge.substr(0, split).rfind(',') + 1) + '>' +
                          edge.substr(edge.rfind(',') + 1));
    }
    assert(original.size() == 718 && corrections == 8);
    assert(SortedHash(original) == "0723081702-0114012359");
    assert(SortedHash(corrected) == "0725609111-0962938392");
    assert(SortedHash(heights) == "1783772806-1876288635");
    // A real additional movement is still detected; no hash lane is excluded.
    const size_t zStart = corrected[0].rfind(',') + 1;
    const double movedHeight = std::stod(corrected[0].substr(zStart)) + 0.2;
    corrected[0].replace(zStart, std::string::npos, Format("%.1f", movedHeight));
    assert(SortedHash(corrected) != "0725609111-0962938392");
}

int main(int argc, char** argv)
{
    assert(argc == 2);
    assert(std::fesetround(FE_TONEAREST) == 0);
    int p = -1;
    assert(Tpf2mpLuaFixedPrecision("%f", &p) && p == 6);
    assert(Tpf2mpLuaFixedPrecision("%+08.1f", &p) && p == 1);
    assert(Tpf2mpLuaFixedPrecision("%-#99.99F", &p) && p == 99);
    assert(Tpf2mpLuaFixedPrecision("%.f", &p) && p == 0);
    for (const char* s : {"", "f", "%g", "%e", "%a", "%lf", "%Lf", "%*f", "%.*f", "%1$f",
                          "%100f", "%.100f", "%f%f", "%f ", "%%", "%n", "%s", "%d"})
        assert(!Tpf2mpLuaFixedPrecision(s, &p));
    assert(!Tpf2mpLuaFixedPrecision(nullptr, &p));
    assert(!Tpf2mpLuaFixedPrecision("%f", nullptr));
    for (double value : {69.25, 8.25, 7.25, 2.25}) {
        const auto up = std::to_string(int(value)) + ".3";
        const auto down = std::to_string(int(value)) + ".2";
        assert(Format("%.1f", value) == up);
        assert(Format("%.1f", -value) == '-' + up);
        assert(Format("%.1f", std::nextafter(value, 0.0)) == down);
        assert(Format("%.1f", std::nextafter(value, INFINITY)) == up);
        assert(Format("%.1f", std::nextafter(-value, 0.0)) == '-' + down);
        assert(Format("%.1f", std::nextafter(-value, -INFINITY)) == '-' + up);
    }
    assert(Format("%.0f", 2.5) == "3");
    assert(Format("%.0f", -2.5) == "-3");
    assert(Format("%.0f", 3.5) == "4"); // glibc already chooses away
    assert(Format("%.0f", -3.5) == "-4");
    assert(Format("%+08.1f", 7.25) == "+00007.3");
    assert(Format("%+08.1f", -7.25) == "-00007.3");
    assert(Format("%-#8.0f", 2.5) == "3.      ");
    assert(Format("% .0f", 2.5) == " 3");
    assert(Format("%f", 0.0078125) == "0.007813");
    assert(Format("%.15f", 32767.9999847412109375) == "32767.999984741210938");
    // One nextafter step would corrupt dozens of earlier digits here. Correct
    // only the final decimal tie, keeping all 99 places and the buffer length.
    assert(Format("%.99f", std::ldexp(1.0, -100)) ==
           "0.000000000000000000000000000000788860905221011805411728565282786229673206435109023004770278930664063");
    assert(Format("%.1f", 0.0) == "0.0");
    assert(Format("%.1f", -0.0) == "-0.0");
    assert(Format("%.1f", INFINITY) == "inf");
    assert(Format("%.1f", -INFINITY) == "-inf");
    assert(Format("%.1f", std::numeric_limits<double>::denorm_min()) == "0.0");
    assert(Format("%.1f", std::numeric_limits<double>::quiet_NaN()) == "nan");
    for (int mode : {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO}) {
        assert(std::fesetround(mode) == 0);
        std::feclearexcept(FE_ALL_EXCEPT);
        std::feraiseexcept(FE_DIVBYZERO | FE_INEXACT);
        const int before = std::fetestexcept(FE_ALL_EXCEPT);
        char result[] = "2.2";
        Tpf2mpWindowsFixedTieCorrection(result, 3, 2.25, 1);
        assert(std::fegetround() == mode && std::fetestexcept(FE_ALL_EXCEPT) == before);
        assert(std::strcmp(result, mode == FE_TONEAREST ? "2.3" : "2.2") == 0);
    }
    std::fesetround(FE_TONEAREST);
    std::feclearexcept(FE_ALL_EXCEPT);
    Geometry(argv[1]);
    std::puts("Lua fixed decimal compatibility: exact ties, neighbors, format grammar, fenv and 718-edge Windows hashes passed");
}
