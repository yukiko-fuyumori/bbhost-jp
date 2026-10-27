// SPDX-License-Identifier: GPL-3.0-or-later
// Trophy IDs, grades, hidden flags and platinum membership checked against
// ayozetr/bloodborne_linux scripts/trophies.py (804651e4, GPL-2.0-or-later).
// Storage and UI integration are native bbhost code; Japanese descriptions are
// written for this fork. No game assets or trophy decryption keys are bundled.
#include "core/trophies.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace trophies {
namespace {
using G = Grade;
const std::array<Info, kCount> kCatalog{{
    {G::Platinum, false, "Bloodborne", "本編のトロフィーをすべて獲得する。"},
    {G::Gold, true, "ヤーナムの夜明け", "狩りの夜を終え、朝を迎える。"},
    {G::Gold, true, "遺志を継ぐ者", "狩人の夢を見守る役目を引き継ぐ。"},
    {G::Gold, true, "幼年期のはじまり", "新たな上位者として目覚める。"},
    {G::Gold, true, "トゥメルの女王、ヤーナム", "古い地下遺跡の女王ヤーナムを倒す。"},
    {G::Gold, false, "狩人の極意", "本編の狩人武器をすべて入手する。"},
    {G::Gold, false, "狩人の秘術", "本編の特殊な狩人道具をすべて入手する。"},
    {G::Silver, false, "武器の極致", "武器を最高段階まで強化する。"},
    {G::Silver, false, "血晶の極意", "非常に貴重な血晶石を入手する。"},
    {G::Silver, false, "カレル文字の極意", "非常に貴重なカレル文字を入手する。"},
    {G::Silver, true, "カインハースト", "廃城カインハーストに到達する。"},
    {G::Silver, true, "聖歌隊", "医療教会の上位に位置する聖歌隊の領域へ到達する。"},
    {G::Silver, true, "捨てられた古工房", "狩人の夢の原型となった古工房を発見する。"},
    {G::Silver, true, "悪夢の教室", "悪夢の中の教室棟へ到達する。"},
    {G::Bronze, true, "ガスコイン神父", "ガスコイン神父を倒す。"},
    {G::Bronze, true, "教区長エミーリア", "教区長エミーリアを倒す。"},
    {G::Bronze, true, "ヤーナムの影", "ヤーナムの影を倒す。"},
    {G::Bronze, true, "白痴の蜘蛛、ロマ", "上位者ロマを倒す。"},
    {G::Bronze, true, "再誕者", "再誕者を倒す。"},
    {G::Bronze, true, "悪夢の主、ミコラーシュ", "悪夢の主ミコラーシュを倒す。"},
    {G::Bronze, true, "メルゴーの乳母", "上位者メルゴーの乳母を倒す。"},
    {G::Bronze, true, "聖職者の獣", "聖職者の獣を倒す。"},
    {G::Bronze, true, "血に渇いた獣", "血に渇いた獣を倒す。"},
    {G::Bronze, true, "ヘムウィックの魔女", "ヘムウィックの魔女を倒す。"},
    {G::Bronze, true, "黒獣パール", "黒獣パールを倒す。"},
    {G::Bronze, true, "アメンドーズ", "上位者アメンドーズを倒す。"},
    {G::Bronze, true, "殉教者ローゲリウス", "殉教者ローゲリウスを倒す。"},
    {G::Bronze, true, "星界からの使者", "星界からの使者を倒す。"},
    {G::Bronze, true, "星の娘、エーブリエタース", "上位者エーブリエタースを倒す。"},
    {G::Bronze, false, "血晶石", "武器を強化する血晶石を初めて入手する。"},
    {G::Bronze, false, "カレル文字", "狩人に力を与えるカレル文字を初めて入手する。"},
    {G::Bronze, false, "トゥメルの聖杯", "トゥメルの地下遺跡へ通じる聖杯を入手する。"},
    {G::Bronze, true, "病めるローランの聖杯", "ローランの地下遺跡へ通じる聖杯を入手する。"},
    {G::Bronze, true, "イズの大聖杯", "イズの地下遺跡へ通じる大聖杯を入手する。"},
    {G::Gold, false, "古狩人の極意", "追加コンテンツの古狩人武器をすべて入手する。"},
    {G::Silver, true, "ゴースの遺子", "ゴースの遺子を倒す。"},
    {G::Bronze, true, "聖剣のルドウイーク", "ルドウイークを倒す。"},
    {G::Bronze, true, "時計塔のマリア", "時計塔のマリアを倒す。"},
    {G::Bronze, true, "失敗作たち", "失敗作たちを倒す。"},
    {G::Bronze, true, "初代教区長ローレンス", "初代教区長ローレンスを倒す。"},
}};
constexpr std::int64_t kLatestTimestamp = 253402300799ll;  // end of year 9999

Snapshot load(const std::filesystem::path& file, std::string* original) {
    Snapshot result;
    original->clear();
    std::error_code ec;
    const bool exists = std::filesystem::exists(file, ec);
    if (ec || (exists && (!std::filesystem::is_regular_file(file, ec) || ec))) {
        result.error = "トロフィー記録を読み込めません。保存先を確認してください。";
        return result;
    }
    if (!exists) return result;
    if (std::filesystem::file_size(file, ec) > 1024 * 1024 || ec) {
        result.error = "トロフィー記録が大きすぎるか、読み取れません。";
        return result;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) { result.error = "トロフィー記録を開けません。"; return result; }
    *original = std::string(std::istreambuf_iterator<char>(in), {});
    if (in.bad()) { result.error = "トロフィー記録の読み込みに失敗しました。"; return result; }
    std::istringstream lines(*original);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream entry(line);
        int id; std::int64_t when;
        if (entry >> id >> when && id >= 0 && id < kCount && when > 0 && when <= kLatestTimestamp && !result.earned[id])
            result.earned[id] = when;
    }
    return result;
}

bool persist(const std::filesystem::path& file, const std::string& text) {
    if (file.empty()) return false;
    std::error_code ec;
    if (!file.parent_path().empty()) std::filesystem::create_directories(file.parent_path(), ec);
    if (ec) return false;
    if (std::filesystem::exists(file, ec)) {
        auto backup = file; backup += ".pre-trophies.bak";
        std::filesystem::copy_file(file, backup, std::filesystem::copy_options::skip_existing, ec);
        if (ec) return false;
    }
    auto temp = file;
    temp += ".tmp-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
#if defined(_WIN32)
    std::FILE* f = _wfopen(temp.c_str(), L"wb");
#else
    std::FILE* f = std::fopen(temp.c_str(), "wb");
#endif
    if (!f) return false;
    bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size() && std::fflush(f) == 0;
#if defined(_WIN32)
    if (ok) ok = _commit(_fileno(f)) == 0;
#else
    if (ok) ok = fdatasync(fileno(f)) == 0;
#endif
    ok = std::fclose(f) == 0 && ok;
    if (ok) {
#if defined(_WIN32)
        ok = MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        std::filesystem::rename(temp, file, ec);
        ok = !ec;
#endif
    }
    if (!ok) std::filesystem::remove(temp, ec);
    return ok;
}
bool platinum_due(const Snapshot& s) {
    return !s.earned[0] && std::all_of(s.earned.begin() + 1, s.earned.begin() + kBaseCount,
                                    [](auto when) { return when > 0; });
}
}  // namespace

const std::array<Info, kCount>& catalog() { return kCatalog; }
const char* grade_name(Grade g) {
    switch (g) {
    case Grade::Platinum: return "プラチナ";
    case Grade::Gold: return "ゴールド";
    case Grade::Silver: return "シルバー";
    default: return "ブロンズ";
    }
}
int Snapshot::count(int begin, int end) const {
    begin = std::clamp(begin, 0, kCount);
    end = std::clamp(end, begin, kCount);
    return std::count_if(earned.begin() + begin, earned.begin() + end,
                         [](auto t) { return t > 0; });
}
Snapshot read(const std::filesystem::path& file) { std::string text; return load(file, &text); }
void Store::open(const std::filesystem::path& file) {
    std::lock_guard lock(mutex_);
    file_ = file; notices_.clear(); state_ = read(file_);
}
Snapshot Store::snapshot() const { std::lock_guard lock(mutex_); return state_; }
Unlock Store::unlock(int id, std::int64_t when) {
    if (id < 0 || id >= kCount) return {Status::InvalidId};
    if (id == 0) return {Status::AutomaticPlatinum};
    return update(id, when);
}
Unlock Store::reconcile_platinum(std::int64_t when) { return update(-1, when); }
Unlock Store::update(int id, std::int64_t when) {
    std::lock_guard lock(mutex_);
    if (when <= 0 || when > kLatestTimestamp || file_.empty()) return {Status::IoError};
    std::string text;
    state_ = load(file_, &text);
    if (!state_.error.empty()) return {Status::IoError};
    if (id >= 0 && state_.earned[id]) return {Status::AlreadyUnlocked};
    Snapshot next = state_;
    if (id >= 0) next.earned[id] = when;
    const bool platinum = platinum_due(next);
    if (id < 0 && !platinum) return {Status::Ok};
    if (platinum) next.earned[0] = when;
    if (!text.empty() && text.back() != '\n') text += '\n';
    if (id >= 0) text += std::to_string(id) + " " + std::to_string(when) + "\n";
    if (platinum) text += "0 " + std::to_string(when) + "\n";
    if (!persist(file_, text)) {
        state_.error = "トロフィーを保存できません。空き容量と保存先の権限を確認してください。";
        return {Status::IoError};
    }
    state_ = next;
    if (id >= 0) notices_.push_back(id);
    if (platinum) notices_.push_back(0);
    return {Status::Ok, platinum ? 0 : -1};
}
int Store::pop_notice() {
    std::lock_guard lock(mutex_);
    if (notices_.empty()) return -1;
    const int id = notices_.front(); notices_.pop_front(); return id;
}
Store& runtime() { static Store store; return store; }
}  // namespace trophies
