#pragma once
#include <cstring>
// Display labels only. Saved setting values stay compatible with upstream.
inline const char* host_japanese_value(const char* value) {
    if (!std::strcmp(value, "Windowed")) return "ウィンドウ";
    if (!std::strcmp(value, "Fullscreen")) return "全画面";
    if (!std::strcmp(value, "On")) return "有効";
    if (!std::strcmp(value, "Off")) return "無効";
    if (!std::strcmp(value, "Uncapped")) return "無制限";
    if (!std::strcmp(value, "Original")) return "原作";
    if (!std::strcmp(value, "Auto")) return "自動";
    if (!std::strcmp(value, "Keyboard")) return "キーボード";
    if (!std::strcmp(value, "Controller")) return "コントローラー";
    if (!std::strcmp(value, "Full")) return "最高";
    if (!std::strcmp(value, "Normal")) return "標準";
    if (!std::strcmp(value, "Low")) return "低";
    if (!std::strcmp(value, "Lowest")) return "最低";
    if (!std::strcmp(value, "Lifted")) return "変換済み SPIR-V";
    return value;
}
