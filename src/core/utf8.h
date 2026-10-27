#pragma once
#include <cstdint>
inline std::uint32_t utf8_next(const char*& p) {
    const auto lead = static_cast<unsigned char>(*p);
    if (!lead) return 0;
    ++p;
    if (lead < 0x80) return lead;
    const int extra = lead >= 0xc2 && lead <= 0xdf ? 1 : lead >= 0xe0 && lead <= 0xef ? 2 : lead >= 0xf0 && lead <= 0xf4 ? 3 : 0;
    if (!extra) return 0xfffd;
    std::uint32_t cp = lead & (0x7f >> extra);
    for (int i = 0; i < extra; ++i) {
        const auto b = static_cast<unsigned char>(*p);
        if ((b & 0xc0) != 0x80) return 0xfffd;
        cp = (cp << 6) | (b & 0x3f);
        ++p;
    }
    if (cp < (extra == 1 ? 0x80u : extra == 2 ? 0x800u : 0x10000u) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return 0xfffd;
    return cp;
}
