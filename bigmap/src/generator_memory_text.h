#pragma once
#include <cstdio>
// Shared text transformation; embedding supplied by each platform build.
static const char kGenAnchor[] = "\t\treturn result";
static const char kGenReplacement[] = "\t\treturn _tpf2_bigmap_generate(result, params) -- tpf2_bigmap memory";
// Appended after the pass: logs what the game handed the generator, then runs
// the pass above 32 x 32 km.
static const char kGenHelper[] = R"BMLUA(
_tpf2_bigmap_generate = function(result, params)
    local x = tonumber(type(params) ~= "nil" and params.mapSizeX) or 0
    local y = tonumber(type(params) ~= "nil" and params.mapSizeY) or 0
    local layers = type(result) == "table" and result.layers
    local n, count, names = 0, 0, {}
    if type(layers) == "table" then
        n = #layers
        for i = 1, n do
            local p = type(layers[i]) == "table" and layers[i].params
            if type(p) == "table" then
                for _, k in ipairs({"input", "input1", "input2", "output"}) do
                    if p[k] ~= nil and not names[p[k]] then names[p[k]] = true; count = count + 1 end
                end
            end
        end
    end
    -- mapSizeX/Y are heightmap samples at 4 m, 64 * tiles + 1 (MEASURED: 12289
    -- for 192 x 192 tiles), so 32 x 32 km (128 x 128 tiles) is 8193 x 8193.
    local big = x * y > 8193 * 8193
    -- Buffers the memory budget holds (4-byte floats): above the pass's
    -- minimum they let independent layers run in parallel. nil = fewest.
    local budget = tonumber(_tpf2_bigmap_budget) or 0
    local cap = (big and budget > 0) and math.floor(budget / (x * y * 4)) or nil
    print(string.format("[tpf2_bigmap] generator memory: %.0f x %.0f samples (%.0f x %.0f tiles), %d layers over %d buffer names%s",
        x, y, (x - 1) / 64, (y - 1) / 64, n, count,
        not big and " (32 x 32 km or less: unchanged)" or cap and string.format(", budget %.0f MB = %d buffers", budget / 1048576, cap) or ""))
    if big then result = _tpf2_bigmap_memory.Optimize(result, cap) end
    return result
end
)BMLUA";

// The patched generator text, malloc'd; nullptr when the anchor is not exactly
// one whole line.
static char* PatchGeneratorText(const char* src, size_t len, size_t* outLen, unsigned long long budgetBytes) {
    const size_t a = sizeof kGenAnchor - 1;
    const char* hit = nullptr;
    if (len < a) return nullptr;
    for (size_t pos = 0; pos <= len - a; ++pos) {
        const char* p = src + pos;
        if (memcmp(p, kGenAnchor, a) != 0) continue;
        const char* e = p + a;
        bool lineStart = p == src || p[-1] == '\n';
        bool lineEnd = e == src + len || *e == '\n' || (*e == '\r' && e + 1 < src + len && e[1] == '\n');
        if (!lineStart || !lineEnd) continue;
        if (hit) return nullptr;   // repeated
        hit = p;
    }
    if (!hit) return nullptr;
    static const char head[] = "\n_tpf2_bigmap_memory = (function()\n";
    static const char tail[] = "\nend)()\n";
    size_t module = 0;
    for (const char* part : kGeneratorMemoryLuaParts) module += strlen(part);
    const size_t r = sizeof kGenReplacement - 1;
    char budget[64];
    int b = std::snprintf(budget, sizeof budget, "_tpf2_bigmap_budget = %llu\n", budgetBytes);
    if (b < 0 || size_t(b) >= sizeof budget) return nullptr;
    size_t total = len - a + r + (sizeof head - 1) + module + (sizeof tail - 1) + (sizeof kGenHelper - 1) + size_t(b);
    char* out = (char*)malloc(total + 1);
    if (!out) return nullptr;
    char* o = out;
    size_t pre = size_t(hit - src);
    memcpy(o, src, pre); o += pre;
    memcpy(o, kGenReplacement, r); o += r;
    memcpy(o, hit + a, len - pre - a); o += len - pre - a;
    memcpy(o, head, sizeof head - 1); o += sizeof head - 1;
    for (const char* part : kGeneratorMemoryLuaParts) { size_t k = strlen(part); memcpy(o, part, k); o += k; }
    memcpy(o, tail, sizeof tail - 1); o += sizeof tail - 1;
    memcpy(o, kGenHelper, sizeof kGenHelper - 1); o += sizeof kGenHelper - 1;
    memcpy(o, budget, size_t(b)); o += b;
    *o = 0;
    *outLen = size_t(o - out);
    return out;
}
