#include "core/sfo.h"

#include <cstdio>
#include <cstring>
#include <vector>

bool sfo_read(const std::string& path, std::map<std::string, SfoValue>* out, std::string* error) {
    out->clear();
    std::FILE* f = path.empty() ? nullptr : std::fopen(path.c_str(), "rb");
    if (!f) {
        *error = "not found";
        return false;
    }
    std::vector<std::uint8_t> b;
    std::uint8_t buf[4096];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) {
        b.insert(b.end(), buf, buf + n);
        if (b.size() > 1024 * 1024) break;
    }
    const bool failed = std::ferror(f) || b.size() > 1024 * 1024;
    std::fclose(f);
    if (failed) { *error = "cannot read SFO (limit 1 MiB)"; return false; }
    auto u16 = [&](std::size_t o) -> std::uint32_t { return o + 2 <= b.size() ? b[o] | (b[o + 1] << 8) : 0; };
    auto u32 = [&](std::size_t o) {
        std::uint32_t v = 0;
        if (o + 4 <= b.size()) std::memcpy(&v, &b[o], 4);
        return v;
    };
    if (b.size() < 20 || std::memcmp(b.data(), "\0PSF", 4) != 0) {
        *error = "not an SFO";
        return false;
    }
    // Header: magic, version, key table, data table, entry count; then 16-byte
    // entries (key offset u16, format u16, length, capacity, data offset).
    const std::uint32_t keys = u32(8), data = u32(12), count = u32(16);
    const auto invalid = [&] { out->clear(); *error = "malformed SFO"; return false; };
    if (count > (b.size() - 20) / 16 || keys < 20 + count * 16 ||
        data < keys || data > b.size()) return invalid();
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t e = 20 + i * 16;
        const std::size_t k = static_cast<std::size_t>(keys) + u16(e);
        const std::uint32_t fmt = u16(e + 2), len = u32(e + 4), cap = u32(e + 8);
        const std::size_t d = static_cast<std::size_t>(data) + u32(e + 12);
        if (k >= data || d > b.size() || cap > b.size() - d || len > cap || !len) return invalid();
        const void* nul = std::memchr(&b[k], 0, data - k);
        if (!nul) return invalid();
        const std::string key(reinterpret_cast<const char*>(&b[k]), static_cast<const std::uint8_t*>(nul) - &b[k]);
        if (out->count(key)) return invalid();
        SfoValue v;
        if (fmt == 0x0404) {  // integer
            if (len != 4) return invalid();
            v.is_int = true;
            v.num = u32(d);
        } else if (fmt == 0x0204 || fmt == 0x0004) {  // UTF-8 text or raw bytes
            const char* s = reinterpret_cast<const char*>(&b[d]);
            const void* z = std::memchr(s, 0, len);
            v.text.assign(s, z ? static_cast<std::size_t>(static_cast<const char*>(z) - s) : len);
        } else return invalid();
        (*out)[key] = std::move(v);
    }
    return true;
}
