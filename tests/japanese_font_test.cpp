#undef NDEBUG
#include "host/overlay_font.h"
#include "core/utf8.h"
#include <cassert>
#include <cmath>
int main() {
    const char* text = "日本語🎮";
    assert(utf8_next(text) == 0x65e5);
    assert(utf8_next(text) == 0x672c);
    assert(utf8_next(text) == 0x8a9e);
    assert(utf8_next(text) == 0x1f3ae);
    assert(utf8_next(text) == 0);
    for (const char* bad : {"\xff", "\xe3", "\xe3\x81", "\xed\xa0\x80", "\xf4\x90\x80\x80"}) {
        const char* p = bad;
        assert(utf8_next(p) == 0xfffd);
        assert(p > bad);
    }
    const auto& atlas = overlay_font::atlas();
    assert(atlas.size() == overlay_font::kAtlasW * overlay_font::kAtlasH);
    const auto unknown = overlay_font::glyph('?');
    for (unsigned cp : {0x65e5, 0x672c, 0x8a9e, 0x8a2d, 0x5b9a, 0x56de, 0x907f, 0x30d7}) {
        const auto g = overlay_font::glyph(cp);
        assert(g.index != unknown.index && g.advance > 0);
        bool ink = false;
        const int x0 = (g.index % overlay_font::kCols) * overlay_font::kCellW;
        const int y0 = (g.index / overlay_font::kCols) * overlay_font::kCellH;
        for (int y = 0; y < overlay_font::kCellH; ++y)
            for (int x = 0; x < overlay_font::kCellW; ++x)
                ink |= atlas[(y0+y)*overlay_font::kAtlasW+x0+x] != 0;
        assert(ink);
    }
    assert(std::abs(overlay_font::width(1, "日本語") - 72) < 1);
    assert(std::abs(overlay_font::width(1, "日本\n語") - 48) < 1);
    assert(overlay_font::wrap(1, 48, "日本語") == "日本\n語");
    assert(overlay_font::wrap(1, 48, "日本\n語") == "日本\n語");
    assert(overlay_font::wrap(1, 24, "日A本") == "日\nA\n本");
}
