// bbport: SDL3 window for the Vulkan swapchain (X11, Wayland or Win32).
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <SDL3/SDL.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "sdl_window.h"
#include "bbport_overlay.h"
#include "bbport_settings.h"

namespace Frontend {

WindowSDL::WindowSDL(s32 width_, s32 height_, const char* title) : width{width_}, height{height_} {
    // Gamepads are sampled by runtime_pad.c; their events are pumped here with the window's.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        UNREACHABLE_MSG("Failed to initialize SDL video: {}", SDL_GetError());
    }
    // SDL3 no longer blanks-proof games by default: long controller-only sessions and
    // cutscenes would let the screen turn off.
    SDL_DisableScreenSaver();
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width_);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height_);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    base_title = title;
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    ASSERT_MSG(window, "Failed to create window: {}", SDL_GetError());
    ApplySettings(); // saved display mode (borderless / exclusive) before the size is read

    const char* driver = SDL_GetCurrentVideoDriver();
    const SDL_PropertiesID wp = SDL_GetWindowProperties(window);
#ifdef _WIN32
    if (driver && !std::strcmp(driver, "windows")) {
        window_info.type = WindowSystemType::Windows;
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    } else
#endif
    if (driver && !std::strcmp(driver, "x11")) {
        window_info.type = WindowSystemType::X11;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        window_info.render_surface = reinterpret_cast<void*>(SDL_GetNumberProperty(wp, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    } else if (driver && !std::strcmp(driver, "wayland")) {
        window_info.type = WindowSystemType::Wayland;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    } else {
        UNREACHABLE_MSG("Unsupported SDL video driver {}", driver ? driver : "(none)");
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    width = w;
    height = h;
    LOG_INFO(Frontend, "Window {}x{} on {}", w, h, driver);
}

WindowSDL::~WindowSDL() {
    SDL_DestroyWindow(window);
}

void WindowSDL::BeginTextInput(const std::string& initial, const std::string& prompt) {
    std::scoped_lock lock{text_mutex};
    text = initial;
    text_prompt = prompt;
    text_state = 0;
    text_requested = true;
}

int WindowSDL::PollTextInput(std::string& out) {
    std::scoped_lock lock{text_mutex};
    out = text;
    return text_state;
}

void WindowSDL::UpdateTextTitle() {
    const std::string title = text_active ? base_title + " \u2014 " + text_prompt + ": " + text + "_  (Enter = OK, Esc = cancel)"
                                          : base_title;
    SDL_SetWindowTitle(window, title.c_str());
}

bool WindowSDL::PollEvents() {
    {
        std::scoped_lock lock{text_mutex};
        if (text_requested) { // SDL text input must be toggled from the window thread
            text_requested = false;
            text_active = true;
            SDL_StartTextInput(window);
            UpdateTextTitle();
        }
    }
    if (!text_active) {
        BbOverlay::UpdateTextInput(window);
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (text_active && (event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_KEY_DOWN)) {
            std::scoped_lock lock{text_mutex};
            if (event.type == SDL_EVENT_TEXT_INPUT) {
                text += event.text.text;
            } else if (event.key.key == SDLK_BACKSPACE && !text.empty()) {
                size_t cut = text.size() - 1; // drop one UTF-8 code point
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
                text.erase(cut);
            } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER || event.key.key == SDLK_ESCAPE) {
                text_state = event.key.key == SDLK_ESCAPE ? 2 : 1;
                text_active = false;
                SDL_StopTextInput(window);
            }
            UpdateTextTitle();
            continue;
        }
        if (BbOverlay::HandleEvent(event)) {
            continue;
        }
        switch (event.type) {
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED: {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            width = w;
            height = h;
            break;
        }
        case SDL_EVENT_KEY_DOWN:
            // F11: to borderless from a window, back to the window from either fullscreen,
            // through the settings so the menu, the ini and the hot-apply share one truth.
            if (event.key.key == SDLK_F11 && !event.key.repeat) {
                auto& s = BbSettings::Get();
                const bool windowed = s.display_mode.load() != BbSettings::DisplayWindowed;
                s.display_mode = windowed ? BbSettings::DisplayWindowed
                                          : BbSettings::DisplayBorderless;
                BbSettings::Save();
            }
            break;
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            is_open = false;
            break;
        default:
            break;
        }
    }
    ApplySettings();
    return is_open;
}

namespace {

SDL_DisplayID DisplayId(int index) {
    if (index < 0) {
        return SDL_GetPrimaryDisplay();
    }
    int count = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&count);
    SDL_DisplayID id = index < count ? ids[index] : 0;
    SDL_free(ids);
    return id;
}

} // namespace

// Hot-applies the display settings (window mode, monitor, cursor). Runs every poll: changes
// from the menu or F11 take effect on the next frame without a restart.
void WindowSDL::ApplySettings() {
    auto& s = BbSettings::Get();
    const int mode = s.display_mode.load();
    const int display = s.display.load();
    const int output = s.output_res.load();
    Uint32 flags = SDL_GetWindowFlags(window);
    if (mode != applied_mode || display != applied_display ||
        (mode == BbSettings::DisplayExclusive && output != applied_output)) {
        if (flags & SDL_WINDOW_FULLSCREEN) {
            SDL_SetWindowFullscreen(window, false); // moving monitors needs a windowed window
        }
        const SDL_DisplayID id = DisplayId(display);
        if (id) {
            const int centered = SDL_WINDOWPOS_CENTERED_DISPLAY(id);
            SDL_SetWindowPosition(window, centered, centered);
        }
        // Exclusive takes the display mode closest in area to the output resolution; the
        // other modes keep the desktop one. An unset mode (also the invalid-monitor
        // fallback) becomes borderless at the desktop size.
        const SDL_DisplayMode* target = nullptr;
        if (mode == BbSettings::DisplayExclusive && id) {
            int count = 0;
            SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(id, &count);
            const int tw = BbSettings::OutputWidths[std::clamp(output, 0, BbSettings::OutputCount - 1)];
            const int th = BbSettings::OutputHeights[std::clamp(output, 0, BbSettings::OutputCount - 1)];
            s64 best_diff = 0;
            for (int i = 0; i < count; ++i) {
                const s64 diff = s64(modes[i]->w) * modes[i]->h - s64(tw) * th;
                const s64 mag = diff < 0 ? -diff : diff;
                if (!target || mag < best_diff) {
                    target = modes[i];
                    best_diff = mag;
                }
            }
            SDL_free(modes);
        }
        SDL_SetWindowFullscreenMode(window, target);
        SDL_SetWindowFullscreen(window, mode != BbSettings::DisplayWindowed);
        flags = SDL_GetWindowFlags(window);
        applied_mode = mode;
        applied_display = display;
        applied_output = output;
    }

    // Cursor visibility: hidden in fullscreen, in the background and while the menu draws
    // its own software cursor; shown in a focused window unless the user says otherwise.
    const int hide = s.hide_cursor.load();
    const bool show = hide == BbSettings::HideCursorAlways ? false
        : hide == BbSettings::HideCursorNever ? true
        : !(flags & SDL_WINDOW_FULLSCREEN) && (flags & SDL_WINDOW_INPUT_FOCUS) &&
          !BbOverlay::CapturesInput();
    if (show) {
        SDL_ShowCursor();
    } else {
        SDL_HideCursor();
    }
}

} // namespace Frontend
