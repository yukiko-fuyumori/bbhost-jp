#include "host/play_history_ui.h"

#if defined(BBHOST_HAVE_SDL3)
#include "imgui.h"
#include <algorithm>

namespace {
std::string label(const play_history::Character& c) {
    return "スロット " + std::to_string(c.slot + 1) + "：" + (c.name.empty() ? "キャラクター" : c.name) +
           " （記録 " + std::to_string(c.id) + "）";
}
void chalice_records(const PlayHistoryUi& ui, const play_history::Character& c, int cycle) {
    const auto& state = ui.chalice_state;
    if (!state.error.empty()) ImGui::TextWrapped("%s", state.error.c_str());
    ImGui::SeparatorText(ui.cumulative ? "聖杯ボス：全周回の累計" : "聖杯ボス：選択した周回");
    const auto rows = chalice_history::totals(state, c.id, ui.cumulative ? -1 : cycle);
    if (rows.empty()) ImGui::TextWrapped("この周回の聖杯ボスの記録はまだありません。聖杯ダンジョンに入ると、ダンジョンと階層ごとに記録します。");
    if (!rows.empty() && ImGui::BeginTable("##chalice-totals", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("聖杯ボス"); ImGui::TableSetupColumn("挑戦"); ImGui::TableSetupColumn("記録した撃破"); ImGui::TableHeadersRow();
        for (const auto& row : rows) {
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextWrapped("%s", chalice_history::boss_name(row.name_id).c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%llu%s", static_cast<unsigned long long>(row.count.attempts), row.count.partial ? " 以上" : "");
            ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(row.count.kills));
        }
        ImGui::EndTable();
    }
    if (ImGui::CollapsingHeader("ダンジョン・階層ごとの記録", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const auto& e : state.encounters) if (e.character == c.id) {
            play_history::BossRecord total{}; bool shown = ui.cumulative && e.prior_clear;
            for (const auto& s : e.stats) if (ui.cumulative || s.cycle == cycle) {
                total.attempts += s.count.attempts; total.kills += s.count.kills; total.partial = total.partial || s.count.partial; shown = true;
            }
            if (!shown) continue;
            ImGui::PushID(static_cast<int>(e.id));
            ImGui::Separator();
            const auto place = e.altar ? "祭壇 " + std::to_string(e.altar) : std::string("簡易祭壇");
            ImGui::TextWrapped("%s：%s・第%d層（記録 %llu）", chalice_history::boss_name(e.name_id).c_str(),
                place.c_str(), e.layer, static_cast<unsigned long long>(e.id));
            if (e.prior_clear) ImGui::TextWrapped("記録開始前に撃破済み。撃破した周回が不明なため、周回別の撃破数には加算しません。");
            else ImGui::Text("挑戦 %llu%s・撃破 %llu%s", static_cast<unsigned long long>(total.attempts), total.partial ? " 以上" : "",
                static_cast<unsigned long long>(total.kills), e.active ? "・挑戦中" : "");
            ImGui::PopID();
        }
    }
    ImGui::Spacing();
    ImGui::TextWrapped("聖杯の記録と累計は、本編・DLC に混ぜません。同じボスでもダンジョンと階層を区別します。NG+ に移っても聖杯の撃破状況は引き継がれるため、同じ撃破を次の周回に再加算しません。");
    ImGui::TextWrapped("固定聖杯・汎聖杯のボスに対応します。記録開始前の挑戦数は不明なため、最初の記録は「以上」と表示します。ボスラッシュ・練習中と他人の世界では記録しません。");
}
}
void play_history_ui_draw(PlayHistoryUi& ui, const std::string& data_dir, bool live) {
    const auto now = std::chrono::steady_clock::now();
    if (ui.directory != data_dir || now - ui.refreshed >= std::chrono::seconds(1)) {
        ui.directory = data_dir; ui.refreshed = now;
        ui.state = live ? play_history::runtime().snapshot() : play_history::read(std::filesystem::u8path(data_dir) / "play-history.txt");
        ui.chalice_state = live ? chalice_history::runtime().snapshot() : chalice_history::read(std::filesystem::u8path(data_dir) / "chalice-history.txt");
    }
    ImGui::BeginChild("##play-history");
    ImGui::TextWrapped("キャラクターごとに NG・NG+ のプレイ時間と、各ボスの挑戦回数・撃破数を記録します。");
    if (!ui.state.error.empty()) ImGui::TextWrapped("%s", ui.state.error.c_str());
    if (ui.state.characters.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("ゲームを開始すると自動的に記録します。記録はデータフォルダーの play-history.txt に保存され、次回起動時も引き継がれます。");
        ImGui::EndChild(); return;
    }
    auto found = std::find_if(ui.state.characters.begin(), ui.state.characters.end(), [&](const auto& c) { return c.id == ui.character; });
    if (found == ui.state.characters.end()) { found = ui.state.characters.end() - 1; ui.character = found->id; ui.cycle = -1; }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##character", label(*found).c_str())) {
        for (const auto& c : ui.state.characters) {
            if (ImGui::Selectable(label(c).c_str(), c.id == ui.character)) { ui.character = c.id; ui.cycle = -1; }
        }
        ImGui::EndCombo();
    }
    // A selection may have changed above.
    const auto& c = *std::find_if(ui.state.characters.begin(), ui.state.characters.end(), [&](const auto& item) { return item.id == ui.character; });
    if (c.cycles.empty()) { ImGui::EndChild(); return; }
    if (ui.cycle < 0 || ui.cycle >= static_cast<int>(c.cycles.size())) ui.cycle = static_cast<int>(c.cycles.size()) - 1;
    ImGui::SeparatorText("周回ごとのプレイ時間");
    if (ImGui::BeginTable("##cycles", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("周回"); ImGui::TableSetupColumn("プレイ時間"); ImGui::TableSetupColumn("状態"); ImGui::TableHeadersRow();
        for (int i = 0; i < static_cast<int>(c.cycles.size()); ++i) {
            const auto& cy = c.cycles[i];
            ImGui::PushID(i); ImGui::TableNextRow(); ImGui::TableNextColumn();
            if (ImGui::Selectable(play_history::cycle_name(cy.number).c_str(), ui.cycle == i, ImGuiSelectableFlags_SpanAllColumns)) {
                ui.cycle = i; ui.cumulative = false;
            }
            ImGui::TableNextColumn();
            ImGui::Text("%s%s", play_history::duration(cy.elapsed_ms).c_str(), cy.partial_time ? " 以上" : "");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(cy.complete ? "終了" : "未完了"); ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::Checkbox("全周回の累計を表示", &ui.cumulative);
    const auto& selected = c.cycles[ui.cycle];
    if (ImGui::RadioButton("本編・DLC", !ui.chalice)) ui.chalice = false;
    ImGui::SameLine();
    if (ImGui::RadioButton("聖杯ダンジョン", ui.chalice)) ui.chalice = true;
    if (ui.chalice) {
        chalice_records(ui, c, selected.number);
        ImGui::EndChild(); return;
    }
    const auto records = ui.cumulative ? play_history::totals(c) : selected.bosses;
    ImGui::SeparatorText(ui.cumulative ? "ボス戦：このキャラクターの全周回累計" : play_history::cycle_name(selected.number).c_str());
    if (ImGui::BeginTable("##boss-records", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("ボス", ImGuiTableColumnFlags_WidthStretch, 2.4f);
        ImGui::TableSetupColumn("挑戦", ImGuiTableColumnFlags_WidthStretch, 1.f);
        ImGui::TableSetupColumn("撃破", ImGuiTableColumnFlags_WidthStretch, .6f);
        ImGui::TableSetupColumn("結果", ImGuiTableColumnFlags_WidthStretch, 1.6f); ImGui::TableHeadersRow();
        for (int i = 0; i < play_history::kBossCount; ++i) {
            const auto& b = records[i];
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextWrapped("%s", play_history::bosses()[i].name);
            ImGui::TableNextColumn();
            if (b.partial && !b.attempts) ImGui::TextUnformatted("不明");
            else ImGui::Text("%llu%s", static_cast<unsigned long long>(b.attempts), b.partial ? " 以上" : "");
            ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(b.kills));
            ImGui::TableNextColumn();
            const bool fighting = !ui.cumulative && ui.cycle == static_cast<int>(c.cycles.size()) - 1 && c.active_boss == i;
            ImGui::TextWrapped("%s", fighting ? "挑戦中" : b.kills ? (b.partial ? "撃破済み（一部不明）" : "撃破済み") : b.attempts ? "未撃破" : "未記録");
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::TextWrapped("挑戦は戦闘開始時に１回加算します。死亡・帰還・途中終了した挑戦も含み、撃破した挑戦を含む回数を表示します。撃破後はその周回の回数を確定します。");
    ImGui::TextWrapped("時間はゲーム内プレイ時間を使用し、次の NG+ に移ると前の周回を終了します。ボスラッシュ・練習中と他人の世界では記録を一時停止します。聖杯のボスは「聖杯ダンジョン」で別に集計します。");
    ImGui::TextWrapped("記録開始前の挑戦回数や NG+ の開始時刻は復元できません。「以上」「不明」はその区間の情報が不足していることを示します。セーブの巻き戻し・作り直しを検出すると、以前の記録を残して別の記録を作成します。");
    ImGui::EndChild();
}
#else
void play_history_ui_draw(PlayHistoryUi&, const std::string&, bool) {}
#endif
