// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_overlay.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include <SDL3/SDL.h>
#include "bbport_menu_blur.h"
#include "bbport_settings.h"
#include "imgui.h"
#include "imgui_internal.h" // ImGuiContext::ErrorCallback
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

// DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license).
#ifdef _WIN32
// PE/COFF assemblers have no .hidden/.previous: the compiler embeds the file (#embed, a GCC
// extension in C++).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
alignas(16) static const unsigned char bb_font_ttf[] = {
#embed BB_FONT_PATH
};
#pragma GCC diagnostic pop
static const unsigned char* const bb_font_ttf_end = bb_font_ttf + sizeof(bb_font_ttf);
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_font_ttf\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".hidden bb_font_ttf_end\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".previous\n");
extern "C" const unsigned char bb_font_ttf[];
extern "C" const unsigned char bb_font_ttf_end[];
#endif

extern "C" void runtime_restart(void); // bb-probe (probe.c)
// Cheat engine bridge (src/runtime_cheats.c): GoldHEN / shadPS4 JSON files for this game,
// toggled from the menu below. The present thread is the only caller after boot.
extern "C" {
int bbcheats_file_count(void);
const char* bbcheats_file_name(int file);
const char* bbcheats_file_dir(void);
int bbcheats_mod_count(int file);
const char* bbcheats_mod_name(int file, int mod);
const char* bbcheats_mod_desc(int file, int mod);
int bbcheats_mod_enabled(int file, int mod);
int bbcheats_mod_one_way(int file, int mod);
const char* bbcheats_toggle(int file, int mod, int enable);
int bbcheats_master_available(int file);
const char* bbcheats_master_name(int file);
int bbcheats_master_enabled(int file);
const char* bbcheats_master_toggle(int file, int enable);
}

namespace BbOverlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
std::atomic<bool> initialized{false};
std::atomic<bool> menu_open{false};
bool l3_down = false, r3_down = false;
bool dirty = false; // settings changed while open: saved on close
float base_scale = 1.0f;
float menu_anim = 0.0f; // open ramp 0..1 (the close stays instant)

/// Smoothstep of the open ramp, shared by the window slide and the backdrop fade.
float MenuEase() {
    return menu_anim * menu_anim * (3.0f - 2.0f * menu_anim);
}

// The Vulkan backend: kept so a swapchain format change (HDR toggle) can rebuild it.
ImGui_ImplVulkan_InitInfo backend_info{};
VkFormat backend_format = VK_FORMAT_UNDEFINED; // points into PipelineRenderingCreateInfo

// Present rate for the FPS counter.
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f;
float frame_ms_max = 0.0f; // recent worst frame, decays so spikes age out
// Driver-measured frame latency (VK_NV_low_latency2); negative while unavailable.
std::atomic<float> latency_ms{-1.0f};

// Texture-cache GC telemetry fed by the presenter (the present thread both feeds and draws).
GcSnapshot gc_stats{};

const char* UpscalerLabel(int upscaler) {
    switch (upscaler) {
    case BbSettings::UpscalerFsr3: return "FSR 3.1";
    case BbSettings::UpscalerFsr4: return "FSR 4";
    case BbSettings::UpscalerFsr411: return "FSR 4.1.1";
    case BbSettings::UpscalerTaa: return "TAA";
    case BbSettings::UpscalerDlss: return "DLSS";
    default: return "";
    }
}

void SetOpen(bool value) {
    if (menu_open.exchange(value) == value) {
        return;
    }
    if (value) {
        menu_anim = 0.0f; // replay the open ramp on every open
    }
    ImGui::GetIO().MouseDrawCursor = value;
    if (!value) {
        ImGui::GetIO().ClearInputKeys(); // no nav keys stuck down across open/close cycles
        if (dirty) {
            dirty = false;
            BbSettings::Save();
        }
    }
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// Marks the settings dirty when a widget changed them.
template <typename T>
void Store(std::atomic<T>& target, T value, bool changed) {
    if (changed) {
        target = value;
        dirty = true;
    }
}

// The widget runs before Store reads v: argument evaluation order is unspecified (clang on
// Windows copied v before the widget changed it, so clicks stored the old value).
void Checkbox(const char* label, std::atomic<bool>& value) {
    bool v = value;
    const bool changed = ImGui::Checkbox(label, &v);
    Store(value, v, changed);
}

void Slider(const char* label, std::atomic<float>& value, float lo, float hi) {
    float v = value;
    const bool changed = ImGui::SliderFloat(label, &v, lo, hi, "%.2f");
    Store(value, v, changed);
}

void Hint(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

enum Page { PageGraphics, PageDisplay, PageEffects, PageCheats, PageAdvanced, PageCount };

const char* PageName(int page) {
    static const char* names[PageCount] = {"画面", "显示", "游戏效果", "作弊", "高级"};
    return names[page];
}

// Bottom-of-page notice for changes that wait for a restart, with the apply button.
void RestartNotice() {
    auto& s = BbSettings::Get();
    bool restart = s.object_motion != s.startup_object_motion ||
                   s.model_lod != s.startup_model_lod ||
                   s.live_resolution != s.startup_live_resolution ||
                   BbSettings::ResolutionNeedsRestart();
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        restart |= s.effects[e] != s.startup_effects[e];
    }
    if (!restart) {
        return;
    }
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "有更改将在重启游戏后生效");
    if (ImGui::Button("应用并重启游戏")) {
        BbSettings::Save();
        runtime_restart();
    }
}

void GraphicsPage() {
    auto& s = BbSettings::Get();
    const bool taa = s.upscaler == BbSettings::UpscalerTaa;
    static const char* upscalers[] = {"关闭", "FSR 3.1", "FSR 4 (INT8)", "FSR 4.1.1 (INT8)",
                                     "TAA（原生抗锯齿）", "DLSS（NVIDIA）"};
    static const char* later[] = {"XeSS"};
    int upscaler = s.upscaler;
    if (ImGui::BeginCombo("超分算法", upscalers[upscaler])) {
        for (int i = 0; i < BbSettings::UpscalerCount; ++i) {
            const bool supported = i == BbSettings::UpscalerFsr4 ? s.fsr4_supported.load()
                : i == BbSettings::UpscalerFsr411 ? s.fsr411_supported.load()
                : i == BbSettings::UpscalerDlss ? s.dlss_supported.load() : true;
            ImGui::BeginDisabled(!supported);
            if (ImGui::Selectable(upscalers[i], i == upscaler)) {
                Store(s.upscaler, i, true);
            }
            ImGui::EndDisabled();
            if (!supported) {
                ImGui::SameLine();
                ImGui::TextDisabled("— 此 GPU 不支持");
            }
        }
        for (const char* name : later) {
            ImGui::BeginDisabled();
            ImGui::Selectable(name, false);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("— 开发中");
        }
        ImGui::EndCombo();
    }
    if (const char* problem = s.fsr4_problem.load()) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "超分不可用：%s", problem);
        if (!BbSettings::IsFsr4(s.upscaler))
            ImGui::TextUnformatted("当前生效的仍是上方所选模式。可重新选择一次以重试。");
        ImGui::PopTextWrapPos();
    }
    if (BbSettings::IsFsr4(s.upscaler)) {
        if (ImGui::CollapsingHeader("FSR 4 选项", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (s.upscaler == BbSettings::UpscalerFsr411) {
                Hint("FSR 4.1.1 的 INT8 模式：模型来自 AMD 4.1.1 DLL，在 Vulkan 上重放（输出与 "
                     "DLL 一致）。Native..Performance 共用一个模型，Ultra Performance 单独一个。"
                     "资源生成：tools/fsr4cap/build_assets.sh（需要 DLL 和 Proton）。");
            } else {
                Hint("FSR 4 的 INT8 模式（模型 v07，来自 AMD FidelityFX SDK 源码）。画质优于 "
                 "FSR 3.1，但开销更大。切换画质档位会重建模型（短暂停顿）。资源："
                 "tools/fetch_fsr4_assets.sh。");
            }
            Checkbox("自动曝光", s.fsr4_auto_exposure);
            Checkbox("反转抖动符号", s.fsr4_invert_jitter);
            Hint("用于检查拖影：FSR 4 网络按曝光归一化颜色，并据此决定何时丢弃过去的帧。"
                 "立即生效，无需重启。");
        }
    }
    if (s.upscaler == BbSettings::UpscalerDlss) {
        if (ImGui::CollapsingHeader("DLSS 选项", ImGuiTreeNodeFlags_DefaultOpen)) {
            static constexpr struct {
                int value;
                const char* name;
            } presets[] = {
                {-1, "驱动默认"},
                {0, "默认（0）"},
                {10, "Preset J — 首代 Transformer"},
                {11, "Preset K — 改良版 Transformer"},
                {12, "Preset L — 4.5 二代（超高效能优化）"},
                {13, "Preset M — 4.5 二代（性能优化）"},
            };
            const int cur = s.dlss_preset.load();
            char cur_label[64];
            const char* found = nullptr;
            for (const auto& p : presets) {
                if (p.value == cur) found = p.name;
            }
            if (found) {
                std::snprintf(cur_label, sizeof(cur_label), "%s", found);
            } else {
                std::snprintf(cur_label, sizeof(cur_label), "自定义 (%d)", cur);
            }
            if (ImGui::BeginCombo("模型预设", cur_label)) {
                for (const auto& p : presets) {
                    if (ImGui::Selectable(p.name, p.value == cur)) {
                        Store(s.dlss_preset, p.value, true);
                    }
                }
                ImGui::EndCombo();
            }
            Hint("覆盖驱动推送的 DLSS 模型。M/L 是 DLSS 4.5 的第二代 Transformer（2026 年起"
                 "的驱动随带，性能档收益最大）；K 是 DLSS 4。切换会短暂重建模型，立即生效。");
        }
    }
    const bool upscaler_on = s.upscaler != BbSettings::UpscalerOff;
    ImGui::BeginDisabled(!upscaler_on);
    ImGui::BeginDisabled(taa);
    int preset = taa ? BbSettings::NativeAA : s.preset.load();
    char preset_label[64];
    std::snprintf(preset_label, sizeof(preset_label), "%s (x%.1f)", BbSettings::PresetName(preset),
                  BbSettings::PresetScale(preset));
    if (ImGui::BeginCombo("画质档位", preset_label)) {
        for (int i = 0; i < BbSettings::PresetCount; ++i) {
            char label[64];
            const float scale = BbSettings::PresetScale(i);
            const int output = s.output_res;
            std::snprintf(label, sizeof(label), "%s (x%.1f, 渲染 %dx%d)",
                          BbSettings::PresetName(i), scale,
                          int(std::lround(BbSettings::OutputWidths[output] / scale / 2) * 2),
                          int(std::lround(BbSettings::OutputHeights[output] / scale / 2) * 2));
            if (ImGui::Selectable(label, i == preset)) {
                Store(s.preset, i, true);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (taa) {
        ImGui::TextWrapped("TAA 在输出分辨率下对场景抗锯齿，不做超分、不用 FSR 模型。"
                           "重新选择超分算法后恢复已保存的画质档位。");
    }
    ImGui::Text("当前场景渲染：%d x %d", s.active_render_width.load(),
                s.active_render_height.load());
    if (BbSettings::FixedRenderSession()) {
        ImGui::Text("启动时档位：%s", BbSettings::PresetName(s.startup_preset));
        if (const char* automatic = std::getenv("BB_AUTO_RENDER_RES");
            automatic && automatic[0] == '1') {
            Hint("输出非 1080p 时整个游戏以档位分辨率渲染（启动时补丁）：在 Steam Deck 和较弱 "
                 "GPU 上最快。档位与输出的更改在重启后生效。下方「运行中改分辨率」可以免重启 "
                 "更改（此时后处理仍为 1080p，较慢）。");
        } else {
            Hint("BB_RENDER_RES 在启动时固定场景大小。去掉这个变量即可免重启更改分辨率和档位。");
        }
    } else {
        Hint("原生抗锯齿：超分算法只作抗锯齿用。其他档位按输出分辨率等比降低场景渲染分辨率。"
             "UI 以输出分辨率绘制。档位从下一帧起生效，无需重启游戏。");
    }
    if (ImGui::CollapsingHeader("超分增强", ImGuiTreeNodeFlags_DefaultOpen)) {
        Checkbox("锐化（RCAS）", s.sharpen);
        ImGui::BeginDisabled(!s.sharpen);
        Slider("锐化强度", s.sharpness, 0.0f, 2.0f);
        Hint("1 以内：超分自带的锐化（RCAS）。1 以上会追加一次 RCAS。DLSS 自身无锐化："
             "由 RCAS 一并完成。Ctrl+点击滑杆可输入精确值。");
        ImGui::EndDisabled();
        Checkbox("亚像素抖动", s.jitter);
        Hint("每帧场景偏移不到一个像素，超分从多帧聚合更多细节。关闭后只剩基于历史帧的抗锯齿。");
    }

    if (ImGui::CollapsingHeader("响应式遮罩与运动矢量")) {
        ImGui::BeginDisabled(taa);
        Checkbox("启用遮罩", s.reactive);
        Hint("标记透明特效（粒子、雾霭），让超分少依赖过去的帧。特效后的拖影减少，"
             "但特效下方会重新出现闪烁。");
        ImGui::BeginDisabled(!s.reactive);
        Slider("强度", s.reactive_scale, 0.0f, 4.0f);
        Slider("阈值", s.reactive_threshold, 0.0f, 1.0f);
        Slider("上限", s.reactive_max, 0.0f, 1.0f);
        bool show_mask = s.debug_view == BbSettings::DebugReactive;
        if (ImGui::Checkbox("显示遮罩（调试）", &show_mask)) {
            s.debug_view = show_mask ? BbSettings::DebugReactive : BbSettings::DebugNone;
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        Checkbox("角色运动矢量", s.object_motion);
        Hint("为动画物体计算精确矢量：衣物和武器运动中更不容易碎裂。静态场景没有额外开销。"
             "更改在重启游戏后生效。");
        bool show_motion = s.debug_view == BbSettings::DebugMotion;
        if (ImGui::Checkbox("显示运动矢量（调试）", &show_motion)) {
            s.debug_view = show_motion ? BbSettings::DebugMotion : BbSettings::DebugNone;
        }
        Hint("红/绿：水平/垂直运动（8 像素 = 全亮度）。蓝：该像素拿到了精确的物体矢量，"
             "而不只是相机运动。运动物体既没有蓝也没有红/绿时会被超分当作静止，"
             "因此出现拖影。");
    }
    ImGui::EndDisabled(); // upscaler off
    RestartNotice();
}

void DisplayPage() {
    auto& s = BbSettings::Get();
    static const char* display_modes[] = {"窗口", "无边框全屏", "独占全屏"};
    int display_mode = s.display_mode;
    if (ImGui::BeginCombo("显示模式", display_modes[display_mode])) {
        for (int i = 0; i < BbSettings::DisplayModeCount; ++i) {
            if (ImGui::Selectable(display_modes[i], i == display_mode)) {
                Store(s.display_mode, i, true);
            }
        }
        ImGui::EndCombo();
    }
    Hint("F11 在窗口与无边框全屏间切换。更改立即生效。独占全屏按输出分辨率选择显示器的"
         "显示模式，可能多一点点性能，但切换更慢，Alt+Tab 会闪屏。");
    static const char* cursors[] = {"自动（全屏或后台时隐藏）", "前台时始终隐藏", "始终显示"};
    int cursor = s.hide_cursor;
    if (ImGui::BeginCombo("鼠标光标", cursors[cursor])) {
        for (int i = 0; i < 3; ++i) {
            if (ImGui::Selectable(cursors[i], i == cursor)) {
                Store(s.hide_cursor, i, true);
            }
        }
        ImGui::EndCombo();
    }
    Hint("自动：全屏、窗口失焦或菜单打开时隐藏系统光标（菜单用自己的软件光标）。");
    int display = s.display;
    {
        int count = 0;
        SDL_DisplayID* ids = SDL_GetDisplays(&count);
        char current[64];
        if (display < 0) {
            std::snprintf(current, sizeof(current), "默认（主显示器）");
        } else {
            std::snprintf(current, sizeof(current), "显示器 %d", display + 1);
        }
        if (ImGui::BeginCombo("显示器", current)) {
            if (ImGui::Selectable("默认（主显示器）", display < 0)) {
                Store(s.display, -1, true);
            }
            for (int i = 0; i < count; ++i) {
                char name[64];
                std::snprintf(name, sizeof(name), "显示器 %d", i + 1);
                if (ImGui::Selectable(name, display == i)) {
                    Store(s.display, i, true);
                }
            }
            ImGui::EndCombo();
        }
        SDL_free(ids);
    }
    Hint("窗口移到所选显示器的中央，立即生效。");

    if (ImGui::CollapsingHeader("输出分辨率##hdr", ImGuiTreeNodeFlags_DefaultOpen)) {
        static const char* outputs[] = {"1280 x 720", "1920 x 1080", "2560 x 1440", "3840 x 2160"};
        int output = s.output_res;
        if (ImGui::BeginCombo("输出分辨率", outputs[output])) {
            for (int i = 0; i < BbSettings::OutputCount; ++i) {
                if (ImGui::Selectable(outputs[i], i == output)) {
                    Store(s.output_res, i, true);
                }
            }
            ImGui::EndCombo();
        }
        if (BbSettings::FixedRenderSession()) {
            Hint("最终画面与 UI 的大小。档位决定场景相对输出的尺寸：4K 性能档 = 1920x1080。"
                 "重启游戏后生效。");
        } else {
            Hint("最终画面与 UI 的大小在下一个帧边界处生效。档位决定场景相对输出的尺寸："
                 "4K 性能档 = 1920x1080。更改大小会重置超分历史，可能有短暂停顿。");
        }
        static const char* live_modes[] = {"自动（按 GPU）", "关闭（更快）", "开启"};
        int live = s.live_resolution + 1;
        if (ImGui::BeginCombo("运行中改分辨率", live_modes[live])) {
            for (int i = 0; i < 3; ++i) {
                if (ImGui::Selectable(live_modes[i], i == live)) {
                    Store(s.live_resolution, i - 1, true);
                }
            }
            ImGui::EndCombo();
        }
        Hint("开启：输出分辨率与档位免重启更改，但游戏的后处理仍为 1080p —— 在 Steam Deck 和"
             "较旧的 GPU 上明显更慢。关闭：一切以档位分辨率渲染，更改需要重启。自动会对强力"
             "独显启用。重启游戏后生效。");
    }
    static const char* ui_sizes[] = {"小 (85%)", "标准 (100%)", "较大 (115%)", "大 (130%)",
                                     "特大 (150%)"};
    static constexpr int ui_steps[] = {85, 100, 115, 130, 150};
    int ui = 1;
    for (int i = 0; i < 5; ++i) {
        if (ui_steps[i] == s.ui_scale) ui = i;
    }
    if (ImGui::BeginCombo("界面缩放", ui_sizes[ui])) {
        for (int i = 0; i < 5; ++i) {
            if (ImGui::Selectable(ui_sizes[i], i == ui)) {
                Store(s.ui_scale, ui_steps[i], true);
            }
        }
        ImGui::EndCombo();
    }
    Hint("设置菜单和 FPS 计数的大小，在输出分辨率的自动缩放之上再乘一档。立即生效。");
    Checkbox("角落 FPS 计数", s.show_fps);
    {
        static const char* details[] = {"帧率", "帧率 + 毫秒", "帧率 + 毫秒 + 最差"};
        int d = s.fps_detail;
        const bool changed = ImGui::Combo("FPS 显示内容", &d, details, 3);
        Store(s.fps_detail, d, changed);
    }
    Hint("立即生效。毫秒是平滑后的平均帧时间，最差是近期最慢的一帧，随时间缓慢回落。");
    Checkbox("低延迟模式（Reflex）", s.low_latency);
    Hint("NVIDIA RTX 专属：驱动把渲染队列压到 present 之前、并按节拍放行下一帧，输入到画面的"
         "延迟可降低约一半；开启后 FPS 读数附带驱动测量的帧延迟。立即生效。");
    RestartNotice();
}

void EffectsPage() {
    auto& s = BbSettings::Get();
    static const char* lods[] = {"最高 (-2)", "游戏默认", "较低 (1)", "最低 (2)"};
    static constexpr int lod_values[] = {-2, 0, 1, 2};
    int lod_index = 1;
    for (int i = 0; i < 4; ++i) {
        if (lod_values[i] == s.model_lod) lod_index = i;
    }
    if (ImGui::BeginCombo("模型细节", lods[lod_index])) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Selectable(lods[i], i == lod_index)) {
                Store(s.model_lod, lod_values[i], true);
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::CollapsingHeader("后处理（去色带 / 调色 / 锐化）")) {
        {
            // One-click looks: they only move the colour-grade sliders below (shadow, contrast,
            // saturation, vibrance, CDL, levels, grain, mono); deband/defog/sharpen stay put.
            struct StylePreset {
                const char* name;
                int shadow, contrast, sat, vib;
                int lr, lg, lb, gr, gg, gb, sr, sg, sb, lbk, lwh, grain;
                bool mono;
            };
            static constexpr StylePreset presets[] = {
                {"原味", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, false},
                {"胶片印象", 10, 5, -8, 0, 4, 4, 8, 0, 0, 0, 0, 0, 0, 6, 0, 30, false},
                {"冷蓝夜曲", 6, 0, -15, 0, 0, 2, 10, -6, 0, 0, 2, 0, 6, 0, 0, 15, false},
                {"暖褐怀旧", 8, 6, -25, 10, 6, 3, -2, 0, 0, 0, 5, 2, -4, 0, 0, 20, false},
                {"黑白惊悚", 5, 15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 35, true},
            };
            int current = -1;
            for (int i = 0; i < 5; ++i) {
                const auto& p = presets[i];
                if (s.post_shadow == p.shadow && s.post_contrast == p.contrast &&
                    s.post_saturation == p.sat && s.post_vibrance == p.vib &&
                    s.post_lift_r == p.lr && s.post_lift_g == p.lg && s.post_lift_b == p.lb &&
                    s.post_gamma_r == p.gr && s.post_gamma_g == p.gg &&
                    s.post_gamma_b == p.gb && s.post_gain_r == p.sr &&
                    s.post_gain_g == p.sg && s.post_gain_b == p.sb &&
                    s.post_levels_black == p.lbk && s.post_levels_white == p.lwh &&
                    s.post_grain == p.grain && s.post_mono == p.mono) {
                    current = i;
                }
            }
            if (ImGui::BeginCombo("风格预设", current >= 0 ? presets[current].name : "自定义")) {
                for (int i = 0; i < 5; ++i) {
                    if (ImGui::Selectable(presets[i].name, i == current)) {
                        const auto& p = presets[i];
                        Store(s.post_shadow, p.shadow, true);
                        Store(s.post_contrast, p.contrast, true);
                        Store(s.post_saturation, p.sat, true);
                        Store(s.post_vibrance, p.vib, true);
                        Store(s.post_lift_r, p.lr, true);
                        Store(s.post_lift_g, p.lg, true);
                        Store(s.post_lift_b, p.lb, true);
                        Store(s.post_gamma_r, p.gr, true);
                        Store(s.post_gamma_g, p.gg, true);
                        Store(s.post_gamma_b, p.gb, true);
                        Store(s.post_gain_r, p.sr, true);
                        Store(s.post_gain_g, p.sg, true);
                        Store(s.post_gain_b, p.sb, true);
                        Store(s.post_levels_black, p.lbk, true);
                        Store(s.post_levels_white, p.lwh, true);
                        Store(s.post_grain, p.grain, true);
                        Store(s.post_mono, p.mono, true);
                    }
                }
                ImGui::EndCombo();
            }
            Hint("一键套用调色：只动下面的调色滑杆（暗部、对比、饱和、智能饱和、三级调色、黑白场、"
                 "颗粒、单色），去色带、去雾与锐化不受影响；滑杆改动后显示为自定义。");
        }
        {
            int v = s.post_deband;
            Hint("去色带：天空、雾与暗部的 8-bit 分层感。mpv/libplacebo 同款算法，0 关闭，"
                 "低档最接近 mpv 默认。全部参数立即生效。");
            const bool changed = ImGui::SliderInt("去色带强度", &v, 0, 100, "%d%%");
            Store(s.post_deband, v, changed);
        }
        {
            int v = s.post_range;
            Hint("去色带采样半径。链在放大之后运行，条带被拉宽：约 2 倍放大建议 16-24；"
                 "大半径平滑更长渐变，小半径更保守。");
            const bool changed = ImGui::SliderInt("去色带半径（像素）", &v, 4, 32);
            Store(s.post_range, v, changed);
        }
        {
            int v = s.post_defog;
            Hint("去雾：减去一层均匀灰雾（100% 相当于 1/4 白的黑位下拉）。对实机远景的"
                 "灰蒙感有效；游戏内的体积浓雾是美术设计，过度去雾会伤氛围。");
            const bool changed = ImGui::SliderInt("去雾强度", &v, 0, 100, "%d%%");
            Store(s.post_defog, v, changed);
        }
        {
            int v = s.post_shadow;
            Hint("暗部提升：近黑区域按乘法曲线提亮（100% 时最亮约 1.3 倍），黑点保持纯黑，"
                 "不雾化黑位。");
            const bool changed = ImGui::SliderInt("暗部提升", &v, 0, 100, "%d%%");
            Store(s.post_shadow, v, changed);
        }
        {
            int v = s.post_contrast;
            Hint("对比度：绕中间灰拉伸明暗（100% 约为 1.3 倍），中间灰与黑点均不动，"
                 "0 不生效。先去雾再拉伸。");
            const bool changed = ImGui::SliderInt("对比度", &v, 0, 100, "%d%%");
            Store(s.post_contrast, v, changed);
        }
        {
            int v = s.post_saturation;
            Hint("饱和度：离开 Rec.709 亮度轴调整彩度（100% 约为 1.4 倍，-100% 约 0.6 倍），"
                 "0 不生效。游戏本身偏浓艳，建议从 10 到 30 起步，负值做低饱和的胶片感。");
            const bool changed = ImGui::SliderInt("饱和度", &v, -100, 100, "%d%%");
            Store(s.post_saturation, v, changed);
        }
        {
            int v = s.post_sharpen;
            Hint("锐化：AMD RCAS（FSR 附带的锐化内核），与画面页超分自带的锐化独立，通常二选一。");
            const bool changed = ImGui::SliderInt("锐化强度", &v, 0, 100, "%d%%");
            Store(s.post_sharpen, v, changed);
        }
        Checkbox("分割对比（左半原帧）", s.post_split);
        Hint("排查用：左半屏保留未处理的原始画面，右半屏走后处理链，用于逐项核对"
             "色彩与算法偏差。");
        if (ImGui::CollapsingHeader("智能饱和（Vibrance）")) {
            int v = s.post_vibrance;
            Hint("ReShade Vibrance 同款：沿亮度轴拉高彩度，越不饱和的像素加得越多，肤色附近"
                 "有保护罩，防止人脸过饱和。0 关闭。");
            const bool changed = ImGui::SliderInt("智能饱和", &v, 0, 100, "%d%%");
            Store(s.post_vibrance, v, changed);
        }
        if (ImGui::CollapsingHeader("三级调色（Lift / Gamma / Gain）")) {
            Hint("ASC CDL（ReShade Lift_Gamma_Gain 同源数学）：增益乘高光、提升垫阴影"
                 "（100% = ±0.20 黑位）、伽马幂调中间调（+ 变亮）。各通道独立可做分离"
                 "色调，0 为中性。");
            {
                int r = s.post_lift_r, g = s.post_lift_g, b = s.post_lift_b;
                const bool cr = ImGui::SliderInt("提升 R", &r, -100, 100);
                const bool cg = ImGui::SliderInt("提升 G", &g, -100, 100);
                const bool cb = ImGui::SliderInt("提升 B", &b, -100, 100);
                Store(s.post_lift_r, r, cr);
                Store(s.post_lift_g, g, cg);
                Store(s.post_lift_b, b, cb);
            }
            {
                int r = s.post_gamma_r, g = s.post_gamma_g, b = s.post_gamma_b;
                const bool cr = ImGui::SliderInt("伽马 R", &r, -100, 100);
                const bool cg = ImGui::SliderInt("伽马 G", &g, -100, 100);
                const bool cb = ImGui::SliderInt("伽马 B", &b, -100, 100);
                Store(s.post_gamma_r, r, cr);
                Store(s.post_gamma_g, g, cg);
                Store(s.post_gamma_b, b, cb);
            }
            {
                int r = s.post_gain_r, g = s.post_gain_g, b = s.post_gain_b;
                const bool cr = ImGui::SliderInt("增益 R", &r, -100, 100);
                const bool cg = ImGui::SliderInt("增益 G", &g, -100, 100);
                const bool cb = ImGui::SliderInt("增益 B", &b, -100, 100);
                Store(s.post_gain_r, r, cr);
                Store(s.post_gain_g, g, cg);
                Store(s.post_gain_b, b, cb);
            }
        }
        if (ImGui::CollapsingHeader("黑白场（Levels）")) {
            int v = s.post_levels_black;
            Hint("输入黑点：低于该亮度的像素压到纯黑，100% 约为 1/2 亮度。收紧黑位、"
                 "提对比的第一手段，0 不动。");
            const bool changed = ImGui::SliderInt("黑场", &v, 0, 100, "%d%%");
            Store(s.post_levels_black, v, changed);
            v = s.post_levels_white;
            Hint("输入白点：高于该亮度的像素推到纯白，100% 约为 1/2 亮度。黑场白场共同决定"
                 "输入范围，收得过窄会硬切画面细节。");
            const bool changed2 = ImGui::SliderInt("白场", &v, 0, 100, "%d%%");
            Store(s.post_levels_white, v, changed2);
        }
        {
            int v = s.post_grain;
            Hint("胶片颗粒：乘性高斯噪声（SweetFX FilmGrain 同源数学），暗部更重、黑点不动，"
                 "最后生还者式的低光颗粒感。与去色带的抖动共用随机流，不额外采样。");
            const bool changed = ImGui::SliderInt("胶片颗粒", &v, 0, 100, "%d%%");
            Store(s.post_grain, v, changed);
        }
        Checkbox("单色（黑白）", s.post_mono);
        Hint("按 Rec.709 亮度去色，配合颗粒与对比即高对比黑白胶片。");
    }
    if (ImGui::CollapsingHeader("游戏效果开关", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (int e = 0; e < BbSettings::EffectCount; ++e) {
            Checkbox(BbSettings::Effects[e].label, s.effects[e]);
        }
        Hint("效果由启动时的游戏补丁开关（patches/Bloodborne.xml）。运动模糊和动态光源阴影占用 "
             "可观的 GPU 时间。");
        Hint("自由视角：按住叉键再按 L3（键盘：Space + Z）。调试菜单：左侧触控板 / Tab。"
             "需要 Nexus mod #253 的 DbgFont14h.ccm 和 DbgFont14h.tpf 放入 dvdroot_ps4/font。"
             "右侧触控板：退格。");
    }
    RestartNotice();
}

void AdvancedPage() {
    auto& s = BbSettings::Get();
    // Frame rate cap: 0 the start-time choice, -1 off, >0 a fixed FPS.
    static const char* caps[] = {"自动（启动配置）", "不限", "30", "45", "60", "90", "120", "144",
                                 "165"};
    static constexpr int cap_values[] = {0, -1, 30, 45, 60, 90, 120, 144, 165};
    const int cap = s.fps_cap.load();
    char cap_label[64];
    const char* found = nullptr;
    int cap_index = 0;
    for (int i = 0; i < 9; ++i) {
        if (cap_values[i] == cap) {
            found = caps[i];
            cap_index = i;
        }
    }
    if (found) {
        std::snprintf(cap_label, sizeof(cap_label), "%s", found);
    } else {
        std::snprintf(cap_label, sizeof(cap_label), "自定义 (%d)", cap);
    }
    if (ImGui::BeginCombo("帧率上限", cap_label)) {
        for (int i = 0; i < 9; ++i) {
            if (ImGui::Selectable(caps[i], i == cap_index)) {
                Store(s.fps_cap, cap_values[i], true);
            }
        }
        ImGui::EndCombo();
    }
    Hint("自动沿用启动配置（BB_FPS_LIMIT，或显示器刷新率与 120 的较小值，因为更高时游戏的"
         "移动计时会走慢）。高于游戏内部节拍的上限以游戏节拍为准。立即生效。");
    {
        int v = s.gc_writeback.load();
        const bool changed =
            ImGui::SliderInt("GC 每轮写回上限", &v, 0, 64, v ? "%d 次" : "0（不限）");
        Store(s.gc_writeback, v, changed);
    }
    Hint("显存吃紧时每轮垃圾回收允许的同步写回（纹理落回 CPU）次数，默认 6，接近临界时会自动"
         "放宽到 4 倍。调高减少卡顿、抬高显存峰值，0 完全不限制。立即生效。");
    {
        int v = s.gc_budget_mb.load();
        const bool changed =
            ImGui::SliderInt("GC 显存预算 (MiB)", &v, 0, 16384, v ? "%d MiB" : "0（自动）");
        Store(s.gc_budget_mb, v, changed);
    }
    Hint("显存使用超过预算的 70% 开始回收、85% 加压、95% 激进。0 按驱动实时预算（集成 GPU 恒"
         "为驱动值）。立即生效。");
    static const char* swaps[] = {"关闭", "A/B（叉 ↔ 圆）", "X/Y（方 ↔ 三角）", "A/B 与 X/Y"};
    const int swap = s.pad_swap.load();
    if (ImGui::BeginCombo("手柄按键交换", swaps[swap])) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Selectable(swaps[i], i == swap)) {
                Store(s.pad_swap, i, true);
            }
        }
        ImGui::EndCombo();
    }
    Hint("任天堂布局手柄选最后一项，立即生效。BB_PAD_SWAP 可在启动时预设同样的值。");
    if (ImGui::CollapsingHeader("资源调度（纹理缓存）")) {
        ImGui::Text("显存用量 %llu MiB（加压线 %llu / 临界线 %llu MiB）",
                    (unsigned long long)(gc_stats.used_memory >> 20),
                    (unsigned long long)(gc_stats.pressure_memory >> 20),
                    (unsigned long long)(gc_stats.critical_memory >> 20));
        ImGui::Text("上轮回收：逐出 %llu 张，写回 %llu 张",
                    (unsigned long long)gc_stats.evictions,
                    (unsigned long long)gc_stats.downloads);
        Hint("显存用量越过预算 70% 开始回收，85% 加压、95% 激进；写回指 GPU 改过的纹理在"
             "逐出前落回 CPU 内存。计数自上次压力报告起累计，约 5 秒一轮。");
    }
}

void CheatsPage() {
    static const char* cheat_error = nullptr;
    if (const int cheat_files = bbcheats_file_count()) {
        for (int f = 0; f < cheat_files; ++f) {
            ImGui::PushID(f);
            if (ImGui::CollapsingHeader(bbcheats_file_name(f),
                                        f == 0 && cheat_files == 1 ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
                if (bbcheats_master_available(f)) {
                    bool master = bbcheats_master_enabled(f);
                    ImGui::BeginDisabled(master); // the master section has no off bytes
                    if (ImGui::Checkbox(bbcheats_master_name(f), &master)) {
                        cheat_error = bbcheats_master_toggle(f, master);
                    }
                    ImGui::EndDisabled();
                    if (master) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("重启后还原");
                    }
                }
                for (int m = 0; m < bbcheats_mod_count(f); ++m) {
                    const bool one_way = bbcheats_mod_one_way(f, m);
                    bool on = bbcheats_mod_enabled(f, m);
                    ImGui::PushID(m);
                    ImGui::BeginDisabled(one_way && on);
                    if (ImGui::Checkbox(bbcheats_mod_name(f, m), &on)) {
                        cheat_error = bbcheats_toggle(f, m, on);
                    }
                    ImGui::EndDisabled();
                    if (one_way) {
                        ImGui::SameLine();
                        ImGui::TextDisabled(on ? "单向（重启还原）" : "单向");
                    }
                    if (const char* d = bbcheats_mod_desc(f, m); d && d[0]) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s", d);
                    }
                    ImGui::PopID();
                }
            }
            ImGui::PopID();
        }
        if (cheat_error) {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s", cheat_error);
        }
        Hint("作弊是 GoldHEN / shadPS4 的 JSON 文件，按游戏序列号和版本放在 bbport.ini 旁的 "
             "\"cheats\" 目录。开关会把文件里的字节补丁写进游戏镜像；单向代码在重启前保持。"
             "作弊文件是数据：每个写入在生效前都会对照游戏镜像校验。");
    } else {
        ImGui::TextDisabled("在 %s 中没有此游戏版本的作弊文件", bbcheats_file_dir());
    }
}

void Menu() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 work = viewport->WorkSize;
    const ImVec2 base(viewport->WorkPos.x + 40.0f * base_scale,
                      viewport->WorkPos.y + 40.0f * base_scale);
    if (menu_anim < 1.0f) {
        // Open ramp: the window slides up into place (the close stays instant).
        const float e = MenuEase();
        ImGui::SetNextWindowPos(ImVec2(base.x, base.y + (1.0f - e) * 24.0f * base_scale),
                                ImGuiCond_Always);
    } else {
        ImGui::SetNextWindowPos(base, ImGuiCond_Appearing);
    }
    const float width = std::min(700.0f * base_scale, work.x - 60.0f * base_scale);
    const float height = std::min(540.0f * base_scale, work.y - 60.0f * base_scale);
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Appearing);
    bool keep_open = true;
    if (!ImGui::Begin("血源诅咒 — 设置  (Insert / L3+R3)", &keep_open,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    ImGui::Text("%.0f FPS  (%.1f ms)", frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f,
                frame_ms_avg);
    ImGui::Spacing();

    static int page = 0;
    const float footer = ImGui::GetFrameHeightWithSpacing();
    if (ImGui::BeginTabBar("##pages")) {
        if (ImGui::BeginTabItem(PageName(PageGraphics))) {
            ImGui::BeginChild("##pg0", ImVec2(0.0f, -footer));
            GraphicsPage();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(PageName(PageDisplay))) {
            ImGui::BeginChild("##pg1", ImVec2(0.0f, -footer));
            DisplayPage();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(PageName(PageEffects))) {
            ImGui::BeginChild("##pg2", ImVec2(0.0f, -footer));
            EffectsPage();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(PageName(PageCheats))) {
            ImGui::BeginChild("##pg3", ImVec2(0.0f, -footer));
            CheatsPage();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(PageName(PageAdvanced))) {
            ImGui::BeginChild("##pg4", ImVec2(0.0f, -footer));
            AdvancedPage();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    if (ImGui::Button("关闭")) {
        keep_open = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("设置保存到 bbport.ini · Insert 或 Esc 关闭 · F11 切换全屏");
    ImGui::End();
    if (!keep_open) {
        SetOpen(false);
    }
}

/// Frosted-glass backdrop: the blurred frame across the whole display plus a slight dim,
/// both fading in with the menu ramp. Runs inside the locked frame, so the lazy texture
/// creation in BbMenuBlur::Texture is safe here.
void DrawBackdrop() {
    const ImTextureID bg = BbMenuBlur::Texture();
    if (!bg) {
        return;
    }
    const float e = MenuEase();
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    ImDrawList* const dl = ImGui::GetBackgroundDrawList();
    dl->AddImageRounded(bg, ImVec2(0.0f, 0.0f), size, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                        ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, 0.90f * e)), 0.0f);
    dl->AddRectFilled(ImVec2(0.0f, 0.0f), size,
                      ImGui::ColorConvertFloat4ToU32(ImVec4(0.0f, 0.0f, 0.0f, 0.28f * e)));
}

void FpsCounter() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad,
                                   viewport->WorkPos.y + pad),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    const auto& s = BbSettings::Get();
    const char* mode = UpscalerLabel(s.upscaler);
    const int detail = s.fps_detail.load();
    // Driver-measured latency (low latency mode on); gate on the mode so the last reading
    // never lingers after a toggle-off or a driver failure.
    const float lat = s.low_latency.load() ? latency_ms.load() : -1.0f;
    if (detail == 0) {
        ImGui::Text("%.0f FPS  %s", frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f, mode);
    } else if (detail == 1) {
        if (lat >= 0.0f) {
            ImGui::Text("%.0f FPS  %.1f ms  延迟 %.1f ms  %s",
                        frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f, frame_ms_avg, lat,
                        mode);
        } else {
            ImGui::Text("%.0f FPS  %.1f ms  %s",
                        frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f, frame_ms_avg, mode);
        }
    } else if (lat >= 0.0f) {
        ImGui::Text("%.0f FPS  %.1f ms  worst %.1f ms  延迟 %.1f ms  %s",
                    frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f, frame_ms_avg, frame_ms_max,
                    lat, mode);
    } else {
        ImGui::Text("%.0f FPS  %.1f ms  worst %.1f ms  %s",
                    frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f, frame_ms_avg, frame_ms_max,
                    mode);
    }
    ImGui::End();
}

} // namespace

void SetLatencyMs(float ms) {
    latency_ms = ms;
}

void SetGcStats(const GcSnapshot& stats) {
    gc_stats = stats;
}

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "bbport";
    // Recoverable errors: log and keep running (NDEBUG builds silence IM_ASSERT entirely).
    io.ConfigErrorRecoveryEnableAssert = false;
    io.ConfigErrorRecoveryEnableTooltip = false;
    ImGui::GetCurrentContext()->ErrorCallback = [](ImGuiContext*, void*, const char* msg) {
        std::printf("Overlay: ImGui error: %s\n", msg);
    };

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    // Glass-adjacent look: rounder corners, breathing room, a quiet amber accent.
    style.WindowRounding = 10.0f;
    style.ChildRounding = 8.0f;
    style.PopupRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.TabRounding = 6.0f;
    style.ScrollbarRounding = 8.0f;
    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.FramePadding = ImVec2(9.0f, 5.0f);
    style.ItemSpacing = ImVec2(10.0f, 7.0f);
    style.ItemInnerSpacing = ImVec2(7.0f, 4.0f);
    style.ScrollbarSize = 13.0f;
    style.WindowBorderSize = 1.0f;
    style.WindowMenuButtonPosition = ImGuiDir_None;
    ImVec4* const c = style.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.085f, 0.085f, 0.095f, 0.86f);
    c[ImGuiCol_PopupBg] = ImVec4(0.085f, 0.085f, 0.095f, 0.94f);
    c[ImGuiCol_Border] = ImVec4(1.0f, 1.0f, 1.0f, 0.09f);
    c[ImGuiCol_TitleBg] = ImVec4(0.070f, 0.070f, 0.080f, 1.0f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.115f, 0.115f, 0.130f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.165f, 0.165f, 0.185f, 0.62f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.225f, 0.225f, 0.250f, 0.70f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.285f, 0.285f, 0.320f, 0.80f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.560f, 0.570f, 0.610f, 1.0f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.890f, 0.660f, 0.280f, 1.0f);
    c[ImGuiCol_CheckMark] = ImVec4(0.890f, 0.660f, 0.280f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.200f, 0.200f, 0.230f, 0.85f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.400f, 0.305f, 0.135f, 0.95f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.480f, 0.370f, 0.165f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.520f, 0.520f, 0.545f, 1.0f);

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_ttf),
                                   int(bb_font_ttf_end - bb_font_ttf), 18.0f, &font_config);
#ifdef _WIN32
    // The embedded face has no CJK glyphs: merge a system CJK face for the Chinese menu.
    bool cjk_merged = false;
    for (const char* path :
         {"C:\\Windows\\Fonts\\msyh.ttc", "C:\\Windows\\Fonts\\simhei.ttf",
          "C:\\Windows\\Fonts\\Deng.ttf"}) {
        ImFontConfig cjk_config;
        cjk_config.MergeMode = true;
        if (io.Fonts->AddFontFromFileTTF(path, 17.0f, &cjk_config)) {
            cjk_merged = true;
            break;
        }
    }
    if (!cjk_merged) {
        std::printf("Overlay: no system CJK font, Chinese menu text will show boxes\n");
    }
#endif

    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                *static_cast<const vk::Instance*>(user), name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    backend_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    info.DescriptorPoolSize = 16;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &backend_format,
    };
    info.CheckVkResultFn = [](VkResult err) {
        if (err != VK_SUCCESS) {
            std::printf("Overlay: ImGui Vulkan backend error %d\n", static_cast<int>(err));
        }
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    backend_info = info;
    initialized = true;
    std::printf("Overlay: menu ready (Insert or L3+R3)\n");
}

void OnFormatChange(vk::Format format) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized || static_cast<VkFormat>(format) == backend_format) {
        return;
    }
    // The device was idled by Swapchain::SetHDR before this: no backend work is in flight.
    ImGui_ImplVulkan_Shutdown();
    backend_format = static_cast<VkFormat>(format);
    if (!ImGui_ImplVulkan_Init(&backend_info)) {
        initialized = false;
        ImGui::DestroyContext();
        std::printf("Overlay: ImGui Vulkan backend rebuild failed\n");
        return;
    }
    BbMenuBlur::OnBackendReset(); // the backend's descriptor pool was destroyed
}

void UpdateTextInput(SDL_Window* window) {
    bool want = false;
    {
        std::scoped_lock lock{imgui_mutex};
        want = initialized && menu_open && ImGui::GetIO().WantTextInput;
    }
    if (want != SDL_TextInputActive(window)) {
        if (want) {
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    }
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool is_open = menu_open;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        if (down && !event.key.repeat &&
            (event.key.key == SDLK_INSERT || (is_open && event.key.key == SDLK_ESCAPE))) {
            SetOpen(event.key.key == SDLK_INSERT ? !is_open : false);
            return true;
        }
        if (!is_open) {
            return false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const u8 button = event.gbutton.button;
        if (button == SDL_GAMEPAD_BUTTON_LEFT_STICK) {
            l3_down = down;
        } else if (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
            r3_down = down;
        }
        if (down && l3_down && r3_down) {
            SetOpen(!is_open);
            return true;
        }
        if (!is_open) {
            return false;
        }
        if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (!is_open) {
            return false;
        }
        // Left stick navigates: official imgui_impl_sdl3 semantics (8000 dead-zone,
        // dead-zone..full scale mapped to 0..1 analog, down at >0.1).
        if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX ||
            event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
            const float raw = static_cast<float>(event.gaxis.value);
            const auto analog = [](float axis, float v0, float v1) {
                return std::clamp((axis - v0) / (v1 - v0), 0.0f, 1.0f);
            };
            const bool x = event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX;
            const float a_neg = analog(raw, -8000.0f, -32768.0f);
            const float a_pos = analog(raw, 8000.0f, 32767.0f);
            io.AddKeyAnalogEvent(x ? ImGuiKey_GamepadLStickLeft : ImGuiKey_GamepadLStickUp,
                                 a_neg > 0.1f, a_neg);
            io.AddKeyAnalogEvent(x ? ImGuiKey_GamepadLStickRight : ImGuiKey_GamepadLStickDown,
                                 a_pos > 0.1f, a_pos);
        }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT: {
        // Typed characters (Ctrl+click on a slider, a text field): key events alone erase but
        // do not type. SDL sends them while text input is on (UpdateTextInput).
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!is_open) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!is_open) {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!is_open) {
            return false;
        }
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    default:
        return false;
    }
}

bool Visible() {
    return initialized && (menu_open || BbSettings::Get().show_fps);
}

/// Whether the frosted backdrop should be recorded this frame (the menu is open).
bool WantsBlur() {
    return menu_open;
}

bool CapturesInput() {
    return menu_open;
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    // Present interval for the FPS readout (measured also while nothing is drawn).
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
        frame_ms_max = ms > frame_ms_max ? ms : frame_ms_max * 0.97f;
    }
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1) and the menu's scale step.
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f) *
                        float(BbSettings::Get().ui_scale.load()) / 100.0f;
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }
    // Menu open ramp: ~0.14s in, instant out (the frame sharpens at once).
    if (menu_open) {
        menu_anim = std::min(menu_anim + io.DeltaTime * 7.0f, 1.0f);
    } else {
        menu_anim = 0.0f;
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (menu_open) {
        DrawBackdrop();
        Menu();
    }
    if (BbSettings::Get().show_fps && !menu_open) {
        FpsCounter();
    }
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

} // namespace BbOverlay
