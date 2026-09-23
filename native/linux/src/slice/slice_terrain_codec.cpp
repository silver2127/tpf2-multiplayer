// Platform-neutral TPTG/TPAS v1 codecs; Windows wire format is unchanged.
#include "slice_terrain_assets.h"
#include <cmath>
#include <cstring>
#include <limits>

namespace slice_terrain_assets {
namespace {
template<class T> void Put(std::vector<uint8_t>& dst, T v)
{
    for (size_t i = 0; i < sizeof(v); ++i) dst.push_back(uint8_t(v >> (i * 8)));
}
void Bytes(std::vector<uint8_t>& dst, const void* src, size_t n)
{
    if (n) dst.insert(dst.end(), static_cast<const uint8_t*>(src), static_cast<const uint8_t*>(src) + n);
}
struct Reader {
    const std::vector<uint8_t>& data;
    size_t pos = 0;
    bool take(void* dst, size_t n) {
        if (n > data.size() - pos) return false;
        if (n) std::memcpy(dst, data.data() + pos, n);
        pos += n;
        return true;
    }
    template<class T> bool get(T* out) {
        if (sizeof(T) > data.size() - pos) return false;
        using U = std::make_unsigned_t<T>;
        U v = 0;
        for (size_t i = 0; i < sizeof(T); ++i) v |= U(data[pos++]) << (i * 8);
        *out = static_cast<T>(v);
        return true;
    }
};
bool Cells(const Grid& g, uint64_t* cells)
{
    if (g.rect[2] < 0 || g.rect[3] < 0) return false;
    *cells = uint64_t(g.rect[2]) * uint64_t(g.rect[3]);
    if (*cells > MaxBytes * 8) return false;
    for (int i = 0; i < 2; ++i) {
        int64_t edge = int64_t(g.rect[i]) + g.rect[i + 2];
        if (edge > INT32_MAX || edge < INT32_MIN) return false;
    }
    return true;
}
bool ValidTerrain(const Terrain& t)
{
    uint64_t h, m, k;
    if (!Cells(t.height, &h) || !Cells(t.material, &m) || !Cells(t.mask, &k)) return false;
    if (t.height.data.size() != h * 8 || t.material.data.size() != m ||
        k != t.bits || t.mask.data.size() != ((k + 31) / 32) * 4) return false;
    return t.height.data.size() + t.material.data.size() + t.mask.data.size() <= MaxBytes &&
           (h || m || k);
}
bool ValidString(const std::string& s, bool required)
{
    return (!required || !s.empty()) && s.size() <= MaxAssetString && s.find('\0') == std::string::npos;
}
}

std::string Base64(const std::vector<uint8_t>& bytes)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(4 * ((bytes.size() + 2) / 3));
    for (size_t i = 0; i < bytes.size(); i += 3) {
        uint32_t v = uint32_t(bytes[i]) << 16;
        if (i + 1 < bytes.size()) v |= uint32_t(bytes[i + 1]) << 8;
        if (i + 2 < bytes.size()) v |= bytes[i + 2];
        out += alphabet[(v >> 18) & 63]; out += alphabet[(v >> 12) & 63];
        out += i + 1 < bytes.size() ? alphabet[(v >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? alphabet[v & 63] : '=';
    }
    return out;
}
bool Unbase64(const std::string& text, std::vector<uint8_t>* out)
{
    out->clear();
    unsigned q[4]; size_t count = 0; bool padded = false;
    for (unsigned char c : text) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (padded) return false;
        unsigned v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=') v = 64;
        else return false;
        q[count++] = v;
        if (count != 4) continue;
        count = 0;
        if (q[0] >= 64 || q[1] >= 64 || (q[2] == 64 && q[3] != 64)) return false;
        if (q[2] == 64 && (q[1] & 15)) return false;
        if (q[3] == 64 && q[2] != 64 && (q[2] & 3)) return false;
        out->push_back(uint8_t((q[0] << 2) | (q[1] >> 4)));
        if (q[2] != 64) out->push_back(uint8_t((q[1] << 4) | (q[2] >> 2)));
        if (q[3] != 64) out->push_back(uint8_t((q[2] << 6) | q[3]));
        padded = q[3] == 64;
    }
    return count == 0;
}
bool EncodeTerrain(const Terrain& terrain, std::vector<uint8_t>* out)
{
    out->clear();
    if (!ValidTerrain(terrain)) return false;
    Bytes(*out, "TPTG", 4); Put(*out, uint32_t(1));
    out->resize(8 + 0x80 + 0x70, 0); // synthesized Windows tail; no process pointers
    const Grid* grids[] = {&terrain.height, &terrain.material, &terrain.mask};
    for (unsigned i = 0; i < 3; ++i) {
        std::memcpy(out->data() + 8 + i * 0x28, grids[i]->rect.data(), 16);
    }
    std::memcpy(out->data() + 8 + 0x78, &terrain.bits, 8);
    for (const Grid* grid : grids) {
        Put(*out, uint64_t(grid->data.size()));
        Bytes(*out, grid->data.data(), grid->data.size());
    }
    // Normalize padding bits, including an odd final u32 from a Linux u64 word.
    if (terrain.bits % 32) {
        const size_t at = out->size() - 4;
        uint32_t word;
        std::memcpy(&word, out->data() + at, 4);
        word &= (uint32_t(1) << (terrain.bits % 32)) - 1;
        std::memcpy(out->data() + at, &word, 4);
    }
    return true;
}
bool DecodeTerrain(const std::vector<uint8_t>& bytes, Terrain* out)
{
    *out = {};
    if (bytes.size() < 8 + 0x80 + 0x70 + 24 || bytes.size() > MaxBytes + 512) return false;
    Reader r{bytes}; char magic[4]; uint32_t version;
    if (!r.take(magic, 4) || std::memcmp(magic, "TPTG", 4) || !r.get(&version) || version != 1) return false;
    Grid* grids[] = {&out->height, &out->material, &out->mask};
    for (unsigned i = 0; i < 3; ++i) std::memcpy(grids[i]->rect.data(), bytes.data() + 8 + i * 0x28, 16);
    std::memcpy(&out->bits, bytes.data() + 8 + 0x78, 8);
    r.pos = 8 + 0x80 + 0x70;
    for (Grid* grid : grids) {
        uint64_t n;
        if (!r.get(&n) || n > MaxBytes || n > bytes.size() - r.pos) return false;
        grid->data.resize(n);
        if (!r.take(grid->data.data(), n)) return false;
    }
    return r.pos == bytes.size() && ValidTerrain(*out);
}
bool EncodeAssets(const Assets& assets, std::vector<uint8_t>* out)
{
    out->clear();
    if (assets.groups.size() > MaxAssetGroups) return false;
    if (assets.groups.empty() && !assets.originalRemovals) return false;
    Bytes(*out, "TPAS", 4); Put(*out, uint32_t(2));
    Put(*out, uint32_t(assets.groups.size())); Put(*out, assets.originalRemovals);
    size_t totalModels = 0;
    for (const auto& group : assets.groups) {
        if (group.empty() || group.size() > MaxAssetModels || (totalModels += group.size()) > MaxTotalModels) return false;
        Put(*out, uint32_t(group.size()));
        for (const Model& model : group) {
            if (!ValidString(model.model, true) || !ValidString(model.extra, false)) return false;
            for (float f : model.matrix) if (!std::isfinite(f)) return false;
            Put(*out, uint32_t(model.model.size())); Bytes(*out, model.model.data(), model.model.size());
            Put(*out, uint32_t(model.extra.size())); Bytes(*out, model.extra.data(), model.extra.size());
            Bytes(*out, model.matrix.data(), 64);
        }
    }
    return true;
}
bool DecodeAssets(const std::vector<uint8_t>& bytes, Assets* out)
{
    *out = {};
    Reader r{bytes}; char magic[4]; uint32_t version, count;
    if (!r.take(magic, 4) || std::memcmp(magic, "TPAS", 4) || !r.get(&version) || version != 2 ||
        !r.get(&count) || count > MaxAssetGroups || !r.get(&out->originalRemovals) ||
        (!count && !out->originalRemovals)) return false;
    size_t totalModels = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t n;
        if (!r.get(&n) || !n || n > MaxAssetModels || (totalModels += n) > MaxTotalModels || n > (bytes.size() - r.pos) / 73) return false;
        std::vector<Model> group(n);
        for (Model& m : group) {
            for (std::string* s : {&m.model, &m.extra}) {
                uint32_t len;
                if (!r.get(&len) || len > MaxAssetString || len > bytes.size() - r.pos) return false;
                s->resize(len);
                if (!r.take(s->data(), len) || !ValidString(*s, s == &m.model)) return false;
            }
            if (!r.take(m.matrix.data(), 64)) return false;
            for (float f : m.matrix) if (!std::isfinite(f)) return false;
        }
        out->groups.push_back(std::move(group));
    }
    return r.pos == bytes.size();
}
bool ParseAssetsFile(const std::string& text, Assets* out)
{
    const size_t nl = text.find('\n');
    if (text.compare(0, 3, "rm ") || nl == std::string::npos) return false;
    size_t end = nl;
    while (end > 3 && (text[end - 1] == '\r' || text[end - 1] == ' ')) --end;
    std::vector<int32_t> ids;
    if (!(end == 4 && text[3] == '-')) {
        size_t i = 3;
        if (i == end) return false;
        while (i < end) {
            if (text[i] < '0' || text[i] > '9') return false;
            uint64_t id = 0;
            while (i < end && text[i] >= '0' && text[i] <= '9') {
                id = id * 10 + unsigned(text[i++] - '0');
                if (id > INT32_MAX) return false;
            }
            if (!id) return false;
            ids.push_back(int32_t(id));
            if (i == end) break;
            if (text[i++] != ',' || i == end) return false;
        }
    }
    std::vector<uint8_t> raw;
    if (!Unbase64(text.substr(nl + 1), &raw) || !DecodeAssets(raw, out)) return false;
    // Local position resolution may legitimately find fewer IDs than the source.
    if (ids.size() > out->originalRemovals) return false;
    out->removals = std::move(ids);
    return true;
}
}
