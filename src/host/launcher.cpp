#define IMGUI_DEFINE_MATH_OPERATORS
#include "host/launcher.h"
#include "host/trophy_ui.h"
#include "host/play_history_ui.h"
#include "core/game_edition.h"

#include "bbhost_version.h"
#include "core/config.h"
#include "core/config_move.h"
#include "core/sha256.h"
#include "engine/addr.h"
#include "engine/patch_manifest.h"
#include "host/controls_page.h"
#include "host/launcher_ui.h"
#include "host/links.h"
#include "host/options.h"
#include "net/account.h"
#include "net/http.h"
#include "net/nat_examples.h"
#include "net/nat_job.h"
#include "host/plugin_ui.h"
#include "host/shader_compile.h"
#include "host/updater.h"
#include "log.h"

#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace ui = launcher_ui;

// The name the sidebar shows.
constexpr const char kName[] = "BBHOST";

// The online servers the window offers; anything else is "カスタム".
struct Server {
    const char* label;
    const char* host;
    const char* scheme;
    bool verify_tls, require_account;
    const char* auth_server;  // where /auth/* goes ("": the NP server's origin)
};
// The server no longer takes this PC's sign-in (net::account_refused, from
// the sign-in check): what the account places say, and the ways back in.
constexpr const char kRefusedNotice[] =
    "ログインの期限が切れたか、この PC がログアウトされました。Discord で再ログインするか、保存済みアカウントを復旧コードで復元してください。";
// Both take only https and signed-in players; the playtest server as the
// playtest kit's bbhost-playtest.toml has it.
constexpr Server kServers[] = {
    {"本番サーバー (thehuntersdream.com)", "thehuntersdream.com", "https", true, true, "https://thehuntersdream.com"},
    {"テストサーバー (dev.thehuntersdream.com)", "dev.thehuntersdream.com", "https", true, true, "https://dev.thehuntersdream.com"},
};

// Menu text size's values (menu_text_size in host/options.cpp, the same list).
constexpr int kTextSizes[] = {100, 125, 150, 175, 200, 250, 300, 350, 400};
const char* const kTextSizeLabels[] = {"100%", "125%", "150%", "175%", "200%", "250%", "300%", "350%", "400%"};

// The window's pages, as the sidebar lists them.
enum Page { kPageGame, kPageGraphics, kPageControls, kPageEnhancements, kPagePatches, kPageShaders, kPagePlugins, kPageTrophies, kPageHistory, kPageSettings, kPageCount };
struct PageInfo {
    const char* name;
    const char* slug;  // its picture's name (BBHOST_SETUP_TEST=shot)
    launcher_ui::Icon icon;
};
constexpr PageInfo kPages[kPageCount] = {
    {"ゲーム", "game", launcher_ui::Icon::Game},
    {"グラフィックス", "graphics", launcher_ui::Icon::Image},
    {"操作", "controls", launcher_ui::Icon::Keyboard},
    {"PC 追加機能", "pc-enhancements", launcher_ui::Icon::Enhance},
    {"パッチ", "patches", launcher_ui::Icon::Patches},
    {"シェーダー", "shaders", launcher_ui::Icon::Shaders},
    {"プラグイン", "plugins", launcher_ui::Icon::Plugins},
    {"トロフィー", "trophies", launcher_ui::Icon::Enhance},
    {"プレイ履歴", "play-history", launcher_ui::Icon::Game},
    {"設定", "settings", launcher_ui::Icon::Settings},
};

// A text field's buffer.
struct Field {
    char text[1024] = {};
    void set(const std::string& s) { std::snprintf(text, sizeof(text), "%s", s.c_str()); }
    std::string str() const { return text; }
};

// What the system's pickers returned: they may answer on another thread.
std::mutex g_pick_mu;
std::string g_picked;
int g_pick_for = -1;  // which field the answer is for (-1 none pending)

void SDLCALL on_pick(void* userdata, const char* const* files, int) {
    std::lock_guard<std::mutex> lk(g_pick_mu);
    g_pick_for = static_cast<int>(reinterpret_cast<std::intptr_t>(userdata));
    g_picked = files && files[0] ? files[0] : "";
}

bool is_dir(const std::string& p) {
    std::error_code ec;
    return !p.empty() && fs::is_directory(p, ec);
}
// A name config_set_values can write as a key as it is (TOML's bare keys).
bool bare_key(const std::string& k) {
    if (k.empty()) return false;
    for (char ch : k) {
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-')) return false;
    }
    return true;
}

bool is_file(const std::string& p) {
    std::error_code ec;
    return !p.empty() && fs::is_regular_file(p, ec);
}

std::string generic(std::string p) {
    for (char& ch : p) {
        if (ch == '\\') ch = '/';
    }
    return p;
}

// A path as the config keeps it: absolute. The window checks a relative one
// against the working directory (an eboot found beside bbhost), but the config
// reads relative paths against its own folder, where it is not.
std::string saved_path(const std::string& p) {
    std::error_code ec;
    const std::filesystem::path a = std::filesystem::absolute(p, ec);
    return generic(ec ? p : a.lexically_normal().string());
}

// What the window knows about the eboot it was last given.
struct EbootCheck {
    std::string path, sha;
    bool ok = false;
};

EbootCheck check_eboot(const std::string& path) {
    EbootCheck c;
    c.path = path;
    if (!is_file(path)) return c;
    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    c.sha = sha256_hex(data.data(), data.size());
    c.ok = c.sha == kEboot109Sha256;
    return c;
}

std::string toml_string(const std::string& s) {
    std::string out = "\"";
    for (char ch : s) {
        if (ch == '"') out += "\\\"";
        else out += ch;
    }
    return out + "\"";
}

std::string capitalised(std::string s) {
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

bool contains_nocase(const std::string& hay, const char* needle) {
    if (!*needle) return true;
    auto low = [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); };
    std::string h, n;
    for (char ch : hay) h += low(static_cast<unsigned char>(ch));
    for (const char* p = needle; *p; ++p) n += low(static_cast<unsigned char>(*p));
    return h.find(n) != std::string::npos;
}

// The title bar in the window's colours (Windows): dark, and on Windows 11
// the night's ink with ice letters and a violet edge - turned by Menu hue
// and Menu vibrance (launcher_ui::tint) as the rest of the window is. dwmapi's
// DwmSetWindowAttribute, loaded as SDL loads anything, so nothing more is
// linked; a Windows without the attribute leaves its bar as it was.
void dark_title_bar(SDL_Window* win) {
#if defined(_WIN32)
    using SetAttribute = long (*)(void* hwnd, unsigned attribute, const void* value, unsigned size);
    SDL_SharedObject* dwm = SDL_LoadObject("dwmapi.dll");
    const auto set = dwm ? reinterpret_cast<SetAttribute>(SDL_LoadFunction(dwm, "DwmSetWindowAttribute")) : nullptr;
    void* hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(win), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (set && hwnd) {
        const int dark = 1;
        // COLORREFs, 0x00BBGGRR: an ImU32's byte order without its alpha.
        const auto colour = [](unsigned rgb) { return launcher_ui::tint(0xff000000u | rgb) & 0x00ffffffu; };
        const unsigned caption = colour(0x00240C10), text = colour(0x00FFD8BF), edge = colour(0x00FF4D7C);
        set(hwnd, 20, &dark, sizeof(dark));           // DWMWA_USE_IMMERSIVE_DARK_MODE
        set(hwnd, 35, &caption, sizeof(caption));     // DWMWA_CAPTION_COLOR
        set(hwnd, 36, &text, sizeof(text));           // DWMWA_TEXT_COLOR
        set(hwnd, 34, &edge, sizeof(edge));           // DWMWA_BORDER_COLOR
    }
    if (dwm) SDL_UnloadObject(dwm);
#else
    (void)win;
#endif
}

// Dim text, wrapped, in a smaller size when `small`.
void dim_text(const std::string& s, ImU32 col = launcher_ui::kTextDim, bool small = true) {
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    if (small) ImGui::PushFont(nullptr, 14.0f);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(s.c_str());
    ImGui::PopTextWrapPos();
    if (small) ImGui::PopFont();
    ImGui::PopStyleColor();
}

}  // namespace

LauncherResult launcher_run(const HostConfig& cfg, const std::string& reason, bool plugins_tab) {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        host_log("setup: no window (%s); edit %s instead", SDL_GetError(), config_user_file().c_str());
        return LauncherResult::Quit;
    }
    // Controllers, for the Controls page's (ImGui's SDL backend opens them):
    // without it a pad's presses never arrive.
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) host_log("setup: no controllers (%s)", SDL_GetError());
    // The text size, Menu text size (menu_text_size, the same setting as
    // F10's): 100% is the window as it has always been, 1120x720 at Windows'
    // display scaling, and every size in it - the window's own included -
    // grows with the percent (a 1440p or 4K screen: 150% or 200%). A window
    // larger than the screen stops at the screen; the pages then scroll over
    // Play, Save and Quit, which stay in view.
    host_options_load();
    const SDL_DisplayID display = SDL_GetPrimaryDisplay();
    const float screen_scale = std::max(1.0f, SDL_GetDisplayContentScale(display));
    // Its size, or less where the screen is smaller.
    SDL_Rect usable{};
    const bool have_usable = SDL_GetDisplayUsableBounds(display, &usable) && usable.w > 0 && usable.h > 0;
    const auto scale_for = [&](int choice) {
        const int i = std::clamp(choice, 0, static_cast<int>(std::size(kTextSizes)) - 1);
        return screen_scale * static_cast<float>(kTextSizes[i]) / 100.0f;
    };
    int text_choice = std::max(0, host_opt_index("menu_text_size"));
    float scale = scale_for(text_choice);
    const auto window_size = [&](float s, int* w, int* h) {
        *w = static_cast<int>(1120 * s);
        *h = static_cast<int>(720 * s);
        if (have_usable) {
            *w = std::min(*w, usable.w * 19 / 20);
            *h = std::min(*h, usable.h * 9 / 10);
        }
    };
    int win_w = 0, win_h = 0;
    window_size(scale, &win_w, &win_h);
    host_log("setup: menu text size %s (x%.2f; the screen's own x%.2f)", kTextSizeLabels[text_choice], static_cast<double>(scale),
             static_cast<double>(screen_scale));
    SDL_Window* win = SDL_CreateWindow("bbhost 日本語版の設定", win_w, win_h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer* ren = win ? SDL_CreateRenderer(win, nullptr) : nullptr;
    if (!win || !ren) {
        host_log("setup: no window (%s); edit %s instead", SDL_GetError(), config_user_file().c_str());
        if (win) SDL_DestroyWindow(win);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return LauncherResult::Quit;
    }
    SDL_SetWindowMinimumSize(win, std::min(win_w, static_cast<int>(860 * scale)), std::min(win_h, static_cast<int>(580 * scale)));
    dark_title_bar(win);
    // V-Sync paces the window where the driver keeps it; the loop below caps
    // it at 30 a second besides, for where it does not.
    SDL_SetRenderVSync(ren, 1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // nothing written beside the exe
    ImGui_ImplSDL3_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer3_Init(ren);
    // The look (host/launcher_ui.h): the fonts, the style, the painting.
    ui::init(ren, scale);

    // The fields, from what the configuration holds now.
    enum { kApp0, kEboot, kData, kToken, kHost, kFields };
    Field f[kFields];
    f[kApp0].set(cfg.app0);
    f[kEboot].set(cfg.eboot);
    f[kData].set(cfg.data == config_default_data_dir() ? std::string() : cfg.data);
    f[kToken].set(cfg.update_token_file);
    f[kHost].set(cfg.online_host);
    int server = cfg.online_host.empty() ? 0 : 2;  // nothing set yet: the live server; else custom unless it is one of these
    for (int i = 0; i < 2; ++i) {
        if (cfg.online_host == kServers[i].host) server = i;
    }
    char scheme[8] = {};
    std::snprintf(scheme, sizeof(scheme), "%s", cfg.online_scheme.empty() ? "http" : cfg.online_scheme.c_str());
    bool verify_tls = cfg.online_verify_tls, require_account = cfg.online_require_account;
#if defined(BBHOST_RELEASE_BUILD)
    bool check_updates = cfg.update_check != 0;
#else
    bool check_updates = cfg.update_check == 1;
#endif
    bool show_every_start = cfg.setup_always;
    bool animations = cfg.setup_animations;
    bool keep_open = cfg.setup_keep_open;
    // The game while the window stays open (keep_open): this executable again
    // with --no-setup, the window minimized until it ends.
    SDL_Process* game = nullptr;
    int text_applied = text_choice;
    float title_hue = 0.0f, title_vibrance = 0.0f;  // the title bar's colours as last set (dark_title_bar)  // the size the window has; a new choice goes in before the next frame
    bool tooling = cfg.tooling;
    // The F10 screen's start-up settings, through the same calls it uses.
    host_options_load();
    int window_mode = std::max(0, host_opt_index("window_mode"));  // Windowed, Borderless, Fullscreen
    bool stay_borderless = host_opt_get("stay_borderless");  // Windows' switch under the Window row, for Borderless
    bool skip_logos = host_opt_get("skip_logos");
    bool performance_mode = host_opt_get("performance_mode");
    int frame_cap = host_opt_frame_cap();
    // The Native frame clock (experimental, off): F10's DISPLAY row, read as the
    // game starts (engine/frame_rate.cpp; BBHOST_FRAME_CLOCK=0|1 wins).
    bool frame_clock = host_opt_get("frame_clock");
    // Effect timing (60 fps by default): F10's DISPLAY row, read as the game
    // starts (engine/frame_rate.cpp; BBHOST_SFX_STEP wins). The row shows what
    // will run (host_effect_timing_row); a pick, one of the three, is the
    // player's own from Save or Play on.
    int effect_timing = -1;  // the pick, -1 none
    // The game's frame rate (engine/frame_rate.h), in the setting's order: 0 is
    // Display-Limited, the display's refresh rate. The list's labels and
    // bubbles are host_frame_rate_control's, F10's.
    static const int kCaps[] = {30, 60, 90, 120, 150, 180, 240, 360, 480, 960, 0};
    constexpr int kCapCount = static_cast<int>(sizeof(kCaps) / sizeof(kCaps[0]));
    // Frame rate's Custom limit: ticked, the game runs at the number typed (10 to 960).
    bool cap_custom = host_opt_frame_cap_custom();
    int cap_custom_fps = host_opt_frame_cap_custom_fps();
    int cap_choice = kCapCount - 1;
    for (int k = 0; k < kCapCount; ++k) {
        if (kCaps[k] == frame_cap) cap_choice = k;
    }
    // The follow camera's tuning above 30 fps: the setting's index, 0 FPS++, 1 Original.
    int camera_tuning = host_opt_index("camera_tuning") == 1 ? 1 : 0;
    // The Shaders page: the Shaders setting (0 HLSL, 1 Lifted), the caches'
    // state and "今すぐシェーダーをコンパイル" (host/shader_compile.h). This machine's
    // Vulkan devices are listed once, off the window's thread.
    int shaders = host_opt_index("shaders") == 1 ? 1 : 0;
    ShaderCompileJob compile_job;
    // The Connection card's test (net/nat_job.h), from the game's P2P port:
    // never while the game this window started runs, and Play waits for it.
    // The button rests 10 s after a test that sent anything, and 10 minutes
    // after the server said the account's tests this hour are used up.
    net::nat::Job nat_job;
    unsigned nat_tests_seen = 0;
    double nat_wait_until = 0.0;  // ImGui time
    std::string nat_server;       // the server the last test ran against (server_key)
    bool nat_scroll = false;      // bring the card into view (the test hook)
    bool nat_details = false;     // the card's Details open
    // The P2P port setting (online.p2p_port): the next test runs from it at
    // once, Save or Play keep it, and the game opens it as it starts. A port
    // typed that will not do is refused, with why.
    int p2p_port = cfg.p2p_port >= 1024 && cfg.p2p_port <= 65535 ? cfg.p2p_port : 9307;
    std::string p2p_port_note;
    // The relay switch (signed in, net/account.h): turning it off without a
    // test that showed other hunters can get in asks again first.
    bool relay_confirm = false;
    double relay_asked_at = -100.0;  // ImGui time of the last change asked
    std::mutex devices_mu;
    std::vector<std::string> devices;
    std::atomic<bool> devices_ready{false};
    std::thread devices_thread([&] {
        std::vector<std::string> d = shader_vulkan_devices();
        std::lock_guard<std::mutex> lk(devices_mu);
        devices = std::move(d);
        devices_ready.store(true);
    });
    ShaderCacheState cache_state;
    std::chrono::steady_clock::time_point cache_state_at{};
    bool cache_state_devices = false, compile_was_running = false;
    // The PC enhancements, through the same calls: each is read when the
    // game starts (main.cpp, config_set_enhancements). The same ones the
    // game's System > PC Enhancements and F10 > PC ENHANCEMENTS have.
    struct Enhancement {
        const char* key;
        const char* label;
        const char* sub;  // the line under it
        const char* text;  // the whole of it, in its bubble
        bool on;
    };
    Enhancement enhancements[] = {
        {"change_appearance", "狩人の夢の鏡", "工房の鏡で容姿の編集画面を開きます。",
         "工房の鏡で容姿の編集画面を開きます。未使用の「変装」機能です。", false},
        {"rebirth", "嘆きの祭壇で生まれ変わる", "出自の初期レベルに戻し、血の遺志で能力値を振り直せます。",
         "ヤーナムの石があれば、祭壇で初期レベルに戻し、血の遺志を返還します。終了時に確定か取り消しを選べます。",
         false},
        {"five_players", "終盤エリアの５人プレイ", "終盤と DLC エリアで侵入者を２人まで許可します。",
         "メンシス、高楼、辺境、DLC エリアで、聖杯と同様に侵入者を２人まで許可します。全参加者の有効化が必要です。",
         false},
        {"skill_issue", "SKILL ISSUE", "死亡時の表示を SKILL ISSUE に変更します。",
         "すべての言語で、死亡時の表示を SKILL ISSUE に変更します。", false},
        {"full_phantom_hp", "協力・侵入時の最大 HP", "他の狩人の世界でも最大 HP を維持します。",
         "協力・侵入時は通常 HP が 70% になります。有効にすると自分の HP を維持します。訪問者自身の設定を使用し、ホストの設定は他の狩人に影響しません。参加者全員に訪問者の実際の HP が表示されます。",
         false},
        {"auto_replenish", "消耗品の自動補充",
         "自分の世界での死亡時や狩人の夢への帰還時、火炎瓶や投げナイフなどを倉庫から補充します。",
         "輸血液や水銀弾と同様に、火炎瓶、投げナイフ、小石、ヤスリ、丸薬、骨髄の灰など20種類の消耗品を補充します。自分の世界での死亡後と狩人の夢への到着時に行います。",
         false},
    };
    for (Enhancement& e : enhancements) e.on = host_opt_get(e.key);
    // The Patches page: every manifest the loader would read
    // (engine/patch_manifest.h), each with its switch. One that follows a
    // setting (its `option`) shows that setting - the Game page's Skip company
    // logos, Performance mode - so a patch has one switch; the rest (The Old
    // Hunters among them) are [patches] <name> = true or false in bbhost.toml.
    bool quick_reentry = cfg.quick_reentry, all_post_processors = cfg.all_post_processors;
    bool quick_reentry_was = quick_reentry, all_post_processors_was = all_post_processors;
    struct PatchRow {
        PatchListing p;
        bool on = true, was = true;  // its [patches] switch, and what the file says
    };
    std::vector<PatchRow> patch_rows;
    for (PatchListing& p : patch_manifests_list(cfg.mods.empty() ? cfg.data + "/mods" : cfg.mods)) {
        PatchRow r;
        r.on = r.was = patch_manifest_on(p.name, p.enabled);
        r.p = std::move(p);
        patch_rows.push_back(std::move(r));
    }
    // Two of a group on (a bbhost.toml edited by hand): the one the loader
    // applies, the last in the list's order, stays on.
    for (PatchRow& r : patch_rows) {
        if (r.p.group.empty() || !r.on) continue;
        for (const PatchRow& o : patch_rows) {
            if (&o == &r || o.p.group != r.p.group || !o.on || !o.p.option.empty()) continue;
            if (std::make_pair(o.p.order, o.p.name) > std::make_pair(r.p.order, r.p.name)) r.on = false;
        }
    }
    auto patch_switch = [&](PatchRow& r) -> bool* {
        const std::string& o = r.p.option;
        if (o == "startup.skip_intro") return &skip_logos;
        if (o == "startup.performance_mode") return &performance_mode;
        if (o == "loading.quick_reentry") return &quick_reentry;
        if (o == "streaming.all_post_processors") return &all_post_processors;
        return &r.on;
    };
    char patch_find[64] = {};

    EbootCheck eboot = check_eboot(f[kEboot].str());
    // The game folder's version, read again when the folder changes.
    std::string app0_seen;
    App0Version app0_ver;
    PluginUi plugin_ui;
    bool plugin_seen = false;
    // The settings folder's move (core/config_move.h): told at the top of
    // every page until it is dismissed.
    std::vector<ConfigMove> moves = config_moves_to_show();
    std::string saved_note;
    bool saved_ok = true;
    double saved_at = -100.0;
    LauncherResult result = LauncherResult::Quit;
    bool done = false;
    int page = plugins_tab ? kPagePlugins : kPageGame;
    double page_at = 0.0;  // when it was chosen: it fades in
    // BBHOST_SETUP_TEST=play|quit: the window answers itself after a few
    // frames (a test that it opens, draws and hands over). create:NAME makes
    // an account held by this PC through the account section, as its button
    // does, then closes once the server has answered.
    const char* test = std::getenv("BBHOST_SETUP_TEST");
    int frames = 0;
    TrophyUi trophy_ui;
    PlayHistoryUi history_ui;
    if (test && !std::strcmp(test, "trophies")) page = kPageTrophies;
    if (test && !std::strcmp(test, "play-history")) page = kPageHistory;
    if (test && !std::strcmp(test, "chalice-history")) { page = kPageHistory; history_ui.chalice = true; }
    // link: starts a Discord link as its button does and, once the code is
    // up, cancels it and closes.
    const std::string test_create = test && !std::strncmp(test, "create:", 7) ? test + 7 : "";
    const bool test_link = test && !std::strcmp(test, "link");
    bool test_created = false;
    // shot:DIR saves each page as DIR/NN-<page>.png, then the Patches page
    // again with a patch's bubble up (the pointer put on a row), and closes,
    // saving nothing: a look at the window without a person, and the docs'
    // pictures.
    const std::string test_shot = test && !std::strncmp(test, "shot:", 5) ? test + 5 : "";
    // nattest:DIR runs the connection test as its button does, then saves the
    // window with the Connection card in view as DIR/connection.png once the
    // result is shown, and again with its Details open as
    // DIR/connection-details.png, and closes; natcard:DIR saves the card as it
    // opens (once the sign-in check has answered), without a test.
    // natfake:DIR:NAME[+off|+on] shows the card for a made-up finished test of
    // the kind NAME (net/nat_examples.h; nothing is sent) - with the account's
    // relay switch off or on, when said - as DIR/connection-NAME.png and
    // DIR/connection-NAME-details.png.
    const bool nat_hook_test = test && !std::strncmp(test, "nattest:", 8);
    const bool nat_hook_fake = test && !std::strncmp(test, "natfake:", 8);
    std::string test_nat = nat_hook_test || nat_hook_fake ? test + 8 : test && !std::strncmp(test, "natcard:", 8) ? test + 8 : "";
    std::string nat_shot_name = "connection";
    net::nat::Outcome nat_fake;
    bool nat_fake_on = false;
    int nat_fake_relay = 0;  // 1 off, -1 on, 0 as the sign-in check has it
    if (nat_hook_fake) {
        const std::size_t colon = test_nat.rfind(':');
        std::string spec = colon == std::string::npos ? std::string() : test_nat.substr(colon + 1);
        if (colon != std::string::npos) test_nat.resize(colon);
        nat_shot_name += "-" + spec;
        if (const std::size_t plus = spec.find('+'); plus != std::string::npos) {
            nat_fake_relay = spec.substr(plus + 1) == "off" ? 1 : spec.substr(plus + 1) == "on" ? -1 : 0;
            spec.resize(plus);
        }
        nat_fake_on = net::nat::example_outcome(spec, cfg.p2p_port >= 1024 && cfg.p2p_port <= 65535 ? cfg.p2p_port : 9307,
                                                static_cast<double>(std::time(nullptr)) - 20, &nat_fake);
        if (!nat_fake_on) host_log("setup: natfake: no example called %s", spec.c_str());
    }
    int nat_hook_stage = 0;  // 0 waiting to start, 1 testing, 2 the result shown, 3 saved
    double nat_hook_at = 0.0;
    // relay:DIR walks the relay switch, signed in: the card as it opens
    // (DIR/relay-1-start.png), Off pressed - the warning up, when this PC's
    // last test did not show that others can get in (relay-2-warning.png), and
    // "それでも無効にする" pressed - the card once the server has it
    // (relay-3-off.png), On pressed and the card again (relay-4-on.png).
    // relayfail:DIR presses Off with the account server out of reach and saves
    // what the switch says then (relay-fail.png).
    const bool relay_hook_fail = test && !std::strncmp(test, "relayfail:", 10);
    const std::string test_relay = relay_hook_fail ? test + 10 : test && !std::strncmp(test, "relay:", 6) ? test + 6 : "";
    int relay_hook_step = 0;
    double relay_hook_at = 0.0;
    int relay_press = 0;  // the hook's press: 1 Off, 2 "それでも無効にする", 3 On
    // links:DIR puts the pointer on each of the sidebar's links in turn, saves
    // the window with its bubble as DIR/link-discord.png and
    // DIR/link-website.png, and presses it - which in a test's window logs the
    // address it would open and opens no browser - then closes.
    const std::string test_links = test && !std::strncmp(test, "links:", 6) ? test + 6 : "";
    int links_step = 0;
    double links_at = 0.0;
    ImVec2 link_mid[2] = {ImVec2(-FLT_MAX, -FLT_MAX), ImVec2(-FLT_MAX, -FLT_MAX)};
    // portset:DIR:PORT[,PORT...] types each port into the P2P port field as a
    // player would and saves the card as DIR/port-PORT.png: taken, or refused
    // with why; then closes.
    std::vector<int> port_hook;
    std::string test_port;
    if (test && !std::strncmp(test, "portset:", 8)) {
        test_port = test + 8;
        const std::size_t colon = test_port.rfind(':');
        std::string list = colon == std::string::npos ? std::string() : test_port.substr(colon + 1);
        if (colon != std::string::npos) test_port.resize(colon);
        for (std::size_t at = 0; at < list.size();) {
            const std::size_t comma = std::min(list.find(',', at), list.size());
            port_hook.push_back(std::atoi(list.substr(at, comma - at).c_str()));
            at = comma + 1;
        }
    }
    std::size_t port_hook_next = 0;
    double port_hook_at = 0.0;
    // A test hook's picture, saved once the frame is drawn ("" none).
    std::string hook_shot;
    double hook_shot_at = 0.0;
    int shot = 0;  // the picture being waited for: a page, then the bubble
    double shot_at = 1.2;
    ImVec2 patch_row_at(-FLT_MAX, -FLT_MAX);  // a row's middle, for the bubble
    if (!test_shot.empty() || !test_nat.empty() || !test_relay.empty() || !test_port.empty()) page = kPageGame;

    // The account section's fields (never saved: the name is the account's,
    // the recovery code is typed once to recover).
    char acct_name[64] = {};
    char acct_code[64] = {};
    std::snprintf(acct_name, sizeof(acct_name), "%s", net::account_name().c_str());
    // The server the dropdown names, in memory: the account calls go there
    // whether or not it has been saved yet.
    auto apply_server = [&]() {
        const bool custom = server == 2;
        config_set_online(custom ? f[kHost].str() : kServers[server].host, custom ? scheme : kServers[server].scheme,
                          custom ? verify_tls : kServers[server].verify_tls, custom ? require_account : kServers[server].require_account,
                          custom ? cfg.auth_server : kServers[server].auth_server);
    };
    // The sign-in check (net/account.h): whether the server still takes this
    // PC's sign-in, asked on a thread of its own - at the start when a token is
    // held, again once a sign-in, recovery or link has completed (a new sign-in
    // has no check yet), when the server chosen changes, and on Retry. Nothing
    // waits for it; the account section, the Game page and the line under Play
    // say what it found.
    std::string checked_server;  // the server the last check went to
    auto server_key = [&]() {
        const bool custom = server == 2;
        return std::to_string(server) + "|" + (custom ? f[kHost].str() + "|" + scheme + (verify_tls ? "|v" : "|-") : std::string());
    };
    auto check_sign_in = [&]() {
        apply_server();
        checked_server = server_key();
        net::account_check_start();
    };
    // The connection test, at the server the dropdown names: its STUN host's
    // test ports, from this PC's P2P port, kept for the account when signed in.
    auto start_nat_test = [&]() {
        apply_server();
        net::nat::JobParams p;
        p.host = net::nat::test_host();
        p.ports = net::nat::parse_ports(config().nat_test_ports);
        p.p2p_port = p2p_port;
        if (net::account_logged_in() && !net::account_refused()) p.token = net::account_token();
        p.save_url = net::auth_server_base() + "/auth/nat_test";
        if (nat_job.start(p)) nat_server = server_key();
    };
    // What the window says of the sign-in: the account's line and its colour.
    struct SignIn {
        bool in = false, refused = false, checking = false, valid = false, unchecked = false, needed = false;
        std::string line;      // under the name
        std::string foot;      // the line under Play ("" none)
        ImU32 col = 0;
    };
    auto sign_in_state = [&]() {
        SignIn s;
        s.in = net::account_logged_in();
        s.needed = server == 2 ? require_account : kServers[server].require_account;
        s.col = ui::kTextDim;
        if (!s.in) {
            s.line = s.needed ? "このサーバーでオンラインを利用するにはログインが必要です。" : "オンラインにはログインが必要です";
            if (s.needed) s.foot = "未ログイン（オフライン）";
            return s;
        }
        const net::AccountCheck c = net::account_check();
        // A check on its way (another server chosen, Retry) speaks before an
        // earlier answer.
        s.refused = net::account_refused() && c.state != net::AccountCheckState::Checking;
        if (s.refused) {
            s.line = "期限切れ、またはこの PC はログアウト済み";
            s.foot = "ログインの有効期限切れ";
            s.col = ui::kWarn;
        } else if (c.state == net::AccountCheckState::Valid) {
            s.valid = true;
            s.line = s.foot = "ログイン中: " + net::account_name();
            s.col = ui::kGood;
        } else if (c.state == net::AccountCheckState::Unchecked) {
            s.unchecked = true;
            const std::string why = c.status == 0 ? std::string("the server did not answer")
                                    : c.why.find("ResKind") != std::string::npos
                                        ? "this server has no sign-in check (HTTP " + std::to_string(c.status) + ")"
                                        : "the server answered HTTP " + std::to_string(c.status);
            s.line = "ログインを確認できませんでした：" + why + ".";
            s.foot = "ログイン未確認";
        } else {
            s.checking = true;
            s.line = s.foot = "ログインを確認中…";
        }
        return s;
    };
    auto save = [&]() -> bool {
        std::vector<ConfigEdit> e;
        e.push_back({"paths", "app0", toml_string(saved_path(f[kApp0].str()))});
        e.push_back({"paths", "eboot", toml_string(saved_path(f[kEboot].str()))});
        if (!f[kData].str().empty()) e.push_back({"paths", "data", toml_string(saved_path(f[kData].str()))});
        const bool custom = server == 2;
        e.push_back({"online", "host", toml_string(custom ? f[kHost].str() : kServers[server].host)});
        e.push_back({"online", "scheme", toml_string(custom ? scheme : kServers[server].scheme)});
        e.push_back({"online", "verify_tls", (custom ? verify_tls : kServers[server].verify_tls) ? "true" : "false"});
        e.push_back({"online", "require_account", (custom ? require_account : kServers[server].require_account) ? "true" : "false"});
        if (!custom) e.push_back({"online", "auth_server", toml_string(kServers[server].auth_server)});
        e.push_back({"online", "p2p_port", std::to_string(p2p_port)});
        e.push_back({"update", "check", check_updates ? "true" : "false"});
        if (!f[kToken].str().empty()) e.push_back({"update", "token_file", toml_string(saved_path(f[kToken].str()))});
        e.push_back({"startup", "setup_window", show_every_start ? "true" : "false"});
        e.push_back({"startup", "setup_animations", animations ? "true" : "false"});
        e.push_back({"startup", "setup_keep_open", keep_open ? "true" : "false"});
        e.push_back({"debug", "tooling", tooling ? "true" : "false"});
        // What this bbhost's file looks like (core/config.h): a value saved
        // from here is a choice.
        e.push_back({"bbhost", "config_version", std::to_string(kUserConfigVersion)});
        // The Patches page's own switches, each only once changed.
        for (const PatchRow& r : patch_rows) {
            if (r.p.option.empty() && r.on != r.was && bare_key(r.p.name)) e.push_back({"patches", r.p.name, r.on ? "true" : "false"});
        }
        if (quick_reentry != quick_reentry_was) e.push_back({"loading", "quick_reentry", quick_reentry ? "true" : "false"});
        if (all_post_processors != all_post_processors_was)
            e.push_back({"streaming", "all_post_processors", all_post_processors ? "true" : "false"});
        const std::string file = config_user_file();
        saved_at = ImGui::GetTime();
        if (!config_set_values(file, e)) {
            saved_note = "書き込めません: " + file;
            saved_ok = false;
            return false;
        }
        for (PatchRow& r : patch_rows) r.was = r.on;
        quick_reentry_was = quick_reentry;
        all_post_processors_was = all_post_processors;
        host_opt_set_index("window_mode", window_mode);
        host_opt_set("stay_borderless", stay_borderless);
        host_opt_set("skip_logos", skip_logos);
        host_opt_set("performance_mode", performance_mode);
        host_opt_set_frame_cap(kCaps[cap_choice]);  // unticks Custom limit, set again next
        host_opt_set_frame_cap_custom(cap_custom, cap_custom_fps);
        host_opt_set("frame_clock", frame_clock);
        if (effect_timing >= 0) host_opt_choose_effect_timing(effect_timing);
        host_opt_set_index("camera_tuning", camera_tuning);
        host_opt_set_index("shaders", shaders);
        for (const Enhancement& x : enhancements) host_opt_set(x.key, x.on);
        host_options_save_now();
        saved_note = "保存しました";
        saved_ok = true;
        host_log("setup: saved %s", file.c_str());
        return true;
    };

    // A port typed into the P2P port field: taken - the next test runs from it,
    // Save or Play keeps it - or refused, with why.
    auto take_p2p_port = [&](int typed) {
        if (typed == p2p_port) {
            p2p_port_note.clear();
            return;
        }
        std::string why = net::nat::p2p_port_problem(typed, net::nat::parse_ports(config().nat_test_ports));
        if (why.empty()) {
            if (const std::string t = net::nat::port_taken(static_cast<std::uint16_t>(typed)); !t.empty())
                why = "Another program has UDP port " + std::to_string(typed) + " (" + t + "): choose another.";
        }
        if (why.empty()) {
            host_log("setup: P2P port %d (was %d)", typed, p2p_port);
            p2p_port = typed;
            p2p_port_note.clear();
        } else {
            host_log("setup: P2P port %d refused: %s", typed, why.c_str());
            p2p_port_note = why;
        }
    };
    // A link's button (host/links.h): its address in the browser - in a
    // test's window, only the log says which it would open.
    auto open_link = [&](const links::Link& l) {
        if (!links::known(l.url)) return;
        if (!links::opens_browser(test)) {
            host_log("setup: link: %s would open %s (a test's window opens no browser)", l.title, l.url);
            return;
        }
        host_log("setup: link: %s: %s", l.title, l.url);
        if (!SDL_OpenURL(l.url)) host_log("setup: link: %s did not open: %s", l.url, SDL_GetError());
    };

    // The window's pace: 30 frames a second at most. V-Sync alone left the loop
    // drawing as fast as it could wherever the driver ignores it - a driver
    // setting that turns it off, a backend without it, a window the compositor
    // stopped showing - a whole core spent on a settings page. Each frame's
    // deadline moves on by a thirtieth of a second on SDL's nanosecond clock,
    // and what is left of it is slept after the present; a frame that ran a
    // whole frame past it starts the count again rather than hurrying to
    // catch up.
    constexpr Uint64 kFrameNs = SDL_NS_PER_SECOND / 30;
    Uint64 frame_due = SDL_GetTicksNS();
    while (!done) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            // The Controls page choosing a binding takes the next press first.
            if (controls_capture_event(ev)) continue;
            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) done = true;
        }
        // The Graphics page's rows hand their changes to the window's pump, as
        // F10's do in the game (host/options.h).
        host_options_frame(0.0f, 0.0f);
        // A sign-in or sign-out in the account section: the token is kept in
        // the options file, written at once rather than at Save.
        if (host_account_take_save()) {
            host_options_save_now();
            if (net::account_logged_in()) std::snprintf(acct_name, sizeof(acct_name), "%s", net::account_name().c_str());
        }
        // The sign-in check: for a sign-in nothing has checked yet, or when the
        // server chosen is not the one last asked - not while a field is being
        // typed in (the custom host), nor while an account action runs.
        if (net::account_logged_in() && !host_account_busy() && !ImGui::IsAnyItemActive() &&
            (net::account_check().state == net::AccountCheckState::None || server_key() != checked_server))
            check_sign_in();
        {
            std::lock_guard<std::mutex> lk(g_pick_mu);
            if (g_pick_for >= 0) {
                if (!g_picked.empty()) f[g_pick_for].set(generic(g_picked));
                if (g_pick_for == kEboot) eboot = check_eboot(f[kEboot].str());
                g_pick_for = -1;
            }
        }
        // The game it started has ended: the window comes back as a fresh
        // start of its own (what the game saved - F10's settings, a sign-in -
        // read again), or, if that cannot start, as it was.
        if (game) {
            int code = 0;
            if (SDL_WaitProcess(game, false, &code)) {
                SDL_DestroyProcess(game);
                game = nullptr;
                host_log("setup: the game ended (%d)", code);
                std::string err;
                if (SDL_Process* again = static_cast<SDL_Process*>(host_start_again("--setup", false, err))) {
                    SDL_DestroyProcess(again);
                    done = true;
                    continue;
                }
                host_log("setup: %s", err.c_str());
                SDL_RestoreWindow(win);
                SDL_RaiseWindow(win);
            }
        }
        // Minimized, hidden or covered: nothing is drawn, and the loop waits
        // for the next event, a tenth of a second at most (a sign-in finished
        // in the browser is still saved above). A test's window draws on, so
        // its frames still count.
        if (!test && (SDL_GetWindowFlags(win) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN | SDL_WINDOW_OCCLUDED))) {
            SDL_WaitEventTimeout(nullptr, 100);
            frame_due = SDL_GetTicksNS();
            continue;
        }
        // A new text size: the style and fonts take it, and the window
        // grows or shrinks to what it opens at with that size.
        if (text_choice != text_applied) {
            text_applied = text_choice;
            host_opt_set_index("menu_text_size", text_choice);  // saved at once, as F10 saves it
            scale = scale_for(text_choice);
            ui::set_scale(scale);
            window_size(scale, &win_w, &win_h);
            SDL_SetWindowMinimumSize(win, std::min(win_w, static_cast<int>(860 * scale)), std::min(win_h, static_cast<int>(580 * scale)));
            if (!(SDL_GetWindowFlags(win) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN))) SDL_SetWindowSize(win, win_w, win_h);
            host_log("setup: menu text size %s (x%.2f)", kTextSizeLabels[text_choice], static_cast<double>(scale));
        }
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        // A test's pictures: the pointer where the test puts it, not where the
        // desk's is.
        if (!test_shot.empty()) io.AddMousePosEvent(shot == kPageCount ? patch_row_at.x : -FLT_MAX, shot == kPageCount ? patch_row_at.y : -FLT_MAX);
        // links: the pointer on the link the hook is at, pressed and let go.
        if (!test_links.empty()) {
            const int k = links_step >= 1 && links_step <= 3 ? 0 : links_step >= 5 && links_step <= 7 ? 1 : -1;
            io.AddMousePosEvent(k >= 0 ? link_mid[k].x : -FLT_MAX, k >= 0 ? link_mid[k].y : -FLT_MAX);
            if (links_step == 2 || links_step == 6) {
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
                ++links_step;
            } else if (links_step == 3 || links_step == 7) {
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
                links_at = ImGui::GetTime() + 0.6;
                ++links_step;
            }
        }
        ImGui::NewFrame();
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const ImGuiStyle& st = ImGui::GetStyle();
        // At most a third of the window: a large Menu text size leaves the page room.
        const float side_w = std::round(std::min(236 * scale, vp->WorkSize.x * 0.34f));
        ui::set_animations(animations);
        ui::backdrop(side_w);
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("setup", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                         ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();

        // While the Updates switch is on: one check, on a thread of its own
        // (host/updater.h); a newer release's page opens in the browser.
        if (check_updates) updater::check_once();
        const std::string app0 = f[kApp0].str();
        const std::string edition_error = config_game_edition_error(app0);
        const bool app0_ok = is_dir(app0 + "/dvdroot_ps4") && edition_error.empty();
        // Its version: the 1.09 update's files copied over it, or in an
        // update folder beside it (core/config.h), or the game stops when it
        // reads one. A warning, not a stop: Play stays on.
        if (app0_ok && app0_seen != app0) {
            app0_seen = app0;
            app0_ver = config_app0_version(app0);
        }
        // The compile's progress and the caches' state, which the Shaders page
        // shows and the Game page's recommendation reads: the state again
        // every few seconds, and when a compile ends.
        const ShaderCompileJob::View cv = compile_job.view();
        {
            const auto now = std::chrono::steady_clock::now();
            if (now - cache_state_at > std::chrono::seconds(3) || (compile_was_running && !cv.running) ||
                (devices_ready.load() && !cache_state_devices)) {
                std::lock_guard<std::mutex> lk(devices_mu);
                cache_state = shader_cache_state(f[kData].str(), devices);
                cache_state_at = now;
                cache_state_devices = devices_ready.load();
            }
            compile_was_running = cv.running;
        }
        // Caches that are not this bbhost's or this driver's: Compile shaders
        // now is recommended (never started by itself) until a compile has run
        // in this window.
        const ShaderCompileAdvice advice = shader_compile_advice(cache_state, host_steam_deck(), std::thread::hardware_concurrency());
        const bool recommend_compile = advice.recommend && !cv.running && !cv.finished && !cv.cancelled && cv.error.empty();
        auto start_compile = [&] {
            if (!edition_error.empty() || !eboot.ok) return;
            // The child reads the saved settings: save first.
            if (save()) {
                std::string err;
                compile_job.start(f[kData].str(), err);
            }
        };
        auto go = [&](int p) {
            if (page == p) return;
            page = p;
            page_at = ImGui::GetTime();
        };
        const bool compiling = compile_job.running();
        const net::nat::Job::View nat_view = nat_job.view();
        if (nat_view.tests != nat_tests_seen) {
            nat_tests_seen = nat_view.tests;
            const bool limited = nat_view.outcome.save.state == net::nat::SaveReply::State::Limited;
            if (limited || nat_view.outcome.probe.sent > 0) nat_wait_until = ImGui::GetTime() + (limited ? 600.0 : 10.0);
        }
        // The card's result is the last test's while the server chosen is the
        // one it ran against; else the account's last test (the sign-in
        // check's), which F10 repeats in the game.
        const bool nat_ours = (nat_view.finished && nat_server == server_key()) || nat_fake_on;
        const net::nat::Outcome& nat_outcome = nat_fake_on ? nat_fake : nat_view.outcome;
        const net::AccountCheck nat_check = net::account_check();
        const bool nat_record = !nat_view.running && !nat_ours && nat_check.state == net::AccountCheckState::Valid &&
                                nat_check.nat_test.present;
        net::nat::Record nat_rec;
        if (nat_record)
            nat_rec = net::nat::Record{net::nat::verdict_from_word(nat_check.nat_test.verdict),
                                       net::nat::inbound_from_word(nat_check.nat_test.inbound), nat_check.nat_test.port,
                                       nat_check.nat_test.tested_at};
        // The card's words (net/nat_check.h), every frame whatever the page:
        // F10 repeats them in the game.
        net::nat::CardContext nat_cx;
        nat_cx.now = static_cast<double>(std::time(nullptr));
        nat_cx.p2p_port = p2p_port;
        nat_cx.signed_in = net::account_logged_in() && !net::account_refused();
        if (nat_check.state == net::AccountCheckState::Valid && nat_check.relay.known)
            nat_cx.relay = nat_check.relay.direct ? net::nat::RelaySetting::Off : net::nat::RelaySetting::On;
        if (nat_fake_relay) nat_cx.relay = nat_fake_relay > 0 ? net::nat::RelaySetting::Off : net::nat::RelaySetting::On;
        const net::nat::Card nat_card = nat_ours     ? net::nat::card_of(nat_outcome, nat_cx)
                                        : nat_record ? net::nat::card_of_record(nat_rec, nat_cx)
                                                     : net::nat::card_untested(nat_cx, net::account_logged_in());
        if (nat_ours && !nat_fake_on && nat_card.reading != net::nat::Reading::Cancelled)
            net::nat::remember(net::nat::Remembered{true, nat_outcome.at, nat_outcome.saved(), nat_card});
        else if (nat_record)
            net::nat::remember(net::nat::Remembered{true, nat_rec.at, true, nat_card});
        // This PC's last test, for the relay switch: what it found, and the
        // address and router its steps name.
        bool nat_tested = false, nat_router = true;
        net::nat::Verdict nat_tested_v = net::nat::Verdict::None;
        net::nat::Inbound nat_tested_in = net::nat::Inbound::NotTested;
        std::string nat_lan;
        if (nat_ours && nat_outcome.probe.error == net::nat::ProbeResult::Error::None && nat_outcome.resolved &&
            nat_outcome.probe.local_port == p2p_port) {
            nat_tested = true;
            nat_tested_v = nat_card.reading == net::nat::Reading::Direct  ? net::nat::Verdict::Direct
                           : nat_card.reading == net::nat::Reading::Relay ? net::nat::Verdict::Relay
                                                                          : net::nat::Verdict::Blocked;
            nat_tested_in = nat_outcome.inbound;
            nat_router = net::nat::behind_router(nat_outcome.probe);
            if (nat_outcome.probe.local_addr) nat_lan = net::nat::ipv4_text(nat_outcome.probe.local_addr);
        } else if (nat_record && nat_rec.port == p2p_port && nat_rec.inbound != net::nat::Inbound::NotTested) {
            nat_tested = true;
            nat_tested_v = nat_rec.verdict;
            nat_tested_in = nat_rec.inbound;
        }
        const bool can_play = app0_ok && eboot.ok && !compiling && !game && !nat_view.running;
        bool play = false;
        const SignIn si = sign_in_state();

        // The sidebar: the name, the pages, and Play.
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 o = vp->WorkPos;
            ui::glass(dl, o, o + ImVec2(side_w, vp->WorkSize.y), 0.0f, IM_COL32(10, 8, 26, 160), 0);
            dl->AddLine(o + ImVec2(side_w - 0.5f, 0), o + ImVec2(side_w - 0.5f, vp->WorkSize.y), ui::alpha(ui::kViolet, 64));
            // Play, Save and Quit at the foot, always in view; over them the
            // name and the pages, which scroll when they are taller than the
            // room left (a large Menu text size), and the keys of the menus in
            // the game (host/ingame_menu.h) where there is room.
            const float play_h = std::round(52 * scale), note_h = std::round(13.5f * scale * 1.35f);
            const float hint_h = std::round(std::round(ImGui::GetFontSize() * 0.78f) + 8 * scale);
            const float hints = 2 * (hint_h + 6 * scale) + 6 * scale + st.ItemSpacing.y;  // the two keys, the gap and the spacing under it
            const float side_h = vp->WorkSize.y - 38 * scale;
            // The links' row over them (host/links.h) counts in the foot.
            float foot = play_h + 4 * scale + 2 * ImGui::GetFrameHeight() + note_h + 4 * st.ItemSpacing.y;
            // The keys where the name and the pages (last frame's height) leave room.
            static float nav_h = 0.0f;
            const bool keys = side_h - foot - hints - 8 * scale >= nav_h;
            if (keys) foot += hints;
            ImGui::SetCursorScreenPos(o + ImVec2(16 * scale, 20 * scale));
            ImGui::BeginChild("##side", ImVec2(side_w - 32 * scale, std::max(side_h - foot - 8 * scale, 40 * scale)), 0,
                              ImGuiWindowFlags_NoBackground);
            const float w = ImGui::GetContentRegionAvail().x;
            ui::logo(kName, BBHOST_VERSION, w, vp->WorkSize.y < 700 * scale);
            if (check_updates && updater::update_available()) {
                // A newer release: its tag, a click away from its page.
                const std::string tag = updater::latest_tag() + " が利用できます";
                const float bw = ImGui::CalcTextSize(tag.c_str()).x + st.FramePadding.x * 2;
                ImGui::SetCursorPosX((w - bw) * 0.5f);
                ImGui::PushStyleColor(ImGuiCol_Text, ui::kWarn);
                if (ui::ghost_button(tag.c_str())) updater::open_release_page();
                ImGui::PopStyleColor();
                ui::tooltip_for_item(nullptr, "リリースページを開く");
            }
            ImGui::Dummy(ImVec2(0, 8 * scale));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(st.ItemSpacing.x, 3 * scale));
            for (int i = 0; i < kPageCount; ++i) {
                ImU32 dot = 0;
                char badge[16] = {};
                // The Game page holds the account too: a sign-in the server no
                // longer takes marks it (the check, above).
                if (i == kPageGame && (!(app0_ok && eboot.ok) || si.refused)) dot = ui::kWarn;
                if (i == kPageShaders) {
                    if (cv.running && cv.total) std::snprintf(badge, sizeof(badge), "%d%%", static_cast<int>(100 * cv.done / cv.total));
                    else if (recommend_compile) dot = ui::kWarn;
                }
                if (ui::nav_item(kPages[i].name, kPages[i].icon, page == i, dot, badge[0] ? badge : nullptr)) go(i);
            }
            ImGui::PopStyleVar();
            nav_h = ImGui::GetCursorPosY();
            ImGui::EndChild();
            ImGui::SetCursorScreenPos(o + ImVec2(16 * scale, 20 * scale + side_h - foot));
            ImGui::BeginChild("##foot", ImVec2(w, foot), 0, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            // The community's Discord and its website, a press away on every
            // page: each opens in the browser.
            {
                const float half = (w - st.ItemSpacing.x) * 0.5f;
                for (int i = 0; i < 2; ++i) {
                    const links::Link& l = *links::kAll[i];
                    if (i) ImGui::SameLine();
                    if (ui::ghost_button(l.label, i == 0 ? ui::Icon::Chat : ui::Icon::Online, ImVec2(half, 0))) open_link(l);
                    link_mid[i] = (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) * 0.5f;
                    ui::tooltip_for_item(l.title, l.tip, l.url);
                }
            }
            if (keys) {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(st.ItemSpacing.x, 6 * scale));
                ui::key_hint("F9", "ゲーム内のパッチ");
                ui::key_hint("F10", "ゲーム内メニュー");
                ImGui::PopStyleVar();
                ImGui::Dummy(ImVec2(0, 4 * scale));
            }
            play = ui::play_button("プレイ", ImVec2(w, play_h), can_play) || (test && !std::strcmp(test, "play") && ++frames > 30);
            if (!can_play && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ui::tooltip(nullptr, game                ? "ゲームの終了後、この画面に戻ります。"
                                     : compiling         ? "シェーダーのコンパイルが終わるまでお待ちください。"
                                     : nat_view.running  ? "接続テストがゲームのポートを使用中です。数秒後に開始できます。"
                                                         : "対応するゲームフォルダーと 1.09 の eboot を指定してください。");
            ImGui::Dummy(ImVec2(0, 4 * scale));
            const float half = (w - st.ItemSpacing.x) * 0.5f;
            if (ui::ghost_button("保存", ImVec2(half, 0))) save();
            ui::tooltip_for_item(nullptr, "保存先: " + config_user_file() +
                                              (config_saves_locally() ? "。bbhost と同じ場所の設定が優先されます。共通設定: " + config_user_dir() + "/bbhost.toml."
                                                                      : "。"));
            ImGui::SameLine();
            if (ui::ghost_button("終了", ImVec2(half, 0)) || (test && !std::strcmp(test, "quit") && ++frames > 30)) done = true;
            // One line under them: why Play waits, or what Save did, or else
            // how the sign-in stands (the check, above).
            std::string note;
            ImU32 note_col = ui::kTextDim;
            bool sign_in_note = false;
            if (!saved_note.empty() && (!saved_ok || ImGui::GetTime() - saved_at < 4.0)) {
                note = saved_note;
                note_col = saved_ok ? ui::kGood : ui::kBad;
            } else if (game) {
                note = "ゲームを実行中";
            } else if (compiling) {
                note = "シェーダーをコンパイル中";
            } else if (nat_view.running) {
                note = "接続テストの完了待ち";
            } else if (!can_play) {
                note = "ゲームフォルダーと 1.09 の eboot が必要です";
                note_col = ui::kWarn;
            } else if (!si.foot.empty()) {
                note = si.foot;
                note_col = si.col;
                sign_in_note = true;
            }
            if (!note.empty()) {
                ImGui::PushFont(nullptr, 13.5f);
                note = ui::fit_text(note, w);
                const float nw = ImGui::CalcTextSize(note.c_str()).x;
                ImGui::SetCursorPosX(std::max(0.0f, (w - nw) * 0.5f));
                ImGui::PushStyleColor(ImGuiCol_Text, note_col);
                ImGui::TextUnformatted(note.c_str());
                ImGui::PopStyleColor();
                ImGui::PopFont();
                if (!saved_ok) ui::tooltip_for_item(nullptr, saved_note);
                if (sign_in_note && (si.refused || si.unchecked || !si.in)) ui::tooltip_for_item(nullptr, si.refused ? kRefusedNotice : si.line);
            }
            ImGui::EndChild();
        }
        if (!test_nat.empty() && ++frames > 30) {
            if (nat_hook_stage == 0) {
                // natcard: once the sign-in check has had its answer (or there is no sign-in).
                const net::AccountCheckState cs = net::account_check().state;
                const bool checked = !net::account_logged_in() || (cs != net::AccountCheckState::None && cs != net::AccountCheckState::Checking);
                if (nat_hook_test) {
                    start_nat_test();
                    nat_hook_stage = 1;
                } else if (checked || frames > 300) {
                    nat_hook_stage = 2;
                    nat_hook_at = ImGui::GetTime() + 1.0;
                }
            } else if (nat_hook_stage == 1 && !nat_view.running && nat_view.finished) {
                host_log("setup: connection test test: %s", net::nat::log_line(nat_view.outcome).c_str());
                nat_hook_stage = 2;
                nat_hook_at = ImGui::GetTime() + 1.0;
            }
            if (nat_hook_stage == 2) nat_scroll = true;
        }
        if (!test_relay.empty() && ++frames > 30) {
            // The relay switch's walk (relay:, relayfail:), a step at a time.
            const net::AccountCheck ac = net::account_check();
            const net::RelayChange rc = net::account_relay_change();
            const double now = ImGui::GetTime();
            const bool known = ac.state == net::AccountCheckState::Valid && ac.relay.known;
            auto snap = [&](const char* name, int next) {
                hook_shot = test_relay + "/" + name + ".png";
                hook_shot_at = now;
                relay_hook_step = next;
            };
            nat_scroll = true;
            switch (relay_hook_step) {
            case 0:  // the sign-in check's answer, with the switch
                if (known) {
                    host_log("setup: relay test: the switch reads %s", ac.relay.direct ? "off" : "on");
                    relay_hook_step = relay_hook_fail ? 10 : 1;
                    relay_hook_at = now + 1.0;
                    if (relay_hook_fail) {
                        // The account server out of reach (nothing listens on port 9 here).
                        const HostConfig& c = config();
                        config_set_online(c.online_host, c.online_scheme, c.online_verify_tls, c.online_require_account, "http://127.0.0.1:9");
                    }
                } else if (frames > 600) {
                    host_log("setup: relay test: no relay switch (the sign-in check: %d)", static_cast<int>(ac.state));
                    done = true;
                }
                break;
            case 1:
                if (now >= relay_hook_at) snap("relay-1-start", 2);
                break;
            case 2:
                relay_press = 1;
                relay_hook_step = 3;
                relay_hook_at = now + 0.8;
                break;
            case 3:
                if (now < relay_hook_at) break;
                if (relay_confirm) {
                    snap("relay-2-warning", 4);
                    relay_hook_at = now + 0.6;
                } else {
                    host_log("setup: relay test: off at once (this PC's last test showed others can get in)");
                    relay_hook_step = 5;
                    relay_hook_at = now + 20.0;
                }
                break;
            case 4:
                if (now >= relay_hook_at) {
                    relay_press = 2;
                    relay_hook_step = 5;
                    relay_hook_at = now + 20.0;
                }
                break;
            case 5:
            case 8:  // the server's answer and the check after it
                if ((rc.state == net::RelayChangeState::Saved && known && ac.relay.direct == (relay_hook_step == 5)) ||
                    rc.state == net::RelayChangeState::Failed || now > relay_hook_at) {
                    host_log("setup: relay test: %s; the switch reads %s", rc.state == net::RelayChangeState::Saved ? "saved" : rc.why.c_str(),
                             known ? (ac.relay.direct ? "off" : "on") : "not known");
                    relay_hook_step = relay_hook_step == 5 ? 6 : 9;
                    relay_hook_at = now + 0.8;
                }
                break;
            case 6:
                if (now >= relay_hook_at) snap("relay-3-off", 7);
                break;
            case 7:
                relay_press = 3;
                relay_hook_step = 8;
                relay_hook_at = now + 20.0;
                break;
            case 9:
                if (now >= relay_hook_at) snap("relay-4-on", 99);
                break;
            case 10:
                if (now >= relay_hook_at) {
                    relay_press = 1;
                    relay_hook_step = 11;
                }
                break;
            case 11:
                if (relay_confirm) relay_press = 2;
                relay_hook_step = 12;
                relay_hook_at = now + 20.0;
                break;
            case 12:
                if (rc.state == net::RelayChangeState::Failed || now > relay_hook_at) {
                    host_log("setup: relay test: %s", rc.state == net::RelayChangeState::Failed ? ("not changed: " + rc.why).c_str() : "no answer");
                    relay_hook_step = 13;
                    relay_hook_at = now + 0.6;
                }
                break;
            case 13:
                if (now >= relay_hook_at) snap("relay-fail", 99);
                break;
            default: break;
            }
        }
        if (!test_port.empty() && ++frames > 30) {
            // The ports typed one after another, each saved once shown.
            const double now = ImGui::GetTime();
            nat_scroll = true;
            if (port_hook_at == 0.0) {
                port_hook_at = now + 0.6;
                if (port_hook_next < port_hook.size()) take_p2p_port(port_hook[port_hook_next]);
            } else if (now >= port_hook_at && hook_shot.empty()) {
                if (port_hook_next < port_hook.size()) {
                    hook_shot = test_port + "/port-" + std::to_string(port_hook[port_hook_next]) + ".png";
                    hook_shot_at = now;
                    ++port_hook_next;
                    port_hook_at = 0.0;
                } else {
                    done = true;
                }
            }
        }
        if (!test_links.empty() && ++frames > 30) {
            // The links' walk: the pointer on each, its bubble saved, pressed.
            const double now = ImGui::GetTime();
            if (links_step == 0) {
                links_step = 1;
                links_at = now + 1.2;
            } else if ((links_step == 1 || links_step == 5) && now >= links_at) {
                hook_shot = test_links + (links_step == 1 ? "/link-discord.png" : "/link-website.png");
                hook_shot_at = now;
                ++links_step;
            } else if (links_step == 4 && now >= links_at) {
                links_step = 5;
                links_at = now + 1.2;
            } else if (links_step == 8 && now >= links_at) {
                done = true;
            }
        }
        if (test_link && ++frames > 30) {
            if (!test_created) {
                test_created = true;
                apply_server();
                host_account_action(0, "", "");
            } else if (host_account_detail().rfind("コード ", 0) == 0 || !host_account_busy()) {
                host_log("setup: link test: %s; %s", host_account_outcome().c_str(), host_account_detail().c_str());
                host_account_action(4, "", "");
                done = true;
            }
        }
        if (!test_create.empty() && ++frames > 30) {
            if (!test_created) {
                test_created = true;
                apply_server();
                host_account_action(1, test_create, "");
            } else if (!host_account_busy()) {
                // ... and once the new sign-in's check has its answer.
                const net::AccountCheckState cs = net::account_check().state;
                if (!net::account_logged_in() || (cs != net::AccountCheckState::None && cs != net::AccountCheckState::Checking)) {
                    // The recovery code is never logged, a test's neither.
                    host_log("setup: account test: %s; %s", host_account_outcome().c_str(),
                             host_account_detail().empty() ? "no code shown" : "a recovery code shown (not logged)");
                    if (host_account_take_save()) host_options_save_now();
                    done = true;
                }
            }
        }

        // The page: its heading, then itself, fading in.
        ImGui::SetCursorScreenPos(vp->WorkPos + ImVec2(side_w, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(30 * scale, 24 * scale));
        ImGui::BeginChild("##page", ImVec2(vp->WorkSize.x - side_w, vp->WorkSize.y), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoBackground);
        ImGui::PopStyleVar();
        const ImVec2 head = ImGui::GetCursorScreenPos();
        const float head_right = head.x + ImGui::GetContentRegionAvail().x;
        ImGui::PushFont(ui::font_strong(), 27.0f);
        ImGui::TextUnformatted(kPages[page].name);
        const float title_h = ImGui::GetTextLineHeight();
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 6 * scale));
        // Buttons at the heading's right, `w` wide in all: the cursor goes
        // there and comes back with the returned position.
        auto head_actions = [&](float w) {
            const ImVec2 back = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos(ImVec2(head_right - w, head.y + (title_h - ImGui::GetFrameHeight()) * 0.5f));
            return back;
        };
        const float shown = animations ? static_cast<float>(ImGui::GetTime() - page_at) : 1.0f;
        const float fade = 1.0f - std::pow(1.0f - std::min(1.0f, shown / 0.22f), 3.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::max(0.02f, fade));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (1.0f - fade) * 10 * scale);
        const float gap = 14 * scale;
        const float label_w = std::min(118 * scale, (head_right - head.x) * 0.3f);  // a large Menu text size leaves the fields room

        // A path's row: its state (-1 none, 0 wrong, 1 right; `tip` says
        // which), its label (`label_tip` in its bubble), the field and the
        // picker's button.
        auto path_row = [&](const char* label, int field, bool folder, const char* hint, int state, const std::string& tip,
                            const std::string& label_tip = {}) {
            ImGui::PushID(field);
            if (state >= 0) {
                ui::status_icon(state == 1, tip.empty() ? nullptr : tip.c_str());
                ImGui::SameLine(0, 8 * scale);
            }
            ui::row_label(label, label_w);
            if (!label_tip.empty()) ui::tooltip_for_item(nullptr, label_tip);  // the label is still the last item
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - st.ItemSpacing.x);
            const bool changed = ImGui::InputTextWithHint("##p", hint, f[field].text, sizeof(f[field].text));
            ImGui::SameLine();
            if (ui::icon_button("##browse", folder ? ui::Icon::Folder : ui::Icon::File, 0.0f, folder ? "フォルダーを選択" : "ファイルを選択")) {
                void* ud = reinterpret_cast<void*>(static_cast<std::intptr_t>(field));
                const std::string at = f[field].str();
                if (folder) {
                    SDL_ShowOpenFolderDialog(on_pick, ud, win, at.empty() ? nullptr : at.c_str(), false);
                } else {
                    SDL_ShowOpenFileDialog(on_pick, ud, win, nullptr, 0, at.empty() ? nullptr : at.c_str(), false);
                }
            }
            ImGui::PopID();
            return changed;
        };
        // A problem under a row, lined up with its field.
        auto problem = [&](const std::string& text) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetFrameHeight() * 0.72f + 8 * scale + label_w);
            dim_text(text, ui::kWarn);
        };
        // Equal cards side by side: the height of a caption and `rows` rows.
        auto card_h = [&](float rows) {
            return 2 * 14 * scale + 11.5f * scale + 4 * scale + st.ItemSpacing.y + rows * ImGui::GetFrameHeightWithSpacing() - st.ItemSpacing.y;
        };

        // The settings moved to their new folder: what moved, from where to
        // where, what stayed or went beside what was there, until Dismiss.
        if (!moves.empty()) {
            ui::card_begin("##moved", ImVec2(ImGui::GetContentRegionAvail().x, 0), "設定フォルダー");
            const float fh = ImGui::GetFrameHeight();
            for (std::size_t i = 0; i < moves.size(); ++i) {
                const ConfigMove& m = moves[i];
                if (i) ImGui::Dummy(ImVec2(0, 6 * scale));
                const ImVec2 at = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(fh * 0.8f, fh));
                const bool failed = !m.failed.empty();
                ui::draw_icon(ImGui::GetWindowDrawList(), failed ? ui::Icon::Warning : ui::Icon::Folder, at + ImVec2(fh * 0.4f, fh * 0.5f),
                              fh * 0.62f, failed ? ui::kWarn : ui::kIce);
                ImGui::SameLine(0, 8 * scale);
                ImGui::AlignTextToFramePadding();
                ImGui::PushFont(ui::font_strong(), 0.0f);
                ImGui::TextUnformatted(config_move_heading(m).c_str());
                ImGui::PopFont();
                if (moves.size() > 1) {
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text, ui::kTextFaint);
                    ImGui::TextUnformatted(m.when.c_str());
                    ImGui::PopStyleColor();
                }
                bool first = true;
                for (const ConfigMoveLine& l : config_move_lines(m)) {
                    dim_text(l.text, l.warn ? ui::kWarn : first ? ui::kText : ui::kTextDim, false);
                    first = false;
                }
            }
            ImGui::Dummy(ImVec2(0, 2 * scale));
            // The folder in use, and Dismiss, at the right.
            const std::string folder = moves.back().failed.empty() ? moves.back().to : moves.back().from;
            const char* open_label = "フォルダーを開く";
            const float dw = ImGui::CalcTextSize("閉じる").x + st.FramePadding.x * 3,
                        ow = ImGui::CalcTextSize(open_label).x + st.FramePadding.x * 2;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - dw - ow - st.ItemSpacing.x));
            if (ui::ghost_button(open_label)) SDL_OpenURL(("file:///" + generic(folder)).c_str());
            ui::tooltip_for_item(nullptr, generic(folder));
            ImGui::SameLine();
            if (ui::primary_button("閉じる##moved", ImVec2(dw, 0))) {
                config_moves_dismiss();
                moves.clear();
            }
            ui::tooltip_for_item(nullptr, "この通知を閉じます。設定は変更しません。");
            ui::card_end();
            ImGui::Dummy(ImVec2(0, gap - st.ItemSpacing.y));
        }

        if (page == kPageGame) {
            const float w = ImGui::GetContentRegionAvail().x;
            // Why the window opened, until the paths are right: a welcome
            // when nothing is set yet.
            if (!reason.empty() && !(app0_ok && eboot.ok)) {
                if (cfg.app0.empty() && cfg.eboot.empty())
                    ui::banner(ui::Icon::Moon, ui::kIce, "ゲームフォルダーと 1.09 の eboot を指定し、「プレイ」を選んでください。");
                else
                    ui::banner(ui::Icon::Warning, ui::kWarn, capitalised(reason));
                ImGui::Dummy(ImVec2(0, 2 * scale));
            }
            ui::card_begin("##files", ImVec2(w, 0), "ゲームファイル");
            {
                std::string bad;
                if (!edition_error.empty()) bad = edition_error;
                else if (app0.empty()) bad = "";
                else if (!is_dir(app0)) bad = "フォルダーがありません。";
                else if (!app0_ok && (is_dir(app0 + "/../dvdroot_ps4") || fs::path(app0).filename() == "dvdroot_ps4"))
                    bad = "dvdroot_ps4 を含む親フォルダーを選んでください。";
                else if (!app0_ok) bad = "dvdroot_ps4 がありません。";
                else if (app0_ver.app_ver.empty()) bad = "sce_sys/param.sfo がありません。ゲームフォルダー全体を指定してください。";
                else if (app0_ver.app_ver != "01.09")
                    bad = "バージョン " + app0_ver.app_ver +
                          (app0_ver.update.empty() ? std::string() : " (in " + fs::path(app0_ver.update).filename().string() + ")") +
                          "。更新 1.09 を適用してください。未適用では読み込み中に停止します。";
                const bool good = app0_ok && app0_ver.app_ver == "01.09";
                const std::string tip = good ? (app0_ver.update.empty() ? std::string("dvdroot_ps4 と更新 1.09 を確認しました。")
                                                                        : "dvdroot_ps4 と更新 1.09 を確認しました（更新元: " +
                                                                              fs::path(app0_ver.update).filename().string() + ").")
                                             : app0.empty() ? std::string("未設定。dvdroot_ps4 を含むフォルダーを指定してください。") : bad;
                path_row("ゲームフォルダー", kApp0, true, "dvdroot_ps4 を含むフォルダー", good ? 1 : 0, tip);
                if (!bad.empty()) problem(bad);
            }
            {
                if (path_row("Eboot", kEboot, false, "復号済みの更新 1.09 の eboot.bin", eboot.ok ? 1 : 0,
                             eboot.ok ? "1.09 の eboot を確認しました。"
                             : f[kEboot].str().empty() ? "未設定。復号済みの更新 1.09 の eboot.bin を指定してください。"
                                                       : "対応する 1.09 の eboot ではありません。") ||
                    (eboot.path != f[kEboot].str() && !ImGui::IsAnyItemActive())) {
                    eboot = check_eboot(f[kEboot].str());
                }
                if (!f[kEboot].str().empty() && !eboot.ok)
                    problem(!is_file(f[kEboot].str()) ? std::string("ファイルがありません。")
                                                      : "対応する 1.09 の eboot ではありません（SHA-256 " + eboot.sha.substr(0, 16) + "...).");
            }
            ui::card_end();
            ImGui::Dummy(ImVec2(0, gap - st.ItemSpacing.y));

            // The display, and online at a glance, side by side.
            const float row_lw = std::min(104 * scale, (w - gap) * 0.19f);  // a third of the card at most
            // The display, and online beside it: the server and the account, the
            // way F10 > ACCOUNT has it in the game (no password - this PC linked to
            // the account signed into on the website with Discord, or an account
            // held by this PC with a recovery code). The two cards share the
            // taller one's height.
            const std::string acct_detail = host_account_detail();
            // The sign-in check's lines: a refusal's notice over the ways back
            // in, and an unchecked sign-in's line, which wraps.
            const float online_rows = 8.5f + (server == 2 ? 4.0f : 0.0f) + (acct_detail.empty() ? 0.0f : 1.0f) +
                                      (cfg.profile.empty() ? 0.0f : 1.0f) + (si.in && si.refused ? 3.5f : 0.0f) +
                                      (si.in && si.unchecked ? 1.0f : 0.0f);
            // Stay borderless's switch while Borderless is chosen (Windows).
#if defined(_WIN32)
            const bool stay_borderless_row = window_mode == 1;
#else
            const bool stay_borderless_row = false;
#endif
            // The display's rows: Window, Frame rate, Native frame clock, Effect
            // timing, Resolution, Frame generation (Windows), Skip company logos
            // and Performance mode, with a warning line or two under Frame rate
            // and Frame generation (frame generation's limit).
#if defined(_WIN32)
            const float display_rows = 8.0f;
#else
            const float display_rows = 7.0f;
#endif
            const float cards_h =
                card_h(std::max(display_rows + (stay_borderless_row ? 1.0f : 0.0f) +
                                    1.5f * static_cast<float>(host_frame_rate_warning_count(cap_choice, cap_custom, cap_custom_fps)),
                                online_rows));
            ui::card_begin("##display", ImVec2(std::round((w - gap) * 0.56f), cards_h), "画面");
            {
                ui::row_label("画面モード", row_lw);
                static const char* const kModes[] = {"ウィンドウ", "ボーダーレス", "全画面"};
                ui::segmented("##mode", &window_mode, kModes, 3, ImGui::GetContentRegionAvail().x);
                ui::tooltip_for_item("画面モード",
#if defined(_WIN32)
                                     "ウィンドウは移動・サイズ変更ができます。ボーダーレスはデスクトップの解像度で画面を埋め、素早く切り替えられます。「ボーダーレスを維持」でドライバーによる排他的全画面への切替を防ぎます。全画面は指定解像度に近い表示モードを使用します。AMD ではウィンドウ表示の負荷が高くなるため、ボーダーレスか全画面を推奨します。"
#else
                                     "ボーダーレスはデスクトップの解像度で画面を埋めます。全画面は指定解像度に近い表示モードへ切り替えます。"
#endif
                );
#if defined(_WIN32)
                // Stay borderless (F10's row under Window mode): Borderless
                // that the graphics driver never makes exclusive fullscreen
                // (host/window.cpp). Shown while Borderless is chosen, as in F10.
                if (window_mode == 1)
                    ui::toggle_row("ボーダーレスを維持", nullptr, &stay_borderless,
                                   "有効にすると排他的な全画面への切替を防ぎます。別ウィンドウに切り替えた際など、ゲームは動いているのに映像だけ数秒止まる場合に試してください。AMD では描画負荷が増える場合があるため、必要なときだけ有効にしてください。");
#endif
                ui::row_label("フレームレート", row_lw);
                // The list, or Custom limit's number, and its tick box - F10's own control.
                host_frame_rate_control(&cap_choice, &cap_custom, &cap_custom_fps, ImGui::GetContentRegionAvail().x);
                ui::toggle_row("実フレーム時間で動作（試験的）", nullptr, &frame_clock,
                               "再起動が必要です。設定したフレームレートを維持できない場合も、実際のフレーム時間に合わせてゲームを動かします。");
                ui::row_label("エフェクトの時間処理", row_lw);
                ImGui::SetNextItemWidth(-FLT_MIN);
                {
                    const HostEffectTimingRow et = host_effect_timing_row();
                    const char* labels[4] = {kHostEffectTimingLabels[0], kHostEffectTimingLabels[1], kHostEffectTimingLabels[2],
                                             et.custom.c_str()};
                    const char* tips[4] = {kHostEffectTimingTips[0], kHostEffectTimingTips[1], kHostEffectTimingTips[2], et.tip.c_str()};
                    int shown = effect_timing >= 0 ? effect_timing : et.index;
                    if (ui::combo("##effects", &shown, labels, et.index == 3 ? 4 : 3, tips) && shown < 3) effect_timing = shown;
                }
                ui::tooltip_for_item("エフェクトの時間処理", kHostEffectTimingNote);
                // Resolution: the Graphics page's own setting (host/options.h),
                // changed and saved at once there and here alike.
                ui::row_label("解像度", row_lw);
                ImGui::SetNextItemWidth(-FLT_MIN);
                {
                    int n = 0;
                    const char* const* labels = host_opt_resolution_labels(&n);
                    int res = host_opt_resolution_index();
                    if (ui::combo("##resolution", &res, labels, n)) host_opt_set_resolution_index(res);
                }
                if (const char* note = host_opt_resolution_note()) ui::tooltip_for_item("解像度", note);
                // Frame generation: the same setting as the Graphics page's,
                // changed and saved at once. AMD's frame generation, Windows' alone:
                // no row elsewhere (host/options.cpp keeps it off there).
#if defined(_WIN32)
                {
                    bool fg = host_opt_get("frame_generation");
                    if (ui::toggle_row("フレーム生成", nullptr, &fg,
                                       "AMD FidelityFX で実描画の間に１枚を生成します。3D のみを補間し、HUD とメニューは補間しません。拡大描画が無効でも使用できます。実描画は画面更新の半分以下（180 Hz なら90+90）になり、高すぎる上限は自動調整します。「画面に合わせる」も同様です。ロード画面は上限なしです。"))
                        host_opt_set("frame_generation", fg);
                    // While on, a Frame rate above half the refresh rate comes down to it (F10: as it turns on).
                    if (fg) host_frame_rate_fit(&cap_choice, &cap_custom, &cap_custom_fps);
                    host_frame_generation_warning(cap_choice, cap_custom, cap_custom_fps);
                }
#endif
                ui::toggle_row("企業ロゴをスキップ", nullptr, &skip_logos,
                               "起動時の企業ロゴと前回終了時の警告を省略します。");
                ui::toggle_row("パフォーマンスモード", nullptr, &performance_mode,
                               "SSAO、ブラー、輪郭補正、被写界深度、色収差、ブルーム、動的な光源の影を省いて高速化します。太陽・月の影と霧は維持します。F10 から変更できます。");
            }
            ui::card_end();
            ImGui::SameLine(0, gap);
            ui::card_begin("##online", ImVec2(0, cards_h), "オンライン");
            {
                const float lw = std::min(72 * scale, ImGui::GetContentRegionAvail().x * 0.3f);
                ui::row_label("サーバー", lw);
                static const char* const kServerLabels[] = {kServers[0].label, kServers[1].label, "カスタム"};
                ImGui::SetNextItemWidth(-FLT_MIN);
                ui::combo("##server", &server, kServerLabels, 3);
                ui::tooltip_for_item(nullptr, server == 2 ? (f[kHost].str().empty() ? std::string("自分のサーバーのホスト名を下に入力します。")
                                                                                    : f[kHost].str())
                                                          : std::string(kServers[server].host));
                if (server == 2) {
                    ui::row_label("ホスト", lw);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::InputTextWithHint("##host", "server.example.com", f[kHost].text, sizeof(f[kHost].text));
                    ui::row_label("通信方式", lw);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::InputText("##scheme", scheme, sizeof(scheme));
                    ui::toggle_row("サーバーの証明書を検証する", nullptr, &verify_tls);
                    ui::toggle_row("アカウントを必須にする", nullptr, &require_account, "オンラインには、この画面か F10 のアカウント欄でログインします。");
                }
                if (!cfg.profile.empty()) dim_text("今回の起動設定: " + cfg.profile_config + "。設定内容によっては別のサーバーを使用します。");
                ImGui::Dummy(ImVec2(0, 4 * scale));
                // Who is signed in and how its sign-in stands (the check), with
                // Sign out (Cancel while a Discord link waits for approval) at
                // the right and Retry beside it when the check had no answer; a
                // sign-in the server no longer takes says so under it, over the
                // ways back in.
                const bool busy = host_account_busy();
                const bool in = si.in;
                const float av = std::round(ImGui::GetFrameHeight() * 1.5f);
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const float right = at.x + ImGui::GetContentRegionAvail().x;
                const char* out_label = busy ? "キャンセル" : "ログアウト";
                const float bw = ImGui::CalcTextSize(out_label).x + st.FramePadding.x * 3;
                const char* retry = in && si.unchecked && !busy ? "再試行" : nullptr;
                const float rw = retry ? ImGui::CalcTextSize(retry).x + st.FramePadding.x * 3 + st.ItemSpacing.x : 0.0f;
                ui::avatar(in ? net::account_name() : std::string(), av);
                ImGui::SameLine(0, 12 * scale);
                ImGui::BeginGroup();
                ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, at.y + 1 * scale));
                // The words stop short of the buttons.
                ImGui::PushTextWrapPos(right - bw - rw - 10 * scale - ImGui::GetWindowPos().x);
                ImGui::PushFont(ui::font_strong(), 0.0f);
                ImGui::TextUnformatted(in ? net::account_name().c_str() : "未ログイン");
                ImGui::PopFont();
                // The last action's outcome while it says something of its own
                // (a code asked for, a refusal); else how the sign-in stands.
                const std::string outcome = host_account_outcome();
                const bool plain = outcome.empty() || outcome == host_account_signed_in();
                ImGui::PushFont(nullptr, 14.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, plain ? si.col : ui::kTextDim);
                ImGui::TextUnformatted(!plain ? outcome.c_str() : si.line.c_str());
                ImGui::PopStyleColor();
                ImGui::PopFont();
                ImGui::PopTextWrapPos();
                ImGui::EndGroup();
                const float text_bottom = ImGui::GetItemRectMax().y;
                const float button_y = at.y + (av - ImGui::GetFrameHeight()) * 0.5f;
                if (retry) {
                    ImGui::SetCursorScreenPos(ImVec2(right - bw - rw, button_y));
                    if (ui::ghost_button(retry)) check_sign_in();
                    ui::tooltip_for_item(nullptr, "サーバーへ再問い合わせします。");
                }
                ImGui::SetCursorScreenPos(ImVec2(right - bw, button_y));
                ImGui::BeginDisabled(!busy && !in);
                if (ui::ghost_button(out_label)) {
                    apply_server();
                    host_account_action(4, "", "");
                }
                ImGui::EndDisabled();
                ui::tooltip_for_item(nullptr, busy ? "承認待ちの Discord 連携を中止します。" : "この PC のトークンを削除し、ログアウトします。");
                ImGui::SetCursorScreenPos(ImVec2(at.x, std::max(at.y + av, text_bottom)));
                ImGui::Dummy(ImVec2(0, 2 * scale));
                if (in && si.refused && !busy) {
                    ui::banner(ui::Icon::Warning, ui::kWarn, kRefusedNotice);
                    ImGui::Dummy(ImVec2(0, 2 * scale));
                }
                // A code to type on the website, or a recovery code to keep.
                if (!acct_detail.empty()) {
                    ImGui::AlignTextToFramePadding();
                    ImGui::PushStyleColor(ImGuiCol_Text, ui::kIce);
                    ImGui::PushFont(ui::font_strong(), 0.0f);
                    ImGui::TextUnformatted(acct_detail.c_str());
                    ImGui::PopFont();
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    if (ui::icon_button("##copy", ui::Icon::Copy, 0.0f, "コードをコピー")) {
                        // The code alone: the word after "コード" or "code".
                        std::string code = acct_detail;
                        for (const char* lead : {"コード ", "復旧コード ", "ウェブ用コード "}) {
                            if (acct_detail.rfind(lead, 0) == 0) code = acct_detail.substr(std::strlen(lead));
                        }
                        code = code.substr(0, code.find("  "));
                        SDL_SetClipboardText(code.c_str());
                    }
                }
                const float full = ImGui::GetContentRegionAvail().x;
                const float half = (full - st.ItemSpacing.x) * 0.5f;
                ImGui::BeginDisabled(busy);
                // The way in for most players: lit until someone is signed in,
                // and again when the server no longer takes the sign-in.
                if (in && !si.refused ? ui::ghost_button("Discord でログイン", ImVec2(half, 0))
                                      : ui::primary_button("Discord でログイン", ImVec2(half, 0))) {
                    apply_server();
                    host_account_action(0, "", "");
                }
                ui::tooltip_for_item(nullptr, "サイトを開きます。Discord でログインし、コードを承認してください。");
                ImGui::SameLine();
                ImGui::BeginDisabled(!in || si.refused);  // a code needs a sign-in the server still takes
                if (ui::ghost_button("ウェブサイトのログインコード", ImVec2(half, 0))) {
                    apply_server();
                    host_account_action(3, "", "");
                }
                ImGui::EndDisabled();
                ui::tooltip_for_item(nullptr, "このアカウントでサイトにログインするためのコードです。");
                // Or an account kept on this PC: a name to create it under, and
                // its recovery code to bring it back.
                const float btn = std::max(ImGui::CalcTextSize("アカウントを作成").x, ImGui::CalcTextSize("復旧").x) + st.FramePadding.x * 3;
                const float field = std::max(40 * scale, full - btn - st.ItemSpacing.x);
                ImGui::SetNextItemWidth(field);
                ImGui::InputTextWithHint("##acct_name", "アカウント名", acct_name, sizeof(acct_name));
                ImGui::SameLine();
                if (ui::ghost_button("アカウントを作成", ImVec2(btn, 0))) {
                    apply_server();
                    host_account_action(1, acct_name, "");
                }
                ui::tooltip_for_item(nullptr, "指定した名前でアカウントを作成します。表示される復旧コードを控えてください。");
                ImGui::SetNextItemWidth(field);
                ImGui::InputTextWithHint("##acct_code", "復旧コード", acct_code, sizeof(acct_code));
                ImGui::SameLine();
                if (ui::ghost_button("復旧", ImVec2(btn, 0))) {
                    apply_server();
                    host_account_action(2, acct_name, acct_code);
                    acct_code[0] = 0;
                }
                ui::tooltip_for_item(nullptr, "名前と復旧コードを使って、この PC にアカウントを復旧します。");
                ImGui::EndDisabled();
            }
            ui::card_end();

            // The connection test (net/nat_check.h): can other hunters reach
            // this PC directly, or does co-op go through the server's relay,
            // and does a packet nobody asked for get in. One headline with its
            // status, what it means, the thing to do and whether others can
            // get in; the technical lines under Details. Under them the P2P
            // port setting and, signed in, the account's relay switch.
            ImGui::Dummy(ImVec2(0, gap - st.ItemSpacing.y));
            ui::card_begin("##connection", ImVec2(w, 0), "接続");
            {
                namespace nat = net::nat;
                nat::Card card = nat_card;
                ImU32 col = ui::kText;
                ui::Icon icon = ui::Icon::Online;
                if (nat_view.running) {
                    card = nat::Card{};
                    card.head = "接続をテスト中…";
                    card.sentence = std::string(nat_view.step) + " From UDP port " + std::to_string(p2p_port) + "; a few seconds.";
                    icon = ui::Icon::Refresh;
                } else if (card.tone == nat::Tone::Good) {
                    col = ui::kGood;
                    icon = ui::Icon::Check;
                } else if (card.tone == nat::Tone::Warn) {
                    col = ui::kWarn;
                    icon = ui::Icon::Warning;
                } else if (card.tone == nat::Tone::Bad) {
                    col = ui::kBad;
                    icon = ui::Icon::Problem;
                }
                const float fh = ImGui::GetFrameHeight();
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const float right = at.x + ImGui::GetContentRegionAvail().x;
                // Numbered steps, each wrapping under its own words.
                auto numbered = [&](const std::vector<std::string>& steps, ImU32 num_col) {
                    const float num_w = ImGui::CalcTextSize("0.").x + 7 * scale;
                    for (std::size_t i = 0; i < steps.size(); ++i) {
                        const ImVec2 p0 = ImGui::GetCursorScreenPos();
                        ImGui::SetCursorScreenPos(ImVec2(p0.x + 6 * scale, p0.y));
                        ImGui::PushStyleColor(ImGuiCol_Text, num_col);
                        ImGui::TextUnformatted((std::to_string(i + 1) + ".").c_str());
                        ImGui::PopStyleColor();
                        ImGui::SameLine(0, 0);
                        ImGui::SetCursorScreenPos(ImVec2(p0.x + 6 * scale + num_w, p0.y));
                        ImGui::TextUnformatted(steps[i].c_str());
                    }
                };
                const char* label = nat_view.running ? "テスト中…" : (nat_ours || nat_record) ? "再テスト" : "接続をテスト";
                const float bw = std::max(ImGui::CalcTextSize("接続をテスト").x, ImGui::CalcTextSize(label).x) + st.FramePadding.x * 4;
                ImGui::Dummy(ImVec2(fh * 0.8f, fh));
                ui::draw_icon(ImGui::GetWindowDrawList(), icon, at + ImVec2(fh * 0.4f, fh * 0.5f), fh * 0.62f, col);
                ImGui::SameLine(0, 8 * scale);
                ImGui::BeginGroup();
                ImGui::PushTextWrapPos(right - bw - 14 * scale - ImGui::GetWindowPos().x);
                ImGui::AlignTextToFramePadding();
                ImGui::PushFont(ui::font_strong(), 0.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, col);
                ImGui::TextUnformatted(card.head.c_str());
                ImGui::PopStyleColor();
                ImGui::PopFont();
                ImGui::PushFont(nullptr, 14.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, ui::kTextDim);
                ImGui::TextUnformatted(card.sentence.c_str());
                ImGui::PopStyleColor();
                ImGui::PopTextWrapPos();
                // The rest as wide as the card: the button is above.
                ImGui::PushTextWrapPos(right - ImGui::GetWindowPos().x);
                if (!card.incoming.empty()) {
                    ImGui::Dummy(ImVec2(0, 1 * scale));
                    ImGui::TextUnformatted(card.incoming.c_str());
                }
                if (!card.action.empty()) {
                    ImGui::Dummy(ImVec2(0, 1 * scale));
                    ImGui::PushStyleColor(ImGuiCol_Text, ui::kIce);
                    ImGui::TextUnformatted(card.action.c_str());
                    ImGui::PopStyleColor();
                }
                numbered(card.steps, ui::kIce);
                if (!nat_view.running && !card.details.empty()) {
                    ImGui::Dummy(ImVec2(0, 1 * scale));
                    ui::disclosure("詳細", &nat_details);
                    ui::tooltip_for_item(nullptr, nat_details ? "技術的な情報を隠します。" : "ポート、サーバー側の観測、結果の保存状態を表示します。");
                    if (nat_details) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ui::kTextFaint);
                        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(st.ItemSpacing.x, 3 * scale));
                        for (const std::string& d : card.details) ImGui::TextUnformatted(d.c_str());
                        ImGui::PopStyleVar();
                        ImGui::PopStyleColor();
                    }
                }
                ImGui::PopTextWrapPos();
                ImGui::PopFont();
                ImGui::EndGroup();
                const float bottom = ImGui::GetItemRectMax().y;
                ImGui::SetCursorScreenPos(ImVec2(right - bw, at.y));
                const double wait = nat_wait_until - ImGui::GetTime();
                const bool can_test = !nat_view.running && !game && wait <= 0;
                if (ui::primary_button(label, ImVec2(bw, 0), can_test)) start_nat_test();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) {
                    const std::string what = "Sends a few UDP packets from the game's P2P port (UDP " + std::to_string(p2p_port) +
                                             ") to the server's two test ports, and one the server answers from a third port, "
                                             "then asks the server what it saw. A few seconds; the game must be closed.";
                    const std::string why = game ? std::string("ゲームがポートを使用中です。終了後にここからテストしてください。")
                                            : nat_view.running ? std::string("テスト中です。")
                                            : wait > 0 && nat_view.outcome.save.state == nat::SaveReply::State::Limited
                                                ? "The server takes 10 tests an hour; try again in " + std::to_string(static_cast<int>(wait / 60) + 1) + " minutes."
                                            : wait > 0 ? "Again in " + std::to_string(static_cast<int>(wait) + 1) + " s."
                                                       : std::string(si.in ? "ログイン中のアカウントに結果を保存し、必要に応じて中継を使用します。"
                                                                           : "未ログインのため、この PC の結果のみ表示し、保存しません。");
                    ui::tooltip("接続テスト", what, why);
                }
                ImGui::SetCursorScreenPos(ImVec2(at.x, std::max(bottom, at.y + fh)));
                ImGui::Dummy(ImVec2(0, 4 * scale));

                // The settings under it, each on a row: the P2P port, and
                // (signed in, on a server with the switch) the relay.
                ImDrawList* dl = ImGui::GetWindowDrawList();
                // A rule a pixel high on the pixel grid: a thin line's anti-aliased
                // sliver left gaps under SDL's software renderer.
                const ImVec2 rule = ImGui::GetCursorScreenPos();
                const float rule_y = std::round(rule.y);
                dl->AddRectFilled(ImVec2(std::round(rule.x), rule_y), ImVec2(std::round(right), rule_y + 1.0f), ui::alpha(ui::kViolet, 58));
                ImGui::Dummy(ImVec2(0, 6 * scale));
                const float rl = std::min(92 * scale, (right - at.x) * 0.22f);
                const float field_w = std::round(std::max(ImGui::CalcTextSize("65535").x + st.FramePadding.x * 2, 84 * scale));
                const float seg_w = std::round(std::max(150 * scale, ImGui::CalcTextSize("OnOff").x + 80 * scale));
                static const char* const kPortTip =
                    "他の狩人から接続する UDP ポートです。ルーターで転送してください。同じ PC で２つ起動する場合は異なる番号が必要です。";
                const std::string port_note = "1024 to 65535; 9307 unless changed. The test runs from it at once; Save or Play keeps "
                                              "it, and the game opens it as it starts, so a change made while the game runs "
                                              "applies from its next start.";
                ui::row_label("P2P port", rl);
                ui::tooltip_for_item("P2P port", kPortTip, port_note);
                {
                    // What is being typed lives in the window's storage until it is
                    // taken (Enter, or a click elsewhere), as Frame rate's Custom limit's.
                    ImGuiStorage* store = ImGui::GetStateStorage();
                    const ImGuiID key = ImGui::GetID("##p2p_typing");
                    int typing = store->GetInt(key, p2p_port);
                    ImGui::SetNextItemWidth(field_w);
                    ImGui::InputInt("##p2p_port", &typing, 0, 0);
                    ui::tooltip_for_item("P2P port", kPortTip, port_note);
                    if (ImGui::IsItemDeactivatedAfterEdit()) take_p2p_port(typing);
                    store->SetInt(key, ImGui::IsItemActive() ? typing : p2p_port);
                }
                ImGui::SameLine(0, 12 * scale);
                ImGui::PushFont(nullptr, 14.0f);
                ImGui::AlignTextToFramePadding();
                ImGui::PushStyleColor(ImGuiCol_Text, p2p_port_note.empty() ? ui::kTextDim : ui::kBad);
                ImGui::TextUnformatted(ui::fit_text(!p2p_port_note.empty() ? p2p_port_note
                                                    : p2p_port != cfg.p2p_port
                                                        ? std::string("保存またはプレイで保存し、ゲーム開始時に開きます。")
                                                        : std::string("他の狩人からこの PC に接続するポートです。"),
                                                    right - ImGui::GetCursorScreenPos().x)
                                           .c_str());
                ImGui::PopStyleColor();
                ImGui::PopFont();
                if (!p2p_port_note.empty()) ui::tooltip_for_item("P2P port", p2p_port_note);

                // The relay switch: the account's, as the website's account page
                // has it (net/account.h). Its state is the sign-in check's; a
                // change goes to the server, and the check runs again.
                const net::RelayChange rc = net::account_relay_change();
                const bool relay_known = nat_check.state == net::AccountCheckState::Valid && nat_check.relay.known;
                if (!si.in || !(relay_known || rc.state != net::RelayChangeState::None)) relay_confirm = false;
                if (si.in && (relay_known || rc.state != net::RelayChangeState::None)) {
                    const bool saving = rc.state == net::RelayChangeState::Saving;
                    const bool reading = nat_check.state == net::AccountCheckState::Checking;
                    const bool off_now = nat_fake_relay ? nat_fake_relay > 0
                                         : (saving || (rc.state == net::RelayChangeState::Saved && !relay_known)) ? rc.direct
                                                                                                                 : nat_check.relay.direct;
                    ImGui::Dummy(ImVec2(0, 2 * scale));
                    ui::row_label("中継", rl);
                    static const char* const kRelayWebsite =
                        "協力・対人プレイをサーバー経由にします。直接接続できるとのテスト結果なら無効にできます。相手が中継を使う場合は中継経由です。同じ家庭の接続で遊ぶ全員が無効にしないと直接接続になりません。";
                    ui::tooltip_for_item("中継", kRelayWebsite, "ウェブサイトのアカウント設定と共通です。");
                    static const char* const kRelayLabels[] = {"有効", "無効"};
                    static const char* const kRelayTips[] = {
                        "サーバー経由の協力・対人プレイです。",
                        "直接接続のみです。ルーターで着信を許可する必要があります。テストの着信結果を確認してください。"};
                    int choice = off_now ? 1 : 0;
                    bool want_off = false, want_on = false;
                    ImGui::BeginDisabled(saving || reading || relay_confirm);
                    if (ui::segmented("##relay", &choice, kRelayLabels, 2, seg_w, kRelayTips,
                                      "ウェブサイトのアカウント設定と共通です。")) {
                        want_off = choice == 1;
                        want_on = choice == 0;
                    }
                    ImGui::EndDisabled();
                    if (relay_press == 1) want_off = true;
                    if (relay_press == 3) want_on = true;
                    ImGui::SameLine(0, 12 * scale);
                    std::string relay_line = off_now ? "無効：直接接続のみ。ルーターの着信許可が必要です。"
                                                     : "有効：サーバー経由で接続します。";
                    ImU32 relay_col = ui::kTextDim;
                    if (saving) {
                        relay_line = "保存中…";
                    } else if (rc.state == net::RelayChangeState::Failed) {
                        relay_line = rc.status == 0 ? "変更していません。サーバーから応答がありません（" + rc.why + ")."
                                                    : "変更していません：" + rc.why + ".";
                        relay_col = ui::kBad;
                    } else if (rc.state == net::RelayChangeState::Saved && (reading || !relay_known)) {
                        relay_line = "保存済み。確認中…";
                    } else if (rc.state == net::RelayChangeState::Saved && ImGui::GetTime() - relay_asked_at < 8.0) {
                        relay_line = off_now ? "保存しました。このアカウントの中継は無効です。" : "保存しました。このアカウントの中継は有効です。";
                        relay_col = ui::kGood;
                    }
                    ImGui::PushFont(nullptr, 14.0f);
                    ImGui::AlignTextToFramePadding();
                    ImGui::PushStyleColor(ImGuiCol_Text, relay_col);
                    ImGui::TextUnformatted(ui::fit_text(relay_line, right - ImGui::GetCursorScreenPos().x).c_str());
                    ImGui::PopStyleColor();
                    ImGui::PopFont();
                    ui::tooltip_for_item("中継", relay_line == "保存中…" ? kRelayWebsite : relay_line, kRelayWebsite);
                    auto ask = [&](bool direct) {
                        relay_confirm = false;
                        relay_asked_at = ImGui::GetTime();
                        host_log("setup: relay switch: asking the server to turn it %s", direct ? "off" : "on");
                        net::account_relay_change_start(direct);
                    };
                    if (want_on && off_now) ask(false);
                    if (want_off && !off_now) {
                        // Off with a test that showed others get in: at once; else the
                        // warning first, and a second press.
                        if (nat::relay_off_safe(nat_tested_v, nat_tested_in) && nat_tested) ask(true);
                        else relay_confirm = true;
                    }
                    if (relay_confirm) {
                        const nat::RelayOffWarning wn = nat::relay_off_warning(nat_tested, nat_tested_v, nat_tested_in, nat_cx, nat_lan, nat_router);
                        ImGui::Dummy(ImVec2(0, 2 * scale));
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + rl);
                        ImGui::BeginGroup();
                        ImGui::PushTextWrapPos(right - ImGui::GetWindowPos().x);
                        ImGui::PushFont(nullptr, 14.0f);
                        ImGui::PushStyleColor(ImGuiCol_Text, ui::kWarn);
                        ImGui::TextUnformatted(wn.why.c_str());
                        ImGui::PopStyleColor();
                        numbered(wn.steps, ui::kWarn);
                        ImGui::PopFont();
                        ImGui::PopTextWrapPos();
                        ImGui::Dummy(ImVec2(0, 2 * scale));
                        if (ui::primary_button("中継を維持", ImVec2(ImGui::CalcTextSize("中継を維持").x + st.FramePadding.x * 4, 0))) {
                            relay_confirm = false;
                            host_log("setup: relay switch: kept on");
                        }
                        ui::tooltip_for_item(nullptr, "中継を有効に保ち、サーバー経由の協力・対人プレイを維持します。");
                        ImGui::SameLine();
                        if (ui::ghost_button("それでも無効にする") || relay_press == 2) ask(true);
                        ui::tooltip_for_item(nullptr, "このアカウントの中継を無効にします。いつでもここで有効に戻せます。");
                        ImGui::EndGroup();
                    }
                }
                relay_press = 0;
            }
            ui::card_end();
            if (nat_scroll) {
                ImGui::SetScrollHereY(1.0f);
                nat_scroll = false;
            }

            // Caches that are not this bbhost's or this driver's: one line,
            // once there is a game to compile them for.
            if (recommend_compile && app0_ok && eboot.ok) {
                ImGui::Dummy(ImVec2(0, gap - st.ItemSpacing.y));
                ui::card_begin("##advice", ImVec2(w, 0));
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const float fh = ImGui::GetFrameHeight();
                ImGui::Dummy(ImVec2(fh * 0.8f, fh));
                ui::draw_icon(ImGui::GetWindowDrawList(), ui::Icon::Shaders, at + ImVec2(fh * 0.4f, fh * 0.5f), fh * 0.62f, ui::kWarn);
                ImGui::SameLine(0, 8 * scale);
                const char* btn = "今すぐコンパイル";
                const float bw = ImGui::CalcTextSize(btn).x + st.FramePadding.x * 3;
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted((advice.why + ".").c_str());
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ui::kTextDim);
                ImGui::TextUnformatted((capitalised(advice.time) + ", optional").c_str());
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::SetCursorScreenPos(ImVec2(head_right - 18 * scale - bw, at.y));
                if (ui::primary_button(btn, ImVec2(bw, 0), app0_ok && eboot.ok)) {
                    start_compile();
                    go(kPageShaders);
                }
                ui::tooltip_for_item(nullptr, "事前にすべてのシェーダーをコンパイルします。省略するとプレイ中に処理します。");
                ui::card_end();
            }
        } else if (page == kPageGraphics) {
            // F10's Graphics page in the game, row for row (host/options.h):
            // the effects, detail and screen space reflections. Resolution is
            // the Game page's, the shader form the Shaders page's.
            for (int i = 0; i < host_options_page_count(); ++i) {
                if (std::strcmp(host_options_page_name(i), "グラフィックス") == 0) host_options_draw_setup_page(i);
            }
        } else if (page == kPageControls) {
            // F10's Controls page: the mouse and the prompts, then the
            // bindings - the same settings and file.
            controls_page_draw(true);
        } else if (page == kPageEnhancements) {
            ui::card_begin("##enhancements", ImVec2(ImGui::GetContentRegionAvail().x, 0));
            for (std::size_t i = 0; i < std::size(enhancements); ++i) {
                Enhancement& e = enhancements[i];
                if (i) {
                    const ImVec2 at = ImGui::GetCursorScreenPos();
                    ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x, at.y - st.ItemSpacing.y * 0.5f + 1),
                                                        ImVec2(at.x + ImGui::GetContentRegionAvail().x, at.y - st.ItemSpacing.y * 0.5f + 1),
                                                        ui::alpha(ui::kViolet, 40));
                    ImGui::Dummy(ImVec2(0, 2 * scale));
                }
                ui::toggle_row(e.label, e.sub, &e.on, e.text);
                ImGui::Dummy(ImVec2(0, 2 * scale));
            }
            ui::card_end();
        } else if (page == kPagePatches) {
            // Whether a row's switch can be changed here.
            auto switchable = [&](const PatchRow& r) {
                const bool other_eboot = eboot.ok && !r.p.eboot.empty() && r.p.eboot != eboot.sha;
                const bool unknown_option = !r.p.option.empty() && !patch_manifest_option_known(r.p.option);
                return r.p.error.empty() && !other_eboot && !unknown_option && (!r.p.option.empty() || bare_key(r.p.name));
            };
            // All off, or each patch as its own file has it (a patch that
            // says enabled = false off, the rest on); one that follows a
            // setting keeps that setting's own default.
            auto set_all = [&](bool defaults) {
                for (PatchRow& r : patch_rows) {
                    if (!switchable(r) || (defaults && !r.p.option.empty())) continue;
                    *patch_switch(r) = defaults && r.p.enabled;
                }
            };
            {
                const float find_w = std::round(200 * scale);
                const float b1 = ImGui::CalcTextSize("初期設定").x + st.FramePadding.x * 2, b2 = ImGui::CalcTextSize("すべて無効").x + st.FramePadding.x * 2;
                const ImVec2 back = head_actions(find_w + b1 + b2 + st.ItemSpacing.x * 2);
                const ImVec2 fp = ImGui::GetCursorScreenPos();
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(30 * scale, st.FramePadding.y));
                ImGui::SetNextItemWidth(find_w);
                ImGui::InputTextWithHint("##find", "検索", patch_find, sizeof(patch_find));
                ImGui::PopStyleVar();
                ui::draw_icon(ImGui::GetWindowDrawList(), ui::Icon::Search, fp + ImVec2(15 * scale, ImGui::GetFrameHeight() * 0.5f),
                              14 * scale, ui::kTextDim);
                ImGui::SameLine();
                ImGui::BeginDisabled(patch_rows.empty());
                if (ui::ghost_button("初期設定")) set_all(true);
                ui::tooltip_for_item(nullptr, "各パッチの初期設定に戻します。");
                ImGui::SameLine();
                if (ui::ghost_button("すべて無効")) set_all(false);
                ui::tooltip_for_item(nullptr, "パッチを無効にして原作の動作に戻します。");
                ImGui::EndDisabled();
                ImGui::SetCursorScreenPos(back);
            }
            std::vector<std::size_t> shown;
            for (std::size_t i = 0; i < patch_rows.size(); ++i) {
                if (contains_nocase(patch_rows[i].p.name, patch_find) || contains_nocase(patch_rows[i].p.title, patch_find) ||
                    contains_nocase(patch_rows[i].p.description, patch_find))
                    shown.push_back(i);
            }
            // One line each, in two columns; the bubble has the rest. The
            // card fits the rows, or the page's height with them scrolling.
            const std::size_t per_col = (shown.size() + 1) / 2;
            const float row_h = std::round(ImGui::GetFontSize() + 2 * 3 * scale + 9 * scale);  // a box with the list's padding, and air
            const float fit = 2 * 14 * scale + std::max<float>(1.0f, static_cast<float>(per_col)) * row_h + 4 * scale;
            ui::card_begin("##patchlist", ImVec2(ImGui::GetContentRegionAvail().x, std::min(fit, std::max(ImGui::GetContentRegionAvail().y, 160 * scale))));
            if (patch_rows.empty()) dim_text("パッチがありません。");
            else if (shown.empty()) dim_text("一致するパッチがありません。");
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4 * scale, 3 * scale));
            const float col_gap = 22 * scale;
            const float col_w = (ImGui::GetContentRegionAvail().x - col_gap) * 0.5f;
            const ImVec2 o = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            for (std::size_t k = 0; k < shown.size(); ++k) {
                const std::size_t i = shown[k];
                PatchRow& r = patch_rows[i];
                const float col = static_cast<float>(k / per_col), row = static_cast<float>(k % per_col);
                const ImVec2 a = o + ImVec2(col * (col_w + col_gap), row * row_h);
                const ImVec2 b = a + ImVec2(col_w, row_h - 4 * scale);
                const bool hot = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(a - ImVec2(6 * scale, 0), b);
                if (hot) dl->AddRectFilled(a - ImVec2(6 * scale, 0), b, ImGui::GetColorU32(IM_COL32(255, 255, 255, 16)), 7 * scale);
                if (k == std::min<std::size_t>(2, shown.size() - 1)) patch_row_at = (a + b) * 0.5f;
                const bool other_eboot = eboot.ok && !r.p.eboot.empty() && r.p.eboot != eboot.sha;
                const bool unknown_option = !r.p.option.empty() && !patch_manifest_option_known(r.p.option);
                const bool fixed = !switchable(r);
                bool* on = patch_switch(r);
                bool off = false;
                // Its marks at the right: a gameplay change, or why it cannot be switched.
                float right = b.x - 4 * scale;
                const float fh = ImGui::GetFrameHeight();
                if (fixed) {
                    ui::draw_icon(dl, ui::Icon::Warning, ImVec2(right - fh * 0.32f, (a.y + b.y) * 0.5f), fh * 0.6f, ImGui::GetColorU32(ui::kWarn));
                    right -= fh * 0.7f + 6 * scale;
                }
                if (r.p.gameplay) {
                    const char* tag = "gameplay";
                    const float tfs = std::round(ImGui::GetFontSize() * 0.74f);
                    const ImVec2 ts = ui::font_strong()->CalcTextSizeA(tfs, FLT_MAX, 0.0f, tag);
                    const ImVec2 pa(right - ts.x - 14 * scale, (a.y + b.y) * 0.5f - ts.y * 0.5f - 2 * scale);
                    const ImVec2 pb(right, (a.y + b.y) * 0.5f + ts.y * 0.5f + 2 * scale);
                    dl->AddRectFilled(pa, pb, ImGui::GetColorU32(ui::alpha(ui::kPink, 46)), (pb.y - pa.y) * 0.5f);
                    dl->AddText(ui::font_strong(), tfs, pa + ImVec2(7 * scale, 2 * scale), ImGui::GetColorU32(ui::kPink), tag);
                    right = pa.x - 6 * scale;
                }
                // Its name up to the marks, cut short in a narrow window.
                const float name_w = right - a.x - fh - ImGui::GetStyle().ItemInnerSpacing.x;
                const std::string name = ui::fit_text(patch_display_name(r.p), name_w) + "###patch";
                ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + (b.y - a.y - fh) * 0.5f));
                ImGui::PushID(static_cast<int>(i));
                ImGui::BeginDisabled(fixed);
                // Variants of one patch (a group) are one at a time: turning
                // one on turns the others off.
                if (ImGui::Checkbox(name.c_str(), fixed ? &off : on) && !fixed && *on && !r.p.group.empty()) {
                    for (PatchRow& o2 : patch_rows) {
                        if (&o2 != &r && o2.p.group == r.p.group && switchable(o2)) *patch_switch(o2) = false;
                    }
                }
                ImGui::EndDisabled();
                ImGui::PopID();
                if (hot) {
                    std::string note;
                    auto add = [&](const std::string& s) {
                        if (s.empty()) return;
                        if (!note.empty()) note += "\n";
                        note += s;
                    };
                    if (!r.p.error.empty()) add("読み込めません: " + r.p.error + ".");
                    else if (other_eboot) add("別の eboot 用のため適用しません。");
                    else if (unknown_option) add("連動する設定: " + r.p.option + "。この bbhost では未対応のため適用しません。");
                    else if (r.p.option.empty() && !bare_key(r.p.name)) add("設定キーに使用できない名前のため、常に有効です。");
                    // The patch's name and what it does; a note only for one that cannot be switched.
                    ui::tooltip(patch_display_name(r.p).c_str(), capitalised(r.p.description.empty() ? std::string("説明はありません。") : r.p.description), note);
                }
            }
            ImGui::PopStyleVar();
            ImGui::SetCursorScreenPos(o + ImVec2(0, static_cast<float>(per_col) * row_h));
            ImGui::Dummy(ImVec2(1, 1));
            ui::card_end();
        } else if (page == kPageShaders) {
            const float w = ImGui::GetContentRegionAvail().x;
            // The caches at a glance, and the Shaders setting beside them.
            ui::card_begin("##state", ImVec2(w, 0), "シェーダーキャッシュ");
            {
                // What the caches are: a mark, a headline and a line under it.
                struct State {
                    ui::Icon icon;
                    ImU32 col;
                    std::string head, sub;
                };
                State state{ui::Icon::Check, ui::kGood, "準備完了", "この bbhost と現在のドライバーで作成済みです。"};
                const ShaderCompileResultCounts counts = shader_compile_result_counts(cache_state.last);
                const std::uint64_t last_failed = cv.finished ? cv.failed : counts.failed;
                if (cv.running) state = {ui::Icon::Refresh, ui::kBlue, "コンパイル中", cv.phase};
                else if (!cache_state.have_caches) state = {ui::Icon::Warning, ui::kWarn, "シェーダーキャッシュがありません", "初回起動時にすべてコンパイルします。"};
                else if (!cache_state.translator_current)
                    state = {ui::Icon::Warning, ui::kWarn, "別の bbhost で作成されたキャッシュです", "次回起動時にすべて再コンパイルします。"};
                else if (cache_state.device_current == 0)
                    state = {ui::Icon::Warning, ui::kWarn, "別の GPU またはドライバー用です", "ドライバー側の処理を再コンパイルします。"};
                else if (cache_state.device_current != 1) state = {ui::Icon::Info, ui::kIce, "ドライバーを確認中", "この bbhost で作成済みです。"};
                else if (last_failed)
                    state = {ui::Icon::Warning, ui::kWarn, "一部のコンパイルに失敗", "作成済みのキャッシュを使用できます。下の失敗理由を確認してください。"};
                if (recommend_compile)
                    state = {ui::Icon::Warning, ui::kWarn, "事前コンパイルを推奨",
                             advice.why + "。事前コンパイルの目安: " + advice.time + "。省略するとプレイ中に処理します。"};
                const ui::Icon icon = state.icon;
                const ImU32 col = state.col;
                const std::string& head_line = state.head;
                const std::string& sub = state.sub;
                const float fh = ImGui::GetFrameHeight();
                const ImVec2 at = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(fh * 1.3f, fh * 1.6f));
                ui::draw_icon(ImGui::GetWindowDrawList(), icon, at + ImVec2(fh * 0.6f, fh * 0.8f), fh * 0.95f, ImGui::GetColorU32(col));
                ImGui::SameLine(0, 10 * scale);
                const float seg_w = std::round(190 * scale);
                ImGui::BeginGroup();
                ImGui::PushFont(ui::font_strong(), 19.0f);
                ImGui::TextUnformatted(head_line.c_str());
                ImGui::PopFont();
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(100.0f, ImGui::GetContentRegionAvail().x - seg_w - 20 * scale));
                ImGui::PushStyleColor(ImGuiCol_Text, ui::kTextDim);
                ImGui::TextUnformatted(sub.c_str());
                ImGui::PopStyleColor();
                ImGui::PopTextWrapPos();
                ImGui::EndGroup();
                const ImVec2 after = ImGui::GetCursorScreenPos();
                // The Shaders setting, at the right.
                ImGui::SetCursorScreenPos(ImVec2(head_right - 18 * scale - seg_w, at.y + (fh * 1.6f - fh) * 0.5f));
                static const char* const kShaderModes[] = {"HLSL", "変換済み SPIR-V"};
                ImGui::BeginDisabled(cv.running || !app0_ok || !eboot.ok);
                ui::segmented("##shaders", &shaders, kShaderModes, 2, seg_w, kHostShaderModeTips,
                              "「保存」か「プレイ」で保存し、次回起動時に反映します。");
                ImGui::EndDisabled();
                ImGui::SetCursorScreenPos(after);
                ImGui::Dummy(ImVec2(0, 0));
                if (cache_state.have_caches) {
                    char sizes[256];
                    std::snprintf(sizes, sizeof(sizes), "変換・モジュール %.0f MiB  |  パイプライン %.0f MiB  |  描画済み %.1f + %.1f MiB",
                                  cache_state.translation_bytes / 1048576.0, cache_state.pipeline_bytes / 1048576.0,
                                  cache_state.stage_manifest_bytes / 1048576.0, cache_state.pipeline_manifest_bytes / 1048576.0);
                    dim_text(sizes, ui::kTextFaint);
                    ui::tooltip_for_item(nullptr, cache_state.dir);
                } else {
                    dim_text(cache_state.dir, ui::kTextFaint);
                }
            }
            ui::card_end();
            ImGui::Dummy(ImVec2(0, gap - st.ItemSpacing.y));
            ui::card_begin("##compile", ImVec2(w, 0), "事前コンパイル");
            {
                if (!cv.running) {
                    if (ui::primary_button("今すぐシェーダーをコンパイル", ImVec2(0, 0), app0_ok && eboot.ok)) start_compile();
                    ImGui::SameLine();
                    ImGui::AlignTextToFramePadding();
                    ImGui::PushStyleColor(ImGuiCol_Text, ui::kTextDim);
                    ImGui::TextUnformatted(app0_ok && eboot.ok ? "この PC のドライバーでシェーダーと描画パイプラインをコンパイルします。中止できます。"
                                                               : "対応するゲームフォルダーと 1.09 の eboot が必要です。");
                    ImGui::PopStyleColor();
                } else {
                    if (ui::ghost_button("キャンセル")) compile_job.cancel();
                    ImGui::SameLine();
                    ImGui::AlignTextToFramePadding();
                    ImGui::PushStyleColor(ImGuiCol_Text, ui::kTextDim);
                    ImGui::TextUnformatted(cv.phase.c_str());
                    ImGui::PopStyleColor();
                }
                if (cv.running || cv.finished || cv.cancelled || !cv.error.empty()) {
                    const float frac = cv.total ? static_cast<float>(cv.done) / static_cast<float>(cv.total) : 0.0f;
                    char over[96];
                    std::snprintf(over, sizeof(over), "%llu / %llu", static_cast<unsigned long long>(cv.done),
                                  static_cast<unsigned long long>(cv.total));
                    ImGui::Dummy(ImVec2(0, 2 * scale));
                    ui::progress(frac, over);
                    char line[256];
                    if (cv.eta >= 0 && cv.running) std::snprintf(line, sizeof(line), "経過 %.0f 秒、残り約 %.0f 秒", cv.seconds, cv.eta);
                    else std::snprintf(line, sizeof(line), "%.0f 秒", cv.seconds);
                    std::string stats = line;
                    std::snprintf(line, sizeof(line), "  |  DXC %llu、SPIR-V 維持 %llu、失敗 %llu、単体対象外 %llu",
                                  static_cast<unsigned long long>(cv.hlsl), static_cast<unsigned long long>(cv.lift),
                                  static_cast<unsigned long long>(cv.failed), static_cast<unsigned long long>(cv.deferred));
                    dim_text(stats + line);
                    if (cv.deferred)
                        dim_text("単体対象外はテッセレーションの評価段階です。対応する描画設定と組み合わせ、記録済みパイプラインの再生時または描画時にコンパイルします。",
                                 ui::kTextDim);
                    if (cv.recipes_total) {
                        std::snprintf(line, sizeof(line), "過去のプレイと同梱リストのパイプライン: %llu / %llu",
                                      static_cast<unsigned long long>(cv.recipes_done), static_cast<unsigned long long>(cv.recipes_total));
                        dim_text(line);
                    }
                    if (cv.finished)
                        dim_text(cv.failed ? "完了しましたが、一部のコンパイルに失敗があります。下の理由を確認してください。"
                                           : "完了しました。作成したキャッシュは次回起動時に使用します。",
                                 cv.failed ? ui::kWarn : ui::kGood, false);
                    if (cv.cancelled) dim_text("中止しました。完成済みのキャッシュは保持し、残りはゲーム中に処理します。", ui::kWarn, false);
                    if (!cv.error.empty()) dim_text(cv.error, ui::kWarn, false);
                    for (const std::string& r : cv.reasons) dim_text("- " + r);
                }
            }
            ui::card_end();
            if (!cache_state.last.empty()) {
                ImGui::Dummy(ImVec2(0, gap - st.ItemSpacing.y));
                // shader-compile.txt's "key value" lines, as sentences: one
                // line, the rest in its bubble.
                std::string res, when, version, stages, hlsl, seconds, device, pipelines;
                std::vector<std::string> failed, lift;
                for (const std::string& l : cache_state.last) {
                    const std::size_t sp = l.find(' ');
                    const std::string k = l.substr(0, sp), v = sp == std::string::npos ? "" : l.substr(sp + 1);
                    if (k == "result") res = v;
                    else if (k == "when") when = v;
                    else if (k == "bbhost") version = v;
                    else if (k == "stages") stages = v;
                    else if (k == "hlsl") hlsl = v;
                    else if (k == "seconds") seconds = v;
                    else if (k == "device") device = v.substr(v.find(' ') == std::string::npos ? 0 : v.find(' ') + 1);
                    else if (k == "failed") failed.push_back(v);
                    else if (k == "pipelines") pipelines = v;
                    else if (k == "lift") lift.push_back(v);
                }
                ui::card_begin("##last", ImVec2(w, 0), "前回のコンパイル");
                std::string details = "On " + device + ": shaders " + stages + "; " + hlsl + ".";
                const ShaderCompileResultCounts counts = shader_compile_result_counts(cache_state.last);
                if (counts.deferred)
                    details += "\n単体コンパイル対象外 " + std::to_string(counts.deferred) +
                               ": テッセレーションの評価段階です。対応する描画設定と組み合わせ、記録済みパイプラインの再生時または描画時にコンパイルします。";
                if (!pipelines.empty() && pipelines.rfind("0 of 0", 0) != 0) details += "\nPipelines " + pipelines + ".";
                for (const std::string& r : failed) details += "\n失敗: " + r;
                for (const std::string& r : lift) details += "\n変換済み SPIR-V を維持: " + r;
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(((res == "done" || res == "完了" ? "完了" : res == "cancelled" ? "中止" : res) + ", " + when + "  |  " + seconds + " s  |  bbhost " + version).c_str());
                ImGui::SameLine();
                const ImVec2 ip = ImGui::GetCursorScreenPos();
                const float fh = ImGui::GetFrameHeight();
                ImGui::Dummy(ImVec2(fh * 0.7f, fh));
                ui::draw_icon(ImGui::GetWindowDrawList(), ui::Icon::Info, ip + ImVec2(fh * 0.35f, fh * 0.5f), fh * 0.55f, ImGui::GetColorU32(ui::kTextDim));
                ui::tooltip_for_item("前回のコンパイル", details);
                ui::card_end();
            }
        } else if (page == kPagePlugins) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
            plugin_ui_draw(plugin_ui, scale);
            ImGui::PopStyleColor();
            ui::child_cards();
            plugin_seen = true;
        } else if (page == kPageTrophies) {
            trophy_ui_draw(trophy_ui, f[kData].str().empty() ? config_default_data_dir() : f[kData].str(), 0.0f);
        } else if (page == kPageHistory) {
            play_history_ui_draw(history_ui, f[kData].str().empty() ? config_default_data_dir() : f[kData].str());
        } else if (page == kPageSettings) {
            const float w = ImGui::GetContentRegionAvail().x;
            ui::card_begin("##folders", ImVec2(w, 0), "フォルダー");
            path_row("データフォルダー", kData, true, config_default_data_dir().c_str(), -1, "",
                     "セーブとキャッシュの保存先。空欄では標準の場所: " + config_default_data_dir());
            ui::row_label("設定ファイル", label_w);
            {
                const std::string file = config_user_file();
                const float bw = ImGui::GetFrameHeight();
                ImGui::PushStyleColor(ImGuiCol_Text, ui::kTextDim);
                ImGui::TextUnformatted(ui::fit_left(generic(file), head_right - 18 * scale - bw - st.ItemSpacing.x - ImGui::GetCursorScreenPos().x).c_str());
                ImGui::PopStyleColor();
                ui::tooltip_for_item(nullptr, generic(file) + (config_saves_locally() ? "\nbbhost と同じ場所の設定です。利用者共通の設定より優先し、ここに保存します。"
                                                                                      : "\nすべての bbhost で共通の設定です。"));
                ImGui::SameLine(0, 0);
                ImGui::SetCursorScreenPos(ImVec2(head_right - 18 * scale - bw, ImGui::GetCursorScreenPos().y));
                if (ui::icon_button("##open", ui::Icon::Folder, 0.0f, "保存先を開く")) {
                    std::string url = "file:///" + generic(fs::path(file).parent_path().string());
                    SDL_OpenURL(url.c_str());
                }
            }
            ui::card_end();
            ImGui::Dummy(ImVec2(0, gap - st.ItemSpacing.y));
            ui::card_begin("##startup", ImVec2(w, 0), "起動");
            ui::toggle_row("起動時にこの画面を表示", "一度だけ開くには --setup を付けて起動します。", &show_every_start);
            ImGui::Dummy(ImVec2(0, 2 * scale));
            ui::toggle_row("メニューのアニメーション", "背景の動き、光の粒、プレイボタンの発光。", &animations,
                           "背景、光の粒、月やボタンの発光、ページ切替の動きを設定します。無効では静止表示します。");
            ImGui::Dummy(ImVec2(0, 2 * scale));
            ui::toggle_row("この画面を開いたままにする", "ゲーム中は最小化し、終了後に戻ります。", &keep_open,
                           "別プロセスでゲームを開始し、この画面はタスクバーで待機します。ゲーム終了後に戻り、再開できます。無効時はこの画面を閉じます。");
            ImGui::Dummy(ImVec2(0, 2 * scale));
            // As a switch's row: its label and a line under it, the control (`cw` wide) at the right.
            const auto choice_row = [&](const char* label, const char* sub, float cw, const auto& control) {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float w = ImGui::GetContentRegionAvail().x;
                const float fs = ImGui::GetFontSize(), sub_fs = std::round(fs * 0.86f);
                ImFont* font = ImGui::GetFont();
                const float lh = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, label).y;
                const float sub_h = font->CalcTextSizeA(sub_fs, FLT_MAX, w - cw - 16 * scale, sub).y + 2 * scale;
                const float h = std::max(ImGui::GetFrameHeight(), lh + sub_h), ty = p.y + (h - lh - sub_h) * 0.5f;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddText(font, fs, ImVec2(p.x, ty), ImGui::GetColorU32(ui::kText), label);
                dl->AddText(font, sub_fs, ImVec2(p.x, ty + lh + 2 * scale), ImGui::GetColorU32(ui::kTextDim), sub, nullptr, w - cw - 16 * scale);
                ImGui::SetCursorScreenPos(ImVec2(p.x + w - cw, p.y + (h - ImGui::GetFrameHeight()) * 0.5f));
                ImGui::SetNextItemWidth(cw);
                control();
                ImGui::SetCursorScreenPos(p);
                ImGui::Dummy(ImVec2(w, h));
            };
            choice_row("メニューの文字サイズ", "この画面と F9・F10 の文字とウィンドウを拡大します。", 128 * scale, [&] {
                ui::combo("##textsize", &text_choice, kTextSizeLabels, static_cast<int>(std::size(kTextSizeLabels)));
                ui::tooltip_for_item("メニューの文字サイズ",
                                     "設定画面と F9・F10 の文字や項目を拡大します。1440p や 4K では 150〜200% が目安です。画面に収まらない場合はスクロールします。即時反映します。");
            });
            // Menu hue and Menu vibrance: F10's sliders (a degree and a percent a
            // step, the value shown), saved at once; the reset beside each, or a
            // double click, puts it back to 0.
            const auto colour_slider = [&](const char* id, const char* key, int steps, int origin, int step, const char* unit,
                                           const char* title, const char* tip) {
                int i = std::max(0, host_opt_index(key));
                const int value = origin + step * i;
                char shown[24];
                std::snprintf(shown, sizeof(shown), value > 0 ? "+%d%s" : "%d%s", value, unit);
                ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - ui::reset_width());
                if (ImGui::SliderInt(id, &i, 0, steps - 1, shown, ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput))
                    host_opt_set_index(key, i);
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    host_opt_set_index(key, -origin / step);
                ui::tooltip_for_item(title, tip);
                ImGui::PushID(id);
                if (ui::reset_button("##reset", value == 0)) host_opt_set_index(key, -origin / step);
                ImGui::PopID();
            };
            ImGui::Dummy(ImVec2(0, 2 * scale));
            choice_row("メニューの色相", "この画面と F9・F10 の色合いを調整します。", 380 * scale, [&] {
                colour_slider("##hue", "menu_hue", 361, -180, 1, "°", "メニューの色相",
                              "設定画面と F9・F10 の色相を調整します。0 は標準の青と紫です。正常・警告・問題を示す緑・黄・赤は維持します。ダブルクリックで 0 に戻します。F10 にも同じ設定があります。");
            });
            ImGui::Dummy(ImVec2(0, 2 * scale));
            choice_row("メニューの鮮やかさ", "-100% で白黒になります。", 380 * scale, [&] {
                colour_slider("##vibrance", "menu_vibrance", 201, -100, 1, "%%", "メニューの鮮やかさ",
                              "メニューの色の鮮やかさです。-100% は白黒、0% は標準、+100% は最大です。ダブルクリックで 0 に戻します。F10 にも同じ設定があります。");
            });
            ImGui::Dummy(ImVec2(0, 2 * scale));
            {
                const std::string sub = check_updates ? updater::status() : std::string("日本語版の配布元で更新を確認します。");
                const std::string tip = check_updates ? updater::detail() : std::string();
                ui::toggle_row("更新を確認", sub.c_str(), &check_updates, tip.empty() ? nullptr : tip.c_str());
            }
            ImGui::Dummy(ImVec2(0, 2 * scale));
            path_row("GitHub トークン", kToken, false, "非公開リリースの確認に使用", -1, "",
                     "非公開リリースの更新確認に使用する GitHub トークンのファイルです。");
            ui::card_end();
            ImGui::Dummy(ImVec2(0, gap - st.ItemSpacing.y));
            ui::card_begin("##diagnostics", ImVec2(w, 0), "診断");
            ui::toggle_row("診断とログ出力", "問題を報告する際は有効にしてください。", &tooling,
                           "ログ、定期レポート、停止検出、GPU 負荷表示、描画履歴を記録します。無効ではログとログウィンドウを省略して処理を軽くします。");
            ui::card_end();
        }
        ImGui::PopStyleVar();  // the fade
        ImGui::EndChild();
        if (play && can_play && save()) {
            std::string err;
            if (keep_open && !test) game = static_cast<SDL_Process*>(host_start_again("--no-setup", false, err));
            if (game) {
                host_log("setup: the game started; the window waits minimized");
                SDL_MinimizeWindow(win);
            } else {
                if (!err.empty()) host_log("setup: %s; the game runs here instead", err.c_str());
                result = LauncherResult::Play;
                done = true;
            }
        }
        ImGui::End();

        if (test && (!std::strcmp(test, "trophies") || !std::strcmp(test, "play-history") || !std::strcmp(test, "chalice-history")) && ++frames > 30) done = true;
        ImGui::Render();
        // Menu hue and Menu vibrance, on everything the frame draws.
        float hue = 0.0f, vibrance = 0.0f;
        host_opt_menu_colour(&hue, &vibrance);
        ui::set_colour(hue, vibrance);
        ui::recolour(ImGui::GetDrawData());
        if (hue != title_hue || vibrance != title_vibrance) {
            title_hue = hue;
            title_vibrance = vibrance;
            dark_title_bar(win);
        }
        const ImU32 ink = ui::tint(ui::kInk);
        SDL_SetRenderDrawColor(ren, static_cast<Uint8>(ink & 0xff), static_cast<Uint8>(ink >> 8 & 0xff), static_cast<Uint8>(ink >> 16 & 0xff), 255);
        SDL_SetRenderScale(
            ren,
            io.DisplayFramebufferScale.x,
            io.DisplayFramebufferScale.y
        );
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
        if (nat_hook_stage == 2 && ImGui::GetTime() >= nat_hook_at) {
            const std::string file = test_nat + "/" + nat_shot_name + (nat_details ? "-details" : "") + ".png";
            if (SDL_Surface* s = SDL_RenderReadPixels(ren, nullptr)) {
                if (!SDL_SavePNG(s, file.c_str())) host_log("setup: shot: %s: %s", file.c_str(), SDL_GetError());
                SDL_DestroySurface(s);
            }
            host_log("setup: shot %s", file.c_str());
            if (!nat_details) {
                // What the card says (the headline and the action; never an address), then the same with Details open.
                static const char* const kTones[] = {"none", "good", "warning", "problem"};
                host_log("setup: connection card: [%s] %s%s%s (%zu steps)", kTones[static_cast<int>(nat_card.tone)], nat_card.head.c_str(),
                         nat_card.action.empty() ? "" : " - ", nat_card.action.c_str(), nat_card.steps.size());
                nat_details = true;
                nat_hook_at = ImGui::GetTime() + 0.8;
            } else {
                nat_hook_stage = 3;
                done = true;
            }
        }
        if (!hook_shot.empty() && ImGui::GetTime() >= hook_shot_at) {
            if (SDL_Surface* s = SDL_RenderReadPixels(ren, nullptr)) {
                if (!SDL_SavePNG(s, hook_shot.c_str())) host_log("setup: shot: %s: %s", hook_shot.c_str(), SDL_GetError());
                SDL_DestroySurface(s);
            }
            host_log("setup: shot %s", hook_shot.c_str());
            hook_shot.clear();
            if (relay_hook_step == 99) done = true;
        }
        if (!test_shot.empty() && ImGui::GetTime() >= shot_at) {
            const std::string name = shot < kPageCount ? kPages[shot].slug : "patches-bubble";
            char path[64];
            std::snprintf(path, sizeof(path), "/%02d-", shot + 1);
            const std::string file = test_shot + path + name + ".png";
            if (SDL_Surface* s = SDL_RenderReadPixels(ren, nullptr)) {
                if (!SDL_SavePNG(s, file.c_str())) host_log("setup: shot: %s: %s", file.c_str(), SDL_GetError());
                SDL_DestroySurface(s);
            }
            host_log("setup: shot %s", file.c_str());
            ++shot;
            if (shot > kPageCount) {
                done = true;
            } else {
                go(shot < kPageCount ? shot : static_cast<int>(kPagePatches));
                shot_at = ImGui::GetTime() + (shot == kPageCount ? 0.6 : 1.0);
            }
        }
        SDL_RenderPresent(ren);
        frame_due += kFrameNs;
        const Uint64 now = SDL_GetTicksNS();
        if (now < frame_due) SDL_DelayNS(frame_due - now);
        else if (now - frame_due > kFrameNs) frame_due = now;
    }

    // Closing the window while a compile runs: it is told to stop and write
    // what it has; the destructor waits for it.
    compile_job.cancel();
    if (game) SDL_DestroyProcess(game);  // a game still running runs on
    if (devices_thread.joinable()) devices_thread.join();
    if (plugin_seen && test_shot.empty()) plugin_ui_mark_seen(plugin_ui);
    ui::shutdown();
    ui::set_animations(true);  // the switch is the setup window's; the in-game screens keep theirs
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    host_log("setup: %s", result == LauncherResult::Play ? "play" : "closed");
    return result;
}
