#include "host/japanese_font.h"
// M+ 1M, embedded so setup and overlays work on a clean Windows installation.
// License and upstream font are in third_party/mplus/.
const unsigned char kJapaneseFont[] = {
#include "japanese_font.inc"
};
const std::size_t kJapaneseFontSize = sizeof(kJapaneseFont);
