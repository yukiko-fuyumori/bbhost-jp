#include "core/utf8.h"
#include "host/overlay_font.h"
#include "host/japanese_font.h"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "../../third_party/imgui/imstb_truetype.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <stdexcept>
namespace overlay_font {
namespace {
std::once_flag ready;
std::vector<std::uint8_t> pixels;
std::array<Glyph, 65536> glyphs{};
void build() {
    stbtt_fontinfo font{};
    if (!stbtt_InitFont(&font, kJapaneseFont, 0)) throw std::runtime_error("Invalid embedded M+ font");
    pixels.resize(kAtlasW * kAtlasH);
    const float scale = stbtt_ScaleForMappingEmToPixels(&font, 24.0f);
    int idx = 0;
    for (unsigned cp = 32; cp < glyphs.size(); ++cp) {
        if (!stbtt_FindGlyphIndex(&font, cp)) continue;
        if ((idx / kCols + 1) * kCellH >= kAtlasH) throw std::runtime_error("Japanese font atlas overflow");
        int x0, y0, x1, y1, advance, bearing;
        stbtt_GetCodepointBitmapBox(&font, cp, scale, scale, &x0, &y0, &x1, &y1);
        stbtt_GetCodepointHMetrics(&font, cp, &advance, &bearing);
        glyphs[cp] = {idx + 1, advance * scale};
        const int w = x1 - x0, h = y1 - y0;
        if (w > 0 && h > 0) {
            std::vector<unsigned char> bitmap(w * h);
            stbtt_MakeCodepointBitmap(&font, bitmap.data(), w, h, w, scale, scale, cp);
            const int cell_x = (idx % kCols) * kCellW, cell_y = (idx / kCols) * kCellH;
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                const int dx = kOriginX + x0 + x, dy = kOriginY + 22 + y0 + y;
                if (dx > 0 && dx < kCellW - 1 && dy > 0 && dy < kCellH - 1)
                    pixels[(cell_y + dy) * kAtlasW + cell_x + dx] = bitmap[y * w + x];
            }
        }
        ++idx;
    }
    pixels[kSolidY * kAtlasW + kSolidX] = 255;
}
}
const std::vector<std::uint8_t>& atlas() { std::call_once(ready, build); return pixels; }
Glyph glyph(std::uint32_t cp) {
    atlas();
    Glyph g = cp < glyphs.size() ? glyphs[cp] : Glyph{};
    if (!g.index) g = glyphs['?'];
    --g.index;
    return g;
}
std::uint32_t next(const char*& p) { return utf8_next(p); }
float width(float scale, const char* text) {
    float line = 0, widest = 0;
    if (!text) return 0;
    while (*text) {
        const auto cp = next(text);
        if (cp == '\n') { widest = std::max(widest, line); line = 0; }
        else line += glyph(cp).advance * scale;
    }
    return std::max(widest, line);
}
std::string wrap(float scale, float max_width, const char* text) {
    std::string out;
    if (!text) return out;
    float line = 0;
    while (*text) {
        const char* start = text;
        const auto cp = next(text);
        if (cp == '\n') {
            out += '\n';
            line = 0;
            continue;
        }
        const float advance = glyph(cp).advance * scale;
        if (line > 0 && line + advance > max_width) {
            out += '\n';
            line = 0;
        }
        out.append(start, text - start);
        line += advance;
    }
    return out;
}
}
