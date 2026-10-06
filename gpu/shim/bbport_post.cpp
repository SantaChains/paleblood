// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_post.h"

#include <array>
#include <cstdio>

#include "bbport_settings.h"
#include "video_core/host_shaders/bbpost_deband_comp.h"
#include "video_core/host_shaders/bbpost_sharpen_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include <vk_mem_alloc.h>

namespace BbPost {

namespace {

struct DebandPush {
    float size[2];
    float threshold;
    float range;
    float lift;
    float defog;
    float contrast;
    float saturation;
    u32 seed;
    u32 split;
};
static_assert(sizeof(DebandPush) == 40);
struct SharpenPush {
    float size[2];
    float amount;
    u32 seed;
};
static_assert(sizeof(SharpenPush) == 16);

// Black lift at 100%: black becomes 0.30 grey, the middle range barely moves.
constexpr float LiftScale = 0.30f;
// Fog removal at 100%: subtract an even haze of 0.25 (a quarter of full white).
constexpr float DefogScale = 0.25f;
// Contrast at 100%: mid-grey pivots in place, blacks/whites stretch by 1.3x.
constexpr float ContrastScale = 0.30f;
// Saturation at 100%: 1.4x away from Rec.709 luma.
constexpr float SaturationScale = 0.40f;

const Vulkan::Instance* instance = nullptr;
vk::Image mid_image{}, out_image{};
VmaAllocation mid_alloc{}, out_alloc{};
vk::ImageView mid_view{}, out_view{};
u32 width = 0, height = 0;
bool mid_defined = false, out_defined = false;
bool failed = false;
u32 frame_seed = 0;
vk::Image output{};

vk::UniqueDescriptorSetLayout desc_layout;
vk::UniquePipelineLayout pipeline_layout;
vk::UniquePipeline deband_pipeline, sharpen_pipeline;

vk::Device Device() {
    return instance->GetDevice();
}

/// 1..100 percent onto 1..7 8-bit levels: 50% is libplacebo's 4/255 default threshold.
float DebandThreshold(int percent) {
    return (1.0f + float(percent) * 0.06f) / 255.0f;
}

void DestroyImages() {
    const vk::Device device = Device();
    if (mid_view) {
        device.destroyImageView(mid_view);
        mid_view = nullptr;
    }
    if (out_view) {
        device.destroyImageView(out_view);
        out_view = nullptr;
    }
    if (mid_image) {
        vmaDestroyImage(instance->GetAllocator(), mid_image, mid_alloc);
        mid_image = nullptr;
    }
    if (out_image) {
        vmaDestroyImage(instance->GetAllocator(), out_image, out_alloc);
        out_image = nullptr;
    }
    mid_defined = out_defined = false;
}

bool CreateImages(u32 w, u32 h) {
    DestroyImages();
    const vk::ImageCreateInfo info = {
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm, // storage is mandatory here; blit-compatible with
                                              // the swapchain's 32-bit 4x8 format class
        .extent = {w, h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage |
                 vk::ImageUsageFlagBits::eTransferSrc,
    };
    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };
    for (int i = 0; i < 2; ++i) {
        VkImage unsafe_image{};
        VkImageCreateInfo unsafe_info = static_cast<VkImageCreateInfo>(info);
        VmaAllocation* alloc = i == 0 ? &mid_alloc : &out_alloc;
        if (vmaCreateImage(instance->GetAllocator(), &unsafe_info, &alloc_info, &unsafe_image,
                           alloc, nullptr) != VK_SUCCESS) {
            std::printf("BbPost: image allocation failed (%ux%u)\n", w, h);
            DestroyImages(); // drop the image already built inside this loop
            return false;
        }
        vk::Image& image = i == 0 ? mid_image : out_image;
        image = unsafe_image;
        const vk::ImageViewCreateInfo view_info = {
            .image = image,
            .viewType = vk::ImageViewType::e2D,
            .format = info.format,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        vk::ImageView& view = i == 0 ? mid_view : out_view;
        view = Vulkan::Check(Device().createImageView(view_info));
    }
    width = w;
    height = h;
    return true;
}

void CreatePipelines() {
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
                                      .size = sizeof(DebandPush)};
    pipeline_layout = Vulkan::Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    }));
    const auto build = [&](const u32* code, size_t words) {
        const vk::ShaderModule module = Vulkan::CompileSPV(std::span{code, words}, device);
        auto pipeline = Vulkan::Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = module,
                              .pName = "main"},
                    .layout = *pipeline_layout,
                }));
        device.destroyShaderModule(module);
        return pipeline;
    };
    deband_pipeline = build(BBPOST_DEBAND_COMP, sizeof(BBPOST_DEBAND_COMP) / sizeof(u32));
    sharpen_pipeline = build(BBPOST_SHARPEN_COMP, sizeof(BBPOST_SHARPEN_COMP) / sizeof(u32));
}

void Dispatch(vk::CommandBuffer cmdbuf, vk::Pipeline pipeline, vk::ImageView src,
              vk::ImageView dst, const void* push, u32 push_size) {
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
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, writes);
    cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, push_size, push);
    cmdbuf.dispatch((width + 7) / 8, (height + 7) / 8, 1);
}

/// Reads-before-writes across the frame boundary for one work image (kept in eGeneral).
vk::ImageMemoryBarrier MakeEntryBarrier(vk::Image image, bool defined) {
    return vk::ImageMemoryBarrier{
        .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
        .dstAccessMask = vk::AccessFlagBits::eShaderWrite,
        .oldLayout = defined ? vk::ImageLayout::eGeneral : vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
}

bool Active() {
    const auto& s = BbSettings::Get();
    return s.post_deband.load() > 0 || s.post_shadow.load() > 0 || s.post_sharpen.load() > 0 ||
           s.post_defog.load() > 0 || s.post_contrast.load() > 0 || s.post_saturation.load() > 0 ||
           s.post_split.load();
}

} // namespace

void Init(const Vulkan::Instance& inst) {
    instance = &inst;
    CreatePipelines();
}

void Shutdown() {
    if (!instance) {
        return;
    }
    DestroyImages();
    deband_pipeline.reset();
    sharpen_pipeline.reset();
    pipeline_layout.reset();
    desc_layout.reset();
    instance = nullptr;
}

bool Prepare(u32 w, u32 h, vk::Format surface_format) {
    if (failed || !instance || !Active()) {
        return false;
    }
    if (surface_format == vk::Format::eB8G8R8A8Srgb ||
        surface_format == vk::Format::eR8G8B8A8Srgb) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::printf("BbPost: sRGB swapchain, post chain disabled\n");
        }
        return false;
    }
    if (mid_image && width == w && height == h) {
        return true;
    }
    if (!CreateImages(w, h)) {
        failed = true;
        return false;
    }
    return true;
}

void Record(vk::CommandBuffer cmdbuf, vk::ImageView src_view, u32 w, u32 h) {
    const auto& s = BbSettings::Get();
    const bool pass1_on = s.post_deband.load() > 0 || s.post_shadow.load() > 0 ||
                          s.post_defog.load() > 0 || s.post_contrast.load() > 0 ||
                          s.post_saturation.load() > 0 || s.post_split.load();
    const bool sharpen_on = s.post_sharpen.load() > 0;
    const u32 seed = ++frame_seed;

    std::array<vk::ImageMemoryBarrier, 2> entries{};
    u32 entry_count = 0;
    if (pass1_on) {
        entries[entry_count++] = MakeEntryBarrier(mid_image, mid_defined);
    }
    if (sharpen_on) {
        entries[entry_count++] = MakeEntryBarrier(out_image, out_defined);
    }
    if (entry_count > 0) {
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eComputeShader,
                               vk::DependencyFlagBits::eByRegion, {}, {},
                               vk::ArrayProxy<vk::ImageMemoryBarrier>(entry_count, entries.data()));
    }
    mid_defined = pass1_on;
    out_defined = sharpen_on;

    vk::ImageView last_view = src_view;
    if (pass1_on) {
        const DebandPush push{{float(w), float(h)},
                              DebandThreshold(s.post_deband.load()),
                              float(s.post_range.load()),
                              LiftScale * float(s.post_shadow.load()) / 100.0f,
                              DefogScale * float(s.post_defog.load()) / 100.0f,
                              1.0f + ContrastScale * float(s.post_contrast.load()) / 100.0f,
                              1.0f + SaturationScale * float(s.post_saturation.load()) / 100.0f,
                              seed,
                              s.post_split.load() ? 1u : 0u};
        Dispatch(cmdbuf, *deband_pipeline, last_view, mid_view, &push, sizeof(push));
        const vk::MemoryBarrier deband_done{.srcAccessMask = vk::AccessFlagBits::eShaderWrite,
                                            .dstAccessMask = vk::AccessFlagBits::eShaderRead};
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                               vk::PipelineStageFlagBits::eComputeShader,
                               vk::DependencyFlagBits::eByRegion, deband_done, {}, {});
        last_view = mid_view;
        output = mid_image;
    }
    if (sharpen_on) {
        const SharpenPush push{{float(w), float(h)}, float(s.post_sharpen.load()) / 100.0f, seed};
        Dispatch(cmdbuf, *sharpen_pipeline, last_view, out_view, &push, sizeof(push));
        output = out_image;
    }
    // The presenter blits the result into the swapchain right after this.
    const vk::MemoryBarrier write_done{.srcAccessMask = vk::AccessFlagBits::eShaderWrite,
                                       .dstAccessMask = vk::AccessFlagBits::eTransferRead};
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                           vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
                           write_done, {}, {});
}

vk::Image OutputImage() {
    return output;
}

} // namespace BbPost
