// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: user settings changed at run time from the in-game menu (bbport_overlay.h) and kept
// in bbport.ini (BB_CONFIG overrides the path). Environment variables override the file at
// start. Readers load the atomics every frame; writers are the menu and Load().

#pragma once

#include <atomic>

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
    /// Present-path post chain (BbPost), hot-applied every frame. Each percent strength turns
    /// its effect off at 0, and the whole chain is bypassed when everything is 0.
    std::atomic<int> post_deband{50};    ///< f3kdb-style banding threshold strength, percent
    std::atomic<int> post_shadow{0};     ///< black-level lift, percent (100% = black at 0.30)
    std::atomic<int> post_sharpen{0};    ///< RCAS strength after upscaling, percent
    std::atomic<int> post_defog{0};      ///< uniform haze removal (black-point pull), percent
    std::atomic<int> post_contrast{0};   ///< contrast around mid-grey, percent (100% = x1.30)
    std::atomic<int> post_saturation{0}; ///< saturation over Rec.709 luma, percent (100% = x1.40)
    std::atomic<int> post_range{12};     ///< deband sample radius, pixels 4..32
    std::atomic<bool> post_split{false}; ///< debug: left half of pass 1 stays untouched
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

} // namespace BbSettings
