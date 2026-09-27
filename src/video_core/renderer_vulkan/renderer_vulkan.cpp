// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/memory_detect.h"
#include "common/microprofile.h"
#include "common/settings.h"
#include "core/3ds.h"
#include "core/core.h"
#include "core/frontend/emu_window.h"
#include "video_core/gpu.h"
#include "video_core/overlay.h"
#include "video_core/pica/pica_core.h"
#include "video_core/renderer_vulkan/renderer_vulkan.h"
#include "video_core/renderer_vulkan/vk_memory_util.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

#include "video_core/host_shaders/vulkan_present_anaglyph_frag.h"
#include "video_core/host_shaders/vulkan_present_frag.h"
#include "video_core/host_shaders/vulkan_present_interlaced_frag.h"
#include "video_core/host_shaders/vulkan_present_vert.h"

#include "video_core/host_shaders/vulkan_cursor_frag.h"
#include "video_core/host_shaders/vulkan_cursor_vert.h"
#include "video_core/host_shaders/vulkan_overlay_frag.h"
#include "video_core/host_shaders/vulkan_overlay_vert.h"
#include "video_core/renderer_vulkan/overlay_font.h"

#include <algorithm>
#include <cctype>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

#include <vk_mem_alloc.h>
#if defined(__APPLE__) && !defined(HAVE_LIBRETRO)
#include "common/apple_utils.h"
#endif

#ifdef ENABLE_SDL2
#include <SDL.h>
#endif

MICROPROFILE_DEFINE(Vulkan_RenderFrame, "Vulkan", "Render Frame", MP_RGB(128, 128, 64));

namespace Vulkan {

struct ScreenRectVertex {
    ScreenRectVertex() = default;
    ScreenRectVertex(float x, float y, float u, float v)
        : position{Common::MakeVec(x, y)}, tex_coord{Common::MakeVec(u, v)} {}

    Common::Vec2f position;
    Common::Vec2f tex_coord;
};

constexpr u32 VERTEX_BUFFER_SIZE = sizeof(ScreenRectVertex) * 8192;

// The on-screen overlay gets its own ring, kept separate from the present vertex_buffer above.
// Sharing a ring causes memory exhaustion and thus the menu to overwrite its own tail,
// causing a crash.
constexpr u32 OVERLAY_VERTEX_BUFFER_SIZE = sizeof(float) * 4 * 32768;

constexpr std::array<f32, 4 * 4> MakeOrthographicMatrix(u32 width, u32 height) {
    // clang-format off
    return { 2.f / width, 0.f,         0.f, -1.f,
            0.f,         2.f / height, 0.f, -1.f,
            0.f,         0.f,          1.f,  0.f,
            0.f,         0.f,          0.f,  1.f};
    // clang-format on
}

constexpr static std::array<vk::DescriptorSetLayoutBinding, 1> PRESENT_BINDINGS = {{
    {0, vk::DescriptorType::eCombinedImageSampler, 3, vk::ShaderStageFlagBits::eFragment},
}};

namespace {
static bool IsLowRefreshRate() {
#if (defined(__APPLE__) || defined(ENABLE_SDL2)) && !defined(HAVE_LIBRETRO)
    if (!Settings::values.use_display_refresh_rate_detection) {
        LOG_INFO(Render_Vulkan, "Refresh rate detection is currently disabled via settings");
        return false;
    }
#ifdef __APPLE__
    // Apple's low power mode sometimes limits applications to 30fps without changing the refresh
    // rate, meaning the above code doesn't catch it.
    if (AppleUtils::IsLowPowerModeEnabled()) {
        LOG_WARNING(Render_Vulkan, "Apple's low power mode is enabled, assuming low application "
                                   "framerate. FIFO will be disabled");
        return true;
    }

    const auto cur_refresh_rate = AppleUtils::GetRefreshRate();
#elif defined(ENABLE_SDL2)
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
        LOG_ERROR(Render_Vulkan, "Attempted to check refresh rate via SDL, but failed because "
                                 "SDL_INIT_VIDEO wasn't initialized");
        return false;
    }

    SDL_DisplayMode cur_display_mode;
    SDL_GetCurrentDisplayMode(0, &cur_display_mode); // TODO: Multimonitor handling. -OS

    const auto cur_refresh_rate = cur_display_mode.refresh_rate;
#endif // ENABLE_SDL2

    if (cur_refresh_rate < SCREEN_REFRESH_RATE) {
        LOG_WARNING(Render_Vulkan,
                    "Detected refresh rate lower than the emulated 3DS screen: {}hz. FIFO will "
                    "be disabled",
                    cur_refresh_rate);
        return true;
    } else {
        LOG_INFO(Render_Vulkan, "Refresh rate is above emulated 3DS screen: {}hz. Good.",
                 cur_refresh_rate);
    }
#endif // (defined(__APPLE__) || defined(ENABLE_SDL2)) && !defined(HAVE_LIBRETRO)

    // We have no available method of checking refresh rate. Just assume that everything is fine :)
    return false;
}
} // Anonymous namespace

RendererVulkan::RendererVulkan(Core::System& system, Pica::PicaCore& pica_,
                               Frontend::EmuWindow& window, Frontend::EmuWindow* secondary_window)
    : RendererBase{system, window, secondary_window}, memory{system.Memory()}, pica{pica_},
      instance{window, Settings::values.physical_device.GetValue()}, scheduler{instance},
      renderpass_cache{instance, scheduler},
      main_present_window{window, instance, scheduler, IsLowRefreshRate()},
      vertex_buffer{instance, scheduler, vk::BufferUsageFlagBits::eVertexBuffer,
                    VERTEX_BUFFER_SIZE},
      overlay_vertex_buffer{instance, scheduler, vk::BufferUsageFlagBits::eVertexBuffer,
                            OVERLAY_VERTEX_BUFFER_SIZE},
      update_queue{instance}, rasterizer{memory,
                                         pica,
                                         system.CustomTexManager(),
                                         *this,
                                         render_window,
                                         instance,
                                         scheduler,
                                         renderpass_cache,
                                         update_queue,
                                         main_present_window.ImageCount()},
      present_heap{instance, scheduler.GetMasterSemaphore(), PRESENT_BINDINGS, 32} {
    CompileShaders();
    CreateOverlayFont();
    BuildLayouts();
    BuildPipelines();
    if (secondary_window) {
        secondary_present_window_ptr = std::make_unique<PresentWindow>(
            *secondary_window, instance, scheduler, IsLowRefreshRate());
    }
#ifdef ENABLE_LSFG
    main_present_window.SetOverlayRecorder(this);
#endif
}

RendererVulkan::~RendererVulkan() {
    vk::Device device = instance.GetDevice();
    scheduler.Finish();
    main_present_window.WaitPresent();
#ifdef ENABLE_LSFG
    main_present_window.SetOverlayRecorder(nullptr);
#endif
    device.waitIdle();

    device.destroyShaderModule(present_vertex_shader);
    for (u32 i = 0; i < PRESENT_PIPELINES; i++) {
        device.destroyPipeline(present_pipelines[i]);
        device.destroyShaderModule(present_shaders[i]);
    }

    for (auto& sampler : present_samplers) {
        device.destroySampler(sampler);
    }

    for (auto& info : screen_infos) {
        device.destroyImageView(info.texture.image_view);
        vmaDestroyImage(instance.GetAllocator(), info.texture.image, info.texture.allocation);
    }

    device.destroyPipeline(cursor_pipeline);
    device.destroyShaderModule(cursor_vertex_shader);
    device.destroyShaderModule(cursor_fragment_shader);

    device.destroyPipeline(overlay_pipeline);
    device.destroyShaderModule(overlay_vertex_shader);
    device.destroyShaderModule(overlay_fragment_shader);
    device.destroySampler(overlay_font_sampler);
    device.destroyImageView(overlay_font_view);
    vmaDestroyImage(instance.GetAllocator(), overlay_font_image, overlay_font_allocation);
}

std::array<bool, 3> RendererVulkan::GetPresentedScreens() const {
    // The screenshot pass runs off its own layout, so give it everything.
    if (settings.screenshot_requested.load(std::memory_order_relaxed)) {
        return {true, true, true};
    }

    std::array<bool, 3> presented{};
    const auto mark = [&presented](const Layout::FramebufferLayout& layout) {
        if (layout.top_screen_enabled) {
            if (layout.render_3d_mode == Settings::StereoRenderOption::Off) {
                const u32 eye = static_cast<u32>(Settings::values.mono_render_option.GetValue());
                presented[eye] = true;
            } else {
                presented[0] = presented[1] = true;
            }
        }
        presented[2] |= layout.bottom_screen_enabled;
    };

    mark(render_window.GetFramebufferLayout());
    if (secondary_window) {
        mark(secondary_window->GetFramebufferLayout());
    }
    return presented;
}

void RendererVulkan::PrepareRendertarget() {
    const auto& framebuffer_config = pica.regs.framebuffer_config;
    const auto& regs_lcd = pica.regs_lcd;
    const auto presented = GetPresentedScreens();
    for (u32 i = 0; i < 3; i++) {
        const u32 fb_id = i == 2 ? 1 : 0;
        const auto& framebuffer = framebuffer_config[fb_id];
        auto& texture = screen_infos[i].texture;

        // A hidden screen still has to hold a live view for the present descriptor set, so point it
        // back at its own texture; the cache surface it was showing may since have been retired.
        if (!presented[i] && texture.image_view) {
            screen_infos[i].image_view = texture.image_view;
            continue;
        }

        const auto color_fill = fb_id == 0 ? regs_lcd.color_fill_top : regs_lcd.color_fill_bottom;
        if (color_fill.is_enabled) {
            screen_infos[i].image_view = texture.image_view;
            FillScreen(color_fill.AsVector(), texture);
            continue;
        }

        if (texture.width != framebuffer.width || texture.height != framebuffer.height ||
            texture.format != framebuffer.color_format) {
            ConfigureFramebufferTexture(texture, framebuffer);
        }

        LoadFBToScreenInfo(framebuffer, screen_infos[i], i == 1);
    }
}

void RendererVulkan::PrepareDraw(Frame* frame, const Layout::FramebufferLayout& layout) {
    const auto sampler = present_samplers[!Settings::values.filter_mode.GetValue()];
    const auto present_set = present_heap.Commit();
    for (u32 index = 0; index < screen_infos.size(); index++) {
        update_queue.AddImageSampler(present_set, 0, index, screen_infos[index].image_view,
                                     sampler);
    }

    renderpass_cache.EndRendering();
    scheduler.Record([this, layout, frame, present_set,
                      renderpass = main_present_window.Renderpass(),
                      index = current_pipeline](vk::CommandBuffer cmdbuf) {
        const vk::Viewport viewport = {
            .x = 0.0f,
            .y = 0.0f,
            .width = static_cast<float>(layout.width),
            .height = static_cast<float>(layout.height),
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };

        const vk::Rect2D scissor = {
            .offset = {0, 0},
            .extent = {layout.width, layout.height},
        };

        cmdbuf.setViewport(0, viewport);
        cmdbuf.setScissor(0, scissor);

        const vk::ClearValue clear{.color = clear_color};
        const vk::PipelineLayout layout{*present_pipeline_layout};
        const vk::RenderPassBeginInfo renderpass_begin_info = {
            .renderPass = renderpass,
            .framebuffer = frame->framebuffer,
            .renderArea =
                vk::Rect2D{
                    .offset = {0, 0},
                    .extent = {frame->width, frame->height},
                },
            .clearValueCount = 1,
            .pClearValues = &clear,
        };

        cmdbuf.beginRenderPass(renderpass_begin_info, vk::SubpassContents::eInline);
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, present_pipelines[index]);
        cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 0, present_set, {});
    });
}

namespace {
Layout::FramebufferLayout ScaleLayoutToNative(const Layout::FramebufferLayout& layout,
                                              u32 resolution_scale) {
    const u32 native_top = layout.is_rotated ? Core::kScreenTopWidth : Core::kScreenTopHeight;
    const u32 native_bottom =
        layout.is_rotated ? Core::kScreenBottomWidth : Core::kScreenBottomHeight;

    const auto needed = [resolution_scale](const Common::Rectangle<u32>& rect, u32 native) {
        const u32 width = rect.GetWidth();
        return width == 0
                   ? 0.0f
                   : static_cast<float>(native * resolution_scale) / static_cast<float>(width);
    };

    float scale = 0.0f;
    if (layout.top_screen_enabled) {
        scale = std::max(scale, needed(layout.top_screen, native_top));
    }
    if (layout.bottom_screen_enabled) {
        scale = std::max(scale, needed(layout.bottom_screen, native_bottom));
    }
    if (layout.additional_screen_enabled) {
        scale = std::max(scale,
                         needed(layout.additional_screen,
                                layout.additional_screen_is_bottom ? native_bottom : native_top));
    }

    if (!(scale > 0.0f) || scale >= 1.0f) {
        return layout;
    }

    const u32 width = std::max(1u, static_cast<u32>(std::lround(layout.width * scale)));
    const u32 height = std::max(1u, static_cast<u32>(std::lround(layout.height * scale)));

    const auto coord = [&](u32 value, u32 from, u32 to) {
        return value >= from ? to : static_cast<u32>(std::lround(value * scale));
    };
    const auto scale_rect = [&](const Common::Rectangle<u32>& rect) {
        return Common::Rectangle<u32>{
            coord(rect.left, layout.width, width),
            coord(rect.top, layout.height, height),
            coord(rect.right, layout.width, width),
            coord(rect.bottom, layout.height, height),
        };
    };

    Layout::FramebufferLayout reduced = layout;
    reduced.width = width;
    reduced.height = height;
    reduced.top_screen = scale_rect(layout.top_screen);
    reduced.bottom_screen = scale_rect(layout.bottom_screen);
    reduced.additional_screen = scale_rect(layout.additional_screen);
    reduced.cardboard.top_screen_right_eye =
        static_cast<u32>(std::lround(layout.cardboard.top_screen_right_eye * scale));
    reduced.cardboard.bottom_screen_right_eye =
        static_cast<u32>(std::lround(layout.cardboard.bottom_screen_right_eye * scale));
    reduced.cardboard.user_x_shift =
        static_cast<s32>(std::lround(layout.cardboard.user_x_shift * scale));
    return reduced;
}
} // Anonymous namespace

void RendererVulkan::RenderToWindow(PresentWindow& window, const Layout::FramebufferLayout& layout,
                                    bool flipped) {
    const bool skip_duplicates = Settings::values.use_skip_duplicate_frames.GetValue()
#ifdef ENABLE_LSFG
                                 || Settings::values.use_frame_generation.GetValue()
#endif
        ;
    if (!skip_duplicates || Core::PerfStats::game_frames_updated) {
        Frame* frame = window.GetRenderFrame();

        Layout::FramebufferLayout compose_layout = layout;
#ifdef ENABLE_LSFG
        frame->overlays_deferred = false;
        if (&window == &main_present_window) {
            frame->frame_gen = window.ClassifyFrameGeneration();
            if (frame->frame_gen.state == VideoCore::FrameGenerationState::Active &&
                window.CanScalePresent()) {
                compose_layout = ScaleLayoutToNative(layout, GetResolutionScaleFactor());
                frame->overlays_deferred =
                    compose_layout.width != layout.width || compose_layout.height != layout.height;
            }
        } else {
            frame->frame_gen = {};
        }
#endif

        if (compose_layout.width != frame->width || compose_layout.height != frame->height) {
            window.WaitPresent();
            scheduler.Finish();
            window.RecreateFrame(frame, compose_layout.width, compose_layout.height);
        }
        frame->present_width = layout.width;
        frame->present_height = layout.height;

        clear_color.float32[0] = Settings::values.bg_red.GetValue();
        clear_color.float32[1] = Settings::values.bg_green.GetValue();
        clear_color.float32[2] = Settings::values.bg_blue.GetValue();
        clear_color.float32[3] = 1.0f;

        DrawScreens(frame, compose_layout, layout, flipped);
        scheduler.Flush(frame->render_ready);
        window.Present(frame);
        if ((secondaryWindowEnabled && isSecondaryWindow) || (!secondaryWindowEnabled)) {
            Core::PerfStats::game_frames_updated = false;
            screenRendered = true;
        }
    }
}

void RendererVulkan::LoadFBToScreenInfo(const Pica::FramebufferConfig& framebuffer,
                                        ScreenInfo& screen_info, bool right_eye) {

    if (framebuffer.address_right1 == 0 || framebuffer.address_right2 == 0) {
        right_eye = false;
    }

    const PAddr framebuffer_addr =
        framebuffer.active_fb == 0
            ? (right_eye ? framebuffer.address_right1 : framebuffer.address_left1)
            : (right_eye ? framebuffer.address_right2 : framebuffer.address_left2);

    LOG_TRACE(Render_Vulkan, "0x{:08x} bytes from 0x{:08x}({}x{}), fmt {:x}",
              framebuffer.stride * framebuffer.height, framebuffer_addr, framebuffer.width.Value(),
              framebuffer.height.Value(), framebuffer.format);

    const u32 bpp = Pica::BytesPerPixel(framebuffer.color_format);
    const std::size_t pixel_stride = framebuffer.stride / bpp;

    ASSERT(pixel_stride * bpp == framebuffer.stride);
    ASSERT(pixel_stride % 4 == 0);

    if (!rasterizer.AccelerateDisplay(framebuffer, framebuffer_addr, static_cast<u32>(pixel_stride),
                                      screen_info)) {
        // Reset the screen info's display texture to its own permanent texture
        screen_info.image_view = screen_info.texture.image_view;
        screen_info.texcoords = {0.f, 0.f, 1.f, 1.f};

        ASSERT(false);
    }
}

void RendererVulkan::CompileShaders() {
    const vk::Device device = instance.GetDevice();
    const std::string_view preamble =
        instance.IsImageArrayDynamicIndexSupported() ? "#define ARRAY_DYNAMIC_INDEX" : "";
    present_vertex_shader =
        Compile(HostShaders::VULKAN_PRESENT_VERT, vk::ShaderStageFlagBits::eVertex, device);
    present_shaders[0] = Compile(HostShaders::VULKAN_PRESENT_FRAG,
                                 vk::ShaderStageFlagBits::eFragment, device, preamble);
    present_shaders[1] = Compile(HostShaders::VULKAN_PRESENT_ANAGLYPH_FRAG,
                                 vk::ShaderStageFlagBits::eFragment, device, preamble);
    present_shaders[2] = Compile(HostShaders::VULKAN_PRESENT_INTERLACED_FRAG,
                                 vk::ShaderStageFlagBits::eFragment, device, preamble);

    cursor_vertex_shader =
        Compile(HostShaders::VULKAN_CURSOR_VERT, vk::ShaderStageFlagBits::eVertex, device);
    cursor_fragment_shader =
        Compile(HostShaders::VULKAN_CURSOR_FRAG, vk::ShaderStageFlagBits::eFragment, device);

    overlay_vertex_shader =
        Compile(HostShaders::VULKAN_OVERLAY_VERT, vk::ShaderStageFlagBits::eVertex, device);
    overlay_fragment_shader =
        Compile(HostShaders::VULKAN_OVERLAY_FRAG, vk::ShaderStageFlagBits::eFragment, device);

    for (std::size_t i = 0; i < present_samplers.size(); i++) {
        const bool linear = i == 0;
        const vk::Filter filter_mode = linear ? vk::Filter::eLinear : vk::Filter::eNearest;
        const vk::SamplerMipmapMode mipmap_mode =
            linear ? vk::SamplerMipmapMode::eLinear : vk::SamplerMipmapMode::eNearest;
        const vk::SamplerCreateInfo sampler_info = {
            .magFilter = filter_mode,
            .minFilter = filter_mode,
            .mipmapMode = mipmap_mode,
            .addressModeU = vk::SamplerAddressMode::eClampToEdge,
            .addressModeV = vk::SamplerAddressMode::eClampToEdge,
            .anisotropyEnable = VK_FALSE,
            .maxAnisotropy = 1.0f,
            .compareEnable = false,
            .compareOp = vk::CompareOp::eAlways,
            .minLod = 0.0f,
            .maxLod = 0.0f,
            .borderColor = vk::BorderColor::eIntOpaqueBlack,
            .unnormalizedCoordinates = false,
        };

        present_samplers[i] = device.createSampler(sampler_info);
    }
}

void RendererVulkan::CreateOverlayFont() {
    vk::Device device = instance.GetDevice();

    // R8 coverage atlas holding the glyphs.
    const vk::ImageCreateInfo image_info = {
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR8Unorm,
        .extent = {static_cast<u32>(OverlayFont::kAtlasWidth),
                   static_cast<u32>(OverlayFont::kAtlasHeight), 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
    };
    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };
    VkImage unsafe_image{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);
    VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                     &unsafe_image, &overlay_font_allocation, nullptr);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating overlay font atlas with error {}", result);
        UNREACHABLE();
    }
    overlay_font_image = vk::Image{unsafe_image};

    const vk::ImageViewCreateInfo view_info = {
        .image = overlay_font_image,
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR8Unorm,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    overlay_font_view = device.createImageView(view_info);

    const vk::SamplerCreateInfo sampler_info = {
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .anisotropyEnable = false,
        .compareEnable = false,
        .borderColor = vk::BorderColor::eFloatTransparentBlack,
        .unnormalizedCoordinates = false,
    };
    overlay_font_sampler = device.createSampler(sampler_info);

    const vk::DeviceSize atlas_size = sizeof(OverlayFont::kAtlas);
    const vk::BufferCreateInfo staging_info = {
        .size = atlas_size,
        .usage = vk::BufferUsageFlagBits::eTransferSrc,
    };
    const VmaAllocationCreateInfo staging_alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                 VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
    };
    VkBuffer unsafe_staging{};
    VmaAllocation staging_allocation{};
    VmaAllocationInfo staging_mapped{};
    VkBufferCreateInfo unsafe_staging_info = static_cast<VkBufferCreateInfo>(staging_info);
    result = vmaCreateBuffer(instance.GetAllocator(), &unsafe_staging_info, &staging_alloc_info,
                             &unsafe_staging, &staging_allocation, &staging_mapped);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating overlay font staging buffer with error {}",
                     result);
        UNREACHABLE();
    }
    std::memcpy(staging_mapped.pMappedData, OverlayFont::kAtlas, atlas_size);
    vk::Buffer staging_buffer{unsafe_staging};

    renderpass_cache.EndRendering();
    scheduler.Record([image = overlay_font_image, staging_buffer,
                      width = static_cast<u32>(OverlayFont::kAtlasWidth),
                      height = static_cast<u32>(OverlayFont::kAtlasHeight)](
                         vk::CommandBuffer cmdbuf) {
        const vk::ImageSubresourceRange range = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        };
        const vk::ImageMemoryBarrier to_transfer = {
            .srcAccessMask = vk::AccessFlagBits::eNone,
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                               vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, to_transfer);

        const vk::BufferImageCopy copy = {
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, 1},
        };
        cmdbuf.copyBufferToImage(staging_buffer, image, vk::ImageLayout::eTransferDstOptimal, copy);

        const vk::ImageMemoryBarrier to_shader = {
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eShaderRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eFragmentShader, {}, {}, {}, to_shader);
    });
    scheduler.Finish();
    vmaDestroyBuffer(instance.GetAllocator(), staging_buffer, staging_allocation);

    // A persistent descriptor set bound to the atlas.
    const vk::DescriptorSetLayoutBinding binding = {
        .binding = 0,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
    };
    overlay_descriptor_layout = device.createDescriptorSetLayoutUnique({
        .bindingCount = 1,
        .pBindings = &binding,
    });
    const vk::DescriptorPoolSize pool_size = {
        .type = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = 1,
    };
    overlay_descriptor_pool = device.createDescriptorPoolUnique({
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    });
    const vk::DescriptorSetLayout set_layout = *overlay_descriptor_layout;
    overlay_descriptor_set = device.allocateDescriptorSets({
        .descriptorPool = *overlay_descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &set_layout,
    })[0];

    const vk::DescriptorImageInfo image_desc = {
        .sampler = overlay_font_sampler,
        .imageView = overlay_font_view,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    const vk::WriteDescriptorSet write = {
        .dstSet = overlay_descriptor_set,
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .pImageInfo = &image_desc,
    };
    device.updateDescriptorSets(write, {});
}

void RendererVulkan::BuildLayouts() {
    const vk::PushConstantRange push_range = {
        .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = sizeof(PresentUniformData),
    };

    const auto descriptor_set_layout = present_heap.Layout();
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1,
        .pSetLayouts = &descriptor_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
    };
    present_pipeline_layout = instance.GetDevice().createPipelineLayoutUnique(layout_info);

    const vk::PipelineLayoutCreateInfo cursor_layout_info = {};
    cursor_pipeline_layout = instance.GetDevice().createPipelineLayoutUnique(cursor_layout_info);

    // The overlay samples the font atlas and takes a per-batch tint via push constant.
    const vk::PushConstantRange overlay_push_range = {
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = sizeof(float) * 4,
    };
    const vk::DescriptorSetLayout overlay_set_layout = *overlay_descriptor_layout;
    const vk::PipelineLayoutCreateInfo overlay_layout_info = {
        .setLayoutCount = 1,
        .pSetLayouts = &overlay_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &overlay_push_range,
    };
    overlay_pipeline_layout =
        instance.GetDevice().createPipelineLayoutUnique(overlay_layout_info);
}

void RendererVulkan::BuildPipelines() {
    const vk::VertexInputBindingDescription binding = {
        .binding = 0,
        .stride = sizeof(ScreenRectVertex),
        .inputRate = vk::VertexInputRate::eVertex,
    };

    const std::array attributes = {
        vk::VertexInputAttributeDescription{
            .location = 0,
            .binding = 0,
            .format = vk::Format::eR32G32Sfloat,
            .offset = offsetof(ScreenRectVertex, position),
        },
        vk::VertexInputAttributeDescription{
            .location = 1,
            .binding = 0,
            .format = vk::Format::eR32G32Sfloat,
            .offset = offsetof(ScreenRectVertex, tex_coord),
        },
    };

    const vk::PipelineVertexInputStateCreateInfo vertex_input_info = {
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = static_cast<u32>(attributes.size()),
        .pVertexAttributeDescriptions = attributes.data(),
    };

    const vk::PipelineInputAssemblyStateCreateInfo input_assembly = {
        .topology = vk::PrimitiveTopology::eTriangleStrip,
        .primitiveRestartEnable = false,
    };

    const vk::PipelineRasterizationStateCreateInfo raster_state = {
        .depthClampEnable = false,
        .rasterizerDiscardEnable = false,
        .cullMode = vk::CullModeFlagBits::eNone,
        .frontFace = vk::FrontFace::eClockwise,
        .depthBiasEnable = false,
        .lineWidth = 1.0f,
    };

    const vk::PipelineMultisampleStateCreateInfo multisampling = {
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
        .sampleShadingEnable = false,
    };

    const vk::PipelineColorBlendAttachmentState colorblend_attachment = {
        .blendEnable = true,
        .srcColorBlendFactor = vk::BlendFactor::eConstantAlpha,
        .dstColorBlendFactor = vk::BlendFactor::eOneMinusConstantAlpha,
        .colorBlendOp = vk::BlendOp::eAdd,
        .srcAlphaBlendFactor = vk::BlendFactor::eConstantAlpha,
        .dstAlphaBlendFactor = vk::BlendFactor::eOneMinusConstantAlpha,
        .alphaBlendOp = vk::BlendOp::eAdd,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };

    const vk::PipelineColorBlendStateCreateInfo color_blending = {
        .logicOpEnable = false,
        .attachmentCount = 1,
        .pAttachments = &colorblend_attachment,
    };

    const vk::Viewport placeholder_viewport = vk::Viewport{0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
    const vk::Rect2D placeholder_scissor = vk::Rect2D{{0, 0}, {1, 1}};
    const vk::PipelineViewportStateCreateInfo viewport_info = {
        .viewportCount = 1,
        .pViewports = &placeholder_viewport,
        .scissorCount = 1,
        .pScissors = &placeholder_scissor,
    };

    const std::array dynamic_states = {
        vk::DynamicState::eBlendConstants,
        vk::DynamicState::eViewport,
        vk::DynamicState::eScissor,
    };

    const vk::PipelineDynamicStateCreateInfo dynamic_info = {
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };

    const vk::PipelineDepthStencilStateCreateInfo depth_info = {
        .depthTestEnable = false,
        .depthWriteEnable = false,
        .depthCompareOp = vk::CompareOp::eAlways,
        .depthBoundsTestEnable = false,
        .stencilTestEnable = false,
    };

    for (u32 i = 0; i < PRESENT_PIPELINES; i++) {
        const std::array shader_stages = {
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eVertex,
                .module = present_vertex_shader,
                .pName = "main",
            },
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eFragment,
                .module = present_shaders[i],
                .pName = "main",
            },
        };

        const vk::GraphicsPipelineCreateInfo pipeline_info = {
            .stageCount = static_cast<u32>(shader_stages.size()),
            .pStages = shader_stages.data(),
            .pVertexInputState = &vertex_input_info,
            .pInputAssemblyState = &input_assembly,
            .pViewportState = &viewport_info,
            .pRasterizationState = &raster_state,
            .pMultisampleState = &multisampling,
            .pDepthStencilState = &depth_info,
            .pColorBlendState = &color_blending,
            .pDynamicState = &dynamic_info,
            .layout = *present_pipeline_layout,
            .renderPass = main_present_window.Renderpass(),
        };

        const auto [result, pipeline] =
            instance.GetDevice().createGraphicsPipeline({}, pipeline_info);
        ASSERT_MSG(result == vk::Result::eSuccess, "Unable to build present pipelines");
        present_pipelines[i] = pipeline;
    }

    // Build cursor pipeline (simple position-only, inverted color blending)
    {
        const vk::VertexInputBindingDescription cursor_binding = {
            .binding = 0,
            .stride = sizeof(float) * 2,
            .inputRate = vk::VertexInputRate::eVertex,
        };

        const vk::VertexInputAttributeDescription cursor_attribute = {
            .location = 0,
            .binding = 0,
            .format = vk::Format::eR32G32Sfloat,
            .offset = 0,
        };

        const vk::PipelineVertexInputStateCreateInfo cursor_vertex_input = {
            .vertexBindingDescriptionCount = 1,
            .pVertexBindingDescriptions = &cursor_binding,
            .vertexAttributeDescriptionCount = 1,
            .pVertexAttributeDescriptions = &cursor_attribute,
        };

        const vk::PipelineInputAssemblyStateCreateInfo cursor_input_assembly = {
            .topology = vk::PrimitiveTopology::eTriangleList,
            .primitiveRestartEnable = false,
        };

        const vk::PipelineRasterizationStateCreateInfo cursor_raster = {
            .depthClampEnable = false,
            .rasterizerDiscardEnable = false,
            .cullMode = vk::CullModeFlagBits::eNone,
            .frontFace = vk::FrontFace::eClockwise,
            .depthBiasEnable = false,
            .lineWidth = 1.0f,
        };

        const vk::PipelineMultisampleStateCreateInfo cursor_multisample = {
            .rasterizationSamples = vk::SampleCountFlagBits::e1,
            .sampleShadingEnable = false,
        };

        const vk::PipelineColorBlendAttachmentState cursor_blend_attachment = {
            .blendEnable = true,
            .srcColorBlendFactor = vk::BlendFactor::eOneMinusDstColor,
            .dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcColor,
            .colorBlendOp = vk::BlendOp::eAdd,
            .srcAlphaBlendFactor = vk::BlendFactor::eOne,
            .dstAlphaBlendFactor = vk::BlendFactor::eZero,
            .alphaBlendOp = vk::BlendOp::eAdd,
            .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
        };

        const vk::PipelineColorBlendStateCreateInfo cursor_color_blending = {
            .logicOpEnable = false,
            .attachmentCount = 1,
            .pAttachments = &cursor_blend_attachment,
        };

        const vk::Viewport placeholder_vp = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
        const vk::Rect2D placeholder_sc = {{0, 0}, {1, 1}};
        const vk::PipelineViewportStateCreateInfo cursor_viewport = {
            .viewportCount = 1,
            .pViewports = &placeholder_vp,
            .scissorCount = 1,
            .pScissors = &placeholder_sc,
        };

        const std::array cursor_dynamic_states = {
            vk::DynamicState::eViewport,
            vk::DynamicState::eScissor,
        };

        const vk::PipelineDynamicStateCreateInfo cursor_dynamic = {
            .dynamicStateCount = static_cast<u32>(cursor_dynamic_states.size()),
            .pDynamicStates = cursor_dynamic_states.data(),
        };

        const vk::PipelineDepthStencilStateCreateInfo cursor_depth = {
            .depthTestEnable = false,
            .depthWriteEnable = false,
            .depthCompareOp = vk::CompareOp::eAlways,
            .depthBoundsTestEnable = false,
            .stencilTestEnable = false,
        };

        const std::array cursor_shader_stages = {
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eVertex,
                .module = cursor_vertex_shader,
                .pName = "main",
            },
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eFragment,
                .module = cursor_fragment_shader,
                .pName = "main",
            },
        };

        const vk::GraphicsPipelineCreateInfo cursor_pipeline_info = {
            .stageCount = static_cast<u32>(cursor_shader_stages.size()),
            .pStages = cursor_shader_stages.data(),
            .pVertexInputState = &cursor_vertex_input,
            .pInputAssemblyState = &cursor_input_assembly,
            .pViewportState = &cursor_viewport,
            .pRasterizationState = &cursor_raster,
            .pMultisampleState = &cursor_multisample,
            .pDepthStencilState = &cursor_depth,
            .pColorBlendState = &cursor_color_blending,
            .pDynamicState = &cursor_dynamic,
            .layout = *cursor_pipeline_layout,
            .renderPass = main_present_window.Renderpass(),
        };

        const auto [result, pipeline] =
            instance.GetDevice().createGraphicsPipeline({}, cursor_pipeline_info);
        ASSERT_MSG(result == vk::Result::eSuccess, "Unable to build cursor pipeline");
        cursor_pipeline = pipeline;
    }

    // Build the FPS overlay pipeline.
    {
        const vk::VertexInputBindingDescription overlay_binding = {
            .binding = 0,
            .stride = sizeof(float) * 4,
            .inputRate = vk::VertexInputRate::eVertex,
        };

        const std::array overlay_attributes = {
            vk::VertexInputAttributeDescription{
                .location = 0,
                .binding = 0,
                .format = vk::Format::eR32G32Sfloat,
                .offset = 0,
            },
            vk::VertexInputAttributeDescription{
                .location = 1,
                .binding = 0,
                .format = vk::Format::eR32G32Sfloat,
                .offset = sizeof(float) * 2,
            },
        };

        const vk::PipelineVertexInputStateCreateInfo overlay_vertex_input = {
            .vertexBindingDescriptionCount = 1,
            .pVertexBindingDescriptions = &overlay_binding,
            .vertexAttributeDescriptionCount = static_cast<u32>(overlay_attributes.size()),
            .pVertexAttributeDescriptions = overlay_attributes.data(),
        };

        const vk::PipelineInputAssemblyStateCreateInfo overlay_input_assembly = {
            .topology = vk::PrimitiveTopology::eTriangleList,
            .primitiveRestartEnable = false,
        };

        const vk::PipelineRasterizationStateCreateInfo overlay_raster = {
            .depthClampEnable = false,
            .rasterizerDiscardEnable = false,
            .cullMode = vk::CullModeFlagBits::eNone,
            .frontFace = vk::FrontFace::eClockwise,
            .depthBiasEnable = false,
            .lineWidth = 1.0f,
        };

        const vk::PipelineMultisampleStateCreateInfo overlay_multisample = {
            .rasterizationSamples = vk::SampleCountFlagBits::e1,
            .sampleShadingEnable = false,
        };

        const vk::PipelineColorBlendAttachmentState overlay_blend_attachment = {
            .blendEnable = true,
            .srcColorBlendFactor = vk::BlendFactor::eSrcAlpha,
            .dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
            .colorBlendOp = vk::BlendOp::eAdd,
            .srcAlphaBlendFactor = vk::BlendFactor::eOne,
            .dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
            .alphaBlendOp = vk::BlendOp::eAdd,
            .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
        };

        const vk::PipelineColorBlendStateCreateInfo overlay_color_blending = {
            .logicOpEnable = false,
            .attachmentCount = 1,
            .pAttachments = &overlay_blend_attachment,
        };

        const vk::Viewport overlay_placeholder_vp = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
        const vk::Rect2D overlay_placeholder_sc = {{0, 0}, {1, 1}};
        const vk::PipelineViewportStateCreateInfo overlay_viewport = {
            .viewportCount = 1,
            .pViewports = &overlay_placeholder_vp,
            .scissorCount = 1,
            .pScissors = &overlay_placeholder_sc,
        };

        const std::array overlay_dynamic_states = {
            vk::DynamicState::eViewport,
            vk::DynamicState::eScissor,
        };

        const vk::PipelineDynamicStateCreateInfo overlay_dynamic = {
            .dynamicStateCount = static_cast<u32>(overlay_dynamic_states.size()),
            .pDynamicStates = overlay_dynamic_states.data(),
        };

        const vk::PipelineDepthStencilStateCreateInfo overlay_depth = {
            .depthTestEnable = false,
            .depthWriteEnable = false,
            .depthCompareOp = vk::CompareOp::eAlways,
            .depthBoundsTestEnable = false,
            .stencilTestEnable = false,
        };

        const std::array overlay_shader_stages = {
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eVertex,
                .module = overlay_vertex_shader,
                .pName = "main",
            },
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eFragment,
                .module = overlay_fragment_shader,
                .pName = "main",
            },
        };

        const vk::GraphicsPipelineCreateInfo overlay_pipeline_info = {
            .stageCount = static_cast<u32>(overlay_shader_stages.size()),
            .pStages = overlay_shader_stages.data(),
            .pVertexInputState = &overlay_vertex_input,
            .pInputAssemblyState = &overlay_input_assembly,
            .pViewportState = &overlay_viewport,
            .pRasterizationState = &overlay_raster,
            .pMultisampleState = &overlay_multisample,
            .pDepthStencilState = &overlay_depth,
            .pColorBlendState = &overlay_color_blending,
            .pDynamicState = &overlay_dynamic,
            .layout = *overlay_pipeline_layout,
            .renderPass = main_present_window.Renderpass(),
        };

        const auto [result, pipeline] =
            instance.GetDevice().createGraphicsPipeline({}, overlay_pipeline_info);
        ASSERT_MSG(result == vk::Result::eSuccess, "Unable to build overlay pipeline");
        overlay_pipeline = pipeline;
    }
}

void RendererVulkan::ConfigureFramebufferTexture(TextureInfo& texture,
                                                 const Pica::FramebufferConfig& framebuffer) {
    vk::Device device = instance.GetDevice();
    if (texture.image || texture.image_view) {
        // Queued frames still sample the old texture, so it cannot be freed until they are off the
        // GPU. PresentWindow::RecreateFrame does the same for the same reason.
        main_present_window.WaitPresent();
        scheduler.Finish();
    }
    if (texture.image_view) {
        device.destroyImageView(texture.image_view);
    }
    if (texture.image) {
        vmaDestroyImage(instance.GetAllocator(), texture.image, texture.allocation);
    }

    const VideoCore::PixelFormat pixel_format =
        VideoCore::PixelFormatFromGPUPixelFormat(framebuffer.color_format);
    const vk::Format format = instance.GetTraits(pixel_format).native;
    const vk::ImageCreateInfo image_info = {
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {framebuffer.width, framebuffer.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eSampled,
    };

    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkImage unsafe_image{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);

    VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                     &unsafe_image, &texture.allocation, nullptr);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}", result);
        UNREACHABLE();
    }
    texture.image = vk::Image{unsafe_image};

    const vk::ImageViewCreateInfo view_info = {
        .image = texture.image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    texture.image_view = device.createImageView(view_info);

    texture.width = framebuffer.width;
    texture.height = framebuffer.height;
    texture.format = framebuffer.color_format;
}

void RendererVulkan::FillScreen(Common::Vec3<u8> color, const TextureInfo& texture) {
    // When loading some 3GX extensions, FillScreen may be called before texture image is available
    if (!texture.image) {
        return;
    }

    const vk::ClearColorValue clear_color = {
        .float32 =
            std::array{
                color.r() / 255.0f,
                color.g() / 255.0f,
                color.b() / 255.0f,
                1.0f,
            },
    };

    renderpass_cache.EndRendering();
    scheduler.Record([image = texture.image, clear_color](vk::CommandBuffer cmdbuf) {
        const vk::ImageSubresourceRange range = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = VK_REMAINING_MIP_LEVELS,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        };

        const vk::ImageMemoryBarrier pre_barrier = {
            .srcAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eTransferRead,
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };

        const vk::ImageMemoryBarrier post_barrier = {
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eTransferRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader,
                               vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, {}, {}, pre_barrier);

        cmdbuf.clearColorImage(image, vk::ImageLayout::eTransferDstOptimal, clear_color, range);

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eFragmentShader,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barrier);
    });
}

void RendererVulkan::ReloadPipeline(Settings::StereoRenderOption render_3d) {
    switch (render_3d) {
    case Settings::StereoRenderOption::Anaglyph:
        current_pipeline = 1;
        break;
    case Settings::StereoRenderOption::Interlaced:
    case Settings::StereoRenderOption::ReverseInterlaced:
        current_pipeline = 2;
        draw_info.reverse_interlaced = render_3d == Settings::StereoRenderOption::ReverseInterlaced;
        break;
    default:
        current_pipeline = 0;
        break;
    }
}

void RendererVulkan::DrawSingleScreen(u32 screen_id, float x, float y, float w, float h,
                                      Layout::DisplayOrientation orientation) {
    const ScreenInfo& screen_info = screen_infos[screen_id];
    const auto& texcoords = screen_info.texcoords;

    std::array<ScreenRectVertex, 4> vertices;
    switch (orientation) {
    case Layout::DisplayOrientation::Landscape:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x + w, y, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x, y + h, texcoords.top, texcoords.left),
            ScreenRectVertex(x + w, y + h, texcoords.top, texcoords.right),
        }};
        break;
    case Layout::DisplayOrientation::Portrait:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x + w, y, texcoords.top, texcoords.right),
            ScreenRectVertex(x, y + h, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x + w, y + h, texcoords.top, texcoords.left),
        }};
        std::swap(h, w);
        break;
    case Layout::DisplayOrientation::LandscapeFlipped:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.top, texcoords.right),
            ScreenRectVertex(x + w, y, texcoords.top, texcoords.left),
            ScreenRectVertex(x, y + h, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x + w, y + h, texcoords.bottom, texcoords.left),
        }};
        break;
    case Layout::DisplayOrientation::PortraitFlipped:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.top, texcoords.left),
            ScreenRectVertex(x + w, y, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x, y + h, texcoords.top, texcoords.right),
            ScreenRectVertex(x + w, y + h, texcoords.bottom, texcoords.right),
        }};
        std::swap(h, w);
        break;
    default:
        LOG_ERROR(Render_Vulkan, "Unknown DisplayOrientation: {}", orientation);
        break;
    }

    const u64 size = sizeof(ScreenRectVertex) * vertices.size();
    auto [data, offset, invalidate] = vertex_buffer.Map(size, 16);
    std::memcpy(data, vertices.data(), size);
    vertex_buffer.Commit(size);

    const u32 scale_factor = GetResolutionScaleFactor();
    draw_info.i_resolution =
        Common::MakeVec(static_cast<f32>(screen_info.texture.width * scale_factor),
                        static_cast<f32>(screen_info.texture.height * scale_factor),
                        1.0f / static_cast<f32>(screen_info.texture.width * scale_factor),
                        1.0f / static_cast<f32>(screen_info.texture.height * scale_factor));
    draw_info.o_resolution = Common::MakeVec(h, w, 1.0f / h, 1.0f / w);
    draw_info.screen_id_l = screen_id;

    scheduler.Record([this, offset = offset, info = draw_info](vk::CommandBuffer cmdbuf) {
        const u32 first_vertex = static_cast<u32>(offset) / sizeof(ScreenRectVertex);
        cmdbuf.pushConstants(*present_pipeline_layout,
                             vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eVertex,
                             0, sizeof(info), &info);

        cmdbuf.bindVertexBuffers(0, vertex_buffer.Handle(), {0});
        cmdbuf.draw(4, 1, first_vertex, 0);
    });
}

void RendererVulkan::DrawSingleScreenStereo(u32 screen_id_l, u32 screen_id_r, float x, float y,
                                            float w, float h,
                                            Layout::DisplayOrientation orientation) {
    const ScreenInfo& screen_info_l = screen_infos[screen_id_l];
    const auto& texcoords = screen_info_l.texcoords;

    std::array<ScreenRectVertex, 4> vertices;
    switch (orientation) {
    case Layout::DisplayOrientation::Landscape:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x + w, y, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x, y + h, texcoords.top, texcoords.left),
            ScreenRectVertex(x + w, y + h, texcoords.top, texcoords.right),
        }};
        break;
    case Layout::DisplayOrientation::Portrait:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x + w, y, texcoords.top, texcoords.right),
            ScreenRectVertex(x, y + h, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x + w, y + h, texcoords.top, texcoords.left),
        }};
        std::swap(h, w);
        break;
    case Layout::DisplayOrientation::LandscapeFlipped:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.top, texcoords.right),
            ScreenRectVertex(x + w, y, texcoords.top, texcoords.left),
            ScreenRectVertex(x, y + h, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x + w, y + h, texcoords.bottom, texcoords.left),
        }};
        break;
    case Layout::DisplayOrientation::PortraitFlipped:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.top, texcoords.left),
            ScreenRectVertex(x + w, y, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x, y + h, texcoords.top, texcoords.right),
            ScreenRectVertex(x + w, y + h, texcoords.bottom, texcoords.right),
        }};
        std::swap(h, w);
        break;
    default:
        LOG_ERROR(Render_Vulkan, "Unknown DisplayOrientation: {}", orientation);
        break;
    }

    const u64 size = sizeof(ScreenRectVertex) * vertices.size();
    auto [data, offset, invalidate] = vertex_buffer.Map(size, 16);
    std::memcpy(data, vertices.data(), size);
    vertex_buffer.Commit(size);

    const u32 scale_factor = GetResolutionScaleFactor();
    draw_info.i_resolution =
        Common::MakeVec(static_cast<f32>(screen_info_l.texture.width * scale_factor),
                        static_cast<f32>(screen_info_l.texture.height * scale_factor),
                        1.0f / static_cast<f32>(screen_info_l.texture.width * scale_factor),
                        1.0f / static_cast<f32>(screen_info_l.texture.height * scale_factor));
    draw_info.o_resolution = Common::MakeVec(h, w, 1.0f / h, 1.0f / w);
    draw_info.screen_id_l = screen_id_l;
    draw_info.screen_id_r = screen_id_r;

    scheduler.Record([this, offset = offset, info = draw_info](vk::CommandBuffer cmdbuf) {
        const u32 first_vertex = static_cast<u32>(offset) / sizeof(ScreenRectVertex);
        cmdbuf.pushConstants(*present_pipeline_layout,
                             vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eVertex,
                             0, sizeof(info), &info);

        cmdbuf.bindVertexBuffers(0, vertex_buffer.Handle(), {0});
        cmdbuf.draw(4, 1, first_vertex, 0);
    });
}

void RendererVulkan::ApplySecondLayerOpacity(float alpha) {
    scheduler.Record([alpha](vk::CommandBuffer cmdbuf) {
        const std::array<float, 4> blend_constants = {0.0f, 0.0f, 0.0f, alpha};
        cmdbuf.setBlendConstants(blend_constants.data());
    });
}

void RendererVulkan::DrawTopScreen(const Layout::FramebufferLayout& layout,
                                   const Common::Rectangle<u32>& top_screen) {
    if (!layout.top_screen_enabled) {
        return;
    }
    int leftside, rightside;
    leftside = Settings::values.swap_eyes_3d.GetValue() ? 1 : 0;
    rightside = Settings::values.swap_eyes_3d.GetValue() ? 0 : 1;
    const float top_screen_left = static_cast<float>(top_screen.left);
    const float top_screen_top = static_cast<float>(top_screen.top);
    const float top_screen_width = static_cast<float>(top_screen.GetWidth());
    const float top_screen_height = static_cast<float>(top_screen.GetHeight());

    const auto orientation = layout.GetOrientation();
    switch (layout.render_3d_mode) {
    case Settings::StereoRenderOption::Off: {
        const int eye = static_cast<int>(Settings::values.mono_render_option.GetValue());
        DrawSingleScreen(eye, top_screen_left, top_screen_top, top_screen_width, top_screen_height,
                         orientation);
        break;
    }
    case Settings::StereoRenderOption::SideBySide: {
        DrawSingleScreen(leftside, top_screen_left / 2, top_screen_top, top_screen_width / 2,
                         top_screen_height, orientation);
        draw_info.layer = 1;
        DrawSingleScreen(rightside, static_cast<float>((top_screen_left / 2) + (layout.width / 2)),
                         top_screen_top, top_screen_width / 2, top_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::SideBySideFull: {
        DrawSingleScreen(leftside, top_screen_left, top_screen_top, top_screen_width,
                         top_screen_height, orientation);
        draw_info.layer = 1;
        DrawSingleScreen(rightside, top_screen_left + layout.width / 2, top_screen_top,
                         top_screen_width, top_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::CardboardVR: {
        DrawSingleScreen(leftside, top_screen_left, top_screen_top, top_screen_width,
                         top_screen_height, orientation);
        draw_info.layer = 1;
        DrawSingleScreen(
            rightside, top_screen_left - 2.0f * layout.cardboard.user_x_shift + (layout.width / 2),
            top_screen_top, top_screen_width, top_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::Anaglyph:
    case Settings::StereoRenderOption::Interlaced:
    case Settings::StereoRenderOption::ReverseInterlaced: {
        DrawSingleScreenStereo(leftside, rightside, top_screen_left, top_screen_top,
                               top_screen_width, top_screen_height, orientation);
        break;
    }
    }
}

void RendererVulkan::DrawBottomScreen(const Layout::FramebufferLayout& layout,
                                      const Common::Rectangle<u32>& bottom_screen) {
    if (!layout.bottom_screen_enabled) {
        return;
    }

    const float bottom_screen_left = static_cast<float>(bottom_screen.left);
    const float bottom_screen_top = static_cast<float>(bottom_screen.top);
    const float bottom_screen_width = static_cast<float>(bottom_screen.GetWidth());
    const float bottom_screen_height = static_cast<float>(bottom_screen.GetHeight());

    const auto orientation = layout.GetOrientation();

    switch (layout.render_3d_mode) {
    case Settings::StereoRenderOption::Off: {
        DrawSingleScreen(2, bottom_screen_left, bottom_screen_top, bottom_screen_width,
                         bottom_screen_height, orientation);

        break;
    }
    case Settings::StereoRenderOption::SideBySide: // Bottom screen is identical on both sides
    {
        DrawSingleScreen(2, bottom_screen_left / 2, bottom_screen_top, bottom_screen_width / 2,
                         bottom_screen_height, orientation);
        draw_info.layer = 1;
        DrawSingleScreen(2, static_cast<float>((bottom_screen_left / 2) + (layout.width / 2)),
                         bottom_screen_top, bottom_screen_width / 2, bottom_screen_height,
                         orientation);
        break;
    }
    case Settings::StereoRenderOption::SideBySideFull: {
        DrawSingleScreen(2, bottom_screen_left, bottom_screen_top, bottom_screen_width,
                         bottom_screen_height, orientation);
        draw_info.layer = 1;
        DrawSingleScreen(2, bottom_screen_left + layout.width / 2, bottom_screen_top,
                         bottom_screen_width, bottom_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::CardboardVR: {
        DrawSingleScreen(2, bottom_screen_left, bottom_screen_top, bottom_screen_width,
                         bottom_screen_height, orientation);
        draw_info.layer = 1;
        DrawSingleScreen(
            2, bottom_screen_left - 2.0f * layout.cardboard.user_x_shift + (layout.width / 2),
            bottom_screen_top, bottom_screen_width, bottom_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::Anaglyph:
    case Settings::StereoRenderOption::Interlaced:
    case Settings::StereoRenderOption::ReverseInterlaced: {
        DrawSingleScreenStereo(2, 2, bottom_screen_left, bottom_screen_top, bottom_screen_width,
                               bottom_screen_height, orientation);
        break;
    }
    }
}

void RendererVulkan::DrawScreens(Frame* frame, const Layout::FramebufferLayout& layout,
                                 const Layout::FramebufferLayout& overlay_layout, bool flipped) {
    if (settings.bg_color_update_requested.exchange(false)) {
        clear_color.float32[0] = Settings::values.bg_red.GetValue();
        clear_color.float32[1] = Settings::values.bg_green.GetValue();
        clear_color.float32[2] = Settings::values.bg_blue.GetValue();
    }
    if (settings.shader_update_requested.exchange(false)) {
        ReloadPipeline(layout.render_3d_mode);
    }

    // Build overlay geometry before the present render pass opens otherwise a flush
    // can happen mid build causing a crash.
    renderpass_cache.EndRendering();
#ifdef ENABLE_LSFG
    frame->overlay_offset = 0;
#endif
    OverlayDraw fps_overlay = PrepareFpsOverlay(overlay_layout, frame);
    OverlayDraw shader_notice = PrepareShaderNotice(overlay_layout, frame);
    OverlayDraw toast = PrepareToast(overlay_layout, frame);
    OverlayDraw quick_menu = PrepareQuickMenu(overlay_layout, frame);

    PrepareDraw(frame, layout);

    const auto& top_screen = layout.top_screen;
    const auto& bottom_screen = layout.bottom_screen;
    draw_info.modelview = MakeOrthographicMatrix(layout.width, layout.height);

    draw_info.layer = 0;

    // Apply the initial default opacity value; Needed to avoid flickering
    ApplySecondLayerOpacity(1.0f);

    if (!Settings::values.swap_screen.GetValue()) {
        DrawTopScreen(layout, top_screen);
        draw_info.layer = 0;
        if (layout.bottom_opacity < 1) {
            ApplySecondLayerOpacity(layout.bottom_opacity);
        }
        DrawBottomScreen(layout, bottom_screen);
    } else {
        DrawBottomScreen(layout, bottom_screen);
        draw_info.layer = 0;
        if (layout.top_opacity < 1) {
            ApplySecondLayerOpacity(layout.top_opacity);
        }
        DrawTopScreen(layout, top_screen);
    }

    if (layout.additional_screen_enabled) {
        const auto& additional_screen = layout.additional_screen;
        if (!layout.additional_screen_is_bottom) {
            DrawTopScreen(layout, additional_screen);
        } else {
            DrawBottomScreen(layout, additional_screen);
        }
    }

    DrawCursor(layout);

#ifdef ENABLE_LSFG
    if (frame->overlays_deferred) {
        frame->overlays[0] = std::move(fps_overlay);
        frame->overlays[1] = std::move(shader_notice);
        frame->overlays[2] = std::move(toast);
        frame->overlays[3] = std::move(quick_menu);
    } else
#endif
    {
        RecordOverlay(std::move(fps_overlay));
        RecordOverlay(std::move(shader_notice));
        RecordOverlay(std::move(toast));
        RecordOverlay(std::move(quick_menu));
    }

    scheduler.Record([](vk::CommandBuffer cmdbuf) { cmdbuf.endRenderPass(); });
}

void RendererVulkan::DrawCursor(const Layout::FramebufferLayout& layout) {
    const auto cursor = render_window.GetCursorInfo();
    if (!cursor.visible) {
        return;
    }

    const float buf_w = static_cast<float>(layout.width);
    const float buf_h = static_cast<float>(layout.height);

    const float abs_y = layout.bottom_screen.top + cursor.projected_y;
    const float cy = (abs_y / buf_h) * 2.0f - 1.0f;
    const float ratio = static_cast<float>(layout.bottom_screen.GetHeight()) / 30.0f;
    const float rw = ratio / buf_w;
    const float rh = ratio / buf_h;
    const float bt = (layout.bottom_screen.top / buf_h) * 2.0f - 1.0f;
    const float bb = (layout.bottom_screen.bottom / buf_h) * 2.0f - 1.0f;

    // One crosshair is 12 vertices. Labo VR gets a matching crosshair in the
    // right-eye copy so the pointer sits at a comfortable binocular depth.
    std::array<float, 48> vertices{};
    std::size_t vertex_data_size = 0;
    const auto append_crosshair = [&](float horizontal_offset) {
        const float abs_x = layout.bottom_screen.left + cursor.projected_x + horizontal_offset;
        const float cx = (abs_x / buf_w) * 2.0f - 1.0f;
        const float bl = ((layout.bottom_screen.left + horizontal_offset) / buf_w) * 2.0f - 1.0f;
        const float br = ((layout.bottom_screen.right + horizontal_offset) / buf_w) * 2.0f - 1.0f;

        const float vl = std::fmax(cx - rw / 5.0f, bl);
        const float vr = std::fmin(cx + rw / 5.0f, br);
        const float vt = std::fmax(cy - rh, bt);
        const float vb = std::fmin(cy + rh, bb);
        const float hl = std::fmax(cx - rw, bl);
        const float hr = std::fmin(cx + rw, br);
        const float ht = std::fmax(cy - rh / 5.0f, bt);
        const float hb = std::fmin(cy + rh / 5.0f, bb);

        const std::array<float, 24> crosshair{
            vl, vt, vr, vt, vr, vb, vl, vt, vr, vb, vl, vb,
            hl, ht, hr, ht, hr, hb, hl, ht, hr, hb, hl, hb,
        };
        std::copy(crosshair.begin(), crosshair.end(), vertices.begin() + vertex_data_size);
        vertex_data_size += crosshair.size();
    };

    append_crosshair(0.0f);
    if (layout.render_3d_mode == Settings::StereoRenderOption::CardboardVR) {
        append_crosshair(layout.width / 2.0f - 2.0f * layout.cardboard.user_x_shift);
    }

    const u64 size = vertex_data_size * sizeof(float);
    auto [data, offset, invalidate] = vertex_buffer.Map(size, 16);
    std::memcpy(data, vertices.data(), size);
    vertex_buffer.Commit(size);

    scheduler.Record([this, offset = offset, vertex_count = static_cast<u32>(vertex_data_size / 2),
                      pipeline = cursor_pipeline](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
        cmdbuf.bindVertexBuffers(0, vertex_buffer.Handle(), {0});
        const u32 first_vertex = static_cast<u32>(offset) / (sizeof(float) * 2);
        cmdbuf.draw(vertex_count, 1, first_vertex, 0);
    });
}

namespace {
struct OverlayCanvas {
    float width;
    float height;
    u32 rotation; // Degrees clockwise.

    // Font sizes key off the short edge so text keeps its size when the canvas turns.
    float ShortEdge() const {
        return std::min(width, height);
    }
};

OverlayCanvas MakeOverlayCanvas(const Layout::FramebufferLayout& layout) {
    const u32 rotation = VideoCore::GetOverlayRotation();
    const bool upright = rotation == 90 || rotation == 270;
    return {static_cast<float>(upright ? layout.height : layout.width),
            static_cast<float>(upright ? layout.width : layout.height), rotation};
}

// Builds overlay geometry in canvas pixels.
class OverlayBuilder {
public:
    OverlayBuilder(std::vector<float>& verts, const OverlayCanvas& canvas)
        : verts{verts}, canvas{canvas} {
        const bool upright = canvas.rotation == 90 || canvas.rotation == 270;
        inv_w = 2.0f / (upright ? canvas.height : canvas.width);
        inv_h = 2.0f / (upright ? canvas.width : canvas.height);
    }

    u32 VertexCount() const {
        return static_cast<u32>(verts.size() / kFloatsPerVertex);
    }

    void AddRect(float x0, float y0, float x1, float y1) {
        PushQuad(x0, y0, x1, y1, OverlayFont::kWhiteU, OverlayFont::kWhiteV, OverlayFont::kWhiteU,
                 OverlayFont::kWhiteV);
    }

    // A rectangle with rounded, antialiased corners of radius `r`.
    void AddRoundRect(float x0, float y0, float x1, float y1, float r) {
        r = std::clamp(r, 0.0f, std::min(x1 - x0, y1 - y0) / 2.0f);
        if (r < 0.5f) {
            AddRect(x0, y0, x1, y1);
            return;
        }
        AddRect(x0, y0 + r, x1, y1 - r);
        AddRect(x0 + r, y0, x1 - r, y0 + r);
        AddRect(x0 + r, y1 - r, x1 - r, y1);
        AddCorners(x0, y0, x1, y1, r, OverlayFont::kCornerU, OverlayFont::kCornerV, true, true);
    }

    // The same with only the top corners rounded (a band across the top of a panel).
    void AddRoundRectTop(float x0, float y0, float x1, float y1, float r) {
        r = std::clamp(r, 0.0f, std::min(x1 - x0, y1 - y0) / 2.0f);
        AddRect(x0, y0 + r, x1, y1);
        AddRect(x0 + r, y0, x1 - r, y0 + r);
        AddCorners(x0, y0, x1, y1, r, OverlayFont::kCornerU, OverlayFont::kCornerV, true, false);
    }

    // A hairline around a rounded rectangle.
    void AddRoundRing(float x0, float y0, float x1, float y1, float r) {
        r = std::clamp(r, 1.0f, std::min(x1 - x0, y1 - y0) / 2.0f);
        const float t = std::max(1.0f, r * OverlayFont::kRingThickness);
        AddRect(x0 + r, y0, x1 - r, y0 + t);
        AddRect(x0 + r, y1 - t, x1 - r, y1);
        AddRect(x0, y0 + r, x0 + t, y1 - r);
        AddRect(x1 - t, y0 + r, x1, y1 - r);
        AddCorners(x0, y0, x1, y1, r, OverlayFont::kRingU, OverlayFont::kRingV, true, true);
    }

    // A soft shadow for a rounded rectangle: solid under it, fading out over `spread` beyond.
    void AddShadow(float x0, float y0, float x1, float y1, float r, float spread) {
        r = std::clamp(r, 0.0f, std::min(x1 - x0, y1 - y0) / 2.0f);
        const float e = r + spread;
        const float ix0 = x0 + r, iy0 = y0 + r, ix1 = x1 - r, iy1 = y1 - r;
        const float su = OverlayFont::kShadowU, sv = OverlayFont::kShadowV;
        const float ru = OverlayFont::kShapeRadiusU, rv = OverlayFont::kShapeRadiusV;
        AddRect(ix0, iy0, ix1, iy1);
        // The edges fade along one axis, from the shape's centre line.
        PushQuad(ix0, iy0 - e, ix1, iy0, su, sv + rv, su, sv);
        PushQuad(ix0, iy1, ix1, iy1 + e, su, sv, su, sv + rv);
        PushQuad(ix0 - e, iy0, ix0, iy1, su + ru, sv, su, sv);
        PushQuad(ix1, iy0, ix1 + e, iy1, su, sv, su + ru, sv);
        AddCorners(ix0 - e, iy0 - e, ix1 + e, iy1 + e, e, su, sv, true, true);
    }

    // Width in output pixels that a string occupies.
    static float Measure(std::string_view text, float scale) {
        float width = 0.0f;
        for (char c : text) {
            width += OverlayFont::GlyphFor(c).xadvance * scale;
        }
        return width;
    }

    // Draws a string with its line box's top-left at (ox, oy).
    void AddText(float ox, float oy, std::string_view text, float scale) {
        float pen = ox;
        for (char c : text) {
            const OverlayFont::Glyph& g = OverlayFont::GlyphFor(c);
            if (g.w > 0.0f && g.h > 0.0f) {
                const float qx = pen + g.xoff * scale;
                const float qy = oy + g.yoff * scale;
                PushQuad(qx, qy, qx + g.w * scale, qy + g.h * scale, g.u0, g.v0, g.u1, g.v1);
            }
            pen += g.xadvance * scale;
        }
    }

private:
    static constexpr int kFloatsPerVertex = 4;

    // The corners of the rectangle (x0, y0)-(x1, y1), `r` across, from the baked shape centred at
    // (cu, cv): each quad puts the shape's centre on the rounded corner's centre.
    void AddCorners(float x0, float y0, float x1, float y1, float r, float cu, float cv, bool top,
                    bool bottom) {
        const float ru = OverlayFont::kShapeRadiusU, rv = OverlayFont::kShapeRadiusV;
        if (top) {
            PushQuad(x0, y0, x0 + r, y0 + r, cu + ru, cv + rv, cu, cv);
            PushQuad(x1 - r, y0, x1, y0 + r, cu, cv + rv, cu + ru, cv);
        }
        if (bottom) {
            PushQuad(x0, y1 - r, x0 + r, y1, cu + ru, cv, cu, cv + rv);
            PushQuad(x1 - r, y1 - r, x1, y1, cu, cv, cu + ru, cv + rv);
        }
    }

    void PushQuad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
        const std::array<float, 2> tl = Project(x0, y0);
        const std::array<float, 2> tr = Project(x1, y0);
        const std::array<float, 2> br = Project(x1, y1);
        const std::array<float, 2> bl = Project(x0, y1);
        const auto push = [this](const std::array<float, 2>& p, float u, float v) {
            verts.insert(verts.end(), {p[0], p[1], u, v});
        };
        push(tl, u0, v0);
        push(tr, u1, v0);
        push(br, u1, v1);
        push(tl, u0, v0);
        push(br, u1, v1);
        push(bl, u0, v1);
    }

    // Canvas pixels to clip space.
    std::array<float, 2> Project(float x, float y) const {
        float px = x;
        float py = y;
        switch (canvas.rotation) {
        case 90:
            px = canvas.height - y;
            py = x;
            break;
        case 180:
            px = canvas.width - x;
            py = canvas.height - y;
            break;
        case 270:
            px = y;
            py = canvas.width - x;
            break;
        default:
            break;
        }
        return {px * inv_w - 1.0f, py * inv_h - 1.0f};
    }

    std::vector<float>& verts;
    OverlayCanvas canvas;
    float inv_w;
    float inv_h;
};
} // namespace

OverlayDraw RendererVulkan::PrepareFpsOverlay(const Layout::FramebufferLayout& layout,
                                              Frame* frame) {
    if (!Settings::values.show_fps.GetValue()) {
        return {};
    }

    // Refresh the frame rate a couple of times a second so the reading stays legible.
    const auto now = std::chrono::steady_clock::now();
    if (overlay_last_update.time_since_epoch().count() == 0 ||
        now - overlay_last_update >= std::chrono::milliseconds(500)) {
#ifdef __SWITCH__
        overlay_game_fps = static_cast<float>(system.GetLastPerfStats().game_fps);
#else
        overlay_game_fps = static_cast<float>(system.GetAndResetPerfStats().game_fps);
#endif
        overlay_last_update = now;
    }

    char text[16];
    const int fps = std::clamp(static_cast<int>(std::lround(overlay_game_fps)), 0, 999);
    std::snprintf(text, sizeof(text), "FPS %d", fps);

    const OverlayCanvas canvas = MakeOverlayCanvas(layout);
    const float w = canvas.width;
    const float h = canvas.height;
    if (w <= 0.0f || h <= 0.0f) {
        return {};
    }

    // Font em size scaled to the output so the counter stays a consistent size everywhere.
    const float em = std::max(14.0f, std::round(canvas.ShortEdge() / 32.0f));
    const float scale = em / OverlayFont::kBakePixelHeight;
    const float margin = std::round(em * 0.6f);
    const float pad = std::round(em * 0.35f);

    // A *tasteful* box around the menu.
    float ink_top = OverlayFont::kAscent;
    float ink_bottom = 0.0f;
    for (const char* p = text; *p != '\0'; ++p) {
        const OverlayFont::Glyph& g = OverlayFont::GlyphFor(*p);
        if (g.h > 0.0f) {
            ink_top = std::min(ink_top, g.yoff);
            ink_bottom = std::max(ink_bottom, g.yoff + g.h);
        }
    }

    std::vector<float> verts;
    verts.reserve(256);
    OverlayBuilder builder{verts, canvas};

    const float text_w = OverlayBuilder::Measure(text, scale);

    builder.AddRoundRect(margin - pad, margin + ink_top * scale - pad, margin + text_w + pad,
                         margin + ink_bottom * scale + pad, pad);
    const u32 box_vertices = builder.VertexCount();

    builder.AddText(margin, margin, text, scale);
    const u32 glyph_vertices = builder.VertexCount() - box_vertices;

    constexpr std::array<float, 4> box_color = {0.0f, 0.0f, 0.0f, 0.55f};
    constexpr std::array<float, 4> text_color = {0.53f, 1.0f, 0.53f, 1.0f};

    OverlayDraw overlay;
    if (!UploadOverlayVertices(frame, verts, overlay)) {
        return {};
    }
    overlay.batches.push_back({box_color, 0, box_vertices});
    if (glyph_vertices > 0) {
        overlay.batches.push_back({text_color, box_vertices, glyph_vertices});
    }
    return overlay;
}

OverlayDraw RendererVulkan::PrepareShaderNotice(const Layout::FramebufferLayout& layout,
                                                Frame* frame) {
    if (!Settings::values.show_shader_compile_notice.GetValue()) {
        return {};
    }
    const u32 pending = VideoCore::GetPendingShaderCompiles();
    const auto now = std::chrono::steady_clock::now();
    if (pending > 0) {
        shader_notice_until = now + std::chrono::milliseconds(500);
    }
    if (now >= shader_notice_until) {
        return {};
    }

    char text[32];
    if (pending > 0) {
        std::snprintf(text, sizeof(text), "Compiling shaders  %u", pending);
    } else {
        std::snprintf(text, sizeof(text), "Compiling shaders");
    }

    const OverlayCanvas canvas = MakeOverlayCanvas(layout);
    const float w = canvas.width;
    const float h = canvas.height;
    if (w <= 0.0f || h <= 0.0f) {
        return {};
    }

    const float em = std::max(14.0f, std::round(canvas.ShortEdge() / 32.0f));
    const float scale = em / OverlayFont::kBakePixelHeight;
    const float margin = std::round(em * 0.6f);
    const float pad = std::round(em * 0.35f);

    float ink_top = OverlayFont::kAscent;
    float ink_bottom = 0.0f;
    for (const char* p = text; *p != '\0'; ++p) {
        const OverlayFont::Glyph& g = OverlayFont::GlyphFor(*p);
        if (g.h > 0.0f) {
            ink_top = std::min(ink_top, g.yoff);
            ink_bottom = std::max(ink_bottom, g.yoff + g.h);
        }
    }

    std::vector<float> verts;
    verts.reserve(256);
    OverlayBuilder builder{verts, canvas};

    const float text_w = OverlayBuilder::Measure(text, scale);

    // Anchor the box to the bottom-left so it stays clear of the FPS counter.
    const float ox = margin;
    const float oy = h - margin - pad - ink_bottom * scale;

    builder.AddRoundRect(ox - pad, oy + ink_top * scale - pad, ox + text_w + pad,
                         oy + ink_bottom * scale + pad, pad);
    const u32 box_vertices = builder.VertexCount();

    builder.AddText(ox, oy, text, scale);
    const u32 glyph_vertices = builder.VertexCount() - box_vertices;

    constexpr std::array<float, 4> box_color = {0.0f, 0.0f, 0.0f, 0.55f};
    constexpr std::array<float, 4> text_color = {1.0f, 0.82f, 0.35f, 1.0f};

    OverlayDraw overlay;
    if (!UploadOverlayVertices(frame, verts, overlay)) {
        return {};
    }
    overlay.batches.push_back({box_color, 0, box_vertices});
    if (glyph_vertices > 0) {
        overlay.batches.push_back({text_color, box_vertices, glyph_vertices});
    }
    return overlay;
}

OverlayDraw RendererVulkan::PrepareToast(const Layout::FramebufferLayout& layout, Frame* frame) {
    const std::string text = VideoCore::GetOverlayToast();
    if (text.empty()) {
        return {};
    }

    const OverlayCanvas canvas = MakeOverlayCanvas(layout);
    const float w = canvas.width;
    const float h = canvas.height;
    if (w <= 0.0f || h <= 0.0f) {
        return {};
    }

    const float em = std::max(16.0f, std::round(canvas.ShortEdge() / 28.0f));
    const float scale = em / OverlayFont::kBakePixelHeight;
    const float margin = std::round(em * 1.2f);
    const float pad = std::round(em * 0.45f);

    float ink_top = OverlayFont::kAscent;
    float ink_bottom = 0.0f;
    for (const char c : text) {
        const OverlayFont::Glyph& g = OverlayFont::GlyphFor(c);
        if (g.h > 0.0f) {
            ink_top = std::min(ink_top, g.yoff);
            ink_bottom = std::max(ink_bottom, g.yoff + g.h);
        }
    }

    std::vector<float> verts;
    verts.reserve(512);
    OverlayBuilder builder{verts, canvas};

    const float text_w = OverlayBuilder::Measure(text, scale);

    // Bottom centre, above the shader notice's row.
    const float ox = std::round((w - text_w) / 2.0f);
    const float oy = h - margin - pad - ink_bottom * scale;

    // A glass pill like the launcher's notices: a soft shadow, the body and a fine edge.
    const float bx0 = ox - pad * 1.6f, by0 = oy + ink_top * scale - pad;
    const float bx1 = ox + text_w + pad * 1.6f, by1 = oy + ink_bottom * scale + pad;
    const float r = (by1 - by0) / 2.0f;
    builder.AddShadow(bx0, by0 + pad * 0.4f, bx1, by1 + pad * 0.4f, r, em * 0.8f);
    const u32 shadow_vertices = builder.VertexCount();
    builder.AddRoundRect(bx0, by0, bx1, by1, r);
    const u32 box_end = builder.VertexCount();
    builder.AddRoundRing(bx0, by0, bx1, by1, r);
    const u32 ring_end = builder.VertexCount();

    builder.AddText(ox, oy, text, scale);
    const u32 glyph_vertices = builder.VertexCount() - ring_end;

    constexpr std::array<float, 4> shadow_color = {0.0f, 0.0f, 0.0f, 0.45f};
    constexpr std::array<float, 4> box_color = {0.066f, 0.07f, 0.1f, 0.94f};
    constexpr std::array<float, 4> ring_color = {1.0f, 1.0f, 1.0f, 0.12f};
    constexpr std::array<float, 4> text_color = {0.96f, 0.96f, 0.98f, 1.0f};

    OverlayDraw overlay;
    if (!UploadOverlayVertices(frame, verts, overlay)) {
        return {};
    }
    overlay.batches.push_back({shadow_color, 0, shadow_vertices});
    overlay.batches.push_back({box_color, shadow_vertices, box_end - shadow_vertices});
    overlay.batches.push_back({ring_color, box_end, ring_end - box_end});
    if (glyph_vertices > 0) {
        overlay.batches.push_back({text_color, ring_end, glyph_vertices});
    }
    return overlay;
}

namespace {

// The in-game menu shares the launcher's look: a dark glass panel, teal accents and button chips.
constexpr std::array<float, 4> kQmText = {0.96f, 0.96f, 0.98f, 1.0f};
constexpr std::array<float, 4> kQmTextDim = {0.64f, 0.64f, 0.73f, 1.0f};
constexpr std::array<float, 4> kQmAccent = {0.37f, 0.91f, 0.87f, 1.0f};

// A footer hint such as "A Select": a button chip and its label, or just text ("1-10 of 23").
struct HintPart {
    std::string button;
    std::string label;
};

std::vector<HintPart> SplitHints(const std::string& hint) {
    std::vector<HintPart> parts;
    std::size_t start = 0;
    while (start < hint.size()) {
        std::size_t end = hint.find("   ", start);
        if (end == std::string::npos) {
            end = hint.size();
        }
        std::string token = hint.substr(start, end - start);
        start = end + 3;
        while (!token.empty() && token.front() == ' ') {
            token.erase(token.begin());
        }
        while (!token.empty() && token.back() == ' ') {
            token.pop_back();
        }
        if (token.empty()) {
            continue;
        }
        const std::size_t space = token.find(' ');
        const std::string first = token.substr(0, space);
        // Buttons are short and in capitals ("A", "ZL/ZR", "+/-"), or the stick.
        const bool button = space != std::string::npos && first.size() <= 6 &&
                            (first == "Stick" ||
                             std::all_of(first.begin(), first.end(), [](char c) {
                                 return (c >= 'A' && c <= 'Z') || c == '+' || c == '-' || c == '/';
                             }));
        if (button) {
            parts.push_back({first, token.substr(space + 1)});
        } else {
            parts.push_back({"", token});
        }
    }
    return parts;
}

} // namespace

OverlayDraw RendererVulkan::PrepareQuickMenu(const Layout::FramebufferLayout& layout,
                                             Frame* frame) {
    if (!VideoCore::IsOverlayMenuVisible()) {
        quick_menu_open = 0.0f;
        return {};
    }
    const VideoCore::OverlayMenuState state = VideoCore::GetOverlayMenuState();
    if (!state.visible) {
        quick_menu_open = 0.0f;
        return {};
    }

    const OverlayCanvas canvas = MakeOverlayCanvas(layout);
    const float w = canvas.width;
    const float h = canvas.height;
    if (w <= 0.0f || h <= 0.0f) {
        return {};
    }

    // The menu fades and rises in as it opens; the highlight glides between rows.
    const auto now = std::chrono::steady_clock::now();
    if (quick_menu_open <= 0.0f) {
        quick_menu_last = now;
        quick_menu_open = 0.001f;
        quick_menu_highlight = -1.0f;
    }
    const float dt = std::clamp(std::chrono::duration<float>(now - quick_menu_last).count(), 0.0f, 0.1f);
    quick_menu_last = now;
    quick_menu_open = std::min(1.0f, quick_menu_open + dt / 0.16f);
    const float appear = 1.0f - (1.0f - quick_menu_open) * (1.0f - quick_menu_open) * (1.0f - quick_menu_open);
    if (state.title != quick_menu_title) {
        quick_menu_title = state.title;
        quick_menu_highlight = -1.0f;
    }
    if (quick_menu_highlight < 0.0f) {
        quick_menu_highlight = static_cast<float>(state.selected);
    } else {
        quick_menu_highlight = state.selected + (quick_menu_highlight - state.selected) * std::exp2(-dt / 0.04f);
    }

    std::vector<float> verts;
    verts.reserve(4096);
    OverlayBuilder builder{verts, canvas};

    // Sizes follow the output so the menu reads the same docked and handheld.
    const float em = std::max(18.0f, std::round(canvas.ShortEdge() / 27.0f));
    const float scale = em / OverlayFont::kBakePixelHeight;
    const float title_em = std::round(em * 1.3f);
    const float title_scale = title_em / OverlayFont::kBakePixelHeight;
    const float small_em = std::round(em * 0.68f);
    const float small_scale = small_em / OverlayFont::kBakePixelHeight;
    const float line_h = OverlayFont::kLineHeight * scale;
    const float title_line_h = OverlayFont::kLineHeight * title_scale;
    const float small_line_h = OverlayFont::kLineHeight * small_scale;
    const float row_h = std::round(em * 1.9f);
    const float pad = std::round(em * 1.0f);
    const float radius = std::round(em * 0.9f);
    const float col_gap = em * 1.6f;
    const float chip_h = std::round(em * 1.15f);
    const float chip_scale = std::round(em * 0.62f) / OverlayFont::kBakePixelHeight;
    const float hint_scale = std::round(em * 0.8f) / OverlayFont::kBakePixelHeight;
    const float hint_gap = std::round(em * 1.1f);

    // "Quick Menu > Display" shows as a small "QUICK MENU" over a large "Display".
    std::string crumb, title = state.title;
    if (const std::size_t sep = title.rfind(" > "); sep != std::string::npos) {
        crumb = title.substr(0, sep);
        title = title.substr(sep + 3);
        for (char& c : crumb) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
    }

    // The footer's chips and labels, measured once.
    const std::vector<HintPart> hints = SplitHints(state.hint);
    float hints_w = 0.0f;
    for (const HintPart& part : hints) {
        if (!part.button.empty()) {
            hints_w += std::max(chip_h, OverlayBuilder::Measure(part.button, chip_scale) + chip_h * 0.7f) +
                       em * 0.4f;
        }
        hints_w += OverlayBuilder::Measure(part.label, hint_scale) + hint_gap;
    }
    hints_w = std::max(0.0f, hints_w - hint_gap);

    // Size the panel to its contents.
    float max_row_w = 0.0f;
    for (const auto& item : state.items) {
        float rw = OverlayBuilder::Measure(item.label, scale) + em * 1.2f;
        if (!item.value.empty()) {
            rw += col_gap + OverlayBuilder::Measure(item.value, scale);
        }
        max_row_w = std::max(max_row_w, rw);
    }
    const int n = static_cast<int>(state.items.size());
    const float header_h = (crumb.empty() ? 0.0f : small_line_h) + title_line_h;
    const float inner_w = std::max({max_row_w, OverlayBuilder::Measure(title, title_scale),
                                    OverlayBuilder::Measure(crumb, small_scale), hints_w});
    const float panel_w = std::round(std::clamp(inner_w + 2.0f * pad, 0.35f * w, 0.92f * w));
    const float sep_gap = std::round(em * 0.7f);
    const float panel_h = std::round(pad * 0.9f + header_h + sep_gap * 2.0f + static_cast<float>(n) * row_h +
                                     sep_gap + chip_h + pad * 0.9f);
    const float panel_x0 = std::round((w - panel_w) / 2.0f);
    const float rise = std::round(em * 0.8f * (1.0f - appear));
    // The compact panel keeps to an edge so the screens stay visible.
    const float panel_y0 = (!state.compact     ? std::round((h - panel_h) / 2.0f)
                            : state.compact_top ? std::round(pad * 0.6f)
                                                : std::round(h - panel_h - pad * 0.6f)) +
                           rise;
    const float panel_x1 = panel_x0 + panel_w;
    const float panel_y1 = panel_y0 + panel_h;

    std::vector<OverlayDraw::Batch> batches;
    const auto emit = [&](std::array<float, 4> color, u32 start) {
        const u32 count = builder.VertexCount() - start;
        color[3] *= appear;
        if (count > 0 && color[3] > 0.0f) {
            batches.push_back({color, start, count});
        }
    };

    // Dim the running game, unless the player is arranging its screens.
    if (!state.compact) {
        const u32 s = builder.VertexCount();
        builder.AddRect(0.0f, 0.0f, w, h);
        emit({0.012f, 0.012f, 0.03f, 0.62f}, s);
    }
    // Outlines around the two screens, the one being edited in the accent colour. Canvas pixels
    // are framebuffer pixels unless the overlay is rotated, so only then.
    if (state.outline_screen >= 0 && canvas.rotation == 0) {
        const float t = std::max(2.0f, std::round(em / 7.0f));
        const auto outline = [&](const Common::Rectangle<u32>& r) {
            const float x0 = static_cast<float>(r.left), y0 = static_cast<float>(r.top);
            const float x1 = static_cast<float>(r.right), y1 = static_cast<float>(r.bottom);
            builder.AddRect(x0, y0, x1, y0 + t);
            builder.AddRect(x0, y1 - t, x1, y1);
            builder.AddRect(x0, y0 + t, x0 + t, y1 - t);
            builder.AddRect(x1 - t, y0 + t, x1, y1 - t);
        };
        const bool top_first = state.outline_screen == 1;
        {
            const u32 s = builder.VertexCount();
            outline(top_first ? layout.top_screen : layout.bottom_screen);
            emit({1.0f, 1.0f, 1.0f, 0.45f}, s);
        }
        {
            const u32 s = builder.VertexCount();
            outline(top_first ? layout.bottom_screen : layout.top_screen);
            emit(kQmAccent, s);
        }
    }

    // The panel: a soft shadow, dark glass with light across its top, and a fine edge.
    {
        const u32 s = builder.VertexCount();
        const float drop = std::round(em * 0.45f);
        builder.AddShadow(panel_x0, panel_y0 + drop, panel_x1, panel_y1 + drop, radius, em * 1.4f);
        emit({0.0f, 0.0f, 0.0f, 0.5f}, s);
    }
    {
        const u32 s = builder.VertexCount();
        builder.AddRoundRect(panel_x0, panel_y0, panel_x1, panel_y1, radius);
        emit({0.066f, 0.07f, 0.1f, 0.94f}, s);
    }
    {
        // Stacked faint bands make a smooth-looking sheen that fades down the panel.
        const u32 s = builder.VertexCount();
        for (int i = 0; i < 6; ++i) {
            const float band = panel_h * 0.46f * (1.0f - i / 6.0f);
            builder.AddRoundRectTop(panel_x0, panel_y0, panel_x1, panel_y0 + std::max(band, radius * 2.0f), radius);
        }
        emit({1.0f, 1.0f, 1.0f, 0.011f}, s);
    }
    {
        const u32 s = builder.VertexCount();
        builder.AddRoundRing(panel_x0, panel_y0, panel_x1, panel_y1, radius);
        emit({1.0f, 1.0f, 1.0f, 0.12f}, s);
    }

    // Walk down the panel.
    float pen_y = panel_y0 + pad * 0.9f;
    const float crumb_y = pen_y;
    if (!crumb.empty()) {
        pen_y += small_line_h;
    }
    const float title_y = pen_y;
    pen_y += title_line_h + sep_gap;
    const float header_line_y = std::round(pen_y);
    pen_y += sep_gap;
    const float rows_top = pen_y;
    pen_y = rows_top + static_cast<float>(n) * row_h + sep_gap;
    const float footer_y = pen_y;
    const float text_x = panel_x0 + pad;

    // A hairline under the header.
    {
        const u32 s = builder.VertexCount();
        builder.AddRect(panel_x0 + pad, header_line_y, panel_x1 - pad, header_line_y + std::max(1.0f, std::round(em / 16.0f)));
        emit({1.0f, 1.0f, 1.0f, 0.08f}, s);
    }

    const bool has_selection = n > 0 && state.selected >= 0 && state.selected < n &&
                               !state.items[state.selected].is_header;

    // The highlight: a lit pill with an accent bar, gliding to the selected row.
    if (has_selection) {
        const float inset = std::round(row_h * 0.07f);
        const float top = rows_top + quick_menu_highlight * row_h + inset;
        const float bottom = top + row_h - 2.0f * inset;
        const float hx0 = panel_x0 + pad * 0.45f, hx1 = panel_x1 - pad * 0.45f;
        const float hr = std::round(row_h * 0.3f);
        {
            const u32 s = builder.VertexCount();
            builder.AddShadow(hx0, top, hx1, bottom, hr, em * 0.6f);
            emit({kQmAccent[0], kQmAccent[1], kQmAccent[2], 0.10f}, s);
        }
        {
            const u32 s = builder.VertexCount();
            builder.AddRoundRect(hx0, top, hx1, bottom, hr);
            emit({1.0f, 1.0f, 1.0f, 0.1f}, s);
        }
        {
            const u32 s = builder.VertexCount();
            builder.AddRoundRing(hx0, top, hx1, bottom, hr);
            emit({1.0f, 1.0f, 1.0f, 0.14f}, s);
        }
        {
            const u32 s = builder.VertexCount();
            const float bar_w = std::max(3.0f, std::round(em * 0.18f));
            const float bar_x = hx0 + std::round(em * 0.35f);
            const float bar_h = (bottom - top) * 0.5f;
            const float bar_y = top + (bottom - top - bar_h) / 2.0f;
            builder.AddRoundRect(bar_x, bar_y, bar_x + bar_w, bar_y + bar_h, bar_w / 2.0f);
            emit(kQmAccent, s);
        }
    }

    // Header: the crumb and the page's name.
    if (!crumb.empty()) {
        const u32 s = builder.VertexCount();
        builder.AddText(text_x, crumb_y, crumb, small_scale);
        emit(kQmTextDim, s);
    }
    {
        const u32 s = builder.VertexCount();
        builder.AddText(text_x, title_y, title, title_scale);
        emit(kQmText, s);
    }

    const float label_x = text_x + em * 0.6f;
    const auto row_text_y = [&](int i) {
        return std::round(rows_top + static_cast<float>(i) * row_h + (row_h - line_h) / 2.0f);
    };

    // Labels, then values: the selected row's in white and accent, section headers dim and small.
    {
        const u32 s = builder.VertexCount();
        for (int i = 0; i < n; ++i) {
            if (!state.items[i].is_header && i != state.selected) {
                builder.AddText(label_x, row_text_y(i), state.items[i].label, scale);
            }
        }
        emit({0.86f, 0.86f, 0.91f, 1.0f}, s);
    }
    {
        const u32 s = builder.VertexCount();
        for (int i = 0; i < n; ++i) {
            const auto& item = state.items[i];
            if (!item.is_header && i != state.selected && !item.value.empty()) {
                builder.AddText(panel_x1 - pad - OverlayBuilder::Measure(item.value, scale), row_text_y(i), item.value,
                                scale);
            }
        }
        emit(kQmTextDim, s);
    }
    {
        const u32 s = builder.VertexCount();
        for (int i = 0; i < n; ++i) {
            if (!state.items[i].is_header) {
                continue;
            }
            std::string label = state.items[i].label;
            for (char& c : label) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            const float top = rows_top + static_cast<float>(i) * row_h;
            const float ty = std::round(top + (row_h - small_line_h) / 2.0f + small_line_h * 0.12f);
            builder.AddText(text_x, ty, label, small_scale);
            const float rule_x0 = text_x + OverlayBuilder::Measure(label, small_scale) + em * 0.6f;
            const float rule_y = std::round(top + row_h / 2.0f + small_line_h * 0.12f);
            if (panel_x1 - pad > rule_x0) {
                builder.AddRect(rule_x0, rule_y, panel_x1 - pad, rule_y + std::max(1.0f, std::round(em / 16.0f)));
            }
        }
        emit({0.52f, 0.52f, 0.62f, 1.0f}, s);
    }
    if (has_selection) {
        const auto& item = state.items[state.selected];
        {
            const u32 s = builder.VertexCount();
            builder.AddText(label_x, row_text_y(state.selected), item.label, scale);
            emit(kQmText, s);
        }
        if (!item.value.empty()) {
            const u32 s = builder.VertexCount();
            builder.AddText(panel_x1 - pad - OverlayBuilder::Measure(item.value, scale), row_text_y(state.selected),
                            item.value, scale);
            emit(kQmAccent, s);
        }
    }

    // Footer: button chips and what they do, centred.
    if (!hints.empty()) {
        struct Placed {
            float chip_x;
            float chip_w; // 0 for plain text
            float label_x;
            const HintPart* part;
        };
        std::vector<Placed> placed;
        float x = std::round(panel_x0 + (panel_w - hints_w) / 2.0f);
        for (const HintPart& part : hints) {
            Placed p{x, 0.0f, x, &part};
            if (!part.button.empty()) {
                p.chip_w = std::max(chip_h, OverlayBuilder::Measure(part.button, chip_scale) + chip_h * 0.7f);
                p.label_x = x + p.chip_w + em * 0.4f;
            }
            x = p.label_x + OverlayBuilder::Measure(part.label, hint_scale) + hint_gap;
            placed.push_back(p);
        }
        const float chip_text_h = OverlayFont::kLineHeight * chip_scale;
        const float label_h = OverlayFont::kLineHeight * hint_scale;
        {
            const u32 s = builder.VertexCount();
            for (const Placed& p : placed) {
                if (p.chip_w > 0.0f) {
                    builder.AddRoundRect(p.chip_x, footer_y, p.chip_x + p.chip_w, footer_y + chip_h, chip_h / 2.0f);
                }
            }
            emit({0.95f, 0.95f, 0.97f, 1.0f}, s);
        }
        {
            const u32 s = builder.VertexCount();
            for (const Placed& p : placed) {
                if (p.chip_w > 0.0f) {
                    const float tw = OverlayBuilder::Measure(p.part->button, chip_scale);
                    builder.AddText(std::round(p.chip_x + (p.chip_w - tw) / 2.0f),
                                    std::round(footer_y + (chip_h - chip_text_h) / 2.0f), p.part->button, chip_scale);
                }
            }
            emit({0.08f, 0.08f, 0.11f, 1.0f}, s);
        }
        {
            const u32 s = builder.VertexCount();
            for (const Placed& p : placed) {
                builder.AddText(std::round(p.label_x), std::round(footer_y + (chip_h - label_h) / 2.0f), p.part->label,
                                hint_scale);
            }
            emit({0.8f, 0.8f, 0.86f, 1.0f}, s);
        }
    }

    if (batches.empty()) {
        return {};
    }

    OverlayDraw overlay;
    if (!UploadOverlayVertices(frame, verts, overlay)) {
        return {};
    }
    overlay.batches = std::move(batches);
    return overlay;
}

bool RendererVulkan::UploadOverlayVertices(Frame* frame, const std::vector<float>& verts,
                                           OverlayDraw& overlay) {
    const u64 size = verts.size() * sizeof(float);
    if (size == 0) {
        return false;
    }

#ifdef ENABLE_LSFG
    if (frame->overlays_deferred) {
        const u64 offset = Common::AlignUp(frame->overlay_offset, 16);
        if (!frame->overlay_data || offset + size > kOverlayBufferSize) {
            return false;
        }
        std::memcpy(frame->overlay_data + offset, verts.data(), size);
        frame->overlay_offset = offset + size;
        overlay.base_vertex = static_cast<u32>(offset / (sizeof(float) * 4));
        return true;
    }
#endif

    auto [data, offset, invalidate] = overlay_vertex_buffer.Map(size, 16);
    std::memcpy(data, verts.data(), size);
    overlay_vertex_buffer.Commit(size);
    overlay.base_vertex = static_cast<u32>(offset) / (sizeof(float) * 4);
    return true;
}

#ifdef ENABLE_LSFG
void RendererVulkan::RecordOverlays(vk::CommandBuffer cmdbuf, vk::Buffer vertex_buffer,
                                    std::span<const OverlayDraw> overlays) {
    bool bound = false;
    for (const OverlayDraw& overlay : overlays) {
        if (overlay.batches.empty()) {
            continue;
        }
        if (!bound) {
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, overlay_pipeline);
            cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *overlay_pipeline_layout, 0,
                                      overlay_descriptor_set, {});
            cmdbuf.bindVertexBuffers(0, vertex_buffer, {0});
            bound = true;
        }
        for (const auto& b : overlay.batches) {
            cmdbuf.pushConstants(*overlay_pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0,
                                 static_cast<u32>(b.color.size() * sizeof(float)), b.color.data());
            cmdbuf.draw(b.count, 1, overlay.base_vertex + b.first, 0);
        }
    }
}
#endif

void RendererVulkan::RecordOverlay(OverlayDraw overlay) {
    if (overlay.batches.empty()) {
        return;
    }
    scheduler.Record([this, base_vertex = overlay.base_vertex,
                      batches = std::move(overlay.batches)](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, overlay_pipeline);
        cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *overlay_pipeline_layout, 0,
                                  overlay_descriptor_set, {});
        cmdbuf.bindVertexBuffers(0, overlay_vertex_buffer.Handle(), {0});
        for (const auto& b : batches) {
            cmdbuf.pushConstants(*overlay_pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0,
                                 static_cast<u32>(b.color.size() * sizeof(float)), b.color.data());
            cmdbuf.draw(b.count, 1, base_vertex + b.first, 0);
        }
    });
}

void RendererVulkan::SwapBuffers() {
    system.perf_stats->StartSwap();
    screenRendered = false;
#ifndef ANDROID
    if (Settings::values.layout_option.GetValue() == Settings::LayoutOption::SeparateWindows) {
        ASSERT(secondary_window);
        secondaryWindowEnabled = true;
    } else {
        secondaryWindowEnabled = false;
    }
#endif

#ifdef ANDROID
    if (secondary_window) {
        secondaryWindowEnabled = true;
    } else {
        secondaryWindowEnabled = false;
    }
#endif

    const Layout::FramebufferLayout& layout = render_window.GetFramebufferLayout();
    PrepareRendertarget();
    RenderScreenshot();
    isSecondaryWindow = false;
    RenderToWindow(main_present_window, layout, false);
#ifndef ANDROID
    if (Settings::values.layout_option.GetValue() == Settings::LayoutOption::SeparateWindows) {
        ASSERT(secondary_window);
        const auto& secondary_layout = secondary_window->GetFramebufferLayout();
        if (!secondary_present_window_ptr) {
            secondary_present_window_ptr = std::make_unique<PresentWindow>(
                *secondary_window, instance, scheduler, IsLowRefreshRate());
        }
        isSecondaryWindow = true;
        RenderToWindow(*secondary_present_window_ptr, secondary_layout, false);
        secondary_window->PollEvents();
    }
#endif

#ifdef ANDROID
    if (secondary_window) {
        const auto& secondary_layout = secondary_window->GetFramebufferLayout();
        if (!secondary_present_window_ptr) {
            secondary_present_window_ptr = std::make_unique<PresentWindow>(
                *secondary_window, instance, scheduler, IsLowRefreshRate());
        }
        isSecondaryWindow = true;
        RenderToWindow(*secondary_present_window_ptr, secondary_layout, false);
        secondary_window->PollEvents();
    }
#endif
    if (!screenRendered) {
        scheduler.Finish();
    }

    system.perf_stats->EndSwap();
    rasterizer.TickFrame();
    EndFrame();
}

void RendererVulkan::RenderScreenshot() {
    if (!settings.screenshot_requested.exchange(false)) {
        return;
    }

    if (!TryRenderScreenshotWithHostMemory()) {
        RenderScreenshotWithStagingCopy();
    }

    settings.screenshot_complete_callback(false);
}

void RendererVulkan::RenderScreenshotWithStagingCopy() {
    const vk::Device device = instance.GetDevice();

    const Layout::FramebufferLayout layout{settings.screenshot_framebuffer_layout};
    const u32 width = layout.width;
    const u32 height = layout.height;

    const vk::BufferCreateInfo staging_buffer_info = {
        .size = width * height * 4,
        .usage = vk::BufferUsageFlagBits::eTransferDst,
    };

    const VmaAllocationCreateInfo alloc_create_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT |
                 VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkBuffer unsafe_buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo alloc_info;
    VkBufferCreateInfo unsafe_buffer_info = static_cast<VkBufferCreateInfo>(staging_buffer_info);

    VkResult result = vmaCreateBuffer(instance.GetAllocator(), &unsafe_buffer_info,
                                      &alloc_create_info, &unsafe_buffer, &allocation, &alloc_info);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}", result);
        UNREACHABLE();
    }

    vk::Buffer staging_buffer{unsafe_buffer};

    Frame frame{};
    main_present_window.RecreateFrame(&frame, width, height);

    DrawScreens(&frame, layout, layout, false);

    scheduler.Record(
        [width, height, source_image = frame.image, staging_buffer](vk::CommandBuffer cmdbuf) {
            const vk::ImageMemoryBarrier read_barrier = {
                .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = source_image,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = VK_REMAINING_MIP_LEVELS,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            };
            const vk::ImageMemoryBarrier write_barrier = {
                .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                .dstAccessMask = vk::AccessFlagBits::eMemoryWrite,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = source_image,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = VK_REMAINING_MIP_LEVELS,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            };
            static constexpr vk::MemoryBarrier memory_write_barrier = {
                .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
                .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
            };

            const vk::BufferImageCopy image_copy = {
                .bufferOffset = 0,
                .bufferRowLength = 0,
                .bufferImageHeight = 0,
                .imageSubresource =
                    {
                        .aspectMask = vk::ImageAspectFlagBits::eColor,
                        .mipLevel = 0,
                        .baseArrayLayer = 0,
                        .layerCount = 1,
                    },
                .imageOffset = {0, 0, 0},
                .imageExtent = {width, height, 1},
            };

            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                                   vk::PipelineStageFlagBits::eTransfer,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, read_barrier);
            cmdbuf.copyImageToBuffer(source_image, vk::ImageLayout::eTransferSrcOptimal,
                                     staging_buffer, image_copy);
            cmdbuf.pipelineBarrier(
                vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands,
                vk::DependencyFlagBits::eByRegion, memory_write_barrier, {}, write_barrier);
        });

    // Ensure the copy is fully completed before saving the screenshot
    scheduler.Finish();

    // Copy backing image data to the QImage screenshot buffer
    std::memcpy(settings.screenshot_bits, alloc_info.pMappedData, staging_buffer_info.size);

    // QImage::Format_RGB32 expects BGRA byte order. If the swapchain format is RGBA,
    // swap R and B channels so the screenshot colors are correct.
    if (main_present_window.GetSurfaceFormat() == vk::Format::eR8G8B8A8Unorm) {
        u8* pixels = static_cast<u8*>(settings.screenshot_bits);
        for (u32 i = 0; i < width * height; i++) {
            std::swap(pixels[i * 4 + 0], pixels[i * 4 + 2]);
        }
    }

    // Destroy allocated resources
    vmaDestroyBuffer(instance.GetAllocator(), staging_buffer, allocation);
    vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
    device.destroyFramebuffer(frame.framebuffer);
    device.destroyImageView(frame.image_view);
}

bool RendererVulkan::TryRenderScreenshotWithHostMemory() {
    // If the host-memory import alignment matches the allocation granularity of the platform, then
    // the entire span of memory can be trivially imported
    const bool trivial_import =
        instance.IsExternalMemoryHostSupported() &&
        instance.GetMinImportedHostPointerAlignment() == Common::GetPageSize();
    if (!trivial_import) {
        return false;
    }

    const vk::Device device = instance.GetDevice();

    const Layout::FramebufferLayout layout{settings.screenshot_framebuffer_layout};
    const u32 width = layout.width;
    const u32 height = layout.height;

    // For a span of memory [x, x + s], import [AlignDown(x, alignment), AlignUp(x + s, alignment)]
    // and maintain an offset to the start of the data
    const u64 import_alignment = instance.GetMinImportedHostPointerAlignment();
    const uintptr_t address = reinterpret_cast<uintptr_t>(settings.screenshot_bits);
    void* aligned_pointer = reinterpret_cast<void*>(Common::AlignDown(address, import_alignment));
    const u64 offset = address % import_alignment;
    const u64 aligned_size = Common::AlignUp(offset + width * height * 4ull, import_alignment);

    // Buffer<->Image mapping for the imported imported buffer
    const vk::BufferImageCopy buffer_image_copy = {
        .bufferOffset = offset,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource =
            {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {width, height, 1},
    };

    const vk::MemoryHostPointerPropertiesEXT import_properties =
        device.getMemoryHostPointerPropertiesEXT(
            vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT, aligned_pointer);

    if (!import_properties.memoryTypeBits) {
        // Could not import memory
        return false;
    }

    const std::optional<u32> memory_type_index = FindMemoryType(
        instance.GetPhysicalDevice().getMemoryProperties(),
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
        import_properties.memoryTypeBits);

    if (!memory_type_index.has_value()) {
        // Could not find memory type index
        return false;
    }

    const vk::StructureChain<vk::MemoryAllocateInfo, vk::ImportMemoryHostPointerInfoEXT>
        allocation_chain = {
            vk::MemoryAllocateInfo{
                .allocationSize = aligned_size,
                .memoryTypeIndex = memory_type_index.value(),
            },
            vk::ImportMemoryHostPointerInfoEXT{
                .handleType = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
                .pHostPointer = aligned_pointer,
            },
        };

    // Import host memory
    const vk::UniqueDeviceMemory imported_memory =
        device.allocateMemoryUnique(allocation_chain.get());

    const vk::StructureChain<vk::BufferCreateInfo, vk::ExternalMemoryBufferCreateInfo> buffer_info =
        {
            vk::BufferCreateInfo{
                .size = aligned_size,
                .usage = vk::BufferUsageFlagBits::eTransferDst,
                .sharingMode = vk::SharingMode::eExclusive,
            },
            vk::ExternalMemoryBufferCreateInfo{
                .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
            },
        };

    // Bind imported memory to buffer
    const vk::UniqueBuffer imported_buffer = device.createBufferUnique(buffer_info.get());
    device.bindBufferMemory(imported_buffer.get(), imported_memory.get(), 0);

    Frame frame{};
    main_present_window.RecreateFrame(&frame, width, height);

    DrawScreens(&frame, layout, layout, false);

    scheduler.Record([buffer_image_copy, source_image = frame.image,
                      imported_buffer = imported_buffer.get()](vk::CommandBuffer cmdbuf) {
        const vk::ImageMemoryBarrier read_barrier = {
            .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
            .dstAccessMask = vk::AccessFlagBits::eTransferRead,
            .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = source_image,
            .subresourceRange{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };
        const vk::ImageMemoryBarrier write_barrier = {
            .srcAccessMask = vk::AccessFlagBits::eTransferRead,
            .dstAccessMask = vk::AccessFlagBits::eMemoryWrite,
            .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = source_image,
            .subresourceRange{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };
        static constexpr vk::MemoryBarrier memory_write_barrier = {
            .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
            .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
        };

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, {}, {}, read_barrier);
        cmdbuf.copyImageToBuffer(source_image, vk::ImageLayout::eTransferSrcOptimal,
                                 imported_buffer, buffer_image_copy);
        cmdbuf.pipelineBarrier(
            vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands,
            vk::DependencyFlagBits::eByRegion, memory_write_barrier, {}, write_barrier);
    });

    // Ensure the copy is fully completed before saving the screenshot
    scheduler.Finish();

    // QImage::Format_RGB32 expects BGRA byte order. If the swapchain format is RGBA,
    // swap R and B channels so the screenshot colors are correct.
    if (main_present_window.GetSurfaceFormat() == vk::Format::eR8G8B8A8Unorm) {
        u8* pixels = static_cast<u8*>(settings.screenshot_bits);
        for (u32 i = 0; i < width * height; i++) {
            std::swap(pixels[i * 4 + 0], pixels[i * 4 + 2]);
        }
    }

    // Image data has been copied directly to host memory
    device.destroyFramebuffer(frame.framebuffer);
    device.destroyImageView(frame.image_view);

    return true;
}

void RendererVulkan::NotifySurfaceChanged(bool is_second_window) {
    if (is_second_window) {
        if (secondary_present_window_ptr) {
            secondary_present_window_ptr->NotifySurfaceChanged();
        }
    } else {
        main_present_window.NotifySurfaceChanged();
    }
}

} // namespace Vulkan
