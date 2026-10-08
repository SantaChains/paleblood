// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_overlay.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>
#include "bbport_menu_blur.h"
#include "bbport_settings.h"
#include "imgui.h"
#include "imgui_internal.h" // ImGuiContext::ErrorCallback
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // MoveFileExA: UCRT rename() fails with EEXIST on an existing target
#endif

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
/// The page whose selection was last forced through ImGuiTabItemFlags_SetSelected. -1 = the
/// next Menu() frame re-applies ui_page (start-up, or the first frame after reopening, when
/// the tab bar's own selection may no longer exist). See Menu() for why the flag is raised
/// only while this disagrees with the desired page.
int applied_page = -1;
bool l3_down = false, r3_down = false;
float base_scale = 1.0f;
float menu_anim = 0.0f; // open ramp 0..1 (the close stays instant)

/// Smoothstep of the open ramp, shared by the window slide and the backdrop fade.
float MenuEase() {
    return menu_anim * menu_anim * (3.0f - 2.0f * menu_anim);
}

// The Vulkan backend: kept so a swapchain format change (HDR toggle) can rebuild it.
ImGui_ImplVulkan_InitInfo backend_info{};
VkFormat backend_format = VK_FORMAT_UNDEFINED; // points into PipelineRenderingCreateInfo
u32 backend_image_count = 0; // the swapchain image count the backend's buffer ring was sized for

// Present rate for the FPS counter.
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f;
float frame_ms_max = 0.0f; // recent worst frame, decays so spikes age out
// Driver-measured frame latency (VK_NV_low_latency2); negative while unavailable.
std::atomic<float> latency_ms{-1.0f};

// Texture-cache GC telemetry fed by the presenter. The present thread both feeds and reads it
// today, but a 5-field struct cannot be read while another thread writes it: the numbers would
// be from different collections. One atomic per field makes every read a consistent-enough
// snapshot (a second-stale counter at worst) and lets the producer move to another thread.
struct AtomicGcStats {
    std::atomic<u64> used_memory{0}, pressure_memory{0}, critical_memory{0}, evictions{0},
        downloads{0};
};
AtomicGcStats gc_stats;

// Forward declarations for SetOpen(false)'s close-time flushes (definitions below).
extern bool g_custom_look_valid;
void SaveUserStyles();

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
        applied_page = -1; // re-apply ui_page once on the first frame: the tab bar's own
                           // selection may have been collected while the menu was closed
    }
    ImGui::GetIO().MouseDrawCursor = value;
    if (!value) {
        ImGui::GetIO().ClearInputKeys(); // no nav keys stuck down across open/close cycles
        // Both writes are synchronous disk I/O, and this runs with imgui_mutex held (the two
        // HandleEvent call sites lock it) — the present thread takes that lock every frame, so
        // doing the work inline stalls rendering for as long as the disk takes.
        //
        // The ini needs the ImGui context, so it can only be written while holding the lock;
        // BbSettings::Save() only reads atomics and has its own mutex. Both are therefore
        // handed to one detached thread that takes the lock itself, after the caller has
        // released it — trying to take it here would deadlock against the present thread,
        // which is waiting for this thread to leave the critical section.
        //
        // The heartbeat in Render() is what makes edits durable during a session; this close-time
        // flush stays as the final one, so the very last edit before the window goes away is
        // written too. BbSettings::Save() takes its own lock, so overlapping with a heartbeat
        // write is safe: they serialise, and the second one simply rewrites the same values.
        std::thread([] {
            std::scoped_lock lock{imgui_mutex};
            if (initialized) {
                // No fallback to io.IniFilename here: SaveIniSettingsToDisk(nullptr) returns
                // after zeroing the dirty timer (imgui.cpp:15852-15855), so the null form
                // would cancel the pending auto-save AND write nothing — edits made within
                // IniSavingRate of closing the menu would be lost. Pass the path explicitly.
                ImGui::SaveIniSettingsToDisk(ImGui::GetIO().IniFilename);
            }
        }).detach();
        std::thread([] { BbSettings::Save(); }).detach();
        // The custom-look snapshot may have changed since the last save (every manual edit
        // re-captures it). SaveUserStyles copies the containers under g_user_styles_mutex,
        // so the detached writer cannot race the menu's edits.
        if (g_custom_look_valid) {
            std::thread([] { SaveUserStyles(); }).detach();
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

// The ImGui ini, next to bbport.ini (BB_CONFIG) so the UI layout follows the data directory
// like every other user-owned file. Kept in a global because io.IniFilename is a bare const
// char* that ImGui only reads; the alternative (SaveIniSettingsToMemory on demand) would mean
// carrying the buffer across threads for no gain.
std::string UiIniPath() {
    return BbSettings::DataDir() + "bbport_ui.ini";
}

std::string g_ui_ini_path;

// Set whenever a widget actually changes a value. The menu does not write the files on every
// edit (a slider drag would rewrite the ini per frame); instead the flag makes Render() flush
// at most once per second, so a crash, a kill or a power loss costs at most the last second of
// edits instead of everything since the menu was opened.
std::atomic<bool> settings_dirty{false};

/// Set for the rest of the current menu page pass after any slider edit lands. The effects
/// page uses it to snapshot the "自定义" look once per frame instead of once per widget.
bool grade_edited_this_frame = false;

/// When the last auto-save ran, so a burst of edits collapses into one write per second.
std::chrono::steady_clock::time_point last_save{};

// Applies a widget's new value and remembers that something changed. Every menu edit goes
// through here or through an explicit Store(...) call, so this is the single place that has to
// mark the settings dirty.
template <typename T>
void Store(std::atomic<T>& target, T value, bool changed) {
    if (changed) {
        target = value;
        settings_dirty = true;
        grade_edited_this_frame = true;
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

/// Slider that knows its default: the label text turns to the accent colour while the value
/// differs from it, and right-click offers "恢复默认" (the pattern ReShade and SpecialK use so
/// a menu full of tweaks stays readable and every tweak stays reversible). The colour hint is
/// deliberately not a label prefix — changing the label would change the widget ID and break
/// an in-progress drag as the value crosses the default.
void SliderWithDefault(const char* label, std::atomic<int>& target, int def, int lo, int hi,
                       const char* fmt = "%d") {
    const bool modified = target.load() != def;
    if (modified) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.80f, 1.00f, 1.00f));
    }
    int v = target.load();
    const bool changed = ImGui::SliderInt(label, &v, lo, hi, fmt);
    if (modified) {
        ImGui::PopStyleColor();
    }
    Store(target, v, changed);
    if (ImGui::BeginPopupContextItem(label)) {
        if (ImGui::MenuItem("恢复默认")) {
            target = def;
            settings_dirty = true;
        }
        ImGui::EndPopup();
    }
}

/// Float overload: same interaction, "%.2f" by default like the plain slider above.
void SliderWithDefault(const char* label, std::atomic<float>& target, float def, float lo,
                       float hi, const char* fmt = "%.2f") {
    const bool modified = target.load() != def;
    if (modified) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.80f, 1.00f, 1.00f));
    }
    float v = target.load();
    const bool changed = ImGui::SliderFloat(label, &v, lo, hi, fmt);
    if (modified) {
        ImGui::PopStyleColor();
    }
    Store(target, v, changed);
    if (ImGui::BeginPopupContextItem(label)) {
        if (ImGui::MenuItem("恢复默认")) {
            target = def;
            settings_dirty = true;
        }
        ImGui::EndPopup();
    }
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

/// A CollapsingHeader whose open state survives restarts.
///
/// ImGui keeps these states in window->StateStorage, and its ini writer only has handlers for
/// windows and tables (imgui.cpp:4472 registers WindowSettingsHandler_WriteAll,
/// imgui_tables.cpp:4260 the table one); StateStorage appears nowhere near the save path, so
/// every section reopened expanded on the next launch no matter what the user did. Rather than
/// add a custom settings handler (internal API, and the state is per-section data we already
/// own), the states live in bbport.ini as collapse_<label> (BbSettings::CollapseOpen) and the
/// header is driven with SetNextItemOpen, which overrides ImGui's own stored value for this
/// frame. A section only gets a key while it is not at its default, so an untouched menu
/// writes no collapse_ lines at all.
bool CollapsingHeader(const char* label, bool default_open = false) {
    const bool remembered = BbSettings::CollapseOpen(label, default_open);
    ImGui::SetNextItemOpen(remembered, ImGuiCond_Always);
    const bool open = ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_None);
    if (open == default_open) {
        // Back at the default: drop the key rather than storing a redundant one.
        if (BbSettings::ForgetCollapse(label)) {
            settings_dirty = true;
        }
    } else if (BbSettings::SetCollapseOpen(label, open)) {
        settings_dirty = true;
    }
    return open;
}

enum Page { PageGraphics, PageDisplay, PageEffects, PageCheats, PageAdvanced, PageCount };
static_assert(PageCount == BbSettings::UiPageCount, "ui_page clamp must match the page list");

// ---------------------------------------------------------------------------
// Colour-grade style presets
//
// A preset is exactly the set of sliders a "look" moves: shadow, contrast, saturation,
// vibrance, CDL lift/gamma/gain, levels, grain and mono. Deband, defog and sharpen stay
// out of it — they are corrections, not grades, and are tuned per scene rather than per look.
// ---------------------------------------------------------------------------

struct StylePreset {
    const char* name;
    int shadow, contrast, sat, vib;
    int lr, lg, lb, gr, gg, gb, sr, sg, sb, lbk, lwh, grain;
    bool mono;
    /// "原味" only: also reset the correction sliders (deband, defog, sharpen, split, and
    /// the deband radius) so selecting it truly shows the unmodified game picture. The other
    /// presets leave corrections alone — they are tuned per scene, and a grade should not
    /// undo them.
    bool full_reset;
};

/// A preset with a heap-allocated name, so the five built-ins (string literals) and the
/// user slots (read from the save dialog) share one type. The correction tail carries the
/// "自定义" snapshot (the full filter state after the user's last manual edit); for built-in
/// and saved slots it stays at the neutral values, because those do not own corrections.
struct StyleSlot {
    std::string name;
    int shadow, contrast, sat, vib;
    int lr, lg, lb, gr, gg, gb, sr, sg, sb, lbk, lwh, grain;
    bool mono;
    bool full_reset;
    int deband = 0, defog = 0, sharpen = 0, range = 12;
    bool split = false;
};

StyleSlot MakeSlot(std::string name, const StylePreset& p) {
    return StyleSlot{std::move(name), p.shadow, p.contrast, p.sat,   p.vib, p.lr, p.lg,
                     p.lb,      p.gr,           p.gg,      p.gb,   p.sr, p.sg, p.sb,
                     p.lbk,     p.lwh,          p.grain,   p.mono, p.full_reset};
}

// Defined below; UserStyles() calls this on first use so the file is read once, lazily,
// the first time the combo is opened rather than during Init.
void LoadUserStyles();
void SaveUserStyles();
void DeleteUserStyle(int index);
void DrawSaveStylePopup();
bool StyleMatches(const StyleSlot& p, const BbSettings::Values& s);
void ApplyStyle(const StyleSlot& p, BbSettings::Values& s);
std::vector<StyleSlot>& StylePresets();
const std::vector<StyleSlot>& UserStyles();
// Built-in looks, themed on Yharnam (gothic Victorian, blood moon, candlelight, pale dream).
// Amplitudes follow professional grading practice (sfx.thelazy.net ReShade database, DaVinci
// split-toning guidance): per-channel lift/gain offsets stay within ~±0.10 of our ±0.20
// CDL range — "if you can see the wheel move, it's too much" — the gamma wheel counter-rotates
// against the gain wheel so midtones and faces stay put, and contrast sits in the 10..20
// band the ReShade presets actually use. The old "胶片印象/冷蓝夜曲/暖褐怀旧/黑白惊悚" set
// was replaced: those named moods but all graded in the same mild warm direction.
constexpr StylePreset kBuiltinStyles[] = {
    {"原味", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, false, true},
    // Blood moon: teal shadows, ember-orange highlights — the signature Bloodborne sky.
    // Gamma (−4, +3, −2) counter-rotates so midtones stay neutral.
    {"血月", 4, 12, 5, 18, -8, 0, 12, -4, 3, -2, 18, 4, -12, 4, 3, 12, false, false},
    // Paleblood dream: a faded cold print — lifted blacks (10), lowered saturation, all
    // channels slightly brightened through gamma, barely-there cyan shadows.
    {"苍白之梦", 10, 6, -30, 0, 0, 2, 8, 6, 6, 4, 6, 2, 0, 0, 0, 18, false, false},
    // Candlelit nocturne: deep crushed blacks (levels black 10), warm candle-orange
    // highlights, a whisper of warm brown in the shadows.
    {"烛光夜曲", 0, 18, 12, 8, 4, 2, -2, 2, 0, -4, 20, 10, -8, 10, 4, 15, false, false},
    // Silver gelatin print: mono must be last in the chain (it washes all tinting), so this
    // look is purely tonal — both black and white points pulled in, heavy grain.
    {"银盐", 6, 15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6, 6, 28, true, false},
};
constexpr int kBuiltinStyleCount = int(sizeof(kBuiltinStyles) / sizeof(kBuiltinStyles[0]));
/// First index of a user slot inside the combined list (StylePresets()).
constexpr int StyleUserBase = kBuiltinStyleCount;
constexpr int kMaxUserStyles = 32;

/// One-line intent per built-in, shown as the combo entry's tooltip. Index-aligned with
/// kBuiltinStyles; written for a player deciding by mood, not by slider names.
const char* kBuiltinStyleDesc(int index) {
    static constexpr const char* descs[kBuiltinStyleCount] = {
        "完全还原游戏原始画面，包括关闭去色带、去雾与锐化。",
        "血月：阴影沉入青蓝，高光燃起余烬橙红——Bloodborne 血月天空的走向，中间调保持中性。",
        "苍白之梦：褪色的冷调印刷——黑位垫起、饱和收回、整体微亮，梦境与研究楼的空气感。",
        "烛光夜曲：收紧黑位、高光染上烛火暖橙，Yharnam 街道夜战的伦勃朗式明暗。",
        "银盐：黑白胶片相纸——黑位白位同时收紧、高颗粒，影调质感代替色彩。",
    };
    return descs[std::clamp(index, 0, kBuiltinStyleCount - 1)];
}

/// User slots. std::string because the name comes from the popup's input field.
std::vector<StyleSlot> g_user_styles;
bool g_user_styles_loaded = false;
/// Set when a slot is added or removed, so StylePresets() rebuilds its cache.
bool g_style_list_dirty = false;
bool save_style_popup = false;
char save_style_name[64] = "";

/// The user's own look: the full filter state as it stood right after their last manual
/// slider edit, whenever that did not coincide with a preset. The combo shows a 自定义
/// entry while the live state matches no preset; picking it restores this snapshot.
/// Persisted in user-presets.json as "custom" so it survives restarts.
StyleSlot g_custom_look{};
bool g_custom_look_valid = false;
/// Guards g_user_styles/g_custom_look: the close-time save runs on a detached thread while
/// the menu thread may still edit the containers (reopen, save popup, delete, re-capture).
static std::mutex g_user_styles_mutex;

/// user-presets.json next to bbport.ini (the same data-directory convention as
/// cheats/state.txt and mods.json).
std::string UserStylePath() {
    return BbSettings::DataDir() + "user-presets.json";
}

const std::vector<StyleSlot>& UserStyles() {
    if (!g_user_styles_loaded) {
        g_user_styles_loaded = true;
        LoadUserStyles();
    }
    return g_user_styles;
}

/// Built-ins followed by the user's slots, cached: the combo is rebuilt every frame the menu
/// is open, and StylePresets() copies every name, so the result is memoised until a slot is
/// added or deleted (bump g_style_list_dirty).
std::vector<StyleSlot>& StylePresets() {
    static std::vector<StyleSlot> cache;
    static bool built = false;
    if (!built || g_style_list_dirty) {
        cache.clear();
        cache.reserve(kBuiltinStyleCount + UserStyles().size());
        for (const auto& b : kBuiltinStyles) {
            cache.push_back(MakeSlot(b.name, b));
        }
        for (const auto& u : UserStyles()) {
            cache.push_back(u);
        }
        built = true;
        g_style_list_dirty = false;
    }
    return cache;
}

// A deliberately small JSON subset: one flat object per preset, integers and one bool, no
// nesting and no escaping beyond \" and \\. A hand-edited or corrupt file must never take the
// game down, so every parse is bounds-checked and a failure just yields no user slots.
void LoadUserStyles() {
    std::FILE* f = std::fopen(UserStylePath().c_str(), "rb");
    if (!f) {
        return;
    }
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
        text.append(buf, n);
    }
    std::fclose(f);

    size_t pos = 0;
    const auto skip_ws = [&] {
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' ||
                                     text[pos] == '\r' || text[pos] == ',')) {
            ++pos;
        }
    };
    const auto read_string = [&](std::string& out) {
        skip_ws();
        if (pos >= text.size() || text[pos] != '"') {
            return false;
        }
        ++pos;
        out.clear();
        while (pos < text.size() && text[pos] != '"') {
            if (text[pos] == '\\' && pos + 1 < text.size()) {
                ++pos;
            }
            out.push_back(text[pos++]);
        }
        if (pos >= text.size()) {
            return false;
        }
        ++pos; // closing quote
        return true;
    };
    // Reads "key": <number|true|false> and returns the value as a string (numbers verbatim).
    const auto read_member = [&](std::string& key, std::string& value) {
        if (!read_string(key)) {
            return false;
        }
        skip_ws();
        if (pos < text.size() && text[pos] == ':') {
            ++pos;
        }
        skip_ws();
        if (pos >= text.size()) {
            return false;
        }
        value.clear();
        if (text[pos] == '"') {
            return read_string(value);
        }
        while (pos < text.size() && text[pos] != ',' && text[pos] != '}' && text[pos] != ' ' &&
               text[pos] != '\n' && text[pos] != '\r' && text[pos] != '\t') {
            value.push_back(text[pos++]);
        }
        return !value.empty();
    };
    const auto to_int = [](const std::string& v, int fallback) {
        try {
            return std::stoi(v);
        } catch (...) {
            return fallback;
        }
    };

    if (text.find('[') == std::string::npos) {
        return; // not our format
    }
    pos = text.find('[') + 1;
    for (int guard = 0; guard < 4096; ++guard) {
        skip_ws();
        if (pos >= text.size() || text[pos] == ']') {
            break;
        }
        if (text[pos] != '{') {
            break;
        }
        ++pos;
        StyleSlot slot{};
        slot.name = "未命名";
        // Defaults are the neutral grade; a missing member leaves it alone.
        slot.mono = false;
        while (true) {
            skip_ws();
            if (pos >= text.size() || text[pos] == '}') {
                ++pos;
                break;
            }
            std::string key, value;
            if (!read_member(key, value)) {
                return; // malformed: drop what we have, keep the built-ins
            }
            if (key == "name") {
                if (!value.empty()) {
                    slot.name = value;
                }
            } else if (key == "shadow") slot.shadow = to_int(value, 0);
            else if (key == "contrast") slot.contrast = to_int(value, 0);
            else if (key == "sat") slot.sat = to_int(value, 0);
            else if (key == "vib") slot.vib = to_int(value, 0);
            else if (key == "lr") slot.lr = to_int(value, 0);
            else if (key == "lg") slot.lg = to_int(value, 0);
            else if (key == "lb") slot.lb = to_int(value, 0);
            else if (key == "gr") slot.gr = to_int(value, 0);
            else if (key == "gg") slot.gg = to_int(value, 0);
            else if (key == "gb") slot.gb = to_int(value, 0);
            else if (key == "sr") slot.sr = to_int(value, 0);
            else if (key == "sg") slot.sg = to_int(value, 0);
            else if (key == "sb") slot.sb = to_int(value, 0);
            else if (key == "lbk") slot.lbk = to_int(value, 0);
            else if (key == "lwh") slot.lwh = to_int(value, 0);
            else if (key == "grain") slot.grain = to_int(value, 0);
            else if (key == "mono") slot.mono = value == "true" || value == "1";
        }
        if (!slot.name.empty() && int(g_user_styles.size()) < kMaxUserStyles) {
            std::scoped_lock lock{g_user_styles_mutex};
            g_user_styles.push_back(std::move(slot));
        }
    }

    // The custom snapshot lives after the array as "custom": {...}. Searched from the array's
    // end so a user-named slot can never shadow it. A missing or broken snapshot just leaves
    // the entry disabled; nothing here is allowed to take the menu down.
    const size_t array_end = text.find(']', pos);
    if (array_end == std::string::npos) {
        return;
    }
    const size_t custom_key = text.find("\"custom\"", array_end);
    if (custom_key == std::string::npos) {
        return;
    }
    pos = text.find('{', custom_key);
    if (pos == std::string::npos) {
        return;
    }
    ++pos;
    StyleSlot snap{};
    snap.name = "自定义";
    // Missing members keep the neutral values; the file is written whole, so gaps only come
    // from a hand edit.
    snap.mono = false;
    while (true) {
        skip_ws();
        if (pos >= text.size() || text[pos] == '}') {
            break;
        }
        std::string key, value;
        if (!read_member(key, value)) {
            return; // malformed snapshot: leave it invalid rather than half-applied
        }
        if (key == "shadow") snap.shadow = to_int(value, 0);
        else if (key == "contrast") snap.contrast = to_int(value, 0);
        else if (key == "sat") snap.sat = to_int(value, 0);
        else if (key == "vib") snap.vib = to_int(value, 0);
        else if (key == "lr") snap.lr = to_int(value, 0);
        else if (key == "lg") snap.lg = to_int(value, 0);
        else if (key == "lb") snap.lb = to_int(value, 0);
        else if (key == "gr") snap.gr = to_int(value, 0);
        else if (key == "gg") snap.gg = to_int(value, 0);
        else if (key == "gb") snap.gb = to_int(value, 0);
        else if (key == "sr") snap.sr = to_int(value, 0);
        else if (key == "sg") snap.sg = to_int(value, 0);
        else if (key == "sb") snap.sb = to_int(value, 0);
        else if (key == "lbk") snap.lbk = to_int(value, 0);
        else if (key == "lwh") snap.lwh = to_int(value, 0);
        else if (key == "grain") snap.grain = to_int(value, 0);
        else if (key == "mono") snap.mono = value == "true" || value == "1";
        else if (key == "split") snap.split = value == "true" || value == "1";
        else if (key == "deband") snap.deband = to_int(value, 0);
        else if (key == "defog") snap.defog = to_int(value, 0);
        else if (key == "sharpen") snap.sharpen = to_int(value, 0);
        else if (key == "range") snap.range = to_int(value, 12);
    }
    {
        std::scoped_lock lock{g_user_styles_mutex};
        g_custom_look = std::move(snap);
        g_custom_look_valid = true;
    }
}

void SaveUserStyles() {
    // Snapshot under the lock, write outside it: this runs detached (menu close) and must
    // neither race the menu's edits nor hold the lock across disk I/O.
    std::vector<StyleSlot> styles;
    StyleSlot custom{};
    bool custom_valid = false;
    {
        std::scoped_lock lock{g_user_styles_mutex};
        styles = g_user_styles;
        custom = g_custom_look;
        custom_valid = g_custom_look_valid;
    }
    const std::string path = UserStylePath();
    // The thread id keeps concurrent saves from clobbering each other's temporary; the
    // rename target below is still the one shared path.
    std::ostringstream tmp_name;
    tmp_name << path << ".tmp" << std::this_thread::get_id();
    const std::string tmp = tmp_name.str();
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        std::printf("Overlay: cannot write %s\n", tmp.c_str());
        return;
    }
    std::fprintf(f, "{\n  \"presets\": [\n");
    for (size_t i = 0; i < styles.size(); ++i) {
        const auto& p = styles[i];
        // The name comes from a free-text field: escape what JSON treats as structure, or a
        // quote or backslash in it would corrupt the file (and LoadUserStyles would then drop
        // every slot after the broken one).
        std::string esc;
        esc.reserve(p.name.size() + 8);
        for (const char c : p.name) {
            if (c == '"' || c == '\\') {
                esc.push_back('\\');
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                continue; // control characters would break the line-based format
            }
            esc.push_back(c);
        }
        std::fprintf(f,
                     "    {\"name\": \"%s\", \"shadow\": %d, \"contrast\": %d, \"sat\": %d, "
                     "\"vib\": %d, \"lr\": %d, \"lg\": %d, \"lb\": %d, \"gr\": %d, \"gg\": %d, "
                     "\"gb\": %d, \"sr\": %d, \"sg\": %d, \"sb\": %d, \"lbk\": %d, \"lwh\": %d, "
                     "\"grain\": %d, \"mono\": %s}%s\n",
                     esc.c_str(), p.shadow, p.contrast, p.sat, p.vib, p.lr, p.lg, p.lb, p.gr,
                     p.gg, p.gb, p.sr, p.sg, p.sb, p.lbk, p.lwh, p.grain, p.mono ? "true" : "false",
                     i + 1 == styles.size() ? "" : ",");
    }
    std::fprintf(f, "  ]\n");
    // The custom snapshot rides along when one exists, so clicking 自定义 restores the
    // user's own look even after a restart.
    if (custom_valid) {
        std::fprintf(f,
                     "  \"custom\": {\"shadow\": %d, \"contrast\": %d, \"sat\": %d, \"vib\": %d, "
                     "\"lr\": %d, \"lg\": %d, \"lb\": %d, \"gr\": %d, \"gg\": %d, \"gb\": %d, "
                     "\"sr\": %d, \"sg\": %d, \"sb\": %d, \"lbk\": %d, \"lwh\": %d, "
                     "\"grain\": %d, \"mono\": %s, \"split\": %s, "
                     "\"deband\": %d, \"defog\": %d, \"sharpen\": %d, \"range\": %d}\n",
                     custom.shadow, custom.contrast, custom.sat,
                     custom.vib, custom.lr, custom.lg, custom.lb,
                     custom.gr, custom.gg, custom.gb, custom.sr,
                     custom.sg, custom.sb, custom.lbk, custom.lwh,
                     custom.grain, custom.mono ? "true" : "false",
                     custom.split ? "true" : "false", custom.deband,
                     custom.defog, custom.sharpen, custom.range);
    }
    std::fprintf(f, "}\n");
    std::fclose(f);
    // UCRT's rename() fails with EEXIST when the target already exists (POSIX replaces
    // unconditionally), so every save after the first would fail. MoveFileEx with
    // MOVEFILE_REPLACE_EXISTING is the Windows equivalent of POSIX rename.
#ifdef _WIN32
    const bool replaced = MoveFileExA(tmp.c_str(), path.c_str(),
                                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
    const bool replaced = std::rename(tmp.c_str(), path.c_str()) == 0;
#endif
    if (!replaced) {
        std::printf("Overlay: cannot replace %s\n", path.c_str());
        try {
            std::remove(tmp.c_str());
        } catch (...) {
        }
    }
}

void DeleteUserStyle(int index) {
    {
        std::scoped_lock lock{g_user_styles_mutex};
        if (index < 0 || index >= int(g_user_styles.size())) {
            return;
        }
        g_user_styles.erase(g_user_styles.begin() + index);
        g_style_list_dirty = true;
    }
    SaveUserStyles();
}

/// Snapshots the complete filter state as it stands now (called right after a slider edit
/// landed). This is what the 自定义 entry later restores: the look as the user last left it
/// by hand.
void CaptureCustomLook(const BbSettings::Values& s) {
    {
        std::scoped_lock lock{g_user_styles_mutex};
        g_custom_look = StyleSlot{
            "自定义",          s.post_shadow,     s.post_contrast,   s.post_saturation,
            s.post_vibrance,   s.post_lift_r,     s.post_lift_g,     s.post_lift_b,
            s.post_gamma_r,    s.post_gamma_g,    s.post_gamma_b,    s.post_gain_r,
            s.post_gain_g,     s.post_gain_b,     s.post_levels_black, s.post_levels_white,
            s.post_grain,      s.post_mono,       false,
            s.post_deband,     s.post_defog,      s.post_sharpen,    s.post_range,
            s.post_split};
        g_custom_look_valid = true;
    }
}

/// Restores the snapshot. The atomics' current values are the widget source, so each Store
/// lands the remembered value; "changed" is forced because the menu repaints from the atomics.
void RestoreCustomLook(BbSettings::Values& s) {
    if (!g_custom_look_valid) {
        return;
    }
    const StyleSlot& p = g_custom_look;
    Store(s.post_deband, p.deband, true);
    Store(s.post_defog, p.defog, true);
    Store(s.post_sharpen, p.sharpen, true);
    Store(s.post_range, p.range, true);
    Store(s.post_split, p.split, true);
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

void DrawSaveStylePopup() {
    auto& s = BbSettings::Get();
    // Opening is deferred to the frame after the button click: OpenPopup only marks the id,
    // and BeginPopupModal must not be called in the same frame the id was pushed.
    if (!save_style_popup) {
        return;
    }
    save_style_name[0] = '\0';
    ImGui::OpenPopup("保存风格");
    save_style_popup = false;
    ImGui::SetNextWindowSize(ImVec2(360.0f * base_scale, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("保存风格", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        // Not open yet (it appears the frame after OpenPopup) or closed by Esc: when Begin
        // returns false there is no popup on the stack, so EndPopup must not be called.
        return;
    }
    ImGui::TextUnformatted("把当前的调色参数存为一个可复用的风格。");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("名称", save_style_name, sizeof save_style_name);
    const std::string name = save_style_name;
    const bool empty = name.find_first_not_of(" \t") == std::string::npos;
    const bool full = int(g_user_styles.size()) >= kMaxUserStyles;
    if (full) {
        ImGui::TextDisabled("已达上限（%d 个），先删除一个。", kMaxUserStyles);
    }
    ImGui::BeginDisabled(empty || full);
    if (ImGui::Button("保存", ImVec2(120.0f, 0.0f))) {
        {
            std::scoped_lock lock{g_user_styles_mutex};
            g_user_styles.push_back(MakeSlot(name, StylePreset{
                                                   name.c_str(),
                                                   s.post_shadow,    s.post_contrast,
                                                   s.post_saturation, s.post_vibrance,
                                                   s.post_lift_r,    s.post_lift_g,
                                                   s.post_lift_b,    s.post_gamma_r,
                                                   s.post_gamma_g,   s.post_gamma_b,
                                                   s.post_gain_r,    s.post_gain_g,
                                                   s.post_gain_b,    s.post_levels_black,
                                                   s.post_levels_white, s.post_grain,
                                                   s.post_mono, false}));
        }
        SaveUserStyles();
        g_style_list_dirty = true;
        save_style_popup = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("取消", ImVec2(120.0f, 0.0f))) {
        save_style_popup = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/// Whether the correction sliders are all at their neutral values. Any active correction
/// means no preset can be showing — presets do not own corrections — so the combo must read
/// 自定义 (the user's own look) whenever this is false.
bool CorrectionsAtDefault(const BbSettings::Values& s) {
    return s.post_deband == 0 && s.post_defog == 0 && s.post_sharpen == 0 &&
           s.post_range == 12 && !s.post_split;
}

bool StyleMatches(const StyleSlot& p, const BbSettings::Values& s) {
    if (!CorrectionsAtDefault(s)) {
        return false;
    }
    return s.post_shadow == p.shadow && s.post_contrast == p.contrast &&
           s.post_saturation == p.sat && s.post_vibrance == p.vib &&
           s.post_lift_r == p.lr && s.post_lift_g == p.lg && s.post_lift_b == p.lb &&
           s.post_gamma_r == p.gr && s.post_gamma_g == p.gg && s.post_gamma_b == p.gb &&
           s.post_gain_r == p.sr && s.post_gain_g == p.sg && s.post_gain_b == p.sb &&
           s.post_levels_black == p.lbk && s.post_levels_white == p.lwh &&
           s.post_grain == p.grain && s.post_mono == p.mono;
}

/// First matching built-in or user slot, or null while the state is the user's own look.
/// Both the combo highlight and the snapshot gate below run off this.
const StyleSlot* MatchedStyle(const BbSettings::Values& s) {
    for (const auto& p : StylePresets()) {
        if (StyleMatches(p, s)) {
            return &p;
        }
    }
    return nullptr;
}

void ApplyStyle(const StyleSlot& p, BbSettings::Values& s) {
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
    if (p.full_reset) {
        Store(s.post_deband, 0, true);
        Store(s.post_defog, 0, true);
        Store(s.post_sharpen, 0, true);
        Store(s.post_range, 12, true);
        Store(s.post_split, false, true);
    }
}

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
        // Save synchronously: runtime_restart() ends the process with _exit(0), which does not
        // wait for detached threads, so the heartbeat's pending write would be lost and the
        // new process would come up on the settings the user just changed away from.
        BbSettings::Save();
        runtime_restart();
    }
}

void GraphicsPage() {
    auto& s = BbSettings::Get();
    // The 自定义 snapshot captures the whole page's state at the end of the pass when any of
    // its sliders landed an edit this frame. Cleared here so a Store() from another page
    // (display, advanced) cannot leak into it.
    grade_edited_this_frame = false;
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
        if (CollapsingHeader("FSR 4 选项", true)) {
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
        if (CollapsingHeader("DLSS 选项", true)) {
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
    if (CollapsingHeader("超分增强", true)) {
        Checkbox("锐化（RCAS）", s.sharpen);
        ImGui::BeginDisabled(!s.sharpen);
        SliderWithDefault("锐化强度", s.sharpness, 0.30f, 0.0f, 2.0f);
        Hint("1 以内：超分自带的锐化（RCAS）。1 以上会追加一次 RCAS。DLSS 自身无锐化："
             "由 RCAS 一并完成。Ctrl+点击滑杆可输入精确值。");
        ImGui::EndDisabled();
        Checkbox("亚像素抖动", s.jitter);
        Hint("每帧场景偏移不到一个像素，超分从多帧聚合更多细节。关闭后只剩基于历史帧的抗锯齿。");
    }

    if (CollapsingHeader("响应式遮罩与运动矢量")) {
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
            Store(s.debug_view, static_cast<int>(show_mask ? BbSettings::DebugReactive
                                                              : BbSettings::DebugNone),
                  true);
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        Checkbox("角色运动矢量", s.object_motion);
        Hint("为动画物体计算精确矢量：衣物和武器运动中更不容易碎裂。静态场景没有额外开销。"
             "更改在重启游戏后生效。");
        bool show_motion = s.debug_view == BbSettings::DebugMotion;
        if (ImGui::Checkbox("显示运动矢量（调试）", &show_motion)) {
            Store(s.debug_view, static_cast<int>(show_motion ? BbSettings::DebugMotion
                                                               : BbSettings::DebugNone),
                  true);
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

    if (CollapsingHeader("输出分辨率##hdr", true)) {
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
    if (grade_edited_this_frame && !MatchedStyle(s)) {
        // A manual edit landed this frame AND the result matches no preset: that is the
        // moment the user's own look changes, so the 自定义 snapshot follows it. Applying a
        // preset also flows through Store() (hence the flag), but the state then matches the
        // preset — capturing here would overwrite the user's own look with the preset's.
        CaptureCustomLook(s);
    }
    RestartNotice();
}

void EffectsPage() {
    auto& s = BbSettings::Get();
    // The 自定义 snapshot is taken at the end of this page's pass when a filter slider landed
    // an edit this frame; cleared here so edits from other pages cannot leak into it.
    grade_edited_this_frame = false;
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
    if (CollapsingHeader("后处理（去色带 / 调色 / 锐化）")) {
        {
            // One-click looks plus the user's own "自定义" state.
            //
            // Interaction model (ReShade/Lightroom semantics): a preset only ever moves the
            // sliders it owns; the moment any filter slider is edited by hand the combo reads
            // 自定义, and picking that entry restores the state as the user last left it
            // (snapshot taken after every manual edit, persisted in user-presets.json).
            // 原味 additionally clears the correction sliders, so it is the true unmodified
            // game picture; the other built-ins leave corrections alone.
            int current = -1;
            for (int i = 0; i < int(StylePresets().size()); ++i) {
                if (StyleMatches(StylePresets()[i], s)) {
                    current = i;
                    break; // first match wins: "原味" is the neutral grade, a user slot that
                           // happens to be neutral should not shadow it
                }
            }
            // StylePresets()[i].name is a std::string; the preview is read-only, so the
            // pointer only has to outlive this frame — the vector is a function-local static
            // that lives until the process ends.
            const bool on_custom = current < 0;
            const char* preview = on_custom ? "自定义" : StylePresets()[current].name.c_str();
            if (ImGui::BeginCombo("风格预设", preview)) {
                // Built-ins first, then a separator and the user's own slots. Both live in the
                // same combined list, so index i in StylePresets() addresses both.
                const int count = int(StylePresets().size());
                for (int i = 0; i < count; ++i) {
                    if (i == StyleUserBase) {
                        ImGui::Separator();
                        ImGui::TextDisabled("我的风格");
                    }
                    if (ImGui::Selectable(StylePresets()[i].name.c_str(), i == current)) {
                        ApplyStyle(StylePresets()[i], s);
                    }
                    if (i < StyleUserBase && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                        ImGui::SetTooltip(kBuiltinStyleDesc(std::min(i, kBuiltinStyleCount - 1)));
                    }
                    if (i >= StyleUserBase) {
                        // This ImGui version (1.92.9b) dropped EndPopupContextItem: Begin
                        // forwards to BeginPopup, so a true return needs the matching EndPopup.
                        // Also note the index shifts after a delete, which is why the loop
                        // re-reads the size each pass rather than caching it.
                        if (ImGui::BeginPopupContextItem("style_del")) {
                            if (ImGui::MenuItem("删除此风格")) {
                                DeleteUserStyle(i - StyleUserBase);
                            }
                            ImGui::EndPopup();
                        }
                    }
                }
                ImGui::Separator();
                ImGui::TextDisabled("手动状态");
                // The user's own look. Disabled until at least one manual edit has been made
                // (this session or a previous one — the snapshot persists), because before
                // that there is nothing to restore beyond the presets already listed.
                ImGui::BeginDisabled(!g_custom_look_valid);
                if (ImGui::Selectable("自定义", on_custom)) {
                    RestoreCustomLook(s);
                }
                ImGui::EndDisabled();
                if (!g_custom_look_valid && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                    ImGui::SetTooltip("还没有手动调整过滤镜；先动一下下面的滑杆。");
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button("保存为风格")) {
                save_style_popup = true; // drawn by DrawSaveStylePopup() below
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("把当前的滤镜参数存为一个命名槽位（自定义状态会自动记住，"
                                  "不需要保存）");
            }
            if (save_style_popup) {
                DrawSaveStylePopup();
            }
            Hint("预设只改它名下的滑杆；手动调整任一滑杆后显示为自定义，再点自定义即可回到"
                 "上次手动调好的状态（跨启动保留）。原味会把画面完全还原为游戏原样，"
                 "包括关闭去色带、去雾与锐化。");
        }
        {
            Hint("去色带：天空、雾与暗部的 8-bit 分层感。mpv/libplacebo 同款算法，0 关闭，"
                 "低档最接近 mpv 默认。全部参数立即生效。");
            SliderWithDefault("去色带强度", s.post_deband, 50, 0, 100, "%d%%");
        }
        {
            Hint("去色带采样半径。链在放大之后运行，条带被拉宽：约 2 倍放大建议 16-24；"
                 "大半径平滑更长渐变，小半径更保守。");
            SliderWithDefault("去色带半径（像素）", s.post_range, 12, 4, 32);
        }
        {
            Hint("去雾：减去一层均匀灰雾（100% 相当于 1/4 白的黑位下拉）。对实机远景的"
                 "灰蒙感有效；游戏内的体积浓雾是美术设计，过度去雾会伤氛围。");
            SliderWithDefault("去雾强度", s.post_defog, 0, 0, 100, "%d%%");
        }
        {
            Hint("暗部提升：近黑区域按乘法曲线提亮（100% 时最亮约 1.3 倍），黑点保持纯黑，"
                 "不雾化黑位。");
            SliderWithDefault("暗部提升", s.post_shadow, 0, 0, 100, "%d%%");
        }
        {
            Hint("对比度：绕中间灰拉伸明暗（100% 约为 1.3 倍），中间灰与黑点均不动，"
                 "0 不生效。先去雾再拉伸。");
            SliderWithDefault("对比度", s.post_contrast, 0, 0, 100, "%d%%");
        }
        {
            Hint("饱和度：离开 Rec.709 亮度轴调整彩度（100% 约为 1.4 倍，-100% 约 0.6 倍），"
                 "0 不生效。游戏本身偏浓艳，建议从 10 到 30 起步，负值做低饱和的胶片感。");
            SliderWithDefault("饱和度", s.post_saturation, 0, -100, 100, "%d%%");
        }
        {
            Hint("锐化：AMD RCAS（FSR 附带的锐化内核），与画面页超分自带的锐化独立，通常二选一。");
            SliderWithDefault("锐化强度", s.post_sharpen, 0, 0, 100, "%d%%");
        }
        Checkbox("分割对比（左半原帧）", s.post_split);
        Hint("排查用：左半屏保留未处理的原始画面，右半屏走后处理链，用于逐项核对"
             "色彩与算法偏差。");
        if (CollapsingHeader("智能饱和（Vibrance）")) {
            Hint("ReShade Vibrance 同款：沿亮度轴拉高彩度，越不饱和的像素加得越多，肤色附近"
                 "有保护罩，防止人脸过饱和。0 关闭。");
            SliderWithDefault("智能饱和", s.post_vibrance, 0, 0, 100, "%d%%");
        }
        if (CollapsingHeader("三级调色（Lift / Gamma / Gain）")) {
            Hint("ASC CDL（ReShade Lift_Gamma_Gain 同源数学）：增益乘高光、提升垫阴影"
                 "（100% = ±0.20 黑位）、伽马幂调中间调（+ 变亮）。各通道独立可做分离"
                 "色调，0 为中性。");
            {
            SliderWithDefault("提升 R", s.post_lift_r, 0, -100, 100);
            SliderWithDefault("提升 G", s.post_lift_g, 0, -100, 100);
            SliderWithDefault("提升 B", s.post_lift_b, 0, -100, 100);
        }
        {
            SliderWithDefault("伽马 R", s.post_gamma_r, 0, -100, 100);
            SliderWithDefault("伽马 G", s.post_gamma_g, 0, -100, 100);
            SliderWithDefault("伽马 B", s.post_gamma_b, 0, -100, 100);
        }
        {
            SliderWithDefault("增益 R", s.post_gain_r, 0, -100, 100);
            SliderWithDefault("增益 G", s.post_gain_g, 0, -100, 100);
            SliderWithDefault("增益 B", s.post_gain_b, 0, -100, 100);
        }
        }
        if (CollapsingHeader("黑白场（Levels）")) {
            Hint("输入黑点：低于该亮度的像素压到纯黑，100% 约为 1/2 亮度。收紧黑位、"
                 "提对比的第一手段，0 不动。");
            SliderWithDefault("黑场", s.post_levels_black, 0, 0, 100, "%d%%");
            Hint("输入白点：高于该亮度的像素推到纯白，100% 约为 1/2 亮度。黑场白场共同决定"
                 "输入范围，收得过窄会硬切画面细节。");
            SliderWithDefault("白场", s.post_levels_white, 0, 0, 100, "%d%%");
        }
        {
            Hint("胶片颗粒：乘性高斯噪声（SweetFX FilmGrain 同源数学），暗部更重、黑点不动，"
                 "最后生还者式的低光颗粒感。与去色带的抖动共用随机流，不额外采样。");
            SliderWithDefault("胶片颗粒", s.post_grain, 0, 0, 100, "%d%%");
        }
        Checkbox("单色（黑白）", s.post_mono);
        Hint("按 Rec.709 亮度去色，配合颗粒与对比即高对比黑白胶片。");
    }
    if (CollapsingHeader("游戏效果开关", true)) {
        for (int e = 0; e < BbSettings::EffectCount; ++e) {
            Checkbox(BbSettings::Effects[e].label, s.effects[e]);
        }
        Hint("效果由启动时的游戏补丁开关（patches/Bloodborne.xml）。运动模糊和动态光源阴影占用 "
             "可观的 GPU 时间。");
        Hint("自由视角：按住叉键再按 L3（键盘：Space + Z）。调试菜单：左侧触控板 / Tab。"
             "需要 Nexus mod #253 的 DbgFont14h.ccm 和 DbgFont14h.tpf 放入 dvdroot_ps4/font。"
             "右侧触控板：退格。");
    }
    if (grade_edited_this_frame && !MatchedStyle(s)) {
        // Same contract as the capture on the graphics page: a manual filter edit that
        // matches no preset moves the user's own look, so the 自定义 snapshot follows it.
        CaptureCustomLook(s);
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
            ImGui::SliderInt("GC 显存预算 (MiB)", &v, 0, BbSettings::GcBudgetMaxMB,
                             v ? "%d MiB" : "0（自动）");
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
    if (CollapsingHeader("资源调度（纹理缓存）")) {
        ImGui::Text("显存用量 %llu MiB（加压线 %llu / 临界线 %llu MiB）",
                    (unsigned long long)(gc_stats.used_memory.load() >> 20),
                    (unsigned long long)(gc_stats.pressure_memory.load() >> 20),
                    (unsigned long long)(gc_stats.critical_memory.load() >> 20));
        ImGui::Text("上轮回收：逐出 %llu 张，写回 %llu 张",
                    (unsigned long long)gc_stats.evictions.load(),
                    (unsigned long long)gc_stats.downloads.load());
        Hint("显存用量越过预算 70% 开始回收，85% 加压、95% 激进；写回指 GPU 改过的纹理在"
             "逐出前落回 CPU 内存。计数自上次压力报告起累计，约 5 秒一轮。");
    }
}

void CheatsPage() {
    static const char* cheat_error = nullptr;
    if (const int cheat_files = bbcheats_file_count()) {
        for (int f = 0; f < cheat_files; ++f) {
            ImGui::PushID(f);
            if (CollapsingHeader(bbcheats_file_name(f), f == 0 && cheat_files == 1)) {
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

    // The selected tab is not one of the window properties ImGui stores by itself, so it
    // rides along in bbport.ini as ui_page (same path as ui_scale). Written on close with the
    // rest of the menu's changes, so the menu reopens on the page it was left on.
    //
    // ImGuiTabItemFlags_SetSelected must NOT be re-sent every frame: it queues a focus for
    // its tab whenever the tab bar's SelectedTabId differs (imgui_widgets.cpp:10718), so a
    // re-sent flag for the old page swallows the click on a new one the frame after the tab
    // bar applies it, and the selection snaps back for good. The flag is therefore only
    // raised while page and applied_page disagree — i.e. once, to restore ui_page after a
    // restart (or if it ever changes from outside this loop); every later selection is the
    // user's click and is left alone.
    auto& s = BbSettings::Get();
    int page = std::clamp(s.ui_page.load(), 0, int(PageCount) - 1);
    const bool need_apply = page != applied_page;
    const float footer = ImGui::GetFrameHeightWithSpacing();
    if (ImGui::BeginTabBar("##pages")) {
        // BeginTabItem's second parameter is bool* (a close button), not a "selected" flag;
        // selection is requested with ImGuiTabItemFlags_SetSelected on the flags argument.
        for (int p = 0; p < PageCount; ++p) {
            const ImGuiTabItemFlags flags = need_apply && p == page
                                                ? ImGuiTabItemFlags_SetSelected
                                                : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem(PageName(p), nullptr, flags)) {
                page = p;
                // The child's ID is the visible page name scoped under the tab's ID, so the
                // per-page scroll position persists across switches and restarts.
                ImGui::BeginChild(PageName(p), ImVec2(0.0f, -footer));
                switch (p) {
                case PageGraphics: GraphicsPage(); break;
                case PageDisplay: DisplayPage(); break;
                case PageEffects: EffectsPage(); break;
                case PageCheats: CheatsPage(); break;
                default: AdvancedPage(); break;
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    if (page != applied_page) {
        applied_page = page;
        if (page != s.ui_page.load()) {
            Store(s.ui_page, page, true);
        }
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
    // Relaxed is enough: these are counters, not synchronisation. Nothing else depends on the
    // values, and the menu reading them a frame late is harmless.
    gc_stats.used_memory.store(stats.used_memory, std::memory_order_relaxed);
    gc_stats.pressure_memory.store(stats.pressure_memory, std::memory_order_relaxed);
    gc_stats.critical_memory.store(stats.critical_memory, std::memory_order_relaxed);
    gc_stats.evictions.store(stats.evictions, std::memory_order_relaxed);
    gc_stats.downloads.store(stats.downloads, std::memory_order_relaxed);
}

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // Persist the UI layout: window geometry, the selected tab and every CollapsingHeader's
    // open state. ImGui writes this itself (IniSavingRate, 5 s by default) and only reads it
    // once, on the first NewFrame — see LoadIniSettingsFromDisk below. Previously
    // IniFilename was null, so the menu reopened on the graphics tab with every section
    // collapsed on each launch.
    g_ui_ini_path = UiIniPath();
    io.IniFilename = g_ui_ini_path.c_str();
    io.IniSavingRate = 5.0f;
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
        std::printf("Overlay: no system CJK font, Chinese menu text will render as '?'\n");
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
    backend_image_count = image_count;
    initialized = true;
    std::printf("Overlay: menu ready (Insert or L3+R3)\n");
}

void OnSwapchainChanged(vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized ||
        (static_cast<VkFormat>(format) == backend_format && image_count == backend_image_count)) {
        return;
    }
    // The device was idled by the swapchain teardown before this: no backend work is in flight.
    ImGui_ImplVulkan_Shutdown();
    backend_format = static_cast<VkFormat>(format);
    // The backend sizes its per-frame vertex/index buffer ring from ImageCount and only reads
    // it at Init time, so a changed swapchain image count must reach it here — a stale count
    // means an in-flight frame reusing a buffer another frame still renders from.
    backend_info.MinImageCount = std::max(image_count, 2u);
    backend_info.ImageCount = std::max(image_count, 2u);
    backend_image_count = image_count;
    if (!ImGui_ImplVulkan_Init(&backend_info)) {
        initialized = false;
        // The menu dies with the backend, and the UI state must die with it: menu_open stuck
        // true would keep CapturesInput() eating the game's input while every close hotkey is
        // dead (HandleEvent early-returns on !initialized) — no way out but a restart.
        menu_open = false;
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
    case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_WINDOW_MINIMIZED:
        // Alt-tab with the menu open: without this the held keys stay latched in ImGui's
        // state (io.ClearInputKeys is otherwise only called on menu close) and the game
        // receives phantom movement after refocusing.
        io.ClearInputKeys();
        return false;
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
        // Sticks and triggers follow official imgui_impl_sdl3 semantics (8000 dead-zone,
        // dead-zone..full scale mapped to 0..1 analog, down at >0.1; triggers span the full
        // 0..32767 range). Left stick navigates; the right stick and triggers complete the
        // gamepad key set so future widgets (or third-party code via the shared io) see a
        // whole pad instead of half of one.
        const auto analog = [](float axis, float v0, float v1) {
            return std::clamp((axis - v0) / (v1 - v0), 0.0f, 1.0f);
        };
        switch (event.gaxis.axis) {
        case SDL_GAMEPAD_AXIS_LEFTX:
        case SDL_GAMEPAD_AXIS_LEFTY:
        case SDL_GAMEPAD_AXIS_RIGHTX:
        case SDL_GAMEPAD_AXIS_RIGHTY: {
            const float raw = static_cast<float>(event.gaxis.value);
            const bool x = event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX ||
                           event.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHTX;
            const bool left_stick = event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX ||
                                    event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY;
            const float a_neg = analog(raw, -8000.0f, -32768.0f);
            const float a_pos = analog(raw, 8000.0f, 32767.0f);
            const ImGuiKey neg = left_stick ? (x ? ImGuiKey_GamepadLStickLeft
                                                 : ImGuiKey_GamepadLStickUp)
                                            : (x ? ImGuiKey_GamepadRStickLeft
                                                 : ImGuiKey_GamepadRStickUp);
            const ImGuiKey pos = left_stick ? (x ? ImGuiKey_GamepadLStickRight
                                                 : ImGuiKey_GamepadLStickDown)
                                            : (x ? ImGuiKey_GamepadRStickRight
                                                 : ImGuiKey_GamepadRStickDown);
            io.AddKeyAnalogEvent(neg, a_neg > 0.1f, a_neg);
            io.AddKeyAnalogEvent(pos, a_pos > 0.1f, a_pos);
            break;
        }
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: {
            const float a = std::clamp(static_cast<float>(event.gaxis.value) / 32767.0f, 0.0f, 1.0f);
            io.AddKeyAnalogEvent(event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER
                                     ? ImGuiKey_GamepadL2
                                     : ImGuiKey_GamepadR2,
                                 a > 0.1f, a);
            break;
        }
        default: break;
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
    // Auto-save heartbeat, deliberately before the Visible() early-out below: with the menu
    // closed and the FPS counter off there is no other work in this function, and that is
    // exactly the state a killed process is in. The interval bounds how much a crash can cost
    // to the edits of the last second; the file write itself is on a detached thread because
    // this runs while the present thread holds imgui_mutex. The flag is only taken when the
    // save actually fires — clearing it first would swallow edits made inside the interval.
    const auto now_save = std::chrono::steady_clock::now();
    if (now_save - last_save >= std::chrono::seconds(1) && settings_dirty.exchange(false)) {
        last_save = now_save;
        std::thread([] { BbSettings::Save(); }).detach();
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
