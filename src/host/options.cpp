#include "host/japanese_labels.h"
#include "host/japanese_labels.h"
#include "host/options.h"
#include "host/updater.h"
#include "host/settings.h"
#include "host/settings_store.h"

#include "host/bindings.h"
#include "host/overlay.h"
#include "host/plugins.h"
#include "host/render_hooks.h"
#include "host/scene_look.h"
#include "host/upscaler.h"
#include "host/window.h"
#include "core/config.h"
#include "core/portable.h"
#include "engine/frame_rate.h"
#include "engine/graphics_patch.h"
#include "engine/live_resolution.h"
#include "engine/mouse_camera.h"
#include "engine/patch_manifest.h"
#include "hle/modules.h"
#include "hle/np.h"
#include "net/account.h"
#include "net/nat_job.h"
#include "log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <functional>
#include <iterator>
#include <string>
#include <string_view>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#if defined(BBHOST_HAVE_SDL3)
#include "host/launcher_ui.h"

#include "imgui.h"

#include <SDL3/SDL.h>
#endif

namespace {

struct Setting;
bool reads_on(const Setting& s, int index);  // below
int fps_from_label(const char* label);

// "-180°" to "+180°" a step at a time (`unit` after each number), for a
// slider of fine steps; the strings live as long as the program.
// The lowest Custom limit: a rate below the game's own 30 runs converted,
// as the rates above it do (engine/frame_rate.cpp mode_of), in real time.
constexpr int kCustomFpsMin = 10;

std::vector<const char*> signed_steps(int from, int to, const char* unit, bool sign = true) {
    // A deque: its strings never move as more are added, so the pointers
    // handed out stay good (a vector's growth moved short strings' bytes).
    static std::deque<std::string>* store = new std::deque<std::string>();
    std::vector<const char*> out;
    for (int v = from; v <= to; ++v) {
        store->push_back((sign && v > 0 ? "+" : "") + std::to_string(v) + unit);
        out.push_back(store->back().c_str());
    }
    return out;
}

// The steps of a 0-100 slider ("0" to "100").
std::vector<const char*> hundred() { return signed_steps(0, 100, "", false); }

// One setting. Everything here is a choice between a few named values: a
// toggle is a choice of two, which keeps drawing and input to one case.
struct Setting {
    const char* key;                 // what it is saved as
    const char* label;
    std::vector<const char*> values;
    int index = 0;
    const char* note = nullptr;      // shown under the row when it is selected
    bool restart = false;            // takes effect on the next run
};

enum SettingId {
    kWindowMode,
    kStayBorderless,
    kVsync,
    kFrameCap,
    kFrameCapCustom,
    kFrameCapCustomFps,
    kFrameClock,
    kEffectTiming,
    kFpsCounter,
    kMemoryCounter,
    kPerfOverlay,
    kFov,
    kCameraDistance,
    kCameraHeight,
    kCameraTuning,
    kMouseMenu,
    kMouseCamera,
    kMouseSens,
    kMouseInvertX,
    kMouseInvertY,
    kMouseAutoRotation,
    kDrawCursor,
    kButtonPrompts,
    kLeftStickInner,
    kLeftStickOuter,
    kRightStickInner,
    kRightStickOuter,
    kSsao,
    kMotionBlur,
    kAntiAlias,
    kDepthOfField,
    kChromaticAberration,
    kBloom,
    kVignette,
    kShadowDistance,
    kDynamicShadows,
    kShadowResolution,
    kAoStrength,
    kSaturation,
    kFog,
    kModelDetail,
    kTextureQuality,
    kSfxTextureQuality,
    kAnisotropicFiltering,
    kSsr,
    kSsrQuality,
    kHdMotionBlur,
    kHdMotionBlurRes,
    kUpscaler,
    kTaaStrength,
    kUpscaleQuality,
    kDlssPreset,
    kSharpness,
    kFrameGeneration,
    kMotionVectors,
    kCharMotion,
    kResolution,
    kShaders,
    kSkipLogos,
    kPerformanceMode,
    kDebugMenu,
    kChangeAppearance,
    kRebirth,
    kFivePlayers,
    kSkillIssue,
    kFullPhantomHp,
    kAutoReplenish,
    kMenuTextSize,
    kMenuHue,
    kMenuVibrance,
    kTextScale,
    kSettingCount,
};

// Effect timing's choices as the frame rate reads them (core/config.h's
// config_effect_timing), in the setting's order: 30 fps, 60 fps (auto: 60
// above 30) and Frame rate-dependent. Smooth, a fourth until 2026-11-04, is
// still read from bbhost.toml or BBHOST_SFX_STEP (the row's Custom entry).
constexpr int kEffectTimingCount = 3;
constexpr const char* kEffectTimingValues[kEffectTimingCount] = {"30", "auto", "fps"};
constexpr int effect_timing_index(std::string_view v) {
    for (int i = 0; i < kEffectTimingCount; ++i) {
        if (v == kEffectTimingValues[i]) return i;
    }
    return -1;
}
static_assert(effect_timing_index(kEffectTimingDefault) >= 0, "the default effect timing is one of the setting's choices");

// A row on screen: a section header, a setting, or the close button.
struct Row {
    // Text: a field the player types (field 0 the account name, 1 a recovery
    // code). Action: a button (field 0 link with Discord, 1 create an account
    // on this PC, 2 recover, 3 website code, 4 sign out). Info: a line the
    // host writes (field 0 who is signed in, 1 the code in play, if any).
    // Fields 10 and up are the UPDATES rows (host/updater.h): Info 10 the
    // version and what the check found, 11 why it failed or the newer
    // release's page; Action 10 check, 11 open that page in the browser.
    enum Kind { Header, Option, Close, Text, Action, Info } kind;
    const char* text;
    int setting = -1;
    int field = -1;
};

Setting g_set[kSettingCount] = {
    // Borderless was this setting's Fullscreen until 2026-11-04 (an older
    // file's Fullscreen reads as Borderless: host_options_load).
    {"window_mode", "画面モード", {"Windowed", "Borderless", "Fullscreen"}, 0,
#if defined(_WIN32)
     "ウィンドウは移動・サイズ変更ができます。ボーダーレスはデスクトップの解像度で画面を埋め、素早く切り替えられます。「ボーダーレスを維持」でドライバーによる排他的全画面への切替を防ぎます。全画面は指定解像度に近い表示モードを使用します。AMD ではウィンドウ表示の負荷が高くなるため、ボーダーレスか全画面を推奨します。",
#else
     "ボーダーレスはデスクトップの解像度で画面を埋めます。全画面は指定解像度に近い表示モードへ切り替えます。",
#endif
     false},
    // Windows: Borderless's swapchain tells the driver it may not make the
    // window exclusive fullscreen (host/window.cpp, exclusive_policy). Live: a
    // change makes the swapchain again. Shown while Window mode is Borderless
    // (row_hidden). Elsewhere it does nothing and has no row; a value in the
    // file is kept.
    {"stay_borderless", "ボーダーレスを維持", {"Off", "On"}, 0,
     "有効にすると排他的な全画面への切替を防ぎます。別ウィンドウに切り替えた際など、ゲームは動いているのに映像だけ数秒止まる場合に試してください。AMD では描画負荷が増える場合があるため、必要なときだけ有効にしてください。", false},
    {"vsync", "垂直同期", {"Off", "On"}, 1,
     "無効では完成したフレームをすぐに表示します。画面が裂けて見える場合があります。", false},
    // The game's own pace (engine/frame_rate.h), the same eleven the game's
    // Frame Rate pick list offers (engine/option_menu.cpp). The key is the old
    // Frame cap's, so a file keeps its 30 or 60; a file older than version 3
    // holds a cap on presentation, whose 90, 120, 144 and Off ran the game at
    // 60, and reads as 60 (host_options_load).
    // Read as the game starts (engine/frame_rate.cpp), and with it the
    // defaults for the draw work the pool prepares and, on Windows, the run
    // commit's (host/gxvk_homes.h default_off, run_commit_gives_way), which
    // keep what that start decided until the next one. Every change of the
    // rate itself applies while the game runs (frame_rate_request): from the
    // game's own 30 the game is converted then, and a converted game taken to
    // 30 stays converted.
    {"frame_cap", "フレームレート", {"30", "60", "90", "120", "150", "180", "240", "360", "480", "960", "Display-Limited"}, 0,
#if defined(_WIN32)
     "30 is the game's own pace; the others run its logic at that rate. Display-Limited runs it at your monitor's refresh "
     "rate (180 on a 180 Hz monitor); with Frame generation on, at half of it, the frames between generated (90 + 90 at "
     "180 Hz). Changes at once.",
#else
     "30 is the game's own pace; the others run its logic at that rate. Display-Limited runs it at your monitor's refresh "
     "rate (180 on a 180 Hz monitor). Changes at once.",
#endif
     true},
    // Frame rate's Custom limit, its tick box and its number, drawn in Frame
    // rate's row (frame_rate_control) and not rows of their own: ticked, the
    // game runs at the number typed (10 to 960; 45 runs the game at 45, as the
    // listed rates run at theirs, and frame generation shows twice that) in
    // place of the list's rate (frame_rate_of). A rate picked from a list (the
    // game's own System menu, the Steam Deck row) unticks it.
    {"frame_cap_custom", "任意の上限", {"Off", "On"}, 0, kHostFrameRateCustomNote, true},
    {"frame_cap_custom_fps", "任意のフレームレート", signed_steps(kCustomFpsMin, 960, "", false), 60 - kCustomFpsMin, kHostFrameRateCustomNote, true},
    // The frame clock (engine/frame_clock.h), experimental and off by default:
    // what the frame rate otherwise sets once for the rate chosen at start
    // follows each frame's own length. Read once as the game starts
    // (engine/frame_rate.cpp, config_frame_clock): bbhost.toml's [video]
    // frame_clock is its default, BBHOST_FRAME_CLOCK=0|1 wins over both.
    {"frame_clock", "実フレーム時間で動作（試験的）", {"Off", "On"}, 0,
     "再起動が必要です。設定したフレームレートを維持できない場合も、実際のフレーム時間に合わせてゲームを動かします。",
     true},
    // How the effects keep time above 30 fps (engine/frame_rate.cpp), read
    // as the game starts and set again when it changes (frame_rate_retime):
    // BBHOST_SFX_STEP, else this once chosen, else bbhost.toml's [video]
    // effect_steps, else kEffectTimingDefault, 60 fps (config_effect_timing).
    // Written to the file only once chosen (save).
    {"effect_timing", "エフェクトの時間処理", {kHostEffectTimingLabels[0], kHostEffectTimingLabels[1], kHostEffectTimingLabels[2]},
     effect_timing_index(kEffectTimingDefault), kHostEffectTimingNote, false},
    // The game's frames a second, counted as they start (host/window.cpp,
    // host_fps_frame_start), with how busy the GPU is beside it.
    {"fps_counter", "FPS 表示", {"Off", "On"}, 0,
     "右上に実描画の毎秒フレーム数を表示します。フレーム生成中は、生成分を含む表示数を括弧内に示します。", false},
    // What bbhost holds of video memory and of the memory Windows counts as
    // shared GPU memory (host/window.cpp, host_gpu_memory_glance).
    {"memory_counter", "メモリー表示", {"Off", "On"}, 0,
     "ビデオメモリー使用量、解放可能なキャッシュ、GPU が参照するゲームメモリーを表示します。Windows では後者が共有 GPU メモリーに相当します。", false},
    // Preserve the saved option key.
    {"perf_overlay", "パフォーマンス表示", {"Off", "On"}, 0,
     "FPS を制限している処理と各処理の負荷を表示します。GPU、メインスレッド、GX、コマンド処理、FPS 上限などを確認できます。", false},
    // The follow camera's vertical field of view, LockCamParam's camFovY
    // (43 degrees in most areas) widened 5% a step (engine/camera.h).
    {"fov", "視野角", signed_steps(0, 100, "%"), 0,
     "カメラの視野です。+0% は原作、+50% は横約 97 度、+100% は約 118 度です。", false},
    // The same row's camDistTarget and chrOrgOffset_Y: 5 the game's own, 10%
    // a step either way.
    {"camera_distance", "カメラの距離", hundred(), 50,
     "キャラクター後方へのカメラの距離です。50 が原作です。", false},
    {"camera_height", "カメラの高さ", hundred(), 50,
     "キャラクターに対するカメラの高さです。50 が原作です。", false},
    // The follow camera above 30 fps (engine/frame_rate.cpp camera_original):
    // the game's own values, the same camera at every frame rate, whatever is
    // saved here - the menus no longer offer it, and only
    // BBHOST_CAMERA_TUNING=fps++ runs the retune from Kyo's 60 FPS++ list. So
    // the default is Original, and a save records the camera that runs.
    {"camera_tuning", "カメラの調整", {"FPS++ (Kyo)", "Original"}, 1,
     "再起動が必要です。30 fps 超では FPS++ が Kyo の調整値、原作は 30 fps 時の値を使用します。",
     true},
    // On, as in DS3: a click only reaches the menus while one is open
    // (engine/menu_pointer.h), so it cannot swing a weapon in the world.
    {"mouse_menu", "メニューでマウスを使用", {"Off", "On"}, 1,
     "左クリックで決定、右クリックで戻る、ホイールで選択を移動します。", false},
    // On by default, and it only *disables* the automatic behaviour: the
    // mouse turns the camera whenever a menu is not open, which is what DS3
    // does and what a PC player expects. Off hands the camera back to the pad.
    {"mouse_camera", "マウスでカメラ操作", {"Off", "On"}, 1,
     "メニュー外でマウスによるカメラ操作を許可します。無効時はパッドで操作します。", false},
    // 0-100: the 0..10 of the game's own Camera Sensitivity slider, the widget
    // the PC Controls screen draws it with, in tenths - an angle a count from
    // 0.005 to 0.4 degrees, each step about 4.5% further, 50 (0.045) its
    // default (engine/mouse_camera_step.h).
    {"mouse_sens", "マウス感度", hundred(), 50,
     "マウス移動に対するカメラの回転量です。１段階ごとに約 4.5% 増加します。800〜1600 DPI では 50 が目安です。", false},
    {"mouse_invert_x", "マウスの横方向を反転", {"Off", "On"}, 0,
     "マウスの左右のカメラ操作を反転します。", false},
    {"mouse_invert_y", "マウスの縦方向を反転", {"Off", "On"}, 0,
     "マウスの上下のカメラ操作を反転します。", false},
    // Off by default: with the mouse the camera stays where it is put, as
    // the community's "Disable Camera Auto Rotation" patch has it - but only
    // while the keyboard and mouse were used last; a controller gets the
    // game's own camera back (engine/mouse_camera.h).
    {"mouse_auto_rotation", "マウス使用時の自動回転", {"Off", "On"}, 0,
     "無効時は移動中もカメラの向きを維持します。コントローラー使用時は原作のカメラ動作になります。", false},
    {"draw_cursor", "ポインターを表示", {"Off", "On"}, 1,
     "ゲームにはマウスポインターがないため、bbhost が描画します。", false},
    // The game's key guide and its tutorial notes name a pad button as an
    // image tag; the port rewrites the tag into the key bound to it
    // (engine/key_prompts.h). Auto follows the device last used (host_input_device).
    {"button_prompts", "ボタン表示", {"Auto", "Keyboard", "Controller"}, 0,
     "ゲーム内の操作表示です。自動では最後に使ったキーボードまたはコントローラーに合わせます。", false},
    // The controller's sticks (host/window.cpp stick_deadzone): radial, from
    // the centre - inside the inner deadzone the stick reads as at rest, from
    // the outer one out as pushed all the way, and between them the travel is
    // spread over the whole range. 0% and 100%, the defaults, leave the stick
    // as it comes (the game keeps its own small deadzone).
    {"left_stick_inner", "左スティックの内側デッドゾーン", signed_steps(0, 50, "%", false), 0,
     "中心からどこまで動かすと入力されるかを設定します。手を離してもキャラクターが動く場合は増やしてください。0% はすべての入力を通します。", false},
    {"left_stick_outer", "左スティックの外側デッドゾーン", signed_steps(50, 100, "%", false), 50,
     "最大入力とする位置です。劣化したスティックで全力移動できない場合は減らしてください。100% はスティックの端です。", false},
    {"right_stick_inner", "右スティックの内側デッドゾーン", signed_steps(0, 50, "%", false), 0,
     "中心からどこまで動かすとカメラが回るかを設定します。手を離しても回る場合は増やしてください。0% はすべての入力を通します。", false},
    {"right_stick_outer", "右スティックの外側デッドゾーン", signed_steps(50, 100, "%", false), 50,
     "最大入力とする位置です。減らすと浅い傾きでカメラが最高速度に達します。100% はスティックの端です。", false},
    // The post-processing switches. They follow their settings live
    // (engine/graphics_patch.cpp, host/shader_patch.h).
    {"ssao", "アンビエントオクルージョン", {"Off", "On"}, 1,
     "画面空間の接触陰影です。無効にすると軽くなります。", false},
    // Not a byte patch: the blur shader passes every pixel through
    // (host/shader_patch.h).
    {"motion_blur", "モーションブラー", {"Off", "On"}, 1,
     "カメラやキャラクターの速い動きをぼかします。", false},
    {"anti_alias", "アンチエイリアス", {"Off", "On"}, 1,
     "描画後の画面に原作の輪郭補正を適用します。TAA・FSR・DLSS やプラグインの輪郭補正が動作中は、二重処理によるぼけを防ぐため無効です。",
     false},
    // Off is YEBIS's own switch, cleared each view (engine/graphics_patch.cpp):
    // none of the blur's passes run.
    {"depth_of_field", "被写界深度", {"Off", "On"}, 1,
     "遠景と、カットシーンで焦点外の部分をぼかします。屋内では効果が少ない場合があります。無効にするとカットシーンを含め、ぼかし処理を省きます。",
     false},
    {"chromatic_aberration", "色収差", {"Off", "On"}, 1,
     "画面端の色ずれです。", false},
    // Two of the area's draw parameters (param/drawparam), scaled or cleared
    // as the game hands them to YEBIS each frame - engine/graphics_patch.cpp.
    // 10 is the area's own glare; 0 is none.
    {"bloom", "ブルーム", hundred(), 100,
     "明るい光のにじみです。100 が原作で、下げると弱くなります。", false},
    {"vignette", "ビネット", {"Off", "On"}, 1,
     "一部のエリアで画面の周辺を暗くする効果です。", false},
    // The area's shadow cascades, stretched past the first split: 1 + 0.2 a
    // step, so the game's slider row (0..10) is the same setting.
    {"shadow_distance", "影の描画距離",
     {"x1.0", "x1.2", "x1.4", "x1.6", "x1.8", "x2.0", "x2.2", "x2.4", "x2.6", "x2.8", "x3.0"}, 0,
     "日光と月光の影を描く距離です。広げると影の解像感が下がります。", false},
    // The dynamic lights' shadows (the scene context's +0x296e, which its
    // constructor sets): off clears it each view (engine/graphics_patch.cpp).
    // Was Kyo's patches/disable-dynamic-light-shadow.toml: a bbhost.toml that
    // turned it on seeds Off (host_options_load).
    {"dynamic_shadows", "動的な光源の影", {"Off", "On"}, 1,
     "ランプ、松明、炎など動いたり揺れたりする光源の影です。無効にすると軽くなります。即時反映します。",
     false},
    // The shadow maps' sizes (engine/patch_manifest.cpp
    // shadow_resolution_manifest): the sun's at 4096 whatever the area's
    // ShadowParam asks, the dynamic lights' spot maps and cube faces larger
    // than the game's 512 and 256. Made again while the game runs
    // (live_shadows_request) from a render-target heap grown at the start for
    // up to Ultra (engine/live_resolution.cpp); Giga past that waits for the
    // next start (live_shadows_can: the pill).
    {"shadow_resolution", "影の解像度", {"Lowest", "Low", "Default", "High", "Ultra", "Giga"}, 2,
     "太陽・ランプ・松明の影の精細度です。標準は原作。最低は 512/128/64、低は 1024/256/128、高は 4096/1024/512、最高は 4096/2048/1024 で約 500 MB 追加、超高はすべて 4096 でメモリーと VRAM を大きく消費します。最高までは即時反映。超高は起動時から使用していない場合、再起動が必要です。標準時のみパフォーマンスモードの低解像度設定を使います。",
     true},
    // Two more of the area's numbers, scaled: 5 is the area's own, 0 none, 10
    // twice it.
    {"ao_strength", "陰影の濃さ", hundred(), 50,
     "接触部分や隅の陰影の濃さです。50 が原作です。", false},
    {"saturation", "彩度", hundred(), 50,
     "色の鮮やかさです。50 が原作、0 は白黒です。", false},
    // The fog table's opacities (engine/graphics_patch.cpp): 10 the area's own.
    {"fog", "霧", hundred(), 100,
     "遠方と高さ方向の霧の濃さです。100 が原作、0 は霧なしです。", false},
    // The game's level-of-detail bias (engine/graphics_patch.cpp): Full keeps
    // every model at its finest mesh, Low and Lowest move each to a coarser
    // one sooner. Lowest is the community "model LOD 2" (-8% GPU, models
    // visibly simpler up close).
    {"model_detail", "モデルの精細度", {"Lowest", "Low", "Normal", "Full"}, 2,
     "標準は原作の設定です。低・最低では簡略モデルへの切り替えを早め、最高では切り替えません。", false},
    // The textures' size in video memory (host/textures.cpp quality_drop):
    // each texture's top mip levels left out of its image, its own smaller
    // levels in their place - High none (1.0x), Medium one (0.5x), Low two
    // (0.25x), Lowest three (0.125x: mip levels halve, so no 0.10x). Live: a
    // texture is made again at its next use.
    {"texture_quality", "テクスチャ品質", {"Lowest", "Low", "Medium", "High"}, 3,
     "VRAM 内のテクスチャの大きさです。高は原作、中は縦横半分（容量４分の１）、低は４分の１（容量16分の１）、最低は８分の１です。小さいミップを利用して負荷と容量を減らすため、画面は柔らかくなります。地形・装備は全体を縮小し、その他は長辺 256 以上を維持します。メニューと色補正は変更しません。エフェクトは別設定です。次の描画から反映します。",
     false},
    // The same for the effects' textures (sfx/: fire, blood, magic, smoke),
    // apart from the rest (host/textures.cpp quality_drop).
    {"sfx_texture_quality", "エフェクトのテクスチャ品質", {"Lowest", "Low", "Medium", "High"}, 3,
     "炎・血・魔法・煙などのテクスチャを別に設定します。高は原作、中は縦横半分、低は４分の１、最低は８分の１です。即時反映します。",
     false},
    // Anisotropic filtering for the 3D's textures (host/render.cpp
    // anisotropy_samplers): Game leaves each sampler as the game made it;
    // 2x-16x raise every smoothly filtered, mipmapped texture a camera draw
    // samples to at least that (the device's limit at most). The post effects,
    // the HUD, depth comparisons and nearest samplers (the Bloodborne PS1's
    // unfiltered textures) keep theirs.
    {"anisotropic_filtering", "異方性フィルタリング", {"Game", "2x", "4x", "8x", "16x"}, 0,
     "斜めに見た床・道・壁の模様を鮮明にします。原作は一部の面のみ、16x が最も鮮明ですが GPU 負荷が増えます。HUD と画面効果は変更しません。即時反映します。",
     false},
    // The game's screen space reflections, off as it shipped
    // (engine/patch_manifest.cpp ssr_manifest): its reflection pass at the
    // game's own half size. On while the game runs puts the patch in and
    // makes the scene's targets again; off clears the pass's switch each view
    // (engine/graphics_patch.cpp). The first time, a bbhost.toml that turned
    // the old patch file on ([patches] screen-space-reflections) seeds it
    // (host_options_load).
    {"ssr", "画面空間の反射", {"Off", "On"}, 0,
     "濡れた路面、水、磨かれた床に、画面内の映像を反射します。反射品質の解像度を使用し、正面でも消えません。GPU 負荷が増えます。即時反映します。",
     false},
    // The reflection pass's size (engine/patch_manifest.cpp ssr_manifest):
    // Quarter, the default, a quarter of the render size each way (480x270 at
    // 1080p: ~1 ms a frame on the Radeon 8060S), Half the game's own (3.9 ms).
    // Changes at once while reflections are in: the patch made again and the
    // scene's targets with it (targets_follow_settings). Greyed while Screen
    // space reflections is off (row_greyed).
    {"ssr_quality", "反射品質", {"Quarter", "Half"}, 0,
     "反射の解像度です。推奨の４分の１は縦横それぞれを縮小し、半分の約４分の１の GPU 負荷です。半分は原作と同じで鮮明ですが重くなります。即時反映します。",
     false},
    // Kyo's HD Motion Blur patches as a setting (engine/patch_manifest.cpp
    // hd_motion_blur_manifest): their buffers at a share of the render size
    // rather than at one fixed size each. The targets are made as the game
    // starts. A bbhost.toml that turned one of the old files on seeds it
    // (host_options_load).
    {"hd_motion_blur", "高精細モーションブラー", {"Off", "On"}, 0,
     "固定の 640x360 の代わりに、下の品質で速度マップと深度を描画します。カメラのブラーも有効にし、安定したぼかしを得ます。サイズに応じて GPU と VRAM の負荷が増えます。モーションブラー有効時のみ表示。即時反映します。",
     false},
    // Its entries were Quarter, Half and Full until 2026-11-05 (an options
    // file's read as Low, Medium and High: host_options_load); greyed while HD
    // motion blur is off (host_options_draw_page).
    {"hd_motion_blur_res", "高精細ブラーの品質", {"Low", "Medium", "High"}, 2,
     "速度マップの解像度です。低は描画サイズの縦横４分の１、中は半分（原作）、高は全体です。解像度と拡大描画の設定に追従します。高精細モーションブラーが必要です。即時反映します。",
     false},
    // The temporal upscaler (host/upscaler.h, Numity1504's bridge): its
    // picture in place of the game's after the 3D, before the post effects.
    // Off by default; the order of the values is the settings object's
    // (0 off, then DLSS, FSR, FSR 3, TAA) turned round for the menu (Off,
    // TAA, FSR, DLSS). TAA is bbhost's own (host/upscale_pass.cpp): it needs
    // no bridge and never draws the 3D smaller. One choice of the row, so TAA
    // and FSR or DLSS are never on together.
    {"upscaler", "拡大描画", {"Off", "TAA", "FSR 3", "FSR 4", "DLSS"}, 0,
#if defined(_WIN32)
     "TAA (bbhost's own temporal anti-aliasing, any GPU), FSR 4 (AMD's newest, where the GPU and driver offer it), FSR 3 "
     "(any GPU) or DLSS (NVIDIA RTX) in place of the game's own anti-aliasing. FSR and DLSS need upscaler_bridge.dll and "
     "AMD's FidelityFX files beside bbhost.exe, and for DLSS nvngx_dlss.dll; without them they stay off. FSR 4 runs as "
     "FSR 3 where it cannot run. Changes at once.",
#else
     // (host/upscaler.h upscaler_offered: FSR 3 and DLSS where they run, never FSR 4.)
     "TAA (bbhost's own temporal anti-aliasing, any GPU), FSR 3 (AMD's, any GPU: the same FSR 3.1.6 as on Windows) or "
     "DLSS (an NVIDIA RTX card with NVIDIA's driver) in place of the game's own anti-aliasing. FSR 3 needs "
     "libbbhost_fidelityfx_vk.so beside bbhost, DLSS libbbhost_ngx_bridge.so and NVIDIA's libnvidia-ngx-dlss.so, as the "
     "packages have them. FSR 4 and frame generation run on Windows only. Changes at once.",
#endif
     false},
    // TAA's blend (host/upscale_pass.cpp record_taa): how much of each new
    // frame goes into the picture, the rest the history - 0% a fifth, 50% a
    // tenth (Numity1504's, the default), 100% a twenty-fifth (Unreal Engine
    // 4's own TAA). Moving fast, up to a quarter all the same (less trail).
    {"taa_strength", "TAA の強さ", signed_steps(0, 100, "%", false), 50,
     "高いほど過去フレームを多く使い、輪郭が安定しますが残像が増えやすくなります。低いほど鮮明で追従が速くなります。標準は 50%、100% は Unreal Engine の TAA 相当です。TAA 使用中に即時反映します。",
     false},
    // The upscaler's render size against the output (Resolution): Native AA
    // the same (DLAA, FSR native AA), Quality 1/1.5, Balanced about 1/1.7,
    // Performance 1/2 each way (engine/render_scale.cpp). A change rebuilds
    // the scene contexts at the new size (live resolution's rebuild), as
    // Upscaling turned on or off does.
    {"upscale_quality", "拡大描画の品質", {"Performance", "Balanced", "Quality", "Native AA"}, 2,
     "拡大前の 3D のサイズです。等倍 AA は全体、品質優先は約３分の２、均衡は約 59%、速度優先は縦横半分です。低いほど高速ですが柔らかくなります。メニュー・HUD・画面効果のサイズは維持します。即時反映します。",
     false},
    // DLSS's render preset (NGX's DLSS_Hint_Render_Preset for every quality;
    // needs upscaler_bridge 1.1.0 and nvngx_dlss 310 or newer): K the
    // transformer model and NVIDIA's own for DLAA, Quality and Balanced, J
    // like it with less ghosting and more flicker, L made for Ultra
    // Performance, M for Performance.
    {"dlss_preset", "DLSS プリセット", {"K", "J", "L", "M"}, 0,
     "DLSS のモデルです。推奨の K は鮮明な Transformer モデル。J は残像が少なくちらつきがやや増加。L は低解像度向け、M は速度優先向けです。DLSS 専用です。",
     false},
    // RCAS over the upscaler's picture (host/upscale_pass.cpp): FSR 3 and 4
    // through the bridge and DLSS do no sharpening of their own. 0% none, the
    // default (the picture as the upscaler leaves it); 100% AMD's most.
    {"sharpness", "鮮明さ", signed_steps(0, 100, "%", false), 0,
     "TAA・FSR・DLSS 後の 3D を鮮明にします。画面効果と HUD には適用しません。0% は補正なし、100% では輪郭に縁取りが出る場合があります。拡大描画の有効化が必要です。",
     false},
    // AMD's frame generation (host/framegen.h): its swapchain in the
    // presenter's place; a frame it makes from the 3D between two of the
    // game's, the HUD and the menus put back over each. With Upscaling off (or
    // one this GPU cannot run) it takes the same depth and motion at the full
    // size (host/upscale_pass.cpp).
    {"frame_generation", "フレーム生成", {"Off", "On"}, 0,
     "AMD FidelityFX frame generation: a frame made between each two of the game's, from the 3D alone - "
     "the HUD and the menus are never interpolated. Works with any Upscaling choice, Off included, on any GPU. "
     "While on, the game runs at most half your monitor's refresh rate and frame generation doubles it to the refresh rate (90 + 90 at 180 Hz): higher Frame rates are greyed out, and one set higher comes down to half as it turns on. Display-Limited runs the game at half the refresh rate and generates the frames between. Loading screens stay uncapped. Needs AMD's amd_fidelityfx_framegeneration_vk.dll and its loader beside bbhost.exe. Changes at once.",
     false},
    // Where the upscaler's motion for what moves of itself comes from
    // (host/upscale_pass.cpp): the game's motion blur velocity map, or the
    // same meshes' motion rebuilt - the game's velocity shaders writing their
    // exact screen motion at the render size (host/translation_cache.cpp) in
    // 8-, 16- or 32-bit, for comparing accuracy and cost.
    // No longer shown (2026-11-05): the characters' motion comes from their own
    // vertex shaders (upscale_pass.cpp's capture), the rest from the depth
    // and the game's velocity map, so the map is always the game's own;
    // BBHOST_MOTION_VECTORS=8|16|32 still rebuilds it (a test).
    {"motion_vectors", "動きベクトル", {"Velomap", "Rebuilt 8-bit", "Rebuilt 16-bit", "Rebuilt 32-bit"}, 0,
     "拡大描画とフレーム生成で使用するキャラクターの動き情報です。速度マップは原作のブラー用情報、再構築は描画解像度で正確に出力します。ビット数が多いほど精密ですがメモリーと帯域を消費します。再構築時は原作のブラーを無効にします。即時反映します。",
     false},
    // Where the upscalers' motion for the characters comes from
    // (host/upscale_pass.cpp): Precise, their own vertex shaders' positions,
    // this frame's and the last, drawn into the motion (the capture); Off, the
    // game's motion blur velocity map alone. The capture's cost measured at 4K:
    // about 1%. Off was called Velocity map until 2026-11-10 (an options file's
    // reads as Off: host_options_load); the two choices keep their places.
    {"char_motion", "キャラクターの動き", {"Precise", "Off"}, 0,
#if defined(_WIN32)
     "How TAA, FSR, DLSS and frame generation follow the hunter, enemies and other moving characters (the camera's own "
#else
     "How TAA, FSR 3 and DLSS follow the hunter, enemies and other moving characters (the camera's own "
#endif
     "motion always comes from the depth). Precise (the default): each character drawn a second time into the motion, "
     "at its own position this frame and the last - clean edges and no trail behind a running character, for about 1% "
     "of the frame rate at 4K. Off: the game's motion blur map alone - a little faster, with softer edges and a faint "
     "trail behind running characters. Turn it off if your GPU or driver has trouble with Precise, or for the last bit "
     "of speed on a weak GPU. Changes at once.",
     false},
    // The render resolution. res_width and res_height are two int32s the eboot
    // never writes, so this is a data patch and every pass scales together -
    // see engine/graphics_patch.cpp. The game's pick list shows six entries
    // and scrolls through more. In order of size within each shape - 16:9
    // (5K and 8K at its end), 16:10, 4:3, 5:4, 1:1, then the wide ones: 21:9
    // (64:27, 43:18 and 12:5 screens), 16:5 (two 16:10 screens), 32:9, 18:5,
    // 4:1 (three 4:3), 24:5 (three 16:10), 5:1, 16:3 (three 16:9) and three
    // 21:9 screens - and saved by name, not by place, so a saved entry
    // survives the order changing (engine/option_menu.cpp's kResolutions,
    // whose game list holds 32 of them, and tools/pc_option_messages.tsv's
    // texts follow this list). The other shapes are
    // engine/live_resolution.cpp's: the cameras widened (or the height kept for
    // a taller one), the HUD at 16:9 in the middle, the plates and the lock-on
    // where the camera puts them. 960x600 and 1024x640 are the Deck's screen at
    // 75% and 80%: it holds 60 fps there (pixel-bound at 1280x800, ~45), the
    // 3D at that size and the HUD at the screen's (a Resolution below the
    // window's, engine/render_scale.cpp). The largest want a GPU to match - or
    // Upscaling below Native AA, which draws the 3D at a fraction of them.
    {"resolution", "解像度", {"640x360", "960x540", "1280x720", "1366x768", "1440x810", "1600x900", "1920x1080", "2560x1440",
                                  "2880x1620", "3200x1800", "3840x2160", "5120x2880", "7680x4320", "960x600", "1024x640",
                                  "1280x800", "1440x900", "1680x1050", "1920x1200", "2560x1600", "2880x1800", "3840x2400",
                                  "800x600", "1024x768", "1280x960", "1440x1080", "1600x1200", "2048x1536", "1280x1024",
                                  "1080x1080", "1440x1440", "1920x1920", "2560x1080", "3440x1440", "3840x1600", "5120x2160",
                                  "6880x2880", "3840x1200", "3840x1080", "5120x1440", "7680x2160", "4320x1200", "3840x960",
                                  "4800x1200", "5760x1200", "7680x1600", "5400x1080", "3840x720", "4800x900", "5760x1080",
                                  "7680x1440", "11520x2160", "7680x1080", "10320x1440"}, 6,
     "3D の描画解像度です。原作は 1920x1080。ウルトラワイド・複数画面・16:10・4:3・正方形にも対応し、HUD は中央の 16:9 に維持します。画面より小さい場合、拡大描画が無効でも HUD・メニュー・画面効果は画面サイズを維持します。非常に大きいサイズは再起動や、等倍 AA より低い拡大品質が必要な場合があります。",
     true},
    // The shaders' form on the GPU (host/translation_cache.h): HLSL is the
    // lifted shaders printed as HLSL and compiled by DXC on this machine,
    // taken from the cache while the rest compile behind the game; Lifted is
    // the lifter's own SPIR-V alone. The same pictures either way.
    {"shaders", "シェーダー", {"HLSL", "Lifted"}, 0,
     "再起動が必要です。推奨の HLSL はプレイ中に最適化したシェーダーを作り、次回起動時に使用します。変換済み SPIR-V は直接変換のみです。画質は同じです。",
     true},
    {"skip_logos", "企業ロゴをスキップ", {"Off", "On"}, 1,
     "タイトル前の３つのロゴと、前回「ゲーム終了」を使わなかった場合の警告を省略します。", true},
    // Kyo's Giga Performance Patch's post-effects (engine/patch_manifest.cpp's
    // performance-mode): its YEBIS sites put in or taken out while the game
    // runs (apply), SSAO, anti-aliasing, motion blur, chromatic aberration and
    // the dynamic lights' shadows through their settings' own switches
    // (engine/graphics_patch.cpp). Its shadow map sizes (Shadow resolution
    // Default's place) are made again with it (live_shadows_request).
    {"performance_mode", "パフォーマンスモード", {"Off", "On"}, 0,
     "速度を優先して SSAO、ブラー、輪郭補正、被写界深度、色収差、ブルーム、ランプ・松明などの影を無効にします。太陽・月の影と霧は維持します。即時反映し、影の解像度が標準の場合は影も低解像度になります。", false},
    // The old switch for the developers' debug menu, which the Debug Menu
    // plugin replaced (engine/debug_menu.h): no row shows it, and an "有効" an
    // older bbhost saved turns the plugin on once (main.cpp).
    {"debug_menu", "デバッグメニュー", {"Off", "On"}, 0, "デバッグメニュープラグインに置き換わりました。", true},
    // The PC enhancements: additions to the game itself, each read once at
    // start (main.cpp hands them to config_set_enhancements), all on unless
    // bbhost.toml's [world] says otherwise. Off, all of them, is the game as it
    // shipped. (The Old Hunters, a fourth in v0.2.9, is always on: engine/patch_manifest.cpp.)
    {"change_appearance", "狩人の夢の鏡", {"Off", "On"}, 1,
     "再起動が必要です。狩人の夢の鏡で未使用の容姿編集画面を開きます。", true},
    {"rebirth", "祭壇で生まれ変わる", {"Off", "On"}, 1,
     "再起動が必要です。ヤーナムの石があれば、嘆きの祭壇でレベルと能力値を振り直せます。", true},
    {"five_players", "５人プレイ", {"Off", "On"}, 1,
     "再起動が必要です。メンシス・高楼・辺境・DLCで侵入者を２人に増やします。全参加者の有効化が必要です。", true},
    // Off unless bbhost.toml's [world] turns it on: a joke, not an addition.
    {"skill_issue", "SKILL ISSUE", {"Off", "On"}, 1, "再起動が必要です。死亡時の表示を SKILL ISSUE に変更します。",
     true},
    {"full_phantom_hp", "協力・侵入時の最大 HP", {"Off", "On"}, 1,
     "他人の世界でも最大 HP を維持します。訪問者自身の設定を使用します。再起動が必要です。", true},
    {"auto_replenish", "消耗品の自動補充", {"Off", "On"}, 1,
     "自分の世界での死亡時や狩人の夢への帰還時、火炎瓶や投げナイフなどを倉庫から補充します。再起動が必要です。",
     true},
    // bbhost's own menus - the setup window, F9 and F10 - and everything in
    // them grow with it (launcher_ui set_scale). 100% is their usual size: the
    // in-game menus' follows the game's resolution (height / 900), the setup
    // window's is 1120x720 at Windows' scaling, and grows (host/launcher.cpp).
    {"menu_text_size", "メニューの文字サイズ", {"100%", "125%", "150%", "175%", "200%", "250%", "300%", "350%", "400%"}, 0,
     "F9・F10・設定画面の文字とメニューを拡大します。100% が標準です。画面に収まらない場合はスクロールします。",
     false},
    // The colours of the same menus (launcher_ui recolour): 0 degrees and 0 percent
    // are their own blue and violet. Hue turns every colour in them round the
    // colour wheel but the green, amber and red that mean ok, a warning and a
    // problem; vibrance takes colour out (-100%: black and white) or adds it.
    {"menu_hue", "メニューの色相", signed_steps(-180, 180, "°"), 180, "設定画面と F9・F10 の色相を調整します。0 度は標準の青と紫です。",
     false},
    {"menu_vibrance", "メニューの鮮やかさ", signed_steps(-100, 100, "%"), 100, "メニューの色の鮮やかさです。-100% は白黒、0% は標準、+100% は最大です。", false},
    // The game's own text: the scale MenuFont_01 is mapped with, kept in the
    // font map and rewritten there when it changes (engine/graphics_patch.cpp,
    // map_font_hook) - what Kyo's "文字サイズを 50% に変更" patch set to 0.5. A movie
    // reads it as it makes its font handles: a menu window when it is made
    // again, the HUD at a loading screen or the title (tried 2026-11-13).
    {"text_scale", "文字の大きさ", signed_steps(50, 100, "%", false), 50,
     "ゲーム内の文字の大きさです。100% が原作。高解像度では小さくできます。メニューは開き直した時、HUD は次のロードかタイトルへ戻った後に反映します。",
     false},
};

// The in-game menu's pages, one a header, in the setup window's order: Game
// (its Display and Online cards: the window and the frame rate, the start,
// then what only the game shows, then the account), Graphics, Controls
// (INPUT, over the bindings), PC Enhancements - the patches and the plugins
// after it (host/ingame_menu.cpp) - and Settings (the menus, the updates).
const Row g_rows[] = {
    {Row::Header, "Game"},
    {Row::Option, nullptr, kWindowMode},
#if defined(_WIN32)
    {Row::Option, nullptr, kStayBorderless},  // it does nothing elsewhere
#endif
    {Row::Option, nullptr, kVsync},
    {Row::Option, nullptr, kFrameCap},
    {Row::Option, nullptr, kFrameClock},
    {Row::Option, nullptr, kEffectTiming},
#if defined(_WIN32)
    {Row::Option, nullptr, kFrameGeneration},  // AMD's frame generation, Windows' alone: no row elsewhere
#endif
    {Row::Option, nullptr, kSkipLogos},
    {Row::Option, nullptr, kPerformanceMode},
    {Row::Option, nullptr, kFpsCounter},
    {Row::Option, nullptr, kMemoryCounter},
    {Row::Option, nullptr, kPerfOverlay},
    {Row::Option, nullptr, kFov},
    {Row::Option, nullptr, kCameraDistance},
    {Row::Option, nullptr, kCameraHeight},
    // The private server's account (account_page, after the rows above). No
    // passwords: link this PC to the account signed into on the website with
    // Discord, or make one held by this PC with a recovery code. The token is
    // kept in the options file.
    {Row::Info, nullptr, -1, 0},
    {Row::Info, nullptr, -1, 1},
    {Row::Action, "Discord と連携", -1, 0},
    {Row::Text, "名前", -1, 0},
    {Row::Action, "この PC でアカウントを作成", -1, 1},
    {Row::Text, "復旧コード", -1, 1},
    {Row::Action, "アカウントを復旧", -1, 2},
    {Row::Action, "ウェブサイトのログインコード", -1, 3},
    {Row::Action, "ログアウト", -1, 4},
    {Row::Header, "入力"},
    {Row::Option, nullptr, kMouseMenu},
    {Row::Option, nullptr, kMouseCamera},
    {Row::Option, nullptr, kMouseSens},
    {Row::Option, nullptr, kMouseInvertX},
    {Row::Option, nullptr, kMouseInvertY},
    {Row::Option, nullptr, kMouseAutoRotation},
    {Row::Option, nullptr, kDrawCursor},
    {Row::Option, nullptr, kButtonPrompts},
    {Row::Option, nullptr, kLeftStickInner},
    {Row::Option, nullptr, kLeftStickOuter},
    {Row::Option, nullptr, kRightStickInner},
    {Row::Option, nullptr, kRightStickOuter},
    {Row::Header, "グラフィックス"},
    {Row::Option, nullptr, kTextureQuality},
    {Row::Option, nullptr, kSfxTextureQuality},
    {Row::Option, nullptr, kModelDetail},
    {Row::Option, nullptr, kTextScale},
    {Row::Option, nullptr, kAnisotropicFiltering},
    {Row::Option, nullptr, kHdMotionBlur},
    {Row::Option, nullptr, kHdMotionBlurRes},
    {Row::Option, nullptr, kSsao},
    {Row::Option, nullptr, kMotionBlur},
    {Row::Option, nullptr, kAntiAlias},
    {Row::Option, nullptr, kDepthOfField},
    {Row::Option, nullptr, kChromaticAberration},
    {Row::Option, nullptr, kVignette},
    {Row::Option, nullptr, kSsr},
    {Row::Option, nullptr, kSsrQuality},
    {Row::Option, nullptr, kDynamicShadows},
    {Row::Option, nullptr, kShadowResolution},
    // Frame generation on Graphics too, beside the upscaler (Kyo: on both
    // pages); the same setting as the Game page's row, and like it Windows'
    // alone.
#if defined(_WIN32)
    {Row::Option, nullptr, kFrameGeneration},
#endif
    {Row::Option, nullptr, kResolution},
    {Row::Option, nullptr, kUpscaler},
    {Row::Option, nullptr, kTaaStrength},  // (shown with TAA only: row_hidden)
    // The preset row (preset_row): for FSR its preset - the Upscaling quality
    // setting, which has no row of its own - for DLSS that quality and DLSS's
    // preset, greyed with Upscaling off. The Resolution the upscaler brings the
    // 3D back to above them, Sharpness over the upscaler's picture below.
    {Row::Option, nullptr, kDlssPreset},
    {Row::Option, nullptr, kSharpness},
    {Row::Option, nullptr, kCharMotion},  // (shown while an upscaler or frame generation takes the motion: row_hidden)
    {Row::Option, nullptr, kBloom},
    {Row::Option, nullptr, kShadowDistance},
    {Row::Option, nullptr, kAoStrength},
    {Row::Option, nullptr, kSaturation},
    {Row::Option, nullptr, kFog},
    {Row::Option, nullptr, kShaders},
    {Row::Header, "PC 追加機能"},
    {Row::Option, nullptr, kChangeAppearance},
    {Row::Option, nullptr, kRebirth},
    {Row::Option, nullptr, kFivePlayers},
    {Row::Option, nullptr, kSkillIssue},
    {Row::Option, nullptr, kFullPhantomHp},
    {Row::Option, nullptr, kAutoReplenish},
    {Row::Header, "設定"},
    {Row::Option, nullptr, kMenuTextSize},
    {Row::Option, nullptr, kMenuHue},
    {Row::Option, nullptr, kMenuVibrance},
    // The updates (updates_page, after the rows above).
    {Row::Info, nullptr, -1, 10},
    {Row::Info, nullptr, -1, 11},
    {Row::Action, "更新を確認", -1, 10},
    {Row::Action, "リリースページを開く", -1, 11},
    {Row::Close, "閉じる"},
};
constexpr int kRowCount = static_cast<int>(sizeof(g_rows) / sizeof(g_rows[0]));

std::atomic<bool> g_open{false};
// The in-game menu's changes (host_options_draw_page, on the presenter's
// thread), run by the window's thread at its next pump (host_options_frame):
// a setting can touch the window (fullscreen) and the file.
std::mutex g_post_mu;
std::vector<std::function<void()>> g_posted;  // under g_post_mu
// What the menu set and the pump has not applied yet, by setting (-1 none):
// the menu shows it at once, not the value a pump later.
std::array<int, kSettingCount> g_pending = [] {
    std::array<int, kSettingCount> a{};
    a.fill(-1);
    return a;
}();
bool g_dirty = false;  // something changed since the last save
// Which settings are the player's own: the options file held a value for it
// when it was read, or it was set since (set_index); the rest keep their
// defaults, the table's or bbhost.toml's. For host_opt_saved, and save's
// Effect timing.
std::array<bool, kSettingCount> g_saved{};

// What every setting starts with: the table's own indices, taken before
// anything is loaded over them, then whatever bbhost.toml sets for it - the
// frame cap, the logos, and a package's resolution and window mode (the
// Steam Deck kit's) - so the menus' Defaults go back to those.
std::array<int, kSettingCount> g_default_index = [] {
    std::array<int, kSettingCount> a{};
    for (int id = 0; id < kSettingCount; ++id) a[static_cast<std::size_t>(id)] = g_set[id].index;
    return a;
}();

// A resolution change made while the game runs (engine/live_resolution.h)
// asks to be kept, the way Dark Souls III's ApplyScreenSettingJob does: the
// new size at once, then the game's own yes/no dialog, ten seconds, or it
// goes back. While it waits, g_res_prev is the entry it goes back to and the
// file keeps.
int g_res_prev = -1;
bool g_res_reverting = false;  // the change back itself asks nothing

// Whether the resolution entry `index` changes while the game runs. One past
// what this run's render-target heap was grown for at its start (5120x2160
// after a smaller start) applies on the next run like any restart setting.
bool res_live(int index) {
    unsigned w = 0, h = 0;
    return live_resolution_available() && std::sscanf(g_set[kResolution].values[static_cast<std::size_t>(index)], "%ux%u", &w, &h) == 2 &&
           live_resolution_can(w, h);
}

// The account rows. The calls run on their own thread so the menu keeps
// drawing, and their outcome is the two status lines.
std::string g_field[2];       // name, recovery code (never saved)
std::mutex g_acct_mu;
std::string g_acct_status;    // the last outcome, for the first status line
std::string g_acct_detail;    // a code the player needs (link, recovery, website)
std::atomic<bool> g_acct_busy{false};
std::atomic<bool> g_acct_save{false};    // signed in or out: the token wants saving
std::atomic<bool> g_link_cancel{false};  // stops a link that is waiting for approval

void set_account_lines(const std::string& status, const std::string& detail) {
    std::lock_guard<std::mutex> lk(g_acct_mu);
    g_acct_status = status;
    g_acct_detail = detail;
}

// The server no longer takes this PC's sign-in (net::account_refused): what
// F10's account section (the Game page) and the setup window say about it.
constexpr const char kRefusedStatus[] = "ログインの有効期限切れ";
constexpr const char kRefusedText[] = "ログインの期限が切れたか、この PC がログアウトされました。オンラインには再ログインが必要です。";

std::string signed_in_line() {
    // The game read its NpId when it went online, so a sign-in after that
    // names the account to the server but not to the game until the next run.
    if (net::account_refused()) return std::string(kRefusedStatus) + " (" + net::account_name() + ")";
    return "ログイン中: " + net::account_name() +
           (hle_np_context_started() ? " （次回起動で完全に反映）" : "");
}

// Link with Discord: a code the player approves on the website's /link page,
// signed in there with Discord; this thread polls until it is approved,
// denied or expired.
void link_thread() {
    net::DeviceLink link;
    std::string err;
    if (!net::account_link_start(link, err)) {
        set_account_lines("連携に失敗: " + err, "");
        g_acct_busy.store(false);
        return;
    }
    const std::string uri = link.verification_uri + "?code=" + link.user_code;
    set_account_lines("ウェブサイトでの承認を待っています...",
                      "コード " + link.user_code + "  -  承認先: " + link.verification_uri);
    SDL_OpenURL(uri.c_str());
    int interval = link.interval;
    for (int waited = 0; waited < link.expires_in && !g_link_cancel.load();) {
        SDL_Delay(static_cast<Uint32>(interval) * 1000u);
        waited += interval;
        const std::string status = net::account_link_poll(link, err);
        if (status == "pending") continue;
        if (status == "slow_down") {
            ++interval;
            continue;
        }
        if (status == "approved") {
            set_account_lines(signed_in_line(), "");
            g_acct_save.store(true);
        } else {
            set_account_lines(status == "denied" ? "ウェブサイトで連携が拒否されました"
                              : status == "expired" ? "コードが期限切れです。もう一度連携してください"
                                                    : "連携に失敗: " + err, "");
        }
        g_acct_busy.store(false);
        return;
    }
    set_account_lines(g_link_cancel.load() ? "連携をキャンセルしました" : "コードが期限切れです。もう一度連携してください", "");
    g_acct_busy.store(false);
}

// The UPDATES buttons, and whether each can do anything right now.
void update_action(int which) {
    if (which == 10) updater::check_now();
    if (which == 11) updater::open_release_page();
}

bool action_idle(int which) {
    // A website code needs a sign-in the server still takes.
    if (which == 3 && net::account_refused()) return false;
    if (which < 10) return !g_acct_busy.load();
    if (updater::busy()) return false;
    if (which == 11) return updater::update_available();
    return true;
}

void account_action(int which, const std::string& name, const std::string& code) {
    if (which >= 10) {
        update_action(which);
        return;
    }
    if (which == 4) {  // sign out, also cancelling a link in progress
        g_link_cancel.store(true);
        std::thread([] {
            net::account_logout();
            set_account_lines("ログアウトしました", "");
            g_acct_save.store(true);
        }).detach();
        return;
    }
    if (g_acct_busy.load()) return;
    if ((which == 1 || which == 2) && name.empty()) {
        set_account_lines("先に名前を入力してください", "");
        return;
    }
    if (which == 2 && code.empty()) {
        set_account_lines("先に復旧コードを入力してください", "");
        return;
    }
    if (which == 3 && !net::account_logged_in()) {
        set_account_lines("先にログインしてください", "");
        return;
    }
    g_acct_busy.store(true);
    g_link_cancel.store(false);
    if (which == 0) {
        set_account_lines("サーバーにコードを要求中...", "");
        std::thread(link_thread).detach();
        return;
    }
    set_account_lines(which == 1 ? "アカウントを作成中..." : which == 2 ? "復旧中..." : "コードを要求中...", "");
    std::thread([which, name, code] {
        std::string err, out;
        bool ok = false;
        if (which == 1) {
            ok = net::account_create(name, out, err);
        } else if (which == 2) {
            ok = net::account_recover(name, code, out, err);
        } else {
            ok = net::account_web_code(out, err);
        }
        if (!ok) {
            // A server without accounts says so itself; anything else is the
            // server's refusal (a name taken, a daily limit) in its words.
            set_account_lines(err.find("アカウントサービスがありません") != std::string::npos ? err : "拒否されました: " + err, "");
        } else if (which == 3) {
            set_account_lines(signed_in_line(), "ウェブ用コード " + out + "  -  10分以内にアカウントページで使用してください");
        } else {
            // The recovery code is shown here once and never written anywhere.
            set_account_lines(signed_in_line(), "復旧コード " + out + "  -  一度しか表示されません。今すぐ控えてください");
            g_field[1].clear();
            g_acct_save.store(true);
        }
        g_acct_busy.store(false);
    }).detach();
}

std::string account_status() {
    std::lock_guard<std::mutex> lk(g_acct_mu);
    // A refusal outranks an older "ログイン中：" from the last action.
    const bool refused = net::account_logged_in() && net::account_refused();
    if (!g_acct_status.empty() && !(refused && !g_acct_busy.load() && g_acct_status.rfind("ログイン中: ", 0) == 0))
        return g_acct_status;
    if (refused) return std::string(kRefusedStatus) + " (" + net::account_name() + ")";
    return net::account_logged_in() ? "ログイン中: " + net::account_name() : "未ログイン";
}

std::string account_detail() {
    std::lock_guard<std::mutex> lk(g_acct_mu);
    return g_acct_detail;
}

// The frame-rate values are the labels themselves: "Display-Limited" is 0,
// anything else is the number.
int fps_from_label(const char* v) { return std::atoi(v); }

int fps_index(int fps) {
    for (std::size_t v = 0; v < g_set[kFrameCap].values.size(); ++v) {
        if (fps_from_label(g_set[kFrameCap].values[v]) == fps) {
            return static_cast<int>(v);
        }
    }
    return -1;
}

// The rate the game runs at for these indices of Frame rate, Custom limit and
// its number: the number when ticked, else the list's.
int frame_rate_of(int cap, int custom, int custom_fps) {
    if (reads_on(g_set[kFrameCapCustom], custom)) return fps_from_label(g_set[kFrameCapCustomFps].values[static_cast<std::size_t>(custom_fps)]);
    return fps_from_label(g_set[kFrameCap].values[static_cast<std::size_t>(cap)]);
}
int frame_rate_now() { return frame_rate_of(g_set[kFrameCap].index, g_set[kFrameCapCustom].index, g_set[kFrameCapCustomFps].index); }

std::string options_path() {
    // BBHOST_OPTIONS_PATH: a file of its own, so a scripted test that changes
    // settings (tools/menu_drive.sh) never writes the player's.
    if (const char* e = std::getenv("BBHOST_OPTIONS_PATH"); e && e[0]) {
        return e;
    }
    // The per-user folder (config_user_dir), beside the per-user config: the
    // F10 settings and the account outlive any copy of bbhost. A --config is a
    // profile with a file of its own (bbhost-options-<name>.toml), so a dev
    // server's account never mixes with the live one's. The file from before
    // (beside that config, or the config a package or tree used, or in the
    // working directory) is copied over the first time: nobody is signed out.
    static const std::string path = [] {
        const HostConfig& c = config();
        const std::string file = c.profile.empty() ? std::string("bbhost-options.toml") : "bbhost-options-" + c.profile + ".toml";
        // One beside the exe wins, as a bbhost.toml there does
        // (config_user_file): a copy of bbhost kept with settings of its own.
        // Not in a test's folder of its own (BBHOST_CONFIG_DIR).
        const char* sandbox = std::getenv("BBHOST_CONFIG_DIR");
        if (!(sandbox && sandbox[0])) {
            const std::string local = config_exe_dir() + "/" + file;
            std::ifstream here(local);
            if (here) return local;
        }
        const std::string mine = config_user_dir() + "/" + file;
        std::ifstream have(mine);
        if (have) return mine;
        const auto beside = [](const std::string& f) {
            const std::size_t slash = f.find_last_of("/\\");
            return (slash == std::string::npos ? std::string() : f.substr(0, slash + 1)) + "bbhost-options.toml";
        };
        std::vector<std::string> old;
        if (!c.profile.empty()) {
            old.push_back(beside(c.profile_config));
        } else {
            for (const char* f : {"bbhost.toml"}) old.push_back(beside(f));
            old.push_back(beside(config_exe_dir() + "/bbhost.toml"));
        }
        // A folder of its own (BBHOST_CONFIG_DIR: every test harness) takes the
        // settings but not the [account]: one old file beside several test
        // configs signed each of their profiles in as that player, with a
        // token long replaced, and the server refused them all day.
        const char* own_dir = std::getenv("BBHOST_CONFIG_DIR");
        const bool keep_account = !(own_dir && own_dir[0]);
        for (const std::string& f : old) {
            if (f == mine) continue;
            std::ifstream in(f, std::ios::binary);
            if (!in) continue;
            std::ofstream out(mine, std::ios::binary);
            bool in_account = false, dropped = false;
            for (std::string line; std::getline(in, line);) {
                if (!line.empty() && line[0] == '[') in_account = line.rfind("[account]", 0) == 0;
                if (in_account && !keep_account) {
                    dropped = true;
                    continue;
                }
                out << line << '\n';
            }
            if (out) host_log("options: copied %s to %s (the per-user settings every copy of bbhost reads)%s", f.c_str(),
                              mine.c_str(), dropped ? ", not its account (a folder of its own)" : "");
            break;
        }
        return mine;
    }();
    return path;
}

// The resolution as width and height: BBHOST_RES's when it is set (it wins
// here too, or applying the settings at the start undid it), else the
// setting's.
bool resolution_of(unsigned* w, unsigned* h) {
    const char* forced = std::getenv("BBHOST_RES");
    return (forced && forced[0] && std::sscanf(forced, "%ux%u", w, h) == 2) ||
           std::sscanf(g_set[kResolution].values[static_cast<std::size_t>(g_set[kResolution].index)], "%ux%u", w, h) == 2;
}

// The game's size for a Resolution of w x h (host_opt_output_size).
void output_for(int* w, int* h);

// The game's size for the Resolution as resolution_of gives it.
bool output_of(unsigned* w, unsigned* h) {
    if (!resolution_of(w, h)) return false;
    int ow = static_cast<int>(*w), oh = static_cast<int>(*h);
    output_for(&ow, &oh);
    *w = static_cast<unsigned>(ow);
    *h = static_cast<unsigned>(oh);
    return true;
}

// The scene's targets as the settings now want them, while the game runs:
// Screen space reflections' patch in once it is on, at the Reflection
// quality's size (off clears its pass's switch each view,
// engine/graphics_patch.cpp, and the patch stays until the next start; a
// quality changed while it is in makes it again), HD motion blur's sizes and the velocity map's format and size
// made again from the settings (engine/patch_manifest.cpp) - and the targets
// made again at the same size when the game's code changed for them, or when
// reflections were turned on (`ssr_on`: the patched setup sets their switch).
void targets_follow_settings(const char* what, bool ssr_on) {
    const HostSettings hs = host_settings();
    bool rebuild = false, changed = false;
    std::string why;
    const bool ssr_in = patch_manifest_live_state("ssr").applied;
    if (ssr_on && hs.ssr) {
        // Made from the settings now, so at the quality chosen since the start.
        if (!ssr_in && patch_manifest_refresh_live("ssr", true, &changed, &why)) graphics_ssr_live_in();
        rebuild = true;
    } else if (ssr_in && patch_manifest_refresh_live("ssr", true, &changed, &why)) {
        rebuild |= changed;
    }
    if (patch_manifest_refresh_live("hd-motion-blur", hs.hd_motion_blur, &changed, &why)) rebuild |= changed;
    if (patch_manifest_refresh_live("velomap-16f", true, &changed, &why)) rebuild |= changed;
    if (rebuild && live_resolution_available()) live_resolution_rebuild();
    host_log("graphics: %s changed: %s", what, rebuild ? "the scene's targets made again" : "no target changes");
}

// The refresh rate of the display the window is on, the one frame
// generation's limit is half of (engine/frame_rate.cpp fg_cap_target reads
// host_window_refresh_hz: the same display). Before there is a window, the
// primary display's, where it opens: host_desktop_refresh_hz brings SDL's
// video up and down, which the presenter's thread must not, so on Windows the
// display's settings are asked directly, at most every 2 s; elsewhere SDL's
// answer once.
float refresh_hz_now() {
    if (const float w = host_window_refresh_hz(); w > 0.0f) return w;
#if defined(_WIN32)
    static std::atomic<float> hz{0.0f};
    static std::atomic<std::int64_t> asked{0};
    const std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch()).count();
    if (std::int64_t was = asked.load(); now - was > 2000 && asked.compare_exchange_strong(was, now)) {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        hz.store(EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1
                     ? static_cast<float>(dm.dmDisplayFrequency)
                     : 0.0f);
    }
    return hz.load();
#else
    static const float hz = host_desktop_refresh_hz();
    return hz;
#endif
}

// Frame generation's most real frames a second with it `on`: half the
// display's refresh rate (90 at 180 Hz; frame generation shows twice that),
// as the frame rate holds them (engine/frame_rate.cpp). 0 with it off, or no
// display that says a rate of 48 Hz or more (headless).
int fg_limit(bool on) {
    if (!on) return 0;
    const float hz = refresh_hz_now();
    return hz >= 48.0f ? std::max(30, static_cast<int>(hz / 2.0f)) : 0;
}

// Frame rate, Custom limit and its number (indices) brought down to `limit`
// (fg_limit) when the rate they make is above it: a Custom limit's number to
// it; a listed rate to the list's entry for it, or - a limit the list does not
// have (72 at 144 Hz) - Custom limit ticked at it. Display-Limited stays: the
// frame rate holds it to half the refresh rate itself while frames are
// generated. False when nothing changed.
bool frame_rate_fit(int* cap, int* custom, int* custom_fps, int limit) {
    if (limit <= 0) return false;
    const int fps = frame_rate_of(*cap, *custom, *custom_fps);
    if (fps == 0 || fps <= limit) return false;
    if (!reads_on(g_set[kFrameCapCustom], *custom)) {
        if (const int i = fps_index(limit); i >= 0) {
            *cap = i;
            return true;
        }
        *custom = 1;
    }
    *custom_fps = std::clamp(limit, kCustomFpsMin, 960) - kCustomFpsMin;
    return true;
}

void res_keep();
void set_index(int id, int index, bool from_file);

// Applied every time one changes, and once at load, so there is one path to
// the live state instead of a copy in each caller.
void apply(int id) {
    const Setting& s = g_set[id];
    switch (id) {
        case kWindowMode: {
            unsigned w = 0, h = 0;
            output_of(&w, &h);
            host_window_set_mode(s.index, static_cast<int>(w), static_cast<int>(h));
            // With the game at the window's size (an upscaler and a smaller
            // Resolution, a plugin's 3D size) that size is another mode's now.
            unsigned cw = 0, ch = 0;
            live_resolution_current(&cw, &ch);
            if (live_resolution_available() && w && h && cw && ch && (cw != w || ch != h)) live_resolution_request(w, h, false);
            break;
        }
        case kStayBorderless: host_window_set_stay_borderless(reads_on(s, s.index)); break;
        case kVsync: host_window_set_vsync(reads_on(s, s.index)); break;
        case kFrameGeneration: {
            // AMD's swapchain made or dropped at the presenter's next frame
            // (host/framegen.h reads the setting as it makes the swapchain);
            // the velocity map follows whether anything takes the motion.
            static int was = -1;
            if (was >= 0 && was != s.index && live_resolution_available()) {
                host_window_recreate_swapchain();
                targets_follow_settings(s.label, false);
            }
            const bool turned_on = was >= 0 && was != s.index && reads_on(s, s.index);
            const bool turned_off = was >= 0 && was != s.index && !reads_on(s, s.index);
            was = s.index;
            // Turned on: a frame rate above half the refresh rate comes down
            // to it (fg_limit; 120 -> 90 at 180 Hz, a Custom limit's 166 -> 90),
            // and the rate it was is kept: turned off again, the frame rate
            // goes back to it - unless the player chose another since. (It
            // stayed at the lowered rate: Kyo's 960 held at 90 with frame
            // generation off, "it just doesn't work anymore" until the setting
            // was put back by hand.)
            struct Lowered {
                bool on = false;
                int from[3] = {}, to[3] = {};  // Frame rate, Custom limit, its number: before and after
            };
            static Lowered lowered;
            int cap = g_set[kFrameCap].index, custom = g_set[kFrameCapCustom].index, custom_fps = g_set[kFrameCapCustomFps].index;
            if (turned_on && frame_rate_fit(&cap, &custom, &custom_fps, fg_limit(true))) {
                host_log("options: frame generation on: the frame rate %d comes down to %d, half the display's refresh rate",
                         frame_rate_now(), frame_rate_of(cap, custom, custom_fps));
                lowered = {true,
                           {g_set[kFrameCap].index, g_set[kFrameCapCustom].index, g_set[kFrameCapCustomFps].index},
                           {cap, custom, custom_fps}};
                if (custom_fps != g_set[kFrameCapCustomFps].index) set_index(kFrameCapCustomFps, custom_fps, false);
                if (custom != g_set[kFrameCapCustom].index) set_index(kFrameCapCustom, custom, false);
                if (cap != g_set[kFrameCap].index) set_index(kFrameCap, cap, false);
            } else if (turned_off && lowered.on) {
                lowered.on = false;
                if (g_set[kFrameCap].index == lowered.to[0] && g_set[kFrameCapCustom].index == lowered.to[1] &&
                    g_set[kFrameCapCustomFps].index == lowered.to[2]) {
                    const int was_fps = frame_rate_now();
                    if (lowered.from[2] != g_set[kFrameCapCustomFps].index) set_index(kFrameCapCustomFps, lowered.from[2], false);
                    if (lowered.from[1] != g_set[kFrameCapCustom].index) set_index(kFrameCapCustom, lowered.from[1], false);
                    if (lowered.from[0] != g_set[kFrameCap].index) set_index(kFrameCap, lowered.from[0], false);
                    host_log("options: frame generation off: the frame rate back from %d to %d, as it was before it came on", was_fps,
                             frame_rate_now());
                }
            }
            break;
        }
        case kUpscaler:
        case kSsr:
        case kSsrQuality:
        case kHdMotionBlur:
        case kHdMotionBlurRes:
        case kMotionVectors: {
            // The patches that make the scene's targets - Screen space
            // reflections, HD motion blur's sizes, the velocity map's format
            // and size - made again from the settings and the targets with
            // them (targets_follow_settings). Only once the game runs: the
            // start's own applies come before the manifests go in, which take
            // the settings themselves.
            static int was[kSettingCount];
            static bool seeded = false;
            if (!seeded) {
                std::fill(std::begin(was), std::end(was), -1);
                seeded = true;
            }
            if (was[id] >= 0 && was[id] != s.index && live_resolution_available()) targets_follow_settings(s.label, id == kSsr && s.index == 1);
            was[id] = s.index;
            // An upscaler chosen or not moves the game's size (the window's
            // with one and a smaller Resolution): Fullscreen's display mode
            // follows it, as it follows the Resolution - chosen in the setup
            // window, it stayed the Resolution's.
            if (id == kUpscaler && g_set[kWindowMode].index == 2) {
                unsigned w = 0, h = 0;
                if (output_of(&w, &h)) host_window_set_mode(2, static_cast<int>(w), static_cast<int>(h));
            }
            break;
        }
        case kFrameCap:
        case kFrameCapCustom:
        case kFrameCapCustomFps: {
            // From one converted rate to another at the next frame
            // (engine/frame_rate.h); otherwise, and before the game starts,
            // the presentation's cap, which says when it waits for the next
            // start. Custom limit's tick and number the same way: the rate
            // they make with Frame rate's.
            const int fps = frame_rate_now();
            if (!frame_rate_request(fps)) hle_video_set_fps_cap(fps);
            break;
        }
        case kEffectTiming:
            frame_rate_retime();  // nothing before the game runs, or at 30
            break;
        case kMouseMenu:
        case kMouseCamera:
        case kMouseSens:
        case kMouseInvertX:
        case kMouseInvertY:
        case kMouseAutoRotation:
        case kDrawCursor:
        case kButtonPrompts:
        case kLeftStickInner:
        case kLeftStickOuter:
        case kRightStickInner:
        case kRightStickOuter:
        case kTextureQuality:  // the textures follow them at the next submission (host/textures.cpp)
        case kSfxTextureQuality:
        case kAnisotropicFiltering:
            break;  // the settings object carries them (host/settings.h)
        case kSsao:
        case kMotionBlur:
        case kAntiAlias:
        case kDepthOfField:
        case kChromaticAberration:
        case kResolution: {
            unsigned w = 0, h = 0;
            // A change the player made asks to be kept (g_res_prev set by
            // set_index); going back, and the start, ask nothing.
            if (output_of(&w, &h)) {
                // The game's size (the window's, with a smaller
                // Resolution); the 3D's at it is the rebuild's.
                unsigned cw = 0, ch = 0;
                live_resolution_current(&cw, &ch);
                if (cw == w && ch == h) {
                    // The game's size unchanged: only the 3D's moves, nothing
                    // to confirm, and the entry is kept as it is (a change
                    // waiting on the dialog that never comes went on saving
                    // the old one).
                    if (id == kResolution && live_resolution_available()) {
                        live_resolution_rebuild();
                        res_keep();
                    }
                }
                else
                    live_resolution_request(w, h, g_res_prev >= 0 && !g_res_reverting);
                // Fullscreen's display mode follows the game's size.
                if (g_set[kWindowMode].index == 2) host_window_set_mode(2, static_cast<int>(w), static_cast<int>(h));
            }
            break;
        }
        case kPerformanceMode: {
            // Its YEBIS sites in or out now (depth of field, bloom; the motion
            // blur's constructor write for the next scene); the renderers'
            // switches follow from the settings (engine/graphics_patch.cpp).
            // No targets made again: off while the game runs gives the same
            // frame as off from the start (checked 2026-11-05, 1% of pixels
            // apart), and a rebuild with the larger shadow maps left the GPU
            // slower until the next start. Only once the game runs: the
            // start's manifests take the setting.
            static int was = -1;
            if (was >= 0 && was != s.index && live_resolution_available()) {
                bool changed = false;
                std::string why;
                const bool ok = patch_manifest_refresh_live("performance-mode", reads_on(s, s.index), &changed, &why);
                host_log("graphics: Performance mode %s while the game runs%s", reads_on(s, s.index) ? "on" : "off",
                         ok ? "" : " (its YEBIS passes at the next start)");
                // Its shadow sizes stand in for Default's.
                live_shadows_request();
            }
            was = s.index;
            break;
        }
        case kShadowResolution: {
            // The maps made again at the next quiet point
            // (engine/live_resolution.cpp); the start's manifest takes the
            // setting, so only once the game runs. An entry past what the
            // heap was grown for logs that it waits for the next start.
            static int was = -1;
            if (was >= 0 && was != s.index && live_resolution_available()) live_shadows_request();
            was = s.index;
            break;
        }
        case kSkipLogos:
        case kDebugMenu:
        case kChangeAppearance:
        case kRebirth:
        case kFivePlayers:
        case kSkillIssue:
        case kFullPhantomHp:
        case kAutoReplenish:
            break;  // read by the loader before any of this exists
        case kMenuTextSize:
        case kMenuHue:
        case kMenuVibrance:
            break;  // the menus read them every frame (host_opt_menu_text_scale, host_opt_menu_colour)
        case kTextScale:
            break;  // the settings object carries it (engine/graphics_patch.cpp)
        default: break;
    }
}

// The file's format. 2: Mouse in menus defaults to On. A file without a
// version was written by a build where it defaulted to Off, and every save
// writes every setting - so its "Off" is that old default far more often
// than a choice, and reads as On (once: the next save writes version 2).
// 3: frame_cap is the game's frame rate (30, 60, 90, Uncapped; 120 since
// 2026-10-10 and 150 to 960 since 2026-10-28, new values, so the version
// stays - and Uncapped, the display's refresh rate since then, is called
// Display-Limited: a saved "Uncapped" reads as it); an older file's 90, 120,
// 144 and Off were caps on presentation that ran the game at 60, and read as
// 60. camera_tuning (2026-10-10) is a new key whose default, FPS++, is what
// every older build ran, so a file without it reads as before and the
// version stays. 4: the camera zoom actions (zoom_in, zoom_out, zoom_reset on
// the wheel and the middle button); an older file's Transform on the middle
// button, its default until then, gives it to Camera Zoom Reset. 7: Window
// mode's Borderless and an exclusive Fullscreen; an older file's Fullscreen
// was the borderless one. 8: Mouse sensitivity from 0.005 to 0.4 degrees a
// count; an older file saved Dark Souls III's scale, 0.1 to 0.4.
constexpr int kOptionsVersion = 8;

void save() {
    if (!g_dirty) {
        return;
    }
    const std::string path = options_path();
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        host_log("options: cannot write %s", path.c_str());
        return;
    }
    std::fprintf(f, "# bbhost PC port options. Written by the options screen (F10).\n");
    std::fprintf(f, "[options]\n");
    std::fprintf(f, "version = \"%d\"\n", kOptionsVersion);
    for (const Setting& s : g_set) {
        // Effect timing only once it is the player's own (g_saved): written at
        // every save like the rest, it would hold every player at the default
        // of the day they first saved - a new kEffectTimingDefault would reach
        // nobody - and take over from a [video] effect_steps the four choices
        // cannot show (a number of steps, frame) at the first save.
        if (&s == &g_set[kEffectTiming] && !g_saved[kEffectTiming]) continue;
        // A resolution still waiting for its keep-or-revert answer is not
        // saved: the file keeps the one it would go back to.
        const int index = &s == &g_set[kResolution] && g_res_prev >= 0 ? g_res_prev : s.index;
        std::fprintf(f, "%s = \"%s\"\n", s.key, s.values[static_cast<std::size_t>(index)]);
    }
    host_bindings_save(f);
    // The account: its name and token.
    if (net::account_logged_in()) {
        std::fprintf(f, "[account]\nname = \"%s\"\ntoken = \"%s\"\n", net::account_name().c_str(),
                     net::account_token().c_str());
    }
    std::fclose(f);
    g_dirty = false;
    host_log("options: saved %s", path.c_str());
}

// The settings object (host/settings.h): every field from the table, scaled
// the way its consumer wants it, published again whenever a setting changes
// (its serial is host_opt_serial) and told to the listeners. Made on first
// use, so a listener may register from anywhere at any time, and never
// destroyed: threads still reading it while the process exits read memory
// that is still there.
HostSettingsStore& settings_store() {
    static HostSettingsStore& store = *new HostSettingsStore;
    return store;
}

bool on_of(int id) { return reads_on(g_set[id], g_set[id].index); }
int index_of(int id) { return g_set[id].index; }

void rebuild_settings() {
    HostSettings h;
    h.fullscreen = index_of(kWindowMode) != 0;  // Borderless or Fullscreen
    h.stay_borderless = on_of(kStayBorderless);
    h.vsync = on_of(kVsync);
    h.frame_cap = frame_rate_now();
    h.frame_clock = on_of(kFrameClock);
    h.effect_timing = kEffectTimingValues[std::clamp(index_of(kEffectTiming), 0, kEffectTimingCount - 1)];
    h.fps_counter = on_of(kFpsCounter);
    h.memory_counter = on_of(kMemoryCounter);
    h.perf_overlay = on_of(kPerfOverlay);
    {
        // BBHOST_RES=WxH still wins, so an A/B run started with one behaves
        // the way its command line says.
        unsigned w = 0, hh = 0;
        const char* e = std::getenv("BBHOST_RES");
        if (!(e && e[0] && std::sscanf(e, "%ux%u", &w, &hh) == 2)) {
            const Setting& r = g_set[kResolution];
            if (std::sscanf(r.values[static_cast<std::size_t>(r.index)], "%ux%u", &w, &hh) != 2) w = hh = 0;
        }
        if (w && hh) {
            h.res_width = static_cast<int>(w);
            h.res_height = static_cast<int>(hh);
        }
    }
    // Distance and height: 50 is the game's own, 1% a step either way. Field
    // of view: 0 is the game's own, 1% wider a step (BBHOST_FOV overrides).
    const auto around = [](int id) { return 0.5f + 0.01f * static_cast<float>(index_of(id)); };
    h.camera_distance_scale = around(kCameraDistance);
    h.camera_height_scale = around(kCameraHeight);
    h.camera_original = index_of(kCameraTuning) == 1;
    {
        const char* e = std::getenv("BBHOST_FOV");
        const float env = e && *e ? std::strtof(e, nullptr) : 0.0f;
        const int fov = index_of(kFov);
        h.fov_scale = env > 0.0f ? env : fov <= 0 ? 1.0f : 1.0f + 0.01f * static_cast<float>(fov);
    }
    h.mouse_menu = on_of(kMouseMenu);
    h.mouse_camera = on_of(kMouseCamera);
    // Mouse sensitivity's 0..10 (engine/mouse_camera_step.h), a tenth a step
    // (0-100).
    h.mouse_sens = index_of(kMouseSens) < 0 ? 5.0f : static_cast<float>(index_of(kMouseSens)) / 10.0f;
    h.mouse_invert_x = on_of(kMouseInvertX);
    h.mouse_invert_y = on_of(kMouseInvertY);
    h.mouse_auto_rotation = on_of(kMouseAutoRotation);
    h.draw_cursor = on_of(kDrawCursor);
    h.button_prompts = index_of(kButtonPrompts) < 0 ? 0 : index_of(kButtonPrompts);
    h.stick_inner[0] = static_cast<float>(std::max(index_of(kLeftStickInner), 0)) / 100.0f;
    h.stick_outer[0] = static_cast<float>(50 + std::max(index_of(kLeftStickOuter), 0)) / 100.0f;
    h.stick_inner[1] = static_cast<float>(std::max(index_of(kRightStickInner), 0)) / 100.0f;
    h.stick_outer[1] = static_cast<float>(50 + std::max(index_of(kRightStickOuter), 0)) / 100.0f;
    h.ssao = on_of(kSsao);
    h.motion_blur = on_of(kMotionBlur);
    h.anti_alias = on_of(kAntiAlias);
    h.depth_of_field = on_of(kDepthOfField);
    h.chromatic_aberration = on_of(kChromaticAberration);
    h.vignette = on_of(kVignette);
    h.bloom = static_cast<float>(index_of(kBloom)) / 100.0f;
    h.saturation = static_cast<float>(index_of(kSaturation)) / 50.0f;  // 50 the area's own
    h.fog = static_cast<float>(index_of(kFog)) / 100.0f;
    h.ao_strength = static_cast<float>(index_of(kAoStrength)) / 50.0f;
    h.shadow_scale = 1.0f + 0.2f * static_cast<float>(index_of(kShadowDistance));
    h.dynamic_shadows = on_of(kDynamicShadows);
    h.shadow_resolution = std::clamp(index_of(kShadowResolution), 0, 5);
    {
        static constexpr int kBias[] = {2, 1, 0, -2};  // Lowest, Low, Normal, Full
        const int i = index_of(kModelDetail);
        h.lod_bias = i >= 0 && i < 4 ? kBias[i] : 0;
    }
    h.texture_quality = 3 - std::clamp(index_of(kTextureQuality), 0, 3);  // the levels left out: High none, Lowest three
    h.sfx_texture_quality = 3 - std::clamp(index_of(kSfxTextureQuality), 0, 3);
    h.anisotropy = std::clamp(index_of(kAnisotropicFiltering), 0, 4) == 0 ? 0 : 1 << std::clamp(index_of(kAnisotropicFiltering), 0, 4);
    h.ssr = on_of(kSsr);
    h.ssr_half = index_of(kSsrQuality) == 1;
    h.hd_motion_blur = on_of(kHdMotionBlur);
    h.hd_motion_blur_res = std::clamp(index_of(kHdMotionBlurRes), 0, 2);
    {
        const int u = index_of(kUpscaler);
        h.upscaler = u == 1 ? 4 : u == 2 ? 3 : u == 3 ? 2 : u == 4 ? 1 : 0;  // the menu's TAA, FSR 3, FSR 4, DLSS
        h.upscale_quality = 3 - std::clamp(index_of(kUpscaleQuality), 0, 3);  // the menu runs Performance to Native AA
        static constexpr int kPresets[] = {11, 10, 12, 13};  // the menu's K, J, L, M
        h.dlss_preset = kPresets[std::clamp(index_of(kDlssPreset), 0, 3)];
        h.sharpness = static_cast<float>(std::clamp(index_of(kSharpness), 0, 100)) / 100.0f;
        h.taa_strength = static_cast<float>(std::clamp(index_of(kTaaStrength), 0, 100)) / 100.0f;
        h.frame_generation = on_of(kFrameGeneration);
        h.motion_vectors = std::clamp(index_of(kMotionVectors), 0, 3);
        h.char_motion = index_of(kCharMotion) != 1;  // Precise, Off (the game's velocity map alone)
    }
    h.text_scale = static_cast<float>(50 + std::clamp(index_of(kTextScale), 0, 50)) / 100.0f;
    h.skip_logos = on_of(kSkipLogos);
    h.performance_mode = on_of(kPerformanceMode);
    h.debug_menu = on_of(kDebugMenu);
    h.change_appearance = on_of(kChangeAppearance);
    h.rebirth = on_of(kRebirth);
    h.five_players = on_of(kFivePlayers);
    h.skill_issue = on_of(kSkillIssue);
    h.full_phantom_hp = on_of(kFullPhantomHp);
    h.auto_replenish = on_of(kAutoReplenish);
    settings_store().publish(h);
}

void set_index(int id, int index, bool from_file) {
    Setting& s = g_set[id];
    const int n = static_cast<int>(s.values.size());
    index = ((index % n) + n) % n;
    if (index == s.index && from_file) {
        return;
    }
    if (id == kResolution && !from_file && !g_res_reverting && index != s.index && res_live(index)) {
        if (g_res_prev < 0) g_res_prev = s.index;  // a second change while one waits goes back to the first's start
        if (index == g_res_prev) g_res_prev = -1;  // back where it started: nothing to ask
    }
    s.index = index;
    rebuild_settings();
    if (!from_file) {
        g_dirty = true;
        g_saved[static_cast<std::size_t>(id)] = true;
    }
    apply(id);
}

void res_keep() {
    if (g_res_prev < 0) return;
    host_log("options: resolution %s kept", g_set[kResolution].values[static_cast<std::size_t>(g_set[kResolution].index)]);
    g_res_prev = -1;
    g_dirty = true;
    save();
}

void res_revert() {
    if (g_res_prev < 0) return;
    const int prev = g_res_prev;
    g_res_prev = -1;
    host_log("options: resolution back to %s", g_set[kResolution].values[static_cast<std::size_t>(prev)]);
    g_res_reverting = true;
    set_index(kResolution, prev, false);
    g_res_reverting = false;
    // An entry waiting for the next start (res_live) cannot be gone back to
    // now: the picture goes back to the size it had before the change.
    if (!res_live(prev)) live_resolution_go_back();
    save();
}

}  // namespace



void host_options_apply() {
    for (int id = 0; id < kSettingCount; ++id) {
        apply(id);
    }
}

void host_options_load() {
    // Once: the setup window (host/launcher.h) loads them before it shows them.
    static bool loaded = false;
    if (loaded) return;
    loaded = true;
    // bbhost.toml still sets the defaults; the options file is what the player
    // changed, and the environment switches still win over both.
    host_bindings_load_defaults();
    g_set[kSkipLogos].index = config().skip_intro ? 1 : 0;
    // Screen space reflections were a patch file until they became a setting:
    // a bbhost.toml that turned it on keeps them on until the player chooses.
    if (const std::string v = config_value("patches.screen-space-reflections"); !v.empty())
        g_set[kSsr].index = v == "false" || v == "0" ? 0 : 1;
    // So was Disable Dynamic Light Shadows.
    if (const std::string v = config_value("patches.disable-dynamic-light-shadow"); !v.empty())
        g_set[kDynamicShadows].index = v == "false" || v == "0" ? 1 : 0;
    // So were the HD Motion Blur patches, each at one size: the one a
    // bbhost.toml turned on becomes HD motion blur at the share nearest it
    // (360p a quarter of 1440p, 1080p and up the full size).
    for (const auto& [file, res] : {std::pair{"360p", 0}, std::pair{"1080p", 2}, std::pair{"1440p", 2}, std::pair{"4k", 2}}) {
        const std::string v = config_value(std::string("patches.hd-motion-blur-") + file);
        if (v.empty() || v == "false" || v == "0") continue;
        g_set[kHdMotionBlur].index = 1;
        g_set[kHdMotionBlurRes].index = res;
    }
    // The PC enhancements seed from bbhost.toml's [world] (all on unless it
    // says otherwise); the options file then holds the player's choice, and a
    // BBHOST_* switch still wins over both (config_set_enhancements).
    g_set[kChangeAppearance].index = config().change_appearance ? 1 : 0;
    g_set[kRebirth].index = config().rebirth ? 1 : 0;
    g_set[kFivePlayers].index = config().five_players ? 1 : 0;
    g_set[kSkillIssue].index = config().skill_issue ? 1 : 0;
    g_set[kFullPhantomHp].index = config().full_phantom_hp ? 1 : 0;
    g_set[kAutoReplenish].index = config().auto_replenish ? 1 : 0;
    // video.fps_cap is the frame rate's default: one of the setting's rates or
    // 0 (Display-Limited); another number takes the nearest rate below it.
    {
        const int cap = config().fps_cap;
        int mode = cap <= 0 ? 0 : 30;
        for (const char* v : g_set[kFrameCap].values) {
            const int r = fps_from_label(v);
            if (cap > 0 && r > mode && r <= cap) mode = r;
        }
        if (mode != cap) host_log("options: video.fps_cap = %d is not a frame rate; %d", cap, mode);
        g_set[kFrameCap].index = std::max(0, fps_index(mode));
    }
    // video.frame_clock is the Native frame clock's default (off).
    g_set[kFrameClock].index = config().frame_clock ? 1 : 0;
    // video.effect_steps is Effect timing's when it is one of its choices ("60"
    // reads as 60 a second, auto's 60 above 30); another value (a number of
    // steps, frame) leaves the row at the default and still runs until a
    // choice is made (config_effect_timing).
    {
        const std::string v = config_value("video.effect_steps");
        if (const int i = effect_timing_index(v == "60" ? "auto" : v); i >= 0) g_set[kEffectTiming].index = i;
    }
    // A package's own (video.resolution, video.window_mode: the Steam Deck
    // kit's 1280x800, fullscreen; video.model_detail, video.texture_quality),
    // by the entry's name.
    const auto seed = [](int id, const std::string& value) {
        for (std::size_t k = 0; !value.empty() && k < g_set[id].values.size(); ++k)
            if (value == g_set[id].values[k]) g_set[id].index = static_cast<int>(k);
    };
    seed(kResolution, config().resolution);
    seed(kWindowMode, config().window_mode);
    seed(kModelDetail, config().model_detail);
    {
        // (1.0x, 0.5x, 0.25x: its entries' names until 2026-11-05.)
        const std::string& t = config().texture_quality;
        seed(kTextureQuality, t == "1.0x" ? "High" : t == "0.5x" ? "Medium" : t == "0.25x" ? "Low" : t);
    }
    seed(kShaders, config().shaders);
    if (config().setup_text_size > 0) seed(kMenuTextSize, std::to_string(config().setup_text_size) + "%");
    for (const int id : {static_cast<int>(kSkipLogos), static_cast<int>(kFrameCap), static_cast<int>(kFrameClock),
                         static_cast<int>(kEffectTiming), static_cast<int>(kResolution), static_cast<int>(kWindowMode),
                         static_cast<int>(kModelDetail), static_cast<int>(kTextureQuality), static_cast<int>(kShaders),
                         static_cast<int>(kChangeAppearance), static_cast<int>(kRebirth), static_cast<int>(kFivePlayers),
                         static_cast<int>(kSkillIssue), static_cast<int>(kFullPhantomHp), static_cast<int>(kAutoReplenish)})
        g_default_index[static_cast<std::size_t>(id)] = g_set[id].index;
    const std::string path = options_path();
    bool text_scale_saved = false;  // the file has a text_scale (below)
    FILE* f = std::fopen(path.c_str(), "r");
    if (f) {
        char line[512];
        bool keys = false;  // in [keys]: bindings, not settings
        bool pad = false;   // in [pad]: the controller's buttons
        bool account = false;  // in [account]: the name and token
        std::string acct_name, acct_token;
        int file_version = 1;
        std::string frame_cap_val;
        std::string window_mode_val;  // its label as saved (Fullscreen was Borderless before version 7)
        while (std::fgets(line, sizeof(line), f)) {
            if (line[0] == '[') {
                keys = std::strncmp(line, "[keys]", 6) == 0;
                pad = std::strncmp(line, "[pad]", 5) == 0;
                account = std::strncmp(line, "[account]", 9) == 0;
                continue;
            }
            char* eq = std::strchr(line, '=');
            if (!eq || line[0] == '#') {
                continue;
            }
            *eq = 0;
            std::string key(line), val(eq + 1);
            auto trim = [](std::string& v) {
                while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ' ||
                                      v.back() == '\t' || v.back() == '"')) {
                    v.pop_back();
                }
                std::size_t i = 0;
                while (i < v.size() && (v[i] == ' ' || v[i] == '\t' || v[i] == '"')) {
                    ++i;
                }
                v.erase(0, i);
            };
            trim(key);
            trim(val);
            if (keys) {
                host_bindings_parse(key.c_str(), val.c_str());
                continue;
            }
            if (pad) {
                host_pad_parse(key.c_str(), val.c_str());
                continue;
            }
            if (account) {
                if (key == "name") acct_name = val;
                if (key == "token") acct_token = val;
                continue;
            }
            if (key == "version") {
                file_version = std::atoi(val.c_str());
                continue;
            }
            if (key == "old_hunters") continue;  // v0.2.9's PC enhancement: The Old Hunters is always on now
            // The Debug camera: it switched a patch manifest no package carries
            // (patches/debug-camera.toml), so it never worked in a release. A
            // saved one is set aside; the next save drops it.
            if (key == "debug_camera") {
                if (val == "On") host_log("options: the Debug camera is gone (it never worked in a release); the saved On is set aside");
                continue;
            }
            // Looked at again once the file's version is known (below).
            if (key == "frame_cap") frame_cap_val = val;
            if (key == "text_scale") text_scale_saved = true;
            if (key == "window_mode") window_mode_val = val;
            // HD motion blur quality's were Quarter, Half and Full until 2026-11-05.
            if (key == "hd_motion_blur_res") val = val == "Quarter" ? "Low" : val == "Half" ? "Medium" : val == "Full" ? "High" : val;
            // Texture quality's entries were 1.0x, 0.5x and 0.25x until 2026-11-05.
            if (key == "texture_quality") val = val == "1.0x" ? "High" : val == "0.5x" ? "Medium" : val == "0.25x" ? "Low" : val;
            // Display-Limited was called Uncapped until 2026-10-28.
            if (key == "frame_cap" && val == "Uncapped") val = "Display-Limited";
            // Character motion's Off was called Velocity map until 2026-11-10.
            if (key == "char_motion" && (val == "Velocity map" || val == "速度マップ")) val = "Off";
            // Effect timing's names before it had three choices; a saved Smooth,
            // no longer one, reads as not chosen (the default, 60 fps).
            if (key == "effect_timing") {
                if (val == "Original (30)") val = kHostEffectTimingLabels[0];
                else if (val == "60 a second") val = kHostEffectTimingLabels[1];
                else if (val == "Your frame rate") val = kHostEffectTimingLabels[2];
            }
            // The four named steps mouse_sens used to have, on the new scale.
            if (key == "mouse_sens") {
                const char* old[] = {"Low", "Medium", "High", "Very high"};
                const int to[] = {2, 5, 7, 9};
                for (int k = 0; k < 4; ++k) {
                    if (val == old[k]) val = std::to_string(to[k]);
                }
            }
            for (int id = 0; id < kSettingCount; ++id) {
                if (key != g_set[id].key) {
                    continue;
                }
                for (std::size_t v = 0; v < g_set[id].values.size(); ++v) {
                    if (val == g_set[id].values[v] || val == host_japanese_value(g_set[id].values[v])) {
                        g_set[id].index = static_cast<int>(v);
                        g_saved[static_cast<std::size_t>(id)] = true;
                    }
                }
            }
        }
        std::fclose(f);
        // Before version 3 the frame_cap was a cap on presentation, and its 90,
        // 120, 144 and Off ran the game at 60: the frame rate that ran is 60
        // (before any save below writes version 3).
        // Before version 4 there were no camera zoom actions: the middle
        // button they take was Transform's (host/bindings.h).
        // Before version 6 the 0-10 sliders were eleven steps: a saved value is
        // the same place on today's 0-100 (Field of view's percents read as
        // they are).
        if (file_version < 6) {
            for (const int id : {kBloom, kSaturation, kFog, kAoStrength, kCameraDistance, kCameraHeight, kMouseSens}) {
                if (g_saved[static_cast<std::size_t>(id)]) g_set[id].index = std::min(g_set[id].index, 10) * 10;
            }
        }
        // Before version 8 Mouse sensitivity was Dark Souls III's scale, 0.1 to
        // 0.4 degrees a count: a value the player set turns as far as before on
        // today's 0.005 to 0.4, and the old default (50) is today's - DS3's
        // slowest was faster than most mice want (engine/mouse_camera_step.h).
        if (file_version < 8 && g_saved[static_cast<std::size_t>(kMouseSens)]) {
            const int was = g_set[kMouseSens].index;
            if (was != 50) {
                g_set[kMouseSens].index = mouse_camera_setting_from_ds3(was);
                host_log("options: Mouse sensitivity %d on the old scale reads as %d, the same turn for a count", was,
                         g_set[kMouseSens].index);
            }
            g_dirty = true;
        }
        // Before version 7 Window mode's Fullscreen was borderless: it is
        // Borderless now, and Fullscreen the exclusive mode.
        if (file_version < 7 && window_mode_val == "Fullscreen") {
            g_set[kWindowMode].index = 1;
            g_dirty = true;
        }
        if (file_version < 4) {
            host_bindings_migrate_zoom();
            g_dirty = true;
        }
        // Before version 5 there were no menu pages and options (host/bindings.h).
        if (file_version < 5) {
            host_bindings_migrate_menu_keys();
            g_dirty = true;
        }
        if (file_version < 3 &&
            (frame_cap_val == "90" || frame_cap_val == "120" || frame_cap_val == "144" || frame_cap_val == "Off")) {
            g_set[kFrameCap].index = std::max(0, fps_index(60));
            g_dirty = true;
            host_log("options: frame_cap \"%s\" (a cap that ran the game at 60) reads as the frame rate 60",
                     frame_cap_val.c_str());
        }
        if (!acct_name.empty() && !acct_token.empty()) {
            net::account_set(acct_name, acct_token);
            g_field[0] = acct_name;
            host_log("options: account %s from %s", acct_name.c_str(), path.c_str());
        }
        // After the account: save() writes everything it holds, and saving
        // before the account was set dropped the [account] section - a
        // dev-server tester was signed out (2026-10-02).
        if (file_version < 2 && g_set[kMouseMenu].index == 0) {
            g_set[kMouseMenu].index = 1;
            g_dirty = true;
            host_log("options: Mouse in menus read as On - this file predates it defaulting to On");
            save();  // as version 2, so a later Off is the player's and stays
        }
        host_log("options: read %s", path.c_str());
    }
    // Text scale was the patch text-scale-50 until 2026-11-13: a player who
    // had it on keeps 50% (once: the save writes text_scale, which wins from then on).
    if (!text_scale_saved && config_value("patches.text-scale-50") == "true") {
        g_set[kTextScale].index = 0;
        g_dirty = true;
        save();
        host_log("options: the patch text-scale-50 was on; Text scale reads as %s", "50%");
    }
    // BBHOST_ACCOUNT=name:token, for a scripted run that must not touch the file.
    if (const char* e = std::getenv("BBHOST_ACCOUNT"); e && e[0]) {
        const std::string v = e;
        if (const std::size_t c = v.find(':'); c != std::string::npos) {
            net::account_set(v.substr(0, c), v.substr(c + 1));
            g_field[0] = v.substr(0, c);
        }
    }
    // The environment switches the track was brought up with still win, so a
    // run started with one behaves the way it says on the command line.
    if (const char* e = std::getenv("BBHOST_MOUSE_MENU"); e && e[0]) {
        g_set[kMouseMenu].index = e[0] == '0' ? 0 : 1;
    }
    // The post-processing switches, for A/B runs: BBHOST_SSAO=0 and friends.
    for (const auto& [env, id] : {std::pair<const char*, int>{"BBHOST_SSAO", kSsao},
                                  {"BBHOST_MOTION_BLUR", kMotionBlur},
                                  {"BBHOST_ANTI_ALIAS", kAntiAlias},
                                  {"BBHOST_DEPTH_OF_FIELD", kDepthOfField},
                                  {"BBHOST_CHROMATIC_ABERRATION", kChromaticAberration}}) {
        if (const char* e = std::getenv(env); e && e[0]) {
            g_set[id].index = e[0] == '0' ? 0 : 1;  // these are {"Off","On"}
        }
    }
    // BBHOST_FRAMEGEN=1 or 0: Frame generation on or off over the file.
    if (const char* e = std::getenv("BBHOST_FRAMEGEN"); e && e[0]) g_set[kFrameGeneration].index = e[0] == '0' ? 0 : 1;
#if !defined(_WIN32)
    // Frame generation is AMD's FidelityFX on Windows alone (host/framegen.cpp)
    // and has no row elsewhere: off here whatever a file from Windows says, so
    // nothing that cannot be turned off holds the frame rate to its limit.
    g_set[kFrameGeneration].index = 0;
#endif
    if (const char* e = std::getenv("BBHOST_MOUSE_CAMERA"); e && e[0]) {
        g_set[kMouseCamera].index = e[0] == '0' ? 0 : 1;
    }
    if (const char* e = std::getenv("BBHOST_MOUSE_SENS"); e && e[0]) {
        const int v = std::atoi(e);
        g_set[kMouseSens].index = (v < 0 ? 0 : v > 10 ? 10 : v) * 10;  // BBHOST_MOUSE_SENS stays 0..10
    }
    if (const char* e = std::getenv("BBHOST_MOUSE_CURSOR"); e && e[0]) {
        g_set[kDrawCursor].index = e[0] == '0' ? 0 : 1;
    }
    // Published before they are applied: the applies read them (Window mode's
    // the upscaler's, through the game's size - read from the defaults it gave
    // Fullscreen the Resolution's display mode while the game was the screen's
    // size).
    rebuild_settings();
    host_options_apply();
    rebuild_settings();
}

bool host_options_open() { return g_open.load(std::memory_order_relaxed); }

void host_options_set_open(bool open) {
    if (open == g_open.load(std::memory_order_relaxed)) {
        return;
    }
    g_open.store(open, std::memory_order_relaxed);
    if (open) {
        // Nothing has consumed the pointer while the menu was closed, so a
        // click from minutes ago is still latched. Drop it: the game's menus
        // behind must not take it when this one closes.
        host_mouse_state();
    } else {
        save();
    }
    host_log("options: %s", open ? "open" : "closed");
}

void host_options_close() {
    if (!g_open.exchange(false)) return;
    host_options_post([] {
        save();
        host_log("options: closed");
    });
}

void host_options_post(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(g_post_mu);
    g_posted.push_back(std::move(fn));
}

void host_options_frame(float, float) {
    {
        std::vector<std::function<void()>> todo;
        {
            std::lock_guard<std::mutex> lk(g_post_mu);
            todo.swap(g_posted);
        }
        for (const std::function<void()>& fn : todo) fn();
    }
    if (g_acct_save.exchange(false)) {
        g_dirty = true;
        save();
    }
    // BBHOST_RES_PROMPT_TEST=<flip>:<entry>: the Resolution setting changed
    // to that entry at that flip, the way the menus change it (tests).
    static long test_flip = [] {
        const char* e = std::getenv("BBHOST_RES_PROMPT_TEST");
        return e && *e ? std::atol(e) : -1L;
    }();
    if (test_flip >= 0 && static_cast<long>(hle_video_flip_count()) >= test_flip) {
        test_flip = -1;
        const char* e = std::strchr(std::getenv("BBHOST_RES_PROMPT_TEST"), ':');
        if (e) host_opt_set_resolution_index(std::atoi(e + 1));
    }
    // The game's keep-or-revert dialog answered (engine/live_resolution.h).
    if (const int a = live_resolution_take_answer()) {
        if (a == 1) res_keep(); else res_revert();
    }
}

#if defined(BBHOST_HAVE_SDL3)
namespace {

namespace ui = launcher_ui;

// The menu's pages: the table's sections (g_rows' headers), in its order.
struct OptionsPage {
    const char* header;  // the section's header in g_rows
    const char* name;
    const char* blurb;
    int first = 0, end = 0;  // its rows in g_rows, after the header
};

const std::vector<OptionsPage>& pages() {
    static const std::vector<OptionsPage> list = [] {
        static const struct {
            const char* header;
            const char* name;
            const char* blurb;
        } kNames[] = {
            {"Game", "ゲーム", "画面、フレームレート、起動、画面上の情報、オンラインのアカウントを設定します。"},
            {"入力", "入力", "マウス、ポインター、ボタン表示を設定します。"},
            {"グラフィックス", "グラフィックス", "画面効果と精細度を設定します。多くの項目は即時反映します。"},
            {"PC 追加機能", "PC 追加機能", "ゲームの追加機能です。次回起動時に反映します。"},
            {"設定", "設定", "bbhost のメニューと更新を設定します。"},
        };
        std::vector<OptionsPage> out;
        for (int r = 0; r < kRowCount; ++r) {
            if (g_rows[r].kind != Row::Header) continue;
            OptionsPage pg{g_rows[r].text, g_rows[r].text, "", r + 1, kRowCount};
            for (const auto& k : kNames) {
                if (std::strcmp(k.header, g_rows[r].text) == 0) {
                    pg.name = k.name;
                    pg.blurb = k.blurb;
                }
            }
            if (!out.empty()) out.back().end = r;
            out.push_back(pg);
        }
        return out;
    }();
    return list;
}

// Drawing for the setup window (host_options_draw_setup_page), not the game.
bool g_setup_page = false;

// A setting as the menu shows it: what it set and the pump has not applied
// yet, else the setting. The presenter's own (g_pending is never the pump's).
int shown_index(int id) {
    const int p = g_pending[static_cast<std::size_t>(id)];
    if (p >= 0 && p != g_set[id].index) return p;
    g_pending[static_cast<std::size_t>(id)] = -1;
    return g_set[id].index;
}

void change(int id, int index) {
    g_pending[static_cast<std::size_t>(id)] = index;
    host_options_post([id, index] { set_index(id, index, false); });
}

// One setting's row: a switch for On and Off, else its label at the left and
// at the right a choice of a few side by side, a list, or a slider through a
// scale's steps; the note in the bubble. A setting read only as the game
// starts says so under its label (the resolution only when it cannot change
// now).
// `label` and `note`, when given, stand for the setting's own: one row can
// show another face (the upscaler's preset row, preset_row).
// `names`, when given, are what the choices show in place of the setting's
// own values (n of them; what is saved stays the value).
// Each choice's own tooltip, for the rows whose choices need saying apart:
// the upscaler, its presets (FSR's or DLSS's quality - `dlss` - and DLSS's
// model) and HD motion blur's resolution. nullptr: the row's note only.
const char* const* value_tips(int id, bool dlss) {
    static const char* const kUpscalerTips[] = {
        "原作の輪郭補正。3D は指定解像度で描画します。",
        "TAA：bbhost の時間方向の輪郭補正です。ちらつきを抑えますが少し柔らかくなります。どの GPU でも使用でき、低解像度でも HUD は画面の大きさを維持します。この中で最も軽量です。",
#if defined(_WIN32)
        "AMD FSR 3：対応するすべての GPU で使用できます。",
#else
        "AMD FSR 3：Windows と同じ FSR 3.1.6 です。FSR 4 とフレーム生成は Windows 専用です。",
#endif
        "AMD FSR 4：対応する GPU とドライバーで使用し、未対応の場合は FSR 3 を使います。",
        "NVIDIA DLSS：RTX カード専用です。",
    };
    static const char* const kFsrTips[] = {
        "速度優先：縦横半分、画素数４分の１で描画します。最も高速で、柔らかい映像になります。",
        "均衡：縦横約 59% で描画します。速度と鮮明さの中間です。",
        "品質優先：縦横約３分の２で描画し、FSR で拡大します。原寸に近い品質で高速化します。",
        "等倍 AA：原寸で描画し、FSR で輪郭だけ補正します。最も鮮明ですが負荷は高くなります。",
    };
    static const char* const kDlssTips[] = {
        "速度優先：縦横半分、画素数４分の１で描画します。最も高速で、柔らかい映像になります。",
        "均衡：縦横約 59% で描画します。速度と鮮明さの中間です。",
        "品質優先：縦横約３分の２で描画し、DLSS で拡大します。原寸に近い品質で高速化します。",
        "DLAA：原寸で描画し、DLSS で輪郭だけ補正します。最も鮮明ですが負荷は高くなります。",
    };
    static const char* const kPresetTips[] = {
        "K: the transformer model, the sharpest. Recommended.",
        "J: like K, with a little less ghosting and a little more flicker.",
        "L: made for the lowest render sizes (Ultra Performance).",
        "M: made for Performance.",
    };
    static const char* const kHdBlurTips[] = {
        "低：縦横４分の１。最も軽量ですが、ぼかしは粗くなります。",
        "中：縦横半分。原作と同じサイズです。",
        "高：描画サイズと同じ。最もきれいですが負荷が増えます。",
    };
    static const char* const kTextureTips[] = {
        "最低：縦横８分の１、VRAM は64分の１。1024x2048 は 128x256 になります。",
        "低：縦横４分の１、VRAM は16分の１。1024x2048 は 256x512 になります。",
        "中：縦横半分、VRAM は４分の１。1024x2048 は 512x1024 になります。",
        "高：原作のサイズを維持します。",
    };
    static const char* const kCharMotionTips[] = {
        "精密（標準）：キャラクターを動き情報用に再描画し、輪郭と残像を改善します。4K で約 1% の負荷です。",
        "無効：原作のブラー用マップのみを使用します。少し軽くなりますが、輪郭が柔らかく残像が出ます。精密で問題が出る GPU や速度優先時に使用してください。",
    };
    switch (id) {
        case kUpscaler: return kUpscalerTips;
        case kUpscaleQuality: return dlss ? kDlssTips : kFsrTips;
        case kDlssPreset: return kPresetTips;
        case kHdMotionBlurRes: return kHdBlurTips;
        case kTextureQuality:
        case kSfxTextureQuality: return kTextureTips;
        case kCharMotion: return kCharMotionTips;
        default: return nullptr;
    }
}

// fg_limit with frame generation as the menu shows it.
int fg_real_limit() { return fg_limit(reads_on(g_set[kFrameGeneration], shown_index(kFrameGeneration))); }

// A warning under a control, in `width` from `x`.
void warn_line(const std::string& text, float x, float width) {
    ImGui::SetCursorScreenPos(ImVec2(x, ImGui::GetCursorScreenPos().y));
    ImGui::PushTextWrapPos(x + width - ImGui::GetWindowPos().x);  // the window's own x
    ImGui::PushStyleColor(ImGuiCol_Text, ui::kWarn);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
}

// Frame rate's warning with frame generation on, for the rate the three
// make (`fps`, 0 Display-Limited): only above the limit (a file's, or a
// display changed since). Empty when there is nothing to say.
std::string frame_rate_warning(int fps) {
    const int limit = fg_real_limit();
    if (limit <= 0 || fps == 0 || fps <= limit) return {};
    const int hz = limit * 2;
    return "Frame generation doubles this: at most half your refresh rate (" + std::to_string(limit) + " at " + std::to_string(hz) +
           " Hz).";
}

// Frame generation's warning when the Frame rate is above half the refresh
// rate: the game is held to that half while frames are generated.
std::string frame_generation_warning(int fps) {
    const int limit = fg_real_limit();
    if (limit <= 0 || fps == 0 || fps <= limit) return {};
    const std::string rate = std::to_string(fps) + " fps";
    return "Frame rate " + rate + " is above half your " + std::to_string(limit * 2) + " Hz refresh rate: the game runs at " +
           std::to_string(limit) + ", " + std::to_string(limit * 2) + " shown.";
}

// Frame rate's control, the setup window's and F10's alike, in `width`: the
// list of rates - or, with Custom limit ticked, a field for the player's own
// number, taken when the field is left or Enter pressed and held to 30-960 -
// and the Custom limit tick box after it. `cap` is the list's index,
// `custom` the tick, `custom_fps` the number. True when one of them changed.
bool frame_rate_control(int* cap, bool* custom, int* custom_fps, float width) {
    static std::vector<std::string>* names = new std::vector<std::string>();
    static std::vector<const char*> items, tips;
    if (items.empty()) {
        // Display-Limited's own; frame generation's half on Windows alone,
        // where frame generation is (no row elsewhere).
        static const char* const kDisplayLimitedTip =
            "起動時の画面更新頻度を FPS 上限にします。180 Hz なら 180 fps、144 Hz なら 144 fps です。"
#if defined(_WIN32)
            " With Frame generation on, the game runs at half of it and the frames between are generated: 90 + 90 at "
            "180 Hz."
#endif
            ;
        const Setting& f = g_set[kFrameCap];
        for (const char* v : f.values) names->push_back(fps_from_label(v) ? std::string(v) + " fps" : std::string(v));
        for (std::size_t v = 0; v < f.values.size(); ++v) {
            items.push_back((*names)[v].c_str());
            tips.push_back(fps_from_label(f.values[v]) ? nullptr : kDisplayLimitedTip);
        }
    }
    const ImGuiStyle& st = ImGui::GetStyle();
    const char* const kBox = "任意の上限";
    const float box = ImGui::GetFrameHeight() + st.ItemInnerSpacing.x + ImGui::CalcTextSize(kBox).x;
    const float field = std::max(ImGui::GetFrameHeight() * 2.0f, width - box - st.ItemSpacing.x);
    bool changed = false;
    const float x0 = ImGui::GetCursorScreenPos().x;
    ImGui::PushID("##frame_rate");
    ImGui::SetNextItemWidth(field);
    if (*custom) {
        // What is being typed lives in the window's storage until it is taken.
        ImGuiStorage* store = ImGui::GetStateStorage();
        const ImGuiID key = ImGui::GetID("##typing");
        int typing = store->GetInt(key, *custom_fps);
        ImGui::InputInt("##fps", &typing, 0, 0);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            // With frame generation on, at most half the refresh rate (fg_real_limit).
            const int limit = fg_real_limit();
            const int fps = std::clamp(typing, kCustomFpsMin, limit > 0 ? std::max(kCustomFpsMin, limit) : 960);
            if (fps != *custom_fps) {
                *custom_fps = fps;
                changed = true;
            }
        }
        store->SetInt(key, ImGui::IsItemActive() ? typing : *custom_fps);
    } else {
        // With frame generation on, the rates above its limit greyed out (fg_real_limit).
        const int limit = fg_real_limit();
        bool above[64] = {};
        for (std::size_t v = 0; v < items.size() && v < 64; ++v) {
            const int r = fps_from_label(g_set[kFrameCap].values[v]);
            above[v] = limit > 0 && r > limit;
        }
        changed |= ui::combo("##cap", cap, items.data(), static_cast<int>(items.size()), tips.data(), above);
    }
    ui::tooltip_for_item(g_set[kFrameCap].label, g_set[kFrameCap].note);
    ImGui::SameLine(0, st.ItemSpacing.x);
    changed |= ImGui::Checkbox(kBox, custom);
    ui::tooltip_for_item(kBox, kHostFrameRateCustomNote);
    const int fps = *custom ? *custom_fps : fps_from_label(g_set[kFrameCap].values[static_cast<std::size_t>(std::clamp(*cap, 0, static_cast<int>(items.size()) - 1))]);
    if (const std::string warn = frame_rate_warning(fps); !warn.empty()) warn_line(warn, x0, width);
    ImGui::PopID();
    return changed;
}

// The Resolution list as the setup window and F10 show it: each size after
// its aspect ratio, "(16:9) 1280x720" - the list is grouped by ratio already,
// smallest to largest in each. The values stay plain WxH (saved by name and
// read with %ux%u); kResolutionAspects runs parallel to them, and a list of
// another length shows the bare sizes. The game's own list says the same
// (tools/pc_option_messages.tsv, 117010-117077).
const char* const kResolutionAspects[] = {
    "16:9", "16:9", "16:9", "16:9", "16:9", "16:9", "16:9", "16:9", "16:9", "16:9", "16:9", "16:9", "16:9", "16:10",
    "16:10", "16:10", "16:10", "16:10", "16:10", "16:10", "16:10", "16:10", "4:3", "4:3", "4:3", "4:3", "4:3", "4:3",
    "5:4", "1:1", "1:1", "1:1", "21:9", "21:9", "21:9", "21:9", "21:9", "16:5", "32:9", "32:9", "32:9", "18:5",
    "4:1", "4:1", "24:5", "24:5", "5:1", "16:3", "16:3", "16:3", "16:3", "16:3", "64:9", "43:6",
};
const std::vector<const char*>& resolution_labels(const Setting& s) {
    static std::vector<std::string> text;
    static std::vector<const char*> labels;
    if (labels.empty()) {
        constexpr std::size_t kAspects = sizeof(kResolutionAspects) / sizeof(kResolutionAspects[0]);
        for (std::size_t v = 0; v < s.values.size(); ++v)
            text.push_back(s.values.size() == kAspects ? std::string("(") + kResolutionAspects[v] + ") " + s.values[v] : s.values[v]);
        for (const std::string& t : text) labels.push_back(t.c_str());
    }
    return labels;
}

void setting_row(int id, const char* label = nullptr, const char* note = nullptr, const char* const* names = nullptr) {
    const Setting& s = g_set[id];
    if (!label) label = s.label;
    if (!note) note = s.note;
    const float sc = ui::scale();
    const int n = static_cast<int>(s.values.size());
    const int cur = std::clamp(shown_index(id), 0, n - 1);
    // Frame rate's with its Custom limit: the rate the three make.
    const int custom = id == kFrameCap ? shown_index(kFrameCapCustom) : 0;
    const int custom_fps = id == kFrameCap ? shown_index(kFrameCapCustomFps) : 0;
    const bool restart = s.restart && !g_setup_page && !(id == kResolution && res_live(cur)) &&
                         !(id == kFrameCap && frame_rate_live(frame_rate_of(cur, custom, custom_fps))) &&
                         !(id == kShadowResolution && live_shadows_can(cur));
    ImGui::PushID(id);
    int on_i = -1, off_i = -1;
    for (int v = 0; v < n; ++v) {
        if (std::strcmp(s.values[static_cast<std::size_t>(v)], "On") == 0) on_i = v;
        if (std::strcmp(s.values[static_cast<std::size_t>(v)], "Off") == 0) off_i = v;
    }
    if (n == 2 && on_i >= 0 && off_i >= 0) {
        bool v = cur == on_i;
        if (ui::toggle_row(label, restart ? "次回起動時に反映" : nullptr, &v, note)) change(id, v ? on_i : off_i);
        if (id == kFrameGeneration) {
            const std::string warn = frame_generation_warning(
                frame_rate_of(shown_index(kFrameCap), shown_index(kFrameCapCustom), shown_index(kFrameCapCustomFps)));
            if (!warn.empty()) warn_line(warn, ImGui::GetCursorScreenPos().x, ImGui::GetContentRegionAvail().x);
        }
        ImGui::PopID();
        return;
    }
    const float w = ImGui::GetContentRegionAvail().x;
    const float cw = std::min(std::round(330 * sc), w * 0.55f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::BeginGroup();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (restart) {
        ImGui::SameLine();
        ui::pill("次回起動時", ui::kWarn, ui::alpha(ui::kWarn, 38));
    }
    ImGui::EndGroup();
    if (note) ui::tooltip_for_item(label, note);
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - cw, p.y));
    int want = cur;
    bool slider = false;
    if (id == kEffectTiming) {
        // What will run (host_effect_timing_row): one of the three, or a
        // fourth entry for what bbhost.toml or BBHOST_SFX_STEP asks that none
        // of them is. A pick is one of the three, the player's own even when
        // the setting held it already (set_index).
        const HostEffectTimingRow et = host_effect_timing_row();
        const char* labels[4] = {s.values[0], s.values[1], s.values[2], et.custom.c_str()};
        const char* tips[4] = {kHostEffectTimingTips[0], kHostEffectTimingTips[1], kHostEffectTimingTips[2], et.tip.c_str()};
        const int shown = g_pending[static_cast<std::size_t>(id)] >= 0 ? cur : et.index;
        int pick = shown;
        ImGui::SetNextItemWidth(cw);
        ui::combo("##v", &pick, labels, et.index == kEffectTimingCount ? 4 : 3, tips);
        if (pick != shown && pick < kEffectTimingCount) change(id, pick);
    } else if (id == kFrameCap) {
        // The list, or Custom limit's number, and its tick box (frame_rate_control).
        int pick = cur;
        bool on = reads_on(g_set[kFrameCapCustom], custom);
        int fps = fps_from_label(g_set[kFrameCapCustomFps].values[static_cast<std::size_t>(custom_fps)]);
        const bool was_on = on;
        const int was_fps = fps;
        const float fw = std::min(std::round(420 * sc), w * 0.62f);
        ImGui::SetCursorScreenPos(ImVec2(p.x + w - fw, p.y));
        if (frame_rate_control(&pick, &on, &fps, fw)) {
            if (fps != was_fps) change(kFrameCapCustomFps, fps - kCustomFpsMin);
            if (on != was_on) change(kFrameCapCustom, on ? 1 : 0);
            if (pick != cur) change(id, pick);
        }
        ImGui::PopID();
        return;
    } else if (id == kResolution || id == kMenuTextSize || id == kTextScale) {
        ImGui::SetNextItemWidth(cw);
        ui::combo("##v", &want, id == kResolution ? resolution_labels(s).data() : s.values.data(), n);
    } else if (n <= 4 || id == kShadowResolution || id == kUpscaler || id == kAnisotropicFiltering) {
        // Shadow resolution's six sizes and Upscaling's five (Off, TAA, FSR 3,
        // FSR 4, DLSS - a slider once TAA made five) as buttons too, as wide as
        // a slider so their names fit.
        const char* const* tips = id == kShaders ? kHostShaderModeTips : value_tips(id, names != nullptr);
        const char* const* labels = names ? names : s.values.data();
        // Upscaling: the choices this platform offers, and the one made (host/upscaler.h upscaler_offered).
        const char* up_labels[5];
        const char* up_tips[5];
        int up_of[5], m = n, pick = want;
        if (id == kUpscaler) {
            m = 0;
            for (int v = 0; v < n && m < 5; ++v) {
                if (v != cur && !upscaler_offered(v)) continue;
                if (v == cur) pick = m;
                up_labels[m] = labels[v];
                up_tips[m] = tips ? tips[v] : nullptr;
                up_of[m++] = v;
            }
            labels = up_labels;
            if (tips) tips = up_tips;
        }
        float bw = cw;
        if (m > 4) {
            bw = std::min(std::round(520 * sc), w * 0.62f);
            ImGui::SetCursorScreenPos(ImVec2(p.x + w - bw, p.y));
        }
        ui::segmented("##v", &pick, labels, m, bw, tips, id == kShaders ? "次回起動時に反映します。" : nullptr);
        want = id == kUpscaler ? up_of[std::clamp(pick, 0, m - 1)] : pick;
    } else {
        // A scale: the slider shows the step's own name (+10%, x1.4, 5).
        std::string shown;
        for (const char* c = s.values[static_cast<std::size_t>(cur)]; *c; ++c) {
            shown += *c;
            if (*c == '%') shown += '%';
        }
        // Longer than the other controls, with its reset beside it.
        slider = true;
        const float sw = std::min(std::round(520 * sc), w * 0.62f);
        ImGui::SetCursorScreenPos(ImVec2(p.x + w - sw, p.y));
        ImGui::SetNextItemWidth(sw - ui::reset_width());
        ImGui::SliderInt("##v", &want, 0, n - 1, shown.c_str(), ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput);
        if (note) ui::tooltip_for_item(label, note);
        const int def = g_default_index[static_cast<std::size_t>(id)];
        if (ui::reset_button("##reset", cur == def)) want = def;
    }
    if (note && id != kShaders && !slider) ui::tooltip_for_item(label, note);  // a slider's is on the slider, not its reset
    if (want != cur && want >= 0 && want < n) change(id, want);
    ImGui::PopID();
}

// A thin line between two rows of a card.
void row_rule() {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float y = at.y - ImGui::GetStyle().ItemSpacing.y * 0.5f + 1;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x, y), ImVec2(at.x + ImGui::GetContentRegionAvail().x, y), ui::alpha(ui::kViolet, 40));
}

// The ACCOUNT actions' bubbles (account_action's `which`).
const char* const kAccountNotes[] = {
    "コードを表示してサイトを開きます。Discord でログインし、承認してください。",
    "指定した名前でこの PC 用のアカウントを作成します。復旧コードを控えてください。",
    "名前と復旧コードで復旧します。ほかの PC はログアウトします。",
    "この PC のアカウントでサイトにログインするための使い切りコードです。",
    "認証情報を削除します。認証必須のサーバーではゲームからログアウトします。",
};

// An ACCOUNT or UPDATES button, run on the window's thread with the fields as
// typed.
void action_button(const char* label, int which, bool primary, const char* name = "", const char* code = "") {
    const bool idle = action_idle(which);
    bool pressed = false;
    if (primary) {
        pressed = ui::primary_button(label, ImVec2(0, 0), idle);
    } else {
        ImGui::BeginDisabled(!idle);
        pressed = ui::ghost_button(label);
        ImGui::EndDisabled();
    }
    const char* note = which < 5 ? kAccountNotes[which] : which == 10 ? "GitHub で最新リリースを確認します。"
                                                                      : "新しいリリースのページをブラウザーで開きます。";
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ui::tooltip(nullptr, note);
    if (pressed && idle) {
        host_options_post([which, n = std::string(name), c = std::string(code)] {
            if (which == 1 || which == 2) {
                g_field[0] = n;
                g_field[1] = c;
            }
            account_action(which, n, c);
        });
    }
}

char g_name_buf[64] = {};  // the account fields as typed (the presenter's)
char g_code_buf[64] = {};
bool g_bufs_seeded = false;

void account_page() {
    const float sc = ui::scale(), w = ImGui::GetContentRegionAvail().x;
    if (!g_bufs_seeded) {
        g_bufs_seeded = true;
        std::snprintf(g_name_buf, sizeof(g_name_buf), "%s", g_field[0].c_str());
    }
    ui::card_begin("##account", ImVec2(w, 0), "ログイン状況");
    {
        const bool in = net::account_logged_in();
        ui::avatar(in ? net::account_name() : std::string(), std::round(46 * sc));
        ImGui::SameLine(0, 14 * sc);
        ImGui::BeginGroup();
        ImGui::PushFont(ui::font_strong(), 0.0f);
        ImGui::TextUnformatted(account_status().c_str());
        ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, ui::kTextDim);
        ImGui::TextUnformatted(hle_np_online_status().c_str());
        // The connection test's result as the setup window's card said it
        // before the game started (its own test, or the account's last one),
        // in the card's words and colour: the test itself runs only there,
        // from the game's port while the game is closed. Nothing when the
        // setup window did not open.
        if (const net::nat::Remembered c = net::nat::remembered(); c.known) {
            const double now = static_cast<double>(std::time(nullptr));
            const net::nat::Tone t = c.card.tone;
            ImGui::PushStyleColor(ImGuiCol_Text, t == net::nat::Tone::Good ? ui::kGood
                                                 : t == net::nat::Tone::Warn ? ui::kWarn
                                                 : t == net::nat::Tone::Bad  ? ui::kBad
                                                                             : ui::kTextDim);
            ImGui::TextUnformatted(("Connection test (" + net::nat::ago(c.at, now) + (c.saved ? "" : ", not saved") + "): " + c.card.head).c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                std::string text = c.card.sentence;
                for (const std::string* line : {&c.card.incoming, &c.card.action}) {
                    if (!line->empty()) text += " " + *line;
                }
                for (std::size_t i = 0; i < c.card.steps.size(); ++i) text += "\n" + std::to_string(i + 1) + ". " + c.card.steps[i];
                ui::tooltip("接続テスト", text, "ゲーム終了後、設定画面の「ゲーム」で再テストできます（bbhost --setup）。");
            }
        }
        ImGui::PopStyleColor();
        ImGui::EndGroup();
    }
    if (const std::string d = account_detail(); !d.empty()) {
        ImGui::Spacing();
        ui::banner(ui::Icon::Info, ui::kIce, d);
    }
    if (net::account_logged_in() && net::account_refused() && !g_acct_busy.load()) {
        ImGui::Spacing();
        ui::banner(ui::Icon::Warning, ui::kWarn,
                   std::string(kRefusedText) + " Link with Discord again, or recover an account this PC holds with its recovery code below.");
    }
    ImGui::Spacing();
    action_button("Discord と連携", 0, true);
    ImGui::SameLine();
    action_button("ウェブサイトのログインコード", 3, false);
    ImGui::SameLine();
    action_button("ログアウト", 4, false);
    ui::card_end();
    ImGui::Dummy(ImVec2(0, 6 * sc));
    ui::card_begin("##own", ImVec2(w, 0), "この PC 用のアカウント");
    const float bw = std::round(250 * sc);
    const float fw = ImGui::GetContentRegionAvail().x - bw - ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetNextItemWidth(fw);
    ImGui::InputTextWithHint("##name", "アカウント名", g_name_buf, sizeof(g_name_buf));
    ImGui::SameLine();
    action_button("この PC でアカウントを作成", 1, false, g_name_buf, g_code_buf);
    ImGui::SetNextItemWidth(fw);
    ImGui::InputTextWithHint("##code", "復旧コード", g_code_buf, sizeof(g_code_buf));
    ImGui::SameLine();
    action_button("アカウントを復旧", 2, false, g_name_buf, g_code_buf);
    ui::card_end();
}

void updates_page() {
    const float w = ImGui::GetContentRegionAvail().x;
    ui::card_begin("##updates", ImVec2(w, 0), "現在の bbhost");
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(updater::status().c_str());
    if (const std::string d = updater::detail(); !d.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::kTextDim);
        ImGui::TextUnformatted(d.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    action_button("更新を確認", 10, true);
    ImGui::SameLine();
    action_button("リリースページを開く", 11, false);
    ui::card_end();
}

}  // namespace

int host_options_page_count() { return static_cast<int>(pages().size()); }
const char* host_options_page_name(int page) {
    return page >= 0 && page < host_options_page_count() ? pages()[static_cast<std::size_t>(page)].name : "";
}
const char* host_options_page_blurb(int page) {
    return page >= 0 && page < host_options_page_count() ? pages()[static_cast<std::size_t>(page)].blurb : "";
}

// The upscaler's preset row, which shows what the chosen upscaler takes:
// for FSR 4 and FSR 3 the render size against the Resolution (Native AA,
// Quality, Balanced, Performance - Upscaling quality, upscale_quality), for
// DLSS that render size and then DLSS's model (K, J, L, M); with Upscaling
// off, DLSS's model, greyed. A change of the render size rebuilds the scene at
// once (engine/render_scale.cpp).
void preset_row() {
    const int u = shown_index(kUpscaler);  // the menu's Off, TAA, FSR 3, FSR 4, DLSS
    // DLSS at the full size is DLAA: its last choice says so.
    static const char* const kDlssQualityNames[4] = {"Performance", "Balanced", "Quality", "DLAA"};
    const auto quality_note = [](const char* who) {
        static std::string notes[2];
        const bool dlss = who[0] == 'D';
        std::string& n = notes[dlss ? 1 : 0];
        if (n.empty())
            n = std::string("How much smaller than the Resolution the 3D is drawn before ") + who + " brings it back: " +
                (dlss ? "DLAA" : "Native AA") +
                " draws it full size (anti-aliasing only), Quality at two thirds, Balanced at about 59%, Performance at half "
                "each way - faster, softer. The menus, the HUD and the post effects stay at the Resolution. Changes at once.";
        return n.c_str();
    };
    if (u == 2 || u == 3) {
        setting_row(kUpscaleQuality, "FSR プリセット", quality_note("FSR"));
        return;
    }
    if (u == 4) {
        setting_row(kUpscaleQuality, "DLSS の品質", quality_note("DLSS"), kDlssQualityNames);
        row_rule();
    }
    setting_row(kDlssPreset);
}

// The plugin that does the anti-aliasing itself (plugin API 27,
// host/plugins.h): in game the one the plugins' claims name, in the setup
// window - before any plugin runs - the first turned on whose flags say so.
std::string aa_taker() { return g_setup_page ? plugins_aa_owner_scanned() : plugins_aa_owner(); }

// The Upscaling row's note: its own, or - the choice one this GPU or the
// files beside bbhost.exe cannot run - why, and that the game's own
// anti-aliasing runs in its place (frame generation still does); a plugin
// that does the anti-aliasing itself, or a Vulkan layer that keeps TAA off.
const char* upscaler_note() {
    const int u = shown_index(kUpscaler);  // the menu's Off, TAA, FSR 3, FSR 4, DLSS
    static std::string note;
    const char* ours = "TAA, FSR or DLSS";
    if (const std::string who = aa_taker(); !who.empty()) {
        note = g_setup_page ? who + " (a plugin turned on in the Plugins tab) does the anti-aliasing itself: while it is on, bbhost runs no " +
                                  ours + " (nor Sharpness, which follows them), and the 3D is drawn at the Resolution. Your choice is kept for "
                                         "when the plugin is off."
                            : "Off while the " + who + " plugin is on: it does the anti-aliasing itself, so bbhost moves no draws and runs no " +
                                  ours + " (nor Sharpness, which follows them), and the 3D is drawn at the Resolution. Your choice is kept for "
                                         "when the plugin is off (F9).";
        return note.c_str();
    }
    if (u == 1 && !g_setup_page && upscaler_layer_blocks_taa()) {
        note = "bbhost.exe の隣の vulkan-1.dll が独自の輪郭補正レイヤーのため、TAA を無効にしました。TAA を使うにはその DLL を外すか、bbhost の輪郭補正に対応したプラグイン版を使用してください。";
        return note.c_str();
    }
    // (The setup window comes before the bridge loads: nothing known yet. TAA
    // is bbhost's own: it always runs.)
    if (u <= 1 || g_setup_page) return g_set[kUpscaler].note;
    const UpscalerCaps caps = upscaler_caps();
    const bool dlss = u == 4;
#if !defined(_WIN32)
    // Linux: FSR 3 through bbhost's FidelityFX library, DLSS through the NGX bridge, no FSR 4 (a Windows choice kept
    // runs FSR 3).
    if (!dlss && !caps.fsr_loaded) {
        note = "Not running: libbbhost_fidelityfx_vk.so did not load (it belongs beside bbhost, as the packages have it). The game's "
               "own anti-aliasing runs instead.";
    } else if (!dlss && !caps.fsr) {
        note = "この環境では FSR 3 を使用できません" + (caps.fsr_reason.empty() ? std::string() : " (" + caps.fsr_reason + ")") +
               ": the game's own anti-aliasing runs instead.";
    } else if (!dlss && u == 3) {
        note = "FSR 4 は Windows 専用です。この環境では FSR 3 を使用します。";
    } else if (!dlss) {
        return g_set[kUpscaler].note;
    } else if (!caps.loaded) {
        note = "Not running: libbbhost_ngx_bridge.so did not load (it and NVIDIA's libnvidia-ngx-dlss.so belong beside "
               "bbhost, as the Linux package has them). The game's own anti-aliasing runs instead.";
    } else if (!caps.dlss) {
        note = "この環境では DLSS を使用できません" + (caps.dlss_reason.empty() ? std::string() : " (" + caps.dlss_reason + ")") +
               ": the game's own anti-aliasing runs instead.";
    } else {
        return g_set[kUpscaler].note;
    }
#else
    if (!caps.loaded) {
        note = "Not running: upscaler_bridge.dll did not load (it and AMD's FidelityFX files belong beside bbhost.exe). The "
               "game's own anti-aliasing runs instead; Frame generation works without it.";
    } else if (dlss ? !caps.dlss : !caps.fsr) {
        const std::string why = dlss ? caps.dlss_reason : caps.fsr_reason;
        note = std::string(dlss ? "DLSS" : "FSR") + " is not available here" + (why.empty() ? "" : " (" + why + ")") +
               ": the game's own anti-aliasing runs instead. Frame generation works without it." +
               (dlss ? " On an AMD or Intel GPU choose FSR 4 or FSR 3." : "");
    } else {
        return g_set[kUpscaler].note;
    }
#endif
    return note.c_str();
}

// The Anti-aliasing row's note: its own, or that it is off while an upscaler
// or a plugin's own anti-aliasing takes its place (engine/graphics_patch.cpp).
const char* anti_alias_note() {
    static std::string note;
    if (const std::string who = aa_taker(); !who.empty()) {
        note = "Off while the " + who + " plugin is on: it does the anti-aliasing itself.";
        return note.c_str();
    }
    // (In game the upscaler as it runs - a Vulkan layer beside bbhost can keep
    // TAA off, DLSS and FSR need their files - in the setup window as chosen.)
    if (g_setup_page ? shown_index(kUpscaler) != 0 : upscaler_method() != 0)
        return "TAA・FSR・DLSS が輪郭補正を行う間は、二重処理によるぼけを防ぐため無効です。";
    return g_set[kAntiAlias].note;
}

// Rows that only mean something under another setting, left out while it is
// off: the upscaler's preset with Upscaling off, the motion vectors and
// Character motion with Upscaling and Frame generation both off, HD motion
// blur quality with HD motion blur off.
bool row_hidden(int id) {
    if (id == kHdMotionBlurRes) return !reads_on(g_set[kHdMotionBlur], shown_index(kHdMotionBlur));
    if (id == kDlssPreset) return shown_index(kUpscaler) <= 1;  // (Off, TAA: no preset)
    if (id == kTaaStrength) return shown_index(kUpscaler) != 1;  // (TAA's alone)
    if (id == kMotionVectors || id == kCharMotion)
        return shown_index(kUpscaler) == 0 && !reads_on(g_set[kFrameGeneration], shown_index(kFrameGeneration));
    if (id == kStayBorderless) return shown_index(kWindowMode) != 1;  // (Borderless's alone)
    return false;
}

// Rows shown but not to be changed while the setting they need is off.
bool row_greyed(int id) {
    return id == kSsrQuality && !reads_on(g_set[kSsr], shown_index(kSsr));
}

void host_options_draw_page(int page) {
    if (page < 0 || page >= host_options_page_count()) return;
    const OptionsPage& pg = pages()[static_cast<std::size_t>(page)];
    ui::card_begin("##rows", ImVec2(ImGui::GetContentRegionAvail().x, 0));
    bool first = true;
    for (int r = pg.first; r < pg.end; ++r) {
        if (g_rows[r].kind != Row::Option) continue;
        if (g_setup_page && g_rows[r].setting == kShaders) continue;
        if (row_hidden(g_rows[r].setting)) continue;
        if (!first) row_rule();
        first = false;
        const bool greyed = row_greyed(g_rows[r].setting);
        if (greyed) ImGui::BeginDisabled();
        if (g_rows[r].setting == kDlssPreset) preset_row();
        else if (g_rows[r].setting == kUpscaler) setting_row(kUpscaler, nullptr, upscaler_note());
        else if (g_rows[r].setting == kAntiAlias) setting_row(kAntiAlias, nullptr, anti_alias_note());
        else setting_row(g_rows[r].setting);
        if (greyed) ImGui::EndDisabled();
        ImGui::Dummy(ImVec2(0, 1 * ui::scale()));
    }
    ui::card_end();
    // The pages that end in a card of their own, as the setup window's do.
    const bool game = std::strcmp(pg.header, "Game") == 0, settings = std::strcmp(pg.header, "設定") == 0;
    if (game || settings) {
        ImGui::Dummy(ImVec2(0, 8 * ui::scale()));
        if (game) account_page();
        else updates_page();
    }
}

void host_options_draw_setup_page(int page) {
    // Resolution is the Graphics tab's here too (the upscaler's output size
    // and the render size follow it); only the shader form stays the Shaders
    // page's.
    g_setup_page = true;
    host_options_draw_page(page);
    g_setup_page = false;
}

#else  // a build without a window: no menu

void host_options_draw_setup_page(int) {}
int host_options_page_count() { return 0; }
const char* host_options_page_name(int) { return ""; }
const char* host_options_page_blurb(int) { return ""; }
void host_options_draw_page(int) {}

#endif

const char* const kHostShaderModeTips[2] = {
    "推奨設定です。プレイ中に Microsoft のシェーダーコンパイラーで最適化し、次回起動時に使用します。画質は変わりません。",
    "bbhost の直接変換のみを使用し、プレイ中の追加コンパイルを行いません。HLSL で問題が出る場合に試してください。画質は同じです。",
};

const char* const kHostFrameRateCustomNote =
    "10〜960 fps の任意の上限を入力できます。45 なら実描画は 45 fps です。"
#if defined(_WIN32)
    "フレーム生成時は表示数が２倍（45 なら 90）になるため、上限は画面更新の半分です。"
#endif
    "無効時は一覧のフレームレートを使用します。変更は即時反映します。";
const char* const kHostEffectTimingLabels[3] = {"30 fps", "60 fps", "フレームレートに追従"};
const char* const kHostEffectTimingNote =
    "30 fps 超での炎・血・魔法・ランプなどの時間処理です。即時反映します。";
const char* const kHostEffectTimingTips[3] = {
    "フレームレートにかかわらず、原作の 30 fps と同じ毎秒30回で更新します。30 fps を超えると、動きのなめらかさが下がります。",
    "標準です。30 fps 超ではエフェクトを毎秒60回更新します。回数で処理する揺れなども原作の２倍の速度になります。",
    "エフェクトを１フレームに１回更新します。揺らぎなど回数で進める処理は、60 fps で原作の２倍、120 fps で４倍の速さになります。",
};

HostSettings host_settings() { return settings_store().now(); }
const HostSettings& host_settings_ref() { return settings_store().now(); }
void host_opt_republish() { rebuild_settings(); }

void host_settings_listen(HostSettingsListener fn) {
    if (!settings_store().listen(fn)) host_log("options: no room for another settings listener; one consumer will not follow changes");
}

namespace {
// Which way round a two-value setting reads comes from the values themselves,
// not a list of ids kept in step by hand: a new {"On","Off"} setting was
// silently inverted by that list, which is the sort of bug that only shows up
// as "the option does the opposite".
bool reads_on(const Setting& s, int index) {
    const std::size_t i = static_cast<std::size_t>(index);
    if (i < s.values.size()) {
        if (std::strcmp(s.values[i], "On") == 0) return true;
        if (std::strcmp(s.values[i], "Off") == 0) return false;
    }
    return index != 0;  // anything else: the first value is the "no"
}

int setting_id(const char* key) {
    for (int id = 0; key && id < kSettingCount; ++id) {
        if (std::strcmp(key, g_set[id].key) == 0) return id;
    }
    return -1;
}

}  // namespace

std::uint64_t host_opt_serial() { return settings_store().now().serial; }

bool host_opt_default_get(const char* key) {
    const int id = setting_id(key);
    return id >= 0 && reads_on(g_set[id], g_default_index[static_cast<std::size_t>(id)]);
}
int host_opt_default_index(const char* key) {
    const int id = setting_id(key);
    return id < 0 ? -1 : g_default_index[static_cast<std::size_t>(id)];
}
int host_opt_default_frame_cap() {
    return fps_from_label(g_set[kFrameCap].values[static_cast<std::size_t>(g_default_index[kFrameCap])]);
}
int host_opt_default_resolution_index() { return g_default_index[kResolution]; }

bool host_opt_get(const char* key) {
    const int id = setting_id(key);
    return id >= 0 && reads_on(g_set[id], g_set[id].index);
}

bool host_opt_saved(const char* key) {
    const int id = setting_id(key);
    return id >= 0 && g_saved[static_cast<std::size_t>(id)];
}

HostEffectTimingRow host_effect_timing_row() {
    HostEffectTimingRow r;
    const int chosen = std::clamp(g_set[kEffectTiming].index, 0, kEffectTimingCount - 1);
    const EffectTiming t = config_effect_timing(60, kEffectTimingValues[chosen], g_saved[kEffectTiming]);
    const std::string v = t.readable ? t.value : std::string(kEffectTimingDefault);  // one it cannot read runs the default
    r.index = effect_timing_index(v == "60" ? "auto" : v);
    if (r.index >= 0) return r;
    const bool env = std::strcmp(t.from, "BBHOST_SFX_STEP") == 0;
    r.index = kEffectTimingCount;
    r.custom = "カスタム: " + v + (env ? " (BBHOST_SFX_STEP)" : " (bbhost.toml)");
    r.tip = (v == "frame"    ? std::string("エフェクトを各フレームの実際の時間に合わせます。")
             : v == "smooth" ? std::string("エフェクトは指定フレームレートで動作し、回数で処理する部分は毎秒30回を維持します（旧「滑らか」）。")
                             : "エフェクトを毎秒 " + v + " 回更新します。") +
            (env ? "BBHOST_SFX_STEP の指定が、この設定より優先されます。"
                 : "bbhost.toml の effect_steps に従います。ほかの項目を選ぶと、その選択を優先します。");
    return r;
}

void host_opt_choose_effect_timing(int index) {
    if (index < 0 || index >= kEffectTimingCount) return;
    set_index(kEffectTiming, index, false);  // the player's own (g_saved) and written, even when it held it already
    save();
}

bool host_opt_resolution(int* w, int* h) {
    const HostSettings hs = host_settings();
    if (hs.res_width <= 0 || hs.res_height <= 0) return false;
    *w = hs.res_width;
    *h = hs.res_height;
    return true;
}

bool host_opt_upscaler_chosen() {
    if (scene_look_scale()) return false;  // a plugin's 3D at a size of its own, brought up nearest
    if (plugins_aa_claim() == kAaTake) return false;  // a plugin's anti-aliasing (plugin API 27): the 3D at the Resolution
    // BBHOST_UPSCALE, read once: -1 unset, else whether it names an upscaler.
    static const int env = [] {
        const char* e = std::getenv("BBHOST_UPSCALE");
        if (!e || !*e) return -1;
        return std::strcmp(e, "dlss") == 0 || std::strcmp(e, "fsr") == 0 || std::strcmp(e, "fsr3") == 0 || std::strcmp(e, "taa") == 0 ? 1 : 0;
    }();
    // TAA (4) too: a Resolution below the window draws the 3D at it and
    // keeps the game and its HUD at the window's size, as FSR and DLSS do.
    const int u = host_settings_ref().upscaler;
    return env >= 0 ? env != 0 : u != 0;
}

// (TAA wants the motion too, though it is no upscaler that sizes the 3D, and
// so does a plugin's render hook that asks for it: host/render_hooks.h.)
bool host_opt_motion_wanted() {
    return host_opt_upscaler_chosen() || upscaler_method() == kUpscaleTaa || host_settings_ref().frame_generation || render_hooks_want_motion();
}

int host_opt_motion_rebuilt_bits() {
    if (!host_opt_motion_wanted()) return 0;
    static const int env = [] {
        const char* e = std::getenv("BBHOST_MOTION_VECTORS");
        if (!e || !*e) return -1;
        const int b = std::atoi(e);
        return b == 8 || b == 16 || b == 32 ? b : 0;
    }();
    return env > 0 ? env : 0;  // the game's velocity map unless a test asks (the setting is no longer shown)
}

bool host_opt_output_size(int* w, int* h) {
    if (!host_opt_resolution(w, h)) return false;
    output_for(w, h);
    return true;
}

namespace {
// Whether the 3D can be drawn at a size of its own (engine/render_scale.cpp
// render_scale_plan): off with BBHOST_RENDER_SCALE=0 or BBHOST_LIVE_RESOLUTION=0.
bool render_scale_possible() {
    static const bool on = [] {
        for (const char* name : {"BBHOST_RENDER_SCALE", "BBHOST_LIVE_RESOLUTION"})
            if (const char* e = std::getenv(name); e && e[0] == '0') return false;
        return true;
    }();
    return on;
}

void output_for(int* w, int* h) {
    const bool look = scene_look_scale();
    // Upscaling Off too: a Resolution below the window draws the 3D at it and
    // keeps the game and its HUD at the window's size, as TAA, FSR and DLSS
    // do - the 3D brought up by YEBIS's copy (engine/render_scale.cpp).
    if (!host_opt_upscaler_chosen() && !look && !render_scale_possible()) return;
    int ww = 0, wh = 0;
    if (g_set[kWindowMode].index == 0) {
        ww = config().width;
        wh = config().height;
    } else if (!host_window_desktop_size(&ww, &wh) && !host_desktop_primary_size(&ww, &wh)) {
        // The screen the window fills: once the window is made, its display's
        // desktop mode (not Fullscreen's exclusive mode while it is up);
        // before that the primary display's, where the window opens. Not the
        // largest width and height of all displays: with a second display of
        // another shape (a portrait one beside a 2560x1440, 2560x1920) the
        // game was made that shape and shown with bars at the sides, its UI
        // shrunk.
        return;
    }
    if (ww <= 0 || wh <= 0) return;
    // A Resolution as large as the window, or larger, is the size itself -
    // unless a plugin's look draws the 3D at a size of its own: the game and
    // its HUD are then always the window's (a larger Resolution would be
    // filtered down to it), the 3D at most that (render_scale.cpp wanted).
    if (!look && static_cast<long long>(*w) * *h >= static_cast<long long>(ww) * wh) return;
    *w = ww;
    *h = wh;
}
}  // namespace

int host_opt_resolution_index() { return g_set[kResolution].index; }

const char* const* host_opt_resolution_labels(int* n) {
    const std::vector<const char*>& labels = resolution_labels(g_set[kResolution]);
    *n = static_cast<int>(labels.size());
    return labels.data();
}

const char* host_opt_resolution_note() { return g_set[kResolution].note; }

void host_opt_set_resolution_index(int i) {
    if (i < 0 || static_cast<std::size_t>(i) >= g_set[kResolution].values.size()) {
        return;
    }
    if (i == g_set[kResolution].index) return;
    set_index(kResolution, i, false);
    save();
}

int host_opt_frame_cap() {
    return fps_from_label(g_set[kFrameCap].values[static_cast<std::size_t>(g_set[kFrameCap].index)]);
}

void host_opt_set_frame_cap(int fps) {
    const int i = fps_index(fps);
    if (i < 0) {
        return;
    }
    // A rate picked from a list (the game's own System menu) is the list's:
    // Custom limit off with it.
    const bool untick = on_of(kFrameCapCustom);
    if (i == g_set[kFrameCap].index && !untick) return;
    if (i != g_set[kFrameCap].index) set_index(kFrameCap, i, false);
    if (untick) set_index(kFrameCapCustom, 0, false);
    save();
}

bool host_opt_frame_cap_custom() { return on_of(kFrameCapCustom); }
int host_opt_frame_cap_custom_fps() {
    return fps_from_label(g_set[kFrameCapCustomFps].values[static_cast<std::size_t>(g_set[kFrameCapCustomFps].index)]);
}

void host_opt_set_frame_cap_custom(bool on, int fps) {
    const int i = std::clamp(fps, kCustomFpsMin, 960) - kCustomFpsMin;
    const int tick = on ? 1 : 0;
    if (i == g_set[kFrameCapCustomFps].index && tick == g_set[kFrameCapCustom].index) return;
    if (i != g_set[kFrameCapCustomFps].index) set_index(kFrameCapCustomFps, i, false);
    if (tick != g_set[kFrameCapCustom].index) set_index(kFrameCapCustom, tick, false);
    save();
}

int host_opt_frame_rate() { return frame_rate_now(); }

#if defined(BBHOST_HAVE_SDL3)
bool host_frame_rate_control(int* cap_index, bool* custom, int* custom_fps, float width) {
    return frame_rate_control(cap_index, custom, custom_fps, width);
}
bool host_frame_rate_fit(int* cap_index, bool* custom, int* custom_fps) {
    int c = *custom ? 1 : 0, f = std::clamp(*custom_fps, kCustomFpsMin, 960) - kCustomFpsMin;
    if (!frame_rate_fit(cap_index, &c, &f, fg_limit(host_opt_get("frame_generation")))) return false;
    *custom = c != 0;
    *custom_fps = f + kCustomFpsMin;
    return true;
}
void host_frame_generation_warning(int cap_index, bool custom, int custom_fps) {
    const int fps = custom ? custom_fps : fps_from_label(g_set[kFrameCap].values[static_cast<std::size_t>(cap_index)]);
    if (const std::string warn = frame_generation_warning(fps); !warn.empty())
        warn_line(warn, ImGui::GetCursorScreenPos().x, ImGui::GetContentRegionAvail().x);
}
int host_frame_rate_warning_count(int cap_index, bool custom, int custom_fps) {
    const int fps = custom ? custom_fps : fps_from_label(g_set[kFrameCap].values[static_cast<std::size_t>(cap_index)]);
    return (frame_rate_warning(fps).empty() ? 0 : 1) + (frame_generation_warning(fps).empty() ? 0 : 1);
}
#endif

void host_opt_set(const char* key, bool on) {
    if (!key) {
        return;
    }
    for (int id = 0; id < kSettingCount; ++id) {
        if (std::strcmp(key, g_set[id].key) != 0) {
            continue;
        }
        // Which index is "on" comes from the values, as in host_opt_get: a
        // hand-kept list of the {"On","Off"} settings missed Mouse camera and
        // the four graphics switches, so turning one of those on from the
        // game's menu saved it off. Two values that are not On/Off
        // (Windowed/Fullscreen) read the second as on.
        const Setting& s = g_set[id];
        // A setting with no On (Window mode) is on at any value past its
        // first: on keeps Fullscreen as it is, and turns Windowed Borderless.
        const bool has_on = std::any_of(s.values.begin(), s.values.end(), [](const char* v) { return std::strcmp(v, "On") == 0; });
        if (on && !has_on && s.index != 0) return;
        int want = on ? 1 : 0;
        for (std::size_t v = 0; v < s.values.size(); ++v) {
            if (std::strcmp(s.values[v], on ? "On" : "Off") == 0) {
                want = static_cast<int>(v);
            }
        }
        if (want == s.index) return;  // the setup window sets every one; only a change is written
        set_index(id, want, false);
        // The F10 screen saves when it closes; a row in the game's menu has no
        // such moment, so a change made there is written now or not at all.
        save();
        return;
    }
}

float host_opt_menu_text_scale() {
    const Setting& s = g_set[kMenuTextSize];
    const int i = std::clamp(s.index, 0, static_cast<int>(s.values.size()) - 1);
    return static_cast<float>(std::atoi(s.values[static_cast<std::size_t>(i)])) / 100.0f;
}

void host_opt_menu_colour(float* hue_degrees, float* vibrance) {
    const auto value = [](SettingId id) {
        const Setting& s = g_set[id];
        const int i = std::clamp(s.index, 0, static_cast<int>(s.values.size()) - 1);
        return static_cast<float>(std::atoi(s.values[static_cast<std::size_t>(i)]));
    };
    *hue_degrees = value(kMenuHue);
    *vibrance = value(kMenuVibrance) / 100.0f;
}

int host_opt_index(const char* key) {
    for (int id = 0; key && id < kSettingCount; ++id) {
        if (std::strcmp(key, g_set[id].key) == 0) return g_set[id].index;
    }
    return -1;
}

void host_opt_set_index(const char* key, int index) {
    for (int id = 0; key && id < kSettingCount; ++id) {
        if (std::strcmp(key, g_set[id].key) != 0) continue;
        if (index < 0 || static_cast<std::size_t>(index) >= g_set[id].values.size()) return;
        if (index == g_set[id].index) return;
        set_index(id, index, false);
        save();
        return;
    }
}

void host_options_save_now() {
    g_dirty = true;
    save();
}

int host_options_selftest(const std::string& dir) {
    // An options file as v0.2.31 wrote it, Character motion at Off (then
    // called Velocity map) and one other setting away from its default - and
    // the Debug camera on, a setting that is gone.
    const std::string path = dir + "/bbhost-options.toml";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << "# bbhost PC port options. Written by the options screen (F10).\n[options]\nversion = \"8\"\n"
             "char_motion = \"Velocity map\"\ndebug_camera = \"On\"\nssr_quality = \"Half\"\n";
        if (!f) {
            std::printf("options selftest: cannot write %s\n", path.c_str());
            return 1;
        }
    }
#if defined(_WIN32)
    _putenv_s("BBHOST_OPTIONS_PATH", path.c_str());
#else
    setenv("BBHOST_OPTIONS_PATH", path.c_str(), 1);
#endif
    int failed = 0;
    const auto check = [&failed](bool ok, const char* what) {
        std::printf("options selftest: %s %s\n", ok ? "ok  " : "FAIL", what);
        failed += ok ? 0 : 1;
    };
    host_options_load();
    check(host_opt_default_index("char_motion") == 0 && std::strcmp(g_set[kCharMotion].values[0], "Precise") == 0,
          "Character motion's default is Precise");
    check(host_opt_index("char_motion") == 1 && std::strcmp(g_set[kCharMotion].values[1], "Off") == 0,
          "char_motion = \"Velocity map\" reads as Off");
    check(host_opt_saved("char_motion"), "as the player's own choice");
    check(!host_settings_ref().char_motion, "with the characters' capture off");
    check(host_opt_index("ssr_quality") == 1, "the file's other setting reads as saved (ssr_quality Half)");
    host_options_save_now();
    std::ifstream in(path);  // text: the file is written as text (CRLF on Windows)
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    check(text.find("\nchar_motion = \"Off\"\n") != std::string::npos, "written back as char_motion = \"Off\"");
    check(text.find("Velocity map") == std::string::npos, "with no Velocity map left in the file");
    check(text.find("\nssr_quality = \"Half\"\n") != std::string::npos, "and ssr_quality = \"Half\" kept");
    check(text.find("debug_camera") == std::string::npos, "and the Debug camera's line, a setting that is gone, left out");
    std::printf("options selftest: %s (%s)\n", failed ? "FAILED" : "passed", path.c_str());
    return failed ? 1 : 0;
}

bool host_steam_deck() {
    static const bool deck = [] {
        if (const char* e = std::getenv("BBHOST_STEAM_DECK"); e && e[0]) return e[0] == '1';
        // The firmware's vendor and product: Valve's Jupiter (the LCD Deck) or
        // Galileo (the OLED one). Not any Valve machine - another has another
        // GPU, and these two configurations are this one's.
        std::string vendor, product;
#if defined(_WIN32)
        const auto bios = [](const char* name) {
            char buf[128] = {};
            DWORD size = sizeof(buf);
            if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\BIOS", name, RRF_RT_REG_SZ, nullptr, buf, &size) !=
                ERROR_SUCCESS)
                return std::string();
            return std::string(buf);
        };
        vendor = bios("SystemManufacturer");
        product = bios("SystemProductName");
#else
        const auto line = [](const char* path) {
            std::ifstream f(path);
            std::string s;
            std::getline(f, s);
            return s;
        };
        vendor = line("/sys/devices/virtual/dmi/id/sys_vendor");
        product = line("/sys/devices/virtual/dmi/id/product_name");
#endif
        return vendor == "Valve" && (product == "Jupiter" || product == "Galileo");
    }();
    return deck;
}

namespace {
// The Deck's configurations, by the frame rate: 60 or more (or uncapped) is 60 fps.
int deck_profile_of(int fps) { return fps == 0 || fps >= 60 ? 1 : 0; }
}  // namespace

int host_deck_profile() { return deck_profile_of(host_opt_frame_rate()); }
int host_deck_default_profile() { return deck_profile_of(host_opt_default_frame_cap()); }

void host_set_deck_profile(int profile) {
    const bool sixty = profile == 1;
    if (const int i = fps_index(sixty ? 60 : 30); i >= 0 && i != g_set[kFrameCap].index) set_index(kFrameCap, i, false);
    if (on_of(kFrameCapCustom)) set_index(kFrameCapCustom, 0, false);  // the Deck's rate, not a Custom limit's
    if (const int m = sixty ? 1 : 2; m != g_set[kModelDetail].index) set_index(kModelDetail, m, false);  // Low, Normal
    const char* const size = sixty ? "960x600" : "1280x800";
    for (std::size_t v = 0; v < g_set[kResolution].values.size(); ++v) {
        if (std::strcmp(g_set[kResolution].values[v], size) != 0 || static_cast<int>(v) == g_set[kResolution].index) continue;
        // Both sizes are known to fit the Deck: no keep-or-revert question,
        // and one still waiting is settled by this.
        g_res_prev = -1;
        g_res_reverting = true;
        set_index(kResolution, static_cast<int>(v), false);
        g_res_reverting = false;
    }
    host_log("options: Steam Deck at %s", sixty ? "60 fps (960x600 upscaled, Model detail Low)" : "30 fps (1280x800)");
    save();
}

// The ACCOUNT actions for the setup window (host/options.h).
void host_account_action(int which, const std::string& name, const std::string& code) {
    if (which >= 0 && which <= 4) account_action(which, name, code);
}
bool host_account_busy() { return g_acct_busy.load(); }
std::string host_account_signed_in() {
    return net::account_logged_in() ? "ログイン中: " + net::account_name() : "未ログイン";
}
std::string host_account_outcome() {
    std::lock_guard<std::mutex> lk(g_acct_mu);
    return g_acct_status;
}
std::string host_account_detail() { return account_detail(); }
bool host_account_take_save() { return g_acct_save.exchange(false); }
