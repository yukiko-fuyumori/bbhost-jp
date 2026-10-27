#include "core/game_edition.h"
#include "core/sfo.h"

#include <map>

const char* game_edition_name(GameEdition edition) {
    return edition == GameEdition::JapaneseOldHunters
        ? "日本版 Old Hunters（CUSA03014）専用"
        : "欧州 GOTY 版（CUSA03173）専用・個人用 Linux";
}

std::string game_edition_error(const std::string& base, const std::string& update, GameEdition edition) {
    const bool jp = edition == GameEdition::JapaneseOldHunters;
    const char* title = jp ? "CUSA03014" : "CUSA03173";
    const unsigned region = jp ? 0 : 13;
    const std::string prefix = std::string("このビルドは ") + game_edition_name(edition) + " です。";
    const auto check = [&](const std::string& folder) -> std::string {
        const std::string path = folder + "/sce_sys/param.sfo";
        std::map<std::string, SfoValue> values;
        std::string error;
        if (folder.empty() || !sfo_read(path, &values, &error))
            return prefix + " ゲーム版を確認できません。正しい param.sfo が必要です: " + path;
        const auto t = values.find("TITLE_ID"), r = values.find("USER_DEFINED_PARAM_1");
        if (t == values.end() || r == values.end() || t->second.is_int || !r->second.is_int)
            return prefix + " ゲーム版の識別情報がありません: " + path;
        if (t->second.text != title || r->second.num != region)
            return prefix + " 別の版は起動できません（検出: " + t->second.text +
                   "、地域値 " + std::to_string(r->second.num) + "）: " + path;
        return {};
    };
    if (auto error = check(base); !error.empty()) return error;
    return update.empty() ? std::string() : check(update);
}
