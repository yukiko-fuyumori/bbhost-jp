#undef NDEBUG
#include "host/launcher_ui.h"
#include "core/utf8.h"
#include "imgui.h"
#include <cassert>
#include <cmath>
#include <string>

int main() {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280, 720);
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    launcher_ui::init_ingame(1.0f);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(10, 10));
    ImGui::SetNextWindowSize(ImVec2(600, 400));
    ImGui::Begin("日本語メニュー");
    const char* text = "日本語の設定";
    auto* font = launcher_ui::font_strong();
    const float size = ImGui::GetFontSize();
    const float width = font->CalcTextSizeA(size, 1000, 0, text).x;
    assert(std::abs(launcher_ui::spaced_width(font, size, text, 0) - width) < 0.1f);
    assert(std::abs(launcher_ui::spaced_width(font, size, text, 2) - width - 10) < 0.1f);
    for (unsigned cp : {0x65e5u, 0x672cu, 0x8a9eu})
        assert(font->GetFontBaked(size)->FindGlyphNoFallback(cp));

    auto* draw = ImGui::GetWindowDrawList();
    const int before = draw->VtxBuffer.Size;
    launcher_ui::spaced_text(draw, font, size, ImGui::GetCursorScreenPos(), IM_COL32_WHITE, text, 2);
    assert(draw->VtxBuffer.Size - before == 6 * 4);
    for (float limit : {40.f, 70.f, 100.f}) {
        for (const auto& clipped : {launcher_ui::fit_text(text, limit), launcher_ui::fit_left(text, limit)}) {
            const char* p = clipped.c_str();
            while (*p) assert(utf8_next(p) != 0xfffd);
        }
    }
    launcher_ui::avatar("狩人", 40);
    ImGui::End();
    ImGui::EndFrame();
    launcher_ui::shutdown();
    ImGui::DestroyContext();
}
