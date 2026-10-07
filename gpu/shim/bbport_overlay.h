// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: in-game settings menu (Dear ImGui), drawn by the presenter into the swapchain image
// after the game frame, at display resolution. Insert (keyboard) or L3+R3 (gamepad) opens it;
// while it is open the game gets no pad/keyboard input. Settings live in bbport_settings.h.

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

union SDL_Event;
struct SDL_Window;

namespace Vulkan {
class Instance;
}

namespace BbOverlay {

/// Present thread, once: the ImGui context and its Vulkan backend.
void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count);

/// Swap thread, after the device was idled for a format change (HDR toggle):
/// rebuilds the backend's pipeline for the new swapchain format.
void OnFormatChange(vk::Format format);

/// Window thread, for every SDL event: true when the menu consumed it.
bool HandleEvent(const SDL_Event& event);
/// Turns SDL text input on while the menu edits a value (window thread, once per poll).
void UpdateTextInput(SDL_Window* window);

/// Whether anything is drawn this frame (menu open or FPS counter on).
bool Visible();

/// Whether the frosted backdrop should be recorded this frame (the menu is open).
bool WantsBlur();

/// Present thread: draws into `view` (layout ColorAttachmentOptimal).
void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent);

/// The menu is open: the game's input is held neutral.
bool CapturesInput();

/// The presenter feeds the driver-measured latency (VK_NV_low_latency2); negative hides it.
void SetLatencyMs(float ms);

/// Texture-cache GC telemetry for the Advanced menu (fed by the presenter each frame).
struct GcSnapshot {
    u64 used_memory;     ///< live device memory estimate, bytes
    u64 pressure_memory; ///< 85% budget mark, bytes
    u64 critical_memory; ///< 95% budget mark, bytes
    u64 evictions;       ///< images evicted since the last report
    u64 downloads;       ///< dirty images written back since the last report
};

/// The presenter feeds the texture cache's GC telemetry (present thread, once per frame).
void SetGcStats(const GcSnapshot& stats);

} // namespace BbOverlay
