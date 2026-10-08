// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: user settings changed at run time from the in-game menu (bbport_overlay.h) and kept
// in bbport.ini (BB_CONFIG overrides the path). Environment variables override the file at
// start. Readers load the atomics every frame; writers are the menu and Load().

#pragma once

#include <atomic>
#include <string>

namespace BbSettings {

enum Upscaler : int { UpscalerOff = 0, UpscalerFsr3 = 1, UpscalerFsr4 = 2, UpscalerFsr411 = 3,
                      UpscalerTaa = 4, UpscalerDlss = 5, UpscalerCount };
/// FSR 4 v07 or FSR 4.1.1: the same inputs, settings and placement in the frame.
inline bool IsFsr4(int upscaler) {
    return upscaler == UpscalerFsr4 || upscaler == UpscalerFsr411;
}
/// FSR 4, FSR 4.1.1 or DLSS: one frame's inputs to a separate upscaler, which writes the
/// output image (TemporalUpscaler::RecordFsr4); FSR 3.1 and TAA are recorded in place.
inline bool IsFrameUpscaler(int upscaler) {
    return IsFsr4(upscaler) || upscaler == UpscalerDlss;
}
enum Preset : int { NativeAA = 0, Quality, Balanced, Performance, UltraPerformance, PresetCount };
enum DebugView : int { DebugNone = 0, DebugReactive = 1, DebugMotion = 2, DebugViewCount };
enum DisplayMode : int {
    DisplayWindowed = 0,   ///< bordered window, resizable
    DisplayBorderless = 1, ///< borderless desktop fullscreen (F11 toggles, BB_FULLSCREEN)
    DisplayExclusive = 2,  ///< exclusive fullscreen at the output resolution's display mode
    DisplayModeCount,
};
enum HideCursor : int {
    HideCursorAuto = 0, ///< hidden while fullscreen, unfocused or in menus (they draw their own)
    HideCursorAlways = 1, ///< hidden whenever the game window is the foreground one
    HideCursorNever = 2,
};

/// Game effects switched by the community patches at start (patches.py EFFECTS): ini key,
/// menu label, default (the game's own behaviour).
struct Effect {
    const char* key;
    const char* label;
    bool default_on;
};
inline constexpr Effect Effects[] = {
    {"effect_chromatic_aberration", "色差", true},
    {"effect_dof", "景深（DoF）", true},
    {"effect_motion_blur", "运动模糊", true},
    {"effect_ssao", "SSAO 环境光遮蔽", true},
    {"effect_game_aa", "游戏自带抗锯齿", true},
    {"effect_dynamic_shadows", "动态光源阴影", true},
    {"effect_ssr", "SSR 屏幕空间反射（原版没有）", false},
    {"skip_intro", "跳过开场动画", false},
    {"debug_camera", "自由视角（叉键 + L3）", false},
    {"debug_menu", "调试菜单（需要字体文件）", false},
};
inline constexpr int EffectCount = int(sizeof(Effects) / sizeof(Effects[0]));
/// Live output resolutions: the upscaler's output and the UI host targets.
inline constexpr int OutputWidths[] = {1280, 1920, 2560, 3840};
inline constexpr int OutputHeights[] = {720, 1080, 1440, 2160};
inline constexpr int OutputCount = 4;
inline constexpr int OutputDefault = 1; ///< 1920x1080, the game's own size

/// Menu pages (BbOverlay::Page must stay in this order). The settings module needs the count
/// to clamp ui_page, so it lives here rather than in the overlay.
inline constexpr int UiPageCount = 5;

/// Upper bound of gc_budget_mb, shared by the parser and the Advanced menu slider so the
/// file and the UI cannot disagree. 16 GiB covers every current GPU; the effective budget is
/// additionally clamped to the driver's live heapBudget in the texture cache, so a value
/// above the device's real budget cannot push the thresholds past what it tolerates.
inline constexpr int GcBudgetMaxMB = 16384;

struct Values {
    std::atomic<int> upscaler{UpscalerFsr3};
    std::atomic<int> preset{NativeAA};
    std::atomic<bool> sharpen{true};
    std::atomic<float> sharpness{0.3f};
    std::atomic<bool> jitter{true};
    std::atomic<bool> reactive{false};
    std::atomic<bool> object_motion{true};
    std::atomic<float> reactive_scale{1.0f};
    std::atomic<float> reactive_threshold{0.2f};
    std::atomic<float> reactive_max{0.9f};
    std::atomic<int> debug_view{DebugNone};
    std::atomic<bool> show_fps{false};
    /// FPS counter detail: 0 frame rate only, 1 + average ms, 2 + worst ms.
    std::atomic<int> fps_detail{2};
    /// NVIDIA VK_NV_low_latency2 (Reflex-style) low latency mode; hot-applied per present.
    std::atomic<bool> low_latency{false};
    // FSR 4 checks (menu): the provider's auto exposure, the jitter sign it is given.
    std::atomic<bool> fsr4_auto_exposure{true};
    std::atomic<bool> fsr4_invert_jitter{false};
    std::atomic<int> active_render_width{1920}, active_render_height{1080};
    /// Applied at start (patches.py); the menu shows when a restart is needed.
    std::atomic<bool> effects[EffectCount]{};
    std::atomic<int> model_lod{0}; ///< -2 highest .. 2 lowest, 0 the game's
    std::atomic<int> output_res{OutputDefault}; ///< index into OutputWidths
    /// Window mode (DisplayMode); F11 toggles windowed <-> borderless, hot-applied.
    std::atomic<int> display_mode{DisplayWindowed};
    /// Mouse cursor visibility in the game window (HideCursor), hot-applied.
    std::atomic<int> hide_cursor{HideCursorAuto};
    /// Monitor the window lives on, -1 for the primary display; hot-applied.
    std::atomic<int> display{-1};
    /// Overlay UI scale in percent (fixed menu steps); hot-applied.
    std::atomic<int> ui_scale{100};
    /// Menu page open at start (0 graphics, 1 display, 2 effects, 3 cheats, 4 advanced), so
    /// the menu reopens where it was left. Written on close like every other menu change.
    std::atomic<int> ui_page{0};
    /// Present-path post chain (BbPost), hot-applied every frame. Each percent strength turns
    /// its effect off at 0, and the whole chain is bypassed when everything is 0.
    std::atomic<int> post_deband{50};    ///< f3kdb-style banding threshold strength, percent
    std::atomic<int> post_shadow{0};     ///< black-level lift, percent (100% = black at 0.30)
    std::atomic<int> post_sharpen{0};    ///< RCAS strength after upscaling, percent
    std::atomic<int> post_defog{0};      ///< uniform haze removal (black-point pull), percent
    std::atomic<int> post_contrast{0};   ///< contrast around mid-grey, percent (100% = x1.30)
    std::atomic<int> post_saturation{0}; ///< saturation over Rec.709 luma, percent (100% = x1.40, -100% = x0.60)
    std::atomic<int> post_range{12};     ///< deband sample radius, pixels 4..32
    std::atomic<bool> post_split{false}; ///< debug: left half of pass 1 stays untouched
    /// Colour grade, pass 1 (ASC CDL + ReShade-style vibrance and levels), percent
    /// strengths, 0 = neutral; hot-applied every frame.
    std::atomic<int> post_vibrance{0}; ///< luma-weighted saturation along Rec.709 luma
    std::atomic<int> post_lift_r{0}, post_lift_g{0}, post_lift_b{0};    ///< CDL offset
    std::atomic<int> post_gamma_r{0}, post_gamma_g{0}, post_gamma_b{0}; ///< CDL power
    std::atomic<int> post_gain_r{0}, post_gain_g{0}, post_gain_b{0};    ///< CDL slope
    std::atomic<int> post_levels_black{0}, post_levels_white{0};        ///< input black/white
    std::atomic<int> post_grain{0};      ///< film grain, percent (SweetFX/TLOU look), 0 = off
    std::atomic<bool> post_mono{false};  ///< Rec.709 mono before the grain
    /// Advanced: 0 = start-time choice (BB_FPS_LIMIT / display cap), -1 = off, >0 FPS.
    std::atomic<int> fps_cap{0};
    /// GC: sync write-backs per pass (0 = unlimited) and a forced device budget in MiB
    /// (0 = the driver's live budget); hot-applied. BB_GC_DOWNLOADS_PER_PASS /
    /// BB_GC_BUDGET_MB seed these at start.
    std::atomic<int> gc_writeback{6};
    std::atomic<int> gc_budget_mb{0};
    /// Face-button swap bitmask (1 A/B, 2 X/Y, 3 both), hot-applied; BB_PAD_SWAP overrides.
    std::atomic<int> pad_swap{0};
    /// NGX DLSS model preset override: -1 driver default, 0 default, 10..13 = J..M.
    std::atomic<int> dlss_preset{-1};
    /// Live resolution and preset changes (run.sh): 0 off by default (startup patch, fastest
    /// on the Steam Deck and older GPUs), -1 auto (strong discrete GPUs), 1 on. On restart.
    std::atomic<int> live_resolution{0};
    /// Why FSR 4 cannot run (assets, device features), or null. Set by the renderer.
    std::atomic<const char*> fsr4_problem{nullptr};
    std::atomic<bool> fsr4_supported{false}, fsr411_supported{false}, dlss_supported{false};

    /// Startup settings for the explicit BB_RENDER_RES compatibility patch only.
    int startup_preset = NativeAA;
    int startup_upscaler = UpscalerFsr3;
    bool startup_object_motion = true;
    bool startup_effects[EffectCount]{};
    int startup_model_lod = 0;
    int startup_output_res = OutputDefault;
    int startup_live_resolution = 0;
};

Values& Get();

/// Directory holding the user-owned files, derived from BB_CONFIG (the ini's own directory,
/// trailing separator included). Falls back to the working directory when BB_CONFIG is unset.
/// Every module that keeps a file next to bbport.ini — the ImGui layout ini, the style
/// presets — resolves it through here so there is one definition of "the data directory".
std::string DataDir();

/// Reads the file, then the environment overrides. Called once at start.
void Load();
/// Checks the loaded choice before the first frame; unsupported FSR 4 or DLSS uses FSR 3.1.
void ConfigureUpscalerSupport(bool fsr4, bool fsr411, bool dlss);
/// Startup-patched scene dimensions cannot change until run.sh prepares a new image.
bool FixedRenderSession();
int RenderPreset();
bool ResolutionNeedsRestart();
/// Writes the file (menu changes).
void Save();

/// Render resolution divisor of a preset (1.0 native, 1.5 quality, ...).
float PresetScale(int preset);
const char* PresetName(int preset);
const char* UpscalerName(int upscaler);

// CollapsingHeader open/closed states, kept beside the other user-owned files rather than in
// Values: this is menu layout, not runtime state, and Values is handed out as a const reference
// by most readers, which a mutex member could not survive.
//
// ImGui itself does not persist these — they live in window->StateStorage and its ini writer
// only has handlers for windows and tables (imgui.cpp:4472, imgui_tables.cpp:4260), so every
// section reopened expanded on the next launch. They are stored as collapse_<label> keys in
// bbport.ini instead; unknown keys are ignored on load, so renaming a section drops its state.
bool CollapseOpen(const std::string& label, bool default_open);
/// Records the new state; true when it changed from the remembered one.
bool SetCollapseOpen(const std::string& label, bool open);
/// Drops a remembered state (the header is back at its default); true when one was dropped.
bool ForgetCollapse(const std::string& label);

} // namespace BbSettings
