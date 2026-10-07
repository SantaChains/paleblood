// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: frosted-glass backdrop for the settings menu. While the menu is open the presenter
// records a quarter-res separable gaussian over the composited swapchain image; the overlay
// draws the result behind the menu. The game pays nothing while the menu is closed.

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

#include "imgui.h"

namespace Vulkan {
class Instance;
}

namespace BbMenuBlur {

/// Present thread, once: the blur pipeline.
void Init(const Vulkan::Instance& instance);

/// Presenter teardown: drops the images and the ImGui texture descriptor.
void Shutdown();

/// The ImGui Vulkan backend was rebuilt (HDR toggle): its descriptor pool is gone, so the
/// cached backdrop descriptor is dropped and the work images are released; everything
/// re-creates lazily on the next blur.
void OnBackendReset();

/// Present thread, recorded before the UI render pass: downsamples `src` (detoured through
/// TransferSrcOptimal — the only blit-source layout the swapchain image may take, it has no
/// SAMPLED_BIT — and back), blurs it, and leaves `src` in ColorAttachmentOptimal for the
/// overlay's dynamic rendering pass.
void Record(vk::CommandBuffer cmdbuf, vk::Image src, u32 width, u32 height);

/// Descriptor set of the blurred backdrop as an ImTextureID; null until the first blur.
/// Present thread only (the overlay's locked frame).
ImTextureID Texture();

} // namespace BbMenuBlur
