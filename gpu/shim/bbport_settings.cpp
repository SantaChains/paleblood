// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_settings.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace BbSettings {

namespace {

const char* Path() {
    const char* env = std::getenv("BB_CONFIG");
    return env && env[0] ? env : "bbport.ini";
}

float Clamp(float v, float lo, float hi) {
    return std::clamp(v, lo, hi);
}

/// atoi/atof are undefined on overflow and happily parse inf/nan — a hand-edited ini can
/// carry anything, so parse strictly: strtoll saturates into the int range, floats must be
/// finite or the key falls back to 0.
int ParseInt(const std::string& value) {
    char* end = nullptr;
    const long long parsed = std::strtoll(value.c_str(), &end, 10);
    if (end == value.c_str()) {
        return 0;
    }
    return int(std::clamp(parsed, 1LL * INT_MIN, 1LL * INT_MAX));
}

float ParseFloat(const std::string& value) {
    char* end = nullptr;
    const float parsed = std::strtof(value.c_str(), &end);
    return end == value.c_str() || !std::isfinite(parsed) ? 0.0f : parsed;
}

void Set(Values& v, const std::string& key, const std::string& value) {
    const float f = ParseFloat(value);
    const int i = ParseInt(value);
    if (key == "upscaler") {
        for (int u = 0; u < UpscalerCount; ++u) {
            if (value == UpscalerName(u)) {
                v.upscaler = u;
            }
        }
    } else if (key == "preset") {
        v.preset = std::clamp(i, 0, PresetCount - 1);
    } else if (key == "sharpen") {
        v.sharpen = i != 0;
    } else if (key == "sharpness") {
        v.sharpness = Clamp(f, 0.0f, 2.0f);
    } else if (key == "jitter") {
        v.jitter = i != 0;
    } else if (key == "reactive") {
        v.reactive = i != 0;
    } else if (key == "object_motion") {
        v.object_motion = i != 0;
    } else if (key == "reactive_scale") {
        v.reactive_scale = Clamp(f, 0.0f, 16.0f);
    } else if (key == "reactive_threshold") {
        v.reactive_threshold = Clamp(f, 0.0f, 1.0f);
    } else if (key == "reactive_max") {
        v.reactive_max = Clamp(f, 0.0f, 1.0f);
    } else if (key == "debug_view") {
        v.debug_view = std::clamp(i, 0, DebugViewCount - 1);
    } else if (key == "show_fps") {
        v.show_fps = i != 0;
    } else if (key == "fps_detail") {
        v.fps_detail = std::clamp(i, 0, 2);
    } else if (key == "low_latency") {
        v.low_latency = i != 0;
    } else if (key == "fsr4_auto_exposure") {
        v.fsr4_auto_exposure = i != 0;
    } else if (key == "fsr4_invert_jitter") {
        v.fsr4_invert_jitter = i != 0;
    } else if (key == "model_lod") {
        v.model_lod = std::clamp(i, -2, 2);
    } else if (key == "fullscreen") { // legacy key: it meant borderless fullscreen
        v.display_mode = i ? DisplayBorderless : DisplayWindowed;
    } else if (key == "display_mode") {
        v.display_mode = std::clamp(i, 0, DisplayModeCount - 1);
    } else if (key == "hide_cursor") {
        v.hide_cursor = std::clamp(i, 0, int(HideCursorNever));
    } else if (key == "display") {
        v.display = i;
    } else if (key == "ui_scale") { // hand-edited values snap to the menu steps
        static constexpr int steps[] = {85, 100, 115, 130, 150};
        int best = 100;
        for (int step : steps) {
            if (std::abs(i - step) < std::abs(i - best)) best = step;
        }
        v.ui_scale = best;
    } else if (key == "live_resolution") {
        v.live_resolution = value == "auto" ? -1 : std::clamp(i, 0, 1);
    } else if (key == "post_deband") {
        v.post_deband = std::clamp(i, 0, 100);
    } else if (key == "post_shadow") {
        v.post_shadow = std::clamp(i, 0, 100);
    } else if (key == "post_sharpen") {
        v.post_sharpen = std::clamp(i, 0, 100);
    } else if (key == "post_defog") {
        v.post_defog = std::clamp(i, 0, 100);
    } else if (key == "post_contrast") {
        v.post_contrast = std::clamp(i, 0, 100);
    } else if (key == "post_saturation") {
        v.post_saturation = std::clamp(i, -100, 100);
    } else if (key == "post_range") {
        v.post_range = std::clamp(i, 4, 32);
    } else if (key == "post_split") {
        v.post_split = i != 0;
    } else if (key == "post_vibrance") {
        v.post_vibrance = std::clamp(i, 0, 100);
    } else if (key == "post_lift_r") {
        v.post_lift_r = std::clamp(i, -100, 100);
    } else if (key == "post_lift_g") {
        v.post_lift_g = std::clamp(i, -100, 100);
    } else if (key == "post_lift_b") {
        v.post_lift_b = std::clamp(i, -100, 100);
    } else if (key == "post_gamma_r") {
        v.post_gamma_r = std::clamp(i, -100, 100);
    } else if (key == "post_gamma_g") {
        v.post_gamma_g = std::clamp(i, -100, 100);
    } else if (key == "post_gamma_b") {
        v.post_gamma_b = std::clamp(i, -100, 100);
    } else if (key == "post_gain_r") {
        v.post_gain_r = std::clamp(i, -100, 100);
    } else if (key == "post_gain_g") {
        v.post_gain_g = std::clamp(i, -100, 100);
    } else if (key == "post_gain_b") {
        v.post_gain_b = std::clamp(i, -100, 100);
    } else if (key == "post_levels_black") {
        v.post_levels_black = std::clamp(i, 0, 100);
    } else if (key == "post_levels_white") {
        v.post_levels_white = std::clamp(i, 0, 100);
    } else if (key == "post_grain") {
        v.post_grain = std::clamp(i, 0, 100);
    } else if (key == "post_mono") {
        v.post_mono = i != 0;
    } else if (key == "fps_cap") {
        v.fps_cap = std::clamp(i, -1, 480);
    } else if (key == "gc_writeback") {
        v.gc_writeback = std::clamp(i, 0, 64);
    } else if (key == "gc_budget_mb") {
        v.gc_budget_mb = std::clamp(i, 0, 65536);
    } else if (key == "pad_swap") {
        v.pad_swap = std::clamp(i, 0, 3);
    } else if (key == "dlss_preset") {
        v.dlss_preset = i < 0 ? -1 : std::clamp(i, 0, 15);
    } else if (key == "output_res") {
        for (int r = 0; r < OutputCount; ++r) {
            if (value == std::to_string(OutputWidths[r]) + "x" + std::to_string(OutputHeights[r])) {
                v.output_res = r;
            }
        }
    } else {
        for (int e = 0; e < EffectCount; ++e) {
            if (key == Effects[e].key) {
                v.effects[e] = i != 0;
            }
        }
    }
}

} // namespace

Values& Get() {
    static Values values;
    return values;
}

void Load() {
    auto& v = Get();
    for (int e = 0; e < EffectCount; ++e) {
        v.effects[e] = Effects[e].default_on;
    }
    if (FILE* file = std::fopen(Path(), "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            const auto eq = text.find('=');
            if (text.empty() || text[0] == '#' || eq == std::string::npos) {
                continue;
            }
            Set(v, text.substr(0, eq), text.substr(eq + 1));
        }
        std::fclose(file);
        std::printf("Settings: %s\n", Path());
    }
    // Environment overrides (scripts, A/B tests).
    if (const char* env = std::getenv("BB_UPSCALER")) {
        v.upscaler = UpscalerOff;
        for (int u = 0; u < UpscalerCount; ++u) {
            if (std::strcmp(env, UpscalerName(u)) == 0) v.upscaler = u;
        }
    }
    const std::pair<const char*, const char*> env_keys[] = {
        {"BB_FSR_SHARPNESS", "sharpness"},        {"BB_JITTER", "jitter"},
        {"BB_REACTIVE", "reactive"},              {"BB_REACTIVE_SCALE", "reactive_scale"},
        {"BB_REACTIVE_THRESHOLD", "reactive_threshold"}, {"BB_REACTIVE_MAX", "reactive_max"},
        {"BB_UPSCALE_PRESET", "preset"},            {"BB_OBJECT_MOTION", "object_motion"},
        {"BB_LOW_LATENCY", "low_latency"},
        {"BB_PAD_SWAP", "pad_swap"},
        {"BB_GC_DOWNLOADS_PER_PASS", "gc_writeback"}, {"BB_GC_BUDGET_MB", "gc_budget_mb"},
    };
    for (const auto& [env, key] : env_keys) {
        if (const char* value = std::getenv(env)) {
            Set(v, key, value);
        }
    }
    if (const char* env = std::getenv("BB_FULLSCREEN")) { // legacy scripts: 1 = borderless
        v.display_mode = env[0] == '1' ? DisplayBorderless : DisplayWindowed;
    }
    v.startup_preset = v.preset;
    v.startup_upscaler = v.upscaler;
    v.startup_object_motion = v.object_motion;
    for (int e = 0; e < EffectCount; ++e) {
        v.startup_effects[e] = v.effects[e];
    }
    v.startup_model_lod = v.model_lod;
    v.startup_output_res = v.output_res;
    v.startup_live_resolution = v.live_resolution;
}

void ConfigureUpscalerSupport(bool fsr4, bool fsr411, bool dlss) {
    auto& v = Get();
    v.fsr4_supported = fsr4;
    v.fsr411_supported = fsr4 && fsr411;
    v.dlss_supported = dlss;
    const int requested = v.upscaler;
    if (requested == UpscalerDlss && !dlss) {
        v.fsr4_problem = "DLSS needs an NVIDIA RTX GPU, its driver's NGX and nvngx_dlss; using FSR 3.1";
        std::printf("Upscaler: dlss unavailable; falling back to FSR 3.1 before the first frame\n");
        v.upscaler = UpscalerFsr3;
    } else if ((requested == UpscalerFsr4 && !v.fsr4_supported) ||
        (requested == UpscalerFsr411 && !v.fsr411_supported)) {
        v.fsr4_problem = "GPU does not support the selected FSR 4 shaders; using FSR 3.1";
        std::printf("Upscaler: %s unsupported on this GPU; falling back to FSR 3.1 before the first frame\n",
                    UpscalerName(requested));
        v.upscaler = UpscalerFsr3;
    }
}

bool FixedRenderSession() {
    const char* size = std::getenv("BB_RENDER_RES");
    return size && size[0];
}

int RenderPreset() {
    const auto& v = Get();
    return FixedRenderSession() ? v.startup_preset :
        v.upscaler == UpscalerTaa ? NativeAA : v.preset.load();
}

bool ResolutionNeedsRestart() {
    const auto& v = Get();
    // TAA needs the live path (native guest targets): run.sh selects it on restart.
    return FixedRenderSession() &&
        (v.preset != v.startup_preset || v.output_res != v.startup_output_res ||
         (v.upscaler == UpscalerOff) != (v.startup_upscaler == UpscalerOff) ||
         (v.upscaler == UpscalerTaa) != (v.startup_upscaler == UpscalerTaa));
}

void Save() {
    static std::mutex save_mutex; // F11 (window thread) and the menu close save concurrently
    const std::scoped_lock lock{save_mutex};
    const auto& v = Get();
    // Keys this menu does not own (language=, pad_swap=, ... consumed by the launch scripts)
    // must survive a save: Load() ignores them, so they would otherwise be silently dropped.
    static const char* const owned[] = {
        "upscaler",           "preset",             "sharpen",        "sharpness",
        "jitter",             "reactive",           "object_motion",  "reactive_scale",
        "reactive_threshold", "reactive_max",       "debug_view",     "show_fps",
        "fps_detail",
        "fsr4_auto_exposure", "fsr4_invert_jitter", "model_lod",      "output_res",
        "display_mode",       "hide_cursor",        "display",        "ui_scale",
        "low_latency",
        "live_resolution",    "post_deband",        "post_shadow",    "post_sharpen",
        "post_defog",         "post_contrast",      "post_saturation",
        "post_range",         "post_split",
        "post_vibrance",      "post_lift_r",      "post_lift_g",    "post_lift_b",
        "post_gamma_r",       "post_gamma_g",     "post_gamma_b",
        "post_gain_r",        "post_gain_g",      "post_gain_b",
        "post_levels_black",  "post_levels_white",
        "post_grain",         "post_mono",
        "fps_cap",            "gc_writeback",     "gc_budget_mb",   "pad_swap",
        "dlss_preset",
        "fullscreen", // legacy: absorbed so old lines are not kept as foreign
    };
    std::vector<std::string> foreign;
    if (FILE* old = std::fopen(Path(), "r")) {
        char line[512];
        while (std::fgets(line, sizeof(line), old)) {
            std::string text{line};
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            const auto eq = text.find('=');
            if (text.empty() || text[0] == '#' || eq == std::string::npos) {
                continue;
            }
            const std::string key = text.substr(0, eq);
            bool known = false;
            for (const char* o : owned) {
                known = known || key == o;
            }
            for (int e = 0; e < EffectCount && !known; ++e) {
                known = key == Effects[e].key;
            }
            if (!known) {
                foreign.push_back(text);
            }
        }
        std::fclose(old);
    }
    FILE* file = std::fopen(Path(), "w");
    if (!file) {
        std::printf("Settings: cannot write %s\n", Path());
        return;
    }
    std::fprintf(file,
                 "# bbport settings (in-game menu: Insert / L3+R3)\n"
                 "upscaler=%s\npreset=%d\nsharpen=%d\nsharpness=%.2f\njitter=%d\n"
                 "reactive=%d\nobject_motion=%d\nreactive_scale=%.2f\nreactive_threshold=%.2f\nreactive_max=%.2f\n"
                 "debug_view=%d\nshow_fps=%d\nfps_detail=%d\nfsr4_auto_exposure=%d\n"
                 "fsr4_invert_jitter=%d\n",
                 UpscalerName(v.upscaler), v.preset.load(), int(v.sharpen.load()),
                 v.sharpness.load(), int(v.jitter.load()), int(v.reactive.load()),
                 int(v.object_motion.load()),
                 v.reactive_scale.load(), v.reactive_threshold.load(), v.reactive_max.load(),
                 v.debug_view.load(), int(v.show_fps.load()), v.fps_detail.load(),
                 int(v.fsr4_auto_exposure.load()), int(v.fsr4_invert_jitter.load()));
    // Read by patches.py at start.
    for (int e = 0; e < EffectCount; ++e) {
        std::fprintf(file, "%s=%d\n", Effects[e].key, int(v.effects[e].load()));
    }
    std::fprintf(file, "model_lod=%d\noutput_res=%dx%d\n", v.model_lod.load(),
                 OutputWidths[v.output_res], OutputHeights[v.output_res]);
    std::fprintf(file, "display_mode=%d\nhide_cursor=%d\ndisplay=%d\nui_scale=%d\n",
                 v.display_mode.load(), v.hide_cursor.load(), v.display.load(),
                 v.ui_scale.load());
    std::fprintf(file, "low_latency=%d\n", int(v.low_latency.load()));
    std::fprintf(file, "post_deband=%d\npost_shadow=%d\npost_sharpen=%d\n", v.post_deband.load(),
                 v.post_shadow.load(), v.post_sharpen.load());
    std::fprintf(file, "post_defog=%d\npost_contrast=%d\npost_saturation=%d\n", v.post_defog.load(),
                 v.post_contrast.load(), v.post_saturation.load());
    std::fprintf(file, "post_range=%d\npost_split=%d\n", v.post_range.load(),
                 v.post_split.load() ? 1 : 0);
    std::fprintf(file,
                 "post_vibrance=%d\npost_lift_r=%d\npost_lift_g=%d\npost_lift_b=%d\n"
                 "post_gamma_r=%d\npost_gamma_g=%d\npost_gamma_b=%d\n"
                 "post_gain_r=%d\npost_gain_g=%d\npost_gain_b=%d\n"
                 "post_levels_black=%d\npost_levels_white=%d\npost_grain=%d\npost_mono=%d\n",
                 v.post_vibrance.load(), v.post_lift_r.load(), v.post_lift_g.load(),
                 v.post_lift_b.load(), v.post_gamma_r.load(), v.post_gamma_g.load(),
                 v.post_gamma_b.load(), v.post_gain_r.load(), v.post_gain_g.load(),
                 v.post_gain_b.load(), v.post_levels_black.load(), v.post_levels_white.load(),
                 v.post_grain.load(), v.post_mono.load() ? 1 : 0);
    // Advanced page (all hot-applied).
    std::fprintf(file, "fps_cap=%d\ngc_writeback=%d\ngc_budget_mb=%d\npad_swap=%d\n",
                 v.fps_cap.load(), v.gc_writeback.load(), v.gc_budget_mb.load(),
                 v.pad_swap.load());
    std::fprintf(file, "dlss_preset=%d\n", v.dlss_preset.load());
    // Read by run.sh at start.
    std::fprintf(file, "live_resolution=%s\n", v.live_resolution < 0 ? "auto"
                                                  : v.live_resolution ? "1" : "0");
    for (const std::string& kept : foreign) {
        std::fprintf(file, "%s\n", kept.c_str());
    }
    std::fclose(file);
}

float PresetScale(int preset) {
    static constexpr float scales[PresetCount] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
    return scales[std::clamp(preset, 0, PresetCount - 1)];
}

const char* PresetName(int preset) {
    static constexpr const char* names[PresetCount] = {"原生抗锯齿", "质量", "平衡",
                                                       "性能", "极致性能"};
    return names[std::clamp(preset, 0, PresetCount - 1)];
}

const char* UpscalerName(int upscaler) {
    static constexpr const char* names[UpscalerCount] = {"off", "fsr3", "fsr4", "fsr411", "taa",
                                                                  "dlss"};
    return names[std::clamp(upscaler, 0, UpscalerCount - 1)];
}

} // namespace BbSettings
