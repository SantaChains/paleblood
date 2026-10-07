// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_menu_blur.h"

#include <algorithm>
#include <array>
#include <cstdio>

#include "imgui_impl_vulkan.h"
#include "video_core/host_shaders/bbpost_menu_blur_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include <vk_mem_alloc.h>

namespace BbMenuBlur {
namespace {

struct BlurPush {
    float size[2];
    float dir[2]; // tap step in texels, stride included
};
static_assert(sizeof(BlurPush) == 16);

const Vulkan::Instance* instance = nullptr;
vk::Image image_a{}, image_b{}; // a: blit target and output, b: horizontal ping-pong
VmaAllocation alloc_a{}, alloc_b{};
vk::ImageView view_a{}, view_b{};
u32 width = 0, height = 0, small_w = 0, small_h = 0;
bool defined_a = false, defined_b = false;
bool failed = false;

vk::UniqueDescriptorSetLayout desc_layout;
vk::UniquePipelineLayout pipeline_layout;
vk::UniquePipeline blur_pipeline;
// Persistent ImGui descriptor over view_a, which stays in eGeneral for its whole life.
ImTextureID texture = 0;

vk::Device Device() {
    return instance->GetDevice();
}

void DestroyImages() {
    const vk::Device device = Device();
    if (texture) {
        // Rare (resize, shutdown): the backend call touches only the stable backend data and
        // the driver; the ImGui context itself is not involved.
        ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<VkDescriptorSet>(texture));
        texture = 0;
    }
    if (image_a || image_b) {
        device.waitIdle(); // in-flight frames still reference the work images
    }
    if (view_a) {
        device.destroyImageView(view_a);
        view_a = nullptr;
    }
    if (view_b) {
        device.destroyImageView(view_b);
        view_b = nullptr;
    }
    if (image_a) {
        vmaDestroyImage(instance->GetAllocator(), image_a, alloc_a);
        image_a = nullptr;
    }
    if (image_b) {
        vmaDestroyImage(instance->GetAllocator(), image_b, alloc_b);
        image_b = nullptr;
    }
    defined_a = defined_b = false;
}

bool CreateImages(u32 w, u32 h) {
    DestroyImages();
    const vk::ImageCreateInfo info = {
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm, // blit-compatible with the swapchain's 32-bit
                                              // 4x8 format class
        // Quarter-res: trivial passes and a soft upscale; the presenter only routes the blur
        // here for swapchain formats of that class.
        .extent = {std::max(w / 4u, 1u), std::max(h / 4u, 1u), 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage |
                 vk::ImageUsageFlagBits::eTransferDst,
    };
    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };
    for (int i = 0; i < 2; ++i) {
        VkImage unsafe_image{};
        VkImageCreateInfo unsafe_info = static_cast<VkImageCreateInfo>(info);
        VmaAllocation* alloc = i == 0 ? &alloc_a : &alloc_b;
        if (vmaCreateImage(instance->GetAllocator(), &unsafe_info, &alloc_info, &unsafe_image,
                           alloc, nullptr) != VK_SUCCESS) {
            std::printf("BbMenuBlur: image allocation failed (%ux%u)\n", info.extent.width,
                        info.extent.height);
            DestroyImages(); // drop the image already built inside this loop
            return false;
        }
        vk::Image& image = i == 0 ? image_a : image_b;
        image = unsafe_image;
        const vk::ImageViewCreateInfo view_info = {
            .image = image,
            .viewType = vk::ImageViewType::e2D,
            .format = info.format,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        vk::ImageView& view = i == 0 ? view_a : view_b;
        view = Vulkan::Check(Device().createImageView(view_info));
    }
    width = w;
    height = h;
    small_w = info.extent.width;
    small_h = info.extent.height;
    return true;
}

bool Ensure(u32 w, u32 h) {
    if (failed) {
        return false;
    }
    if (image_a && width == w && height == h) {
        return true;
    }
    if (!CreateImages(w, h)) {
        failed = true;
        return false;
    }
    return true;
}

void Dispatch(vk::CommandBuffer cmdbuf, vk::ImageView src, vk::ImageView dst, float dir_x,
              float dir_y) {
    const BlurPush push{{float(small_w), float(small_h)}, {dir_x, dir_y}};
    const vk::DescriptorImageInfo src_info{.imageView = src,
                                           .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo dst_info{.imageView = dst,
                                           .imageLayout = vk::ImageLayout::eGeneral};
    const std::array<vk::WriteDescriptorSet, 2> writes = {{
        {.dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &src_info},
        {.dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .pImageInfo = &dst_info},
    }};
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *blur_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, writes);
    cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                         &push);
    cmdbuf.dispatch((small_w + 7) / 8, (small_h + 7) / 8, 1);
}

/// Reads-before-writes across the frame boundary for one work image.
vk::ImageMemoryBarrier MakeEntryBarrier(vk::Image image, bool defined, vk::AccessFlags dst_access) {
    return vk::ImageMemoryBarrier{
        .srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
        .dstAccessMask = dst_access,
        .oldLayout = defined ? vk::ImageLayout::eGeneral : vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
}

} // namespace

void Init(const Vulkan::Instance& inst) {
    instance = &inst;
    const vk::Device device = Device();
    const std::array<vk::DescriptorSetLayoutBinding, 2> bindings = {{
        {.binding = 0,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
    }};
    desc_layout = Vulkan::Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = u32(bindings.size()),
        .pBindings = bindings.data(),
    }));
    const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                      .offset = 0,
                                      .size = sizeof(BlurPush)};
    pipeline_layout = Vulkan::Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    }));
    const vk::ShaderModule module = Vulkan::CompileSPV(
        std::span{BBPOST_MENU_BLUR_COMP, sizeof(BBPOST_MENU_BLUR_COMP) / sizeof(u32)}, device);
    blur_pipeline = Vulkan::Check(device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                          .module = module,
                          .pName = "main"},
                .layout = *pipeline_layout,
            }));
    device.destroyShaderModule(module);
}

void Shutdown() {
    if (!instance) {
        return;
    }
    DestroyImages();
    blur_pipeline.reset();
    pipeline_layout.reset();
    desc_layout.reset();
    instance = nullptr;
}

void OnBackendReset() {
    // The descriptor set died with the backend's pool (a full pool destroy frees its sets);
    // only the cached handle must go. The work images are dropped too: their content is
    // stale, and the new format may not be blit-eligible (the presenter re-routes and the
    // images re-create lazily when a blur runs again).
    texture = 0;
    DestroyImages();
}

void Record(vk::CommandBuffer cmdbuf, vk::Image src, u32 w, u32 h) {
    // The overlay render pass needs the swapchain back in ColorAttachmentOptimal whatever
    // happens to the blur itself.
    const vk::ImageMemoryBarrier src_ready{
        .srcAccessMask = vk::AccessFlagBits::eTransferRead,
        .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = src,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    if (!Ensure(w, h)) {
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eColorAttachmentOutput, {}, {}, {},
                               src_ready);
        return;
    }
    const std::array entry_barriers{
        MakeEntryBarrier(image_a, defined_a, vk::AccessFlagBits::eTransferWrite),
        MakeEntryBarrier(image_b, defined_b, vk::AccessFlagBits::eShaderWrite),
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                           vk::PipelineStageFlagBits::eTransfer |
                               vk::PipelineStageFlagBits::eComputeShader,
                           {}, {}, {}, entry_barriers);
    defined_a = defined_b = true;

    // Downsample: one hardware bilinear blit of the full-res composited frame.
    const vk::ImageBlit region{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .srcOffsets = std::array<vk::Offset3D, 2>{{{0, 0, 0}, {s32(w), s32(h), 1}}},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstOffsets = std::array<vk::Offset3D, 2>{{{0, 0, 0}, {s32(small_w), s32(small_h), 1}}},
    };
    cmdbuf.blitImage(src, vk::ImageLayout::eTransferSrcOptimal, image_a,
                     vk::ImageLayout::eGeneral, region, vk::Filter::eLinear);
    const vk::ImageMemoryBarrier blit_done{
        .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
        .dstAccessMask = vk::AccessFlagBits::eShaderRead,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eGeneral,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image_a,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                           vk::PipelineStageFlagBits::eComputeShader, {}, {}, {}, blit_done);
    Dispatch(cmdbuf, view_a, view_b, 2.0f, 0.0f); // horizontal
    const vk::MemoryBarrier pass_done{.srcAccessMask = vk::AccessFlagBits::eShaderWrite,
                                      .dstAccessMask = vk::AccessFlagBits::eShaderRead};
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                           vk::PipelineStageFlagBits::eComputeShader, {}, pass_done, {}, {});
    Dispatch(cmdbuf, view_b, view_a, 0.0f, 2.0f); // vertical

    // The output readable by the UI pass; the frame back to its attachment layout.
    const std::array exit_barriers{
        vk::ImageMemoryBarrier{
            .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
            .dstAccessMask = vk::AccessFlagBits::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image_a,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        },
        src_ready,
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader |
                               vk::PipelineStageFlagBits::eTransfer,
                           vk::PipelineStageFlagBits::eFragmentShader |
                               vk::PipelineStageFlagBits::eColorAttachmentOutput,
                           {}, {}, {}, exit_barriers);
}

ImTextureID Texture() {
    if (view_a && !texture) {
        texture = reinterpret_cast<ImTextureID>(
            ImGui_ImplVulkan_AddTexture(view_a, VK_IMAGE_LAYOUT_GENERAL));
    }
    return texture;
}

} // namespace BbMenuBlur
