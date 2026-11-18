#pragma once
#include <cstring>
// Display labels only. Saved setting values stay compatible with upstream.
inline const char* host_japanese_value(const char* value) {
    if (std::strcmp(value, "Windowed") == 0) return "ウィンドウ";
    if (std::strcmp(value, "Borderless") == 0) return "ボーダーレス";
    if (std::strcmp(value, "Fullscreen") == 0) return "全画面";
    if (std::strcmp(value, "Off") == 0) return "無効";
    if (std::strcmp(value, "On") == 0) return "有効";
    if (std::strcmp(value, "Display-Limited") == 0) return "画面に合わせる";
    if (std::strcmp(value, "Original") == 0) return "原作";
    if (std::strcmp(value, "Auto") == 0) return "自動";
    if (std::strcmp(value, "Keyboard") == 0) return "キーボード";
    if (std::strcmp(value, "Controller") == 0) return "コントローラー";
    if (std::strcmp(value, "Lowest") == 0) return "最低";
    if (std::strcmp(value, "Low") == 0) return "低";
    if (std::strcmp(value, "Default") == 0) return "標準";
    if (std::strcmp(value, "High") == 0) return "高";
    if (std::strcmp(value, "Ultra") == 0) return "最高";
    if (std::strcmp(value, "Giga") == 0) return "超高";
    if (std::strcmp(value, "Normal") == 0) return "通常";
    if (std::strcmp(value, "Full") == 0) return "完全";
    if (std::strcmp(value, "Medium") == 0) return "中";
    if (std::strcmp(value, "Game") == 0) return "ゲーム";
    if (std::strcmp(value, "Quarter") == 0) return "４分の１";
    if (std::strcmp(value, "Half") == 0) return "半分";
    if (std::strcmp(value, "Performance") == 0) return "速度優先";
    if (std::strcmp(value, "Balanced") == 0) return "均衡";
    if (std::strcmp(value, "Quality") == 0) return "品質優先";
    if (std::strcmp(value, "Native AA") == 0) return "等倍 AA";
    if (std::strcmp(value, "Velomap") == 0) return "速度マップ";
    if (std::strcmp(value, "Rebuilt 8-bit") == 0) return "８ビットで再構築";
    if (std::strcmp(value, "Rebuilt 16-bit") == 0) return "16ビットで再構築";
    if (std::strcmp(value, "Rebuilt 32-bit") == 0) return "32ビットで再構築";
    if (std::strcmp(value, "Precise") == 0) return "精密";
    if (std::strcmp(value, "Lifted") == 0) return "変換済み SPIR-V";

    if (!std::strcmp(value, "Windowed")) return "ウィンドウ";
    if (!std::strcmp(value, "Fullscreen")) return "全画面";
    if (!std::strcmp(value, "On")) return "有効";
    if (!std::strcmp(value, "Off")) return "無効";
    if (!std::strcmp(value, "Uncapped")) return "無制限";
    if (!std::strcmp(value, "Display-Limited")) return "画面に合わせる";
    if (!std::strcmp(value, "Original")) return "原作";
    if (!std::strcmp(value, "Auto")) return "自動";
    if (!std::strcmp(value, "Keyboard")) return "キーボード";
    if (!std::strcmp(value, "Controller")) return "コントローラー";
    if (!std::strcmp(value, "Full")) return "最高";
    if (!std::strcmp(value, "Normal")) return "標準";
    if (!std::strcmp(value, "Low")) return "低";
    if (!std::strcmp(value, "Lowest")) return "最低";
    if (!std::strcmp(value, "Lifted")) return "変換済み SPIR-V";
    if (!std::strcmp(value, "Smooth")) return "なめらか";
    if (!std::strcmp(value, "Original (30)")) return "原作（毎秒30回）";
    if (!std::strcmp(value, "60 a second")) return "毎秒60回";
    if (!std::strcmp(value, "Your frame rate")) return "フレームレートに合わせる";
    return value;
}
