// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: present-path post chain (bbport_post.h). One optional pass pair on the upscaled game
// frame before it is letterboxed into the swapchain: pass 1 debands (f3kdb/mpv style) and lifts
// the black level, pass 2 sharpens with AMD RCAS. Both write dithered 8-bit, so gradients stay
// smooth through the swapchain. Settings are hot-applied every frame; with everything at zero
// the presenter keeps its direct blit path and nothing is recorded.

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
}

namespace BbPost {

/// Present thread, once: pipelines for both passes.
void Init(const Vulkan::Instance& instance);
/// Present thread teardown, after the schedulers are drained.
void Shutdown();

/// Present thread, each frame before the barriers: true when the chain will run. Creates or
/// recreates the two 8-bit work images at the game frame size. sRGB surfaces return false
/// (texelFetch would decode what the final blit does not re-encode).
bool Prepare(u32 width, u32 height, vk::Format surface_format);

/// Present thread, after the game frame is available for shader reads: records the passes.
/// Ends with the output image in eTransferSrcOptimal, ready as a blit source.
void Record(vk::CommandBuffer cmdbuf, vk::ImageView src_view, u32 width, u32 height);
/// The image Record left the final pixels in (mid when only deband runs, out otherwise).
vk::Image OutputImage();
/// The layout OutputImage is in when Record returns: eTransferSrcOptimal for the blit.
vk::ImageLayout OutputLayout();

} // namespace BbPost
