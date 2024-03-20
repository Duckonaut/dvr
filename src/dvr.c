#include "dvr.h"

#include <math.h>
#include <string.h>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <stb/stb_ds.h>

#ifdef RELEASE
#define DVR_ENABLE_VALIDATION_LAYERS false
#else
#define DVR_ENABLE_VALIDATION_LAYERS true
#endif
#define DVR_MAX_FRAMES_IN_FLIGHT 2

typedef struct dvr_buffer_data {
    dvr_buffer_lifecycle lifecycle;
    struct {
        VkBuffer buffer;
        VkDeviceMemory memory;
        void* memmap;
    } vk;
} dvr_buffer_data;

typedef struct dvr_image_data {
    struct {
        VkImage image;
        VkDeviceMemory memory;
        VkImageView view;
        VkFormat format;
    } vk;
    u32 width;
    u32 height;
    u32 mip_level;
} dvr_image_data;

#define DVR_MAX_BUFFERS 1024
#define DVR_MAX_IMAGES 1024

typedef struct dvr_state {
    struct {
        VkInstance instance;
        VkDebugUtilsMessengerEXT debug_messenger;
        VkPhysicalDevice physical_device;
        VkDevice device;
        VkQueue graphics_queue;
        VkQueue present_queue;
        VkSurfaceKHR surface;
        VkSwapchainKHR swapchain;
        VkImage* swapchain_images;
        VkImageView* swapchain_image_views;
        VkFramebuffer* swapchain_framebuffers;
        VkFormat swapchain_format;
        VkExtent2D swapchain_extent;
        VkRenderPass render_pass;
        VkDescriptorPool descriptor_pool;
        VkDescriptorSetLayout descriptor_set_layout;
        VkPipelineLayout pipeline_layout;
        VkPipeline pipeline;
        VkCommandPool command_pool;
        VkSampleCountFlagBits msaa_samples;
        VkCommandBuffer command_buffers[DVR_MAX_FRAMES_IN_FLIGHT];
        VkSemaphore image_available_sems[DVR_MAX_FRAMES_IN_FLIGHT];
        VkSemaphore render_finished_sems[DVR_MAX_FRAMES_IN_FLIGHT];
        VkFence in_flight_fences[DVR_MAX_FRAMES_IN_FLIGHT];
        u32 current_frame;
    } vk;
    struct {
        dvr_buffer_data buffers[DVR_MAX_BUFFERS];
        usize buffer_usage_map[DVR_MAX_BUFFERS / 64];
        dvr_image_data images[DVR_MAX_IMAGES];
        usize image_usage_map[DVR_MAX_IMAGES / 64];
    } res;
    struct {
        GLFWwindow* window;
        bool just_resized;
    } window;
} dvr_state;

static dvr_state g_dvr_state;

#define DVR_DEVICE g_dvr_state.vk.device

static u16 dvr_find_free_slot(usize* usage_map, u16 len) {
    for (u16 i = 0; i < len; i++) {
        if ((usage_map[i / 64] & (1ULL << (i % 64))) == 0) {
            return i;
        }
    }

    return (u16)~0;
}

static void dvr_set_slot_used(usize* usage_map, u16 slot) {
    usage_map[slot / 64] |= 1ULL << (slot % 64);
}

static void dvr_set_slot_free(usize* usage_map, u16 slot) {
    usage_map[slot / 64] &= ~(1ULL << (slot % 64));
}

static DVR_RESULT(u32)
    dvr_vk_find_memory_type(u32 type_filter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(g_dvr_state.vk.physical_device, &mem_props);

    for (u32 i = 0; i < mem_props.memoryTypeCount; i++) {
        if (type_filter & (1 << i) && (mem_props.memoryTypes[i].propertyFlags & properties)) {
            return DVR_OK(u32, i);
        }
    }

    return DVR_ERROR(u32, "failed to find suitable memory type");
}

static VkCommandBuffer dvr_vk_begin_transient_commands() {
    VkCommandBufferAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandPool = g_dvr_state.vk.command_pool,
        .commandBufferCount = 1,
    };

    VkCommandBuffer command_buffer;
    vkAllocateCommandBuffers(DVR_DEVICE, &alloc_info, &command_buffer);

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };

    vkBeginCommandBuffer(command_buffer, &begin_info);

    return command_buffer;
}

static void dvr_vk_end_transient_commands(VkCommandBuffer command_buffer) {
    vkEndCommandBuffer(command_buffer);

    VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer,
    };

    vkQueueSubmit(g_dvr_state.vk.graphics_queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_dvr_state.vk.graphics_queue);

    vkFreeCommandBuffers(DVR_DEVICE, g_dvr_state.vk.command_pool, 1, &command_buffer);
}

// DVR OBJECT FUNCTIONS

// DVR_BUFFER FUNCTIONS
static dvr_buffer_data* dvr_get_buffer_data(dvr_buffer buffer) {
    if (buffer.id >= DVR_MAX_BUFFERS) {
        DVRLOG_ERROR("buffer id out of range: %u", buffer.id);
        return NULL;
    }

    return &g_dvr_state.res.buffers[buffer.id];
}

static DVR_RESULT(dvr_none) dvr_vk_create_buffer(
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties,
    VkBuffer* buffer,
    VkDeviceMemory* memory
) {
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .flags = 0,
    };

    if (vkCreateBuffer(DVR_DEVICE, &buffer_info, NULL, buffer) != VK_SUCCESS) {
        return DVR_ERROR(dvr_none, "failed to create vertex buffer");
    }

    VkMemoryRequirements mem_reqs;
    vkGetBufferMemoryRequirements(DVR_DEVICE, *buffer, &mem_reqs);

    DVR_RESULT(u32)
    mem_type_id_res = dvr_vk_find_memory_type(mem_reqs.memoryTypeBits, properties);

    DVR_BUBBLE_INTO(dvr_none, mem_type_id_res);

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = DVR_UNWRAP(mem_type_id_res),
    };

    if (vkAllocateMemory(DVR_DEVICE, &alloc_info, NULL, memory) != VK_SUCCESS) {
        return DVR_ERROR(dvr_none, "failed to allocate vertex buffer memory");
    }

    vkBindBufferMemory(DVR_DEVICE, *buffer, *memory, 0);

    return DVR_OK(dvr_none, DVR_NONE);
}

static void dvr_vk_copy_buffer(VkBuffer src, VkBuffer dst, VkDeviceSize size) {
    VkCommandBuffer command_buffer = dvr_vk_begin_transient_commands();

    VkBufferCopy copy_region = {
        .size = size,
        .srcOffset = 0,
        .dstOffset = 0,
    };

    vkCmdCopyBuffer(command_buffer, src, dst, 1, &copy_region);

    dvr_vk_end_transient_commands(command_buffer);
}

static DVR_RESULT(dvr_buffer) dvr_vk_create_static_buffer(dvr_buffer_desc* desc) {
    VkBuffer src_buffer;
    VkDeviceMemory src_memory;

    DVR_RESULT(dvr_none)
    result = dvr_vk_create_buffer(
        desc->data.size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &src_buffer,
        &src_memory
    );
    DVR_BUBBLE_INTO(dvr_buffer, result);

    void* mapped;
    if (desc->data.base != NULL) {
        vkMapMemory(DVR_DEVICE, src_memory, 0, desc->data.size, 0, &mapped);
        memcpy(mapped, desc->data.base, desc->data.size);
        vkUnmapMemory(DVR_DEVICE, src_memory);
    }

    VkBuffer dst_buffer;
    VkDeviceMemory dst_memory;

    result = dvr_vk_create_buffer(
        desc->data.size,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | desc->usage,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        &dst_buffer,
        &dst_memory
    );
    DVR_BUBBLE_INTO(dvr_buffer, result);

    // copy data to buffer
    dvr_vk_copy_buffer(src_buffer, dst_buffer, (VkDeviceSize)desc->data.size);

    dvr_buffer_data buf = {
        .vk.buffer = dst_buffer,
        .vk.memory = dst_memory,
        .vk.memmap = NULL,
        .lifecycle = desc->lifecycle,
    };

    u16 free_slot = dvr_find_free_slot(g_dvr_state.res.buffer_usage_map, DVR_MAX_BUFFERS);
    g_dvr_state.res.buffers[free_slot] = buf;
    dvr_set_slot_used(g_dvr_state.res.buffer_usage_map, free_slot);

    vkDestroyBuffer(DVR_DEVICE, src_buffer, NULL);
    vkFreeMemory(DVR_DEVICE, src_memory, NULL);

    return DVR_OK(dvr_buffer, (dvr_buffer){ .id = free_slot });
}

static DVR_RESULT(dvr_buffer) dvr_vk_create_dynamic_buffer(dvr_buffer_desc* desc) {
    VkBuffer buffer;
    VkDeviceMemory memory;

    DVR_RESULT(dvr_none)
    result = dvr_vk_create_buffer(
        desc->data.size,
        desc->usage,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &buffer,
        &memory
    );
    DVR_BUBBLE_INTO(dvr_buffer, result);

    void* mapped;
    vkMapMemory(DVR_DEVICE, memory, 0, desc->data.size, 0, &mapped);

    dvr_buffer_data buf = {
        .vk.buffer = buffer,
        .vk.memory = memory,
        .vk.memmap = mapped,
        .lifecycle = desc->lifecycle,
    };

    u16 free_slot = dvr_find_free_slot(g_dvr_state.res.buffer_usage_map, DVR_MAX_BUFFERS);
    g_dvr_state.res.buffers[free_slot] = buf;
    dvr_set_slot_used(g_dvr_state.res.buffer_usage_map, free_slot);

    return DVR_OK(dvr_buffer, (dvr_buffer){ .id = free_slot });
}

DVR_RESULT(dvr_buffer) dvr_create_buffer(dvr_buffer_desc* desc) {
    switch (desc->lifecycle) {
        case DVR_BUFFER_LIFECYCLE_STATIC:
            return dvr_vk_create_static_buffer(desc);
        case DVR_BUFFER_LIFECYCLE_DYNAMIC:
            return dvr_vk_create_dynamic_buffer(desc);
        default:
            return DVR_ERROR(dvr_buffer, "unknown buffer lifecycle");
    }
}

void dvr_vk_destroy_buffer(dvr_buffer buffer) {
    dvr_buffer_data* buf = dvr_get_buffer_data(buffer);
    vkDestroyBuffer(DVR_DEVICE, buf->vk.buffer, NULL);
    if (buf->lifecycle == DVR_BUFFER_LIFECYCLE_DYNAMIC) {
        vkUnmapMemory(DVR_DEVICE, buf->vk.memory);
    }
    vkFreeMemory(DVR_DEVICE, buf->vk.memory, NULL);
}

void dvr_destroy_buffer(dvr_buffer buffer) {
    dvr_vk_destroy_buffer(buffer);

    dvr_set_slot_free(g_dvr_state.res.buffer_usage_map, buffer.id);
}

void dvr_write_buffer(dvr_buffer buffer, dvr_range new_data) {
    dvr_buffer_data* buf = dvr_get_buffer_data(buffer);
    if (buf->lifecycle != DVR_BUFFER_LIFECYCLE_DYNAMIC) {
        DVRLOG_ERROR("cannot write to buffers not marked as dynamic");
        return;
    }

    memcpy(buf->vk.memmap, new_data.base, new_data.size);
}

// DVR_IMAGE FUNCTIONS

static dvr_image_data* dvr_get_image_data(dvr_image image) {
    if (image.id >= DVR_MAX_IMAGES) {
        DVRLOG_ERROR("image id out of range: %u", image.id);
        return NULL;
    }

    return &g_dvr_state.res.images[image.id];
}

static void dvr_vk_transition_image_layout(
    VkImage image,
    VkFormat format,
    VkImageLayout old_layout,
    VkImageLayout new_layout,
    u32 mip_levels
) {
    VkImageAspectFlags aspect = 0;

    switch (format) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
            break;
        default:
            aspect = VK_IMAGE_ASPECT_COLOR_BIT;
            break;
    }

    VkCommandBuffer command_buffer = dvr_vk_begin_transient_commands();

    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {
            .aspectMask = aspect,
            .baseMipLevel = 0,
            .levelCount = mip_levels,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };

    VkPipelineStageFlags src_stage = {};
    VkPipelineStageFlags dst_stage = {};

    if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED &&
        new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED && new_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dst_stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED && new_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask =
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;

        src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dst_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    } else {
        DVRLOG_ERROR("unsupported layout transition, expect validation layers to complain");
    }

    vkCmdPipelineBarrier(
        command_buffer,
        src_stage,
        dst_stage,
        0,
        0,
        NULL,
        0,
        NULL,
        1,
        &barrier
    );

    dvr_vk_end_transient_commands(command_buffer);
}

static void dvr_vk_copy_buffer_to_image(VkBuffer buffer, VkImage image, u32 width, u32 height) {
    VkCommandBuffer command_buffer = dvr_vk_begin_transient_commands();

    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = {
            width,
            height,
            1,
        },
    };

    vkCmdCopyBufferToImage(
        command_buffer,
        buffer,
        image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &region
    );

    dvr_vk_end_transient_commands(command_buffer);
}

DVR_RESULT_DEF(VkImageView);

static DVR_RESULT(VkImageView)
    dvr_vk_create_image_view(VkImage image, VkFormat format, u32 mip_levels) {
    VkImageAspectFlags aspect = 0;
    switch (format) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
            break;
        default:
            aspect = VK_IMAGE_ASPECT_COLOR_BIT;
            break;
    }

    VkImageView view;

    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {
            .aspectMask = aspect,
            .baseMipLevel = 0,
            .levelCount = mip_levels,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };

    if (vkCreateImageView(DVR_DEVICE, &view_info, NULL, &view) != VK_SUCCESS) {
        return DVR_ERROR(VkImageView, "failed to create image view");
    }
    return DVR_OK(VkImageView, view);
}

static DVR_RESULT(dvr_none) dvr_vk_generate_image_mipmaps(dvr_image_data* img) {
    VkFormatProperties format_properties;
    vkGetPhysicalDeviceFormatProperties(
        g_dvr_state.vk.physical_device,
        img->vk.format,
        &format_properties
    );

    if (!(format_properties.optimalTilingFeatures &
          VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
        return DVR_ERROR(dvr_none, "image format does not support linear filtering");
    }

    VkCommandBuffer command_buffer = dvr_vk_begin_transient_commands();

    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .image = img->vk.image,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseArrayLayer = 0,
            .layerCount = 1,
            .levelCount = 1,
        },
    };

    i32 mip_width = (i32)img->width;
    i32 mip_height = (i32)img->height;

    for (u32 i = 1; i < img->mip_level; i++) {
        barrier.subresourceRange.baseMipLevel = i - 1;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

        vkCmdPipelineBarrier(
            command_buffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            0,
            NULL,
            0,
            NULL,
            1,
            &barrier
        );

        VkImageBlit blit = {
            .srcOffsets[0] = { 0, 0, 0 },
            .srcOffsets[1] = { mip_width, mip_height, 1 },
            .srcSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = i - 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .dstOffsets[0] = { 0, 0, 0 },
            .dstOffsets[1] = { mip_width > 1 ? mip_width / 2 : 1, mip_height > 1 ? mip_height / 2 : 1, 1 },
            .dstSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = i,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };

        vkCmdBlitImage(
            command_buffer,
            img->vk.image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            img->vk.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1,
            &blit,
            VK_FILTER_LINEAR
        );

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(
            command_buffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0,
            0,
            NULL,
            0,
            NULL,
            1,
            &barrier
        );

        if (mip_width > 1)
            mip_width /= 2;
        if (mip_height > 1)
            mip_height /= 2;
    }

    barrier.subresourceRange.baseMipLevel = img->mip_level - 1;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(
        command_buffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,
        0,
        NULL,
        0,
        NULL,
        1,
        &barrier
    );

    dvr_vk_end_transient_commands(command_buffer);

    return DVR_OK(dvr_none, DVR_NONE);
}

DVR_RESULT(dvr_image) dvr_vk_create_image(dvr_image_desc* desc) {
    if (desc->num_samples == 0) {
        desc->num_samples = VK_SAMPLE_COUNT_1_BIT;
    }

    u32 mip_levels = 1;
    if (desc->generate_mipmaps) {
        mip_levels = (u32)log2(fmax(desc->width, desc->height)) + 1;
    }

    bool has_data = desc->data.base != NULL;

    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .extent.width = desc->width,
        .extent.height = desc->height,
        .extent.depth = 1,
        .mipLevels = mip_levels,
        .arrayLayers = 1,
        .format = desc->format,
        .tiling = desc->tiling,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = desc->usage | (has_data ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .samples = desc->num_samples,
        .flags = 0,
    };

    VkImage image;
    VkDeviceMemory memory;

    if (vkCreateImage(DVR_DEVICE, &image_info, NULL, &image) != VK_SUCCESS) {
        return DVR_ERROR(dvr_image, "failed to create image");
    }

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(DVR_DEVICE, image, &mem_reqs);

    DVR_RESULT(u32)
    memory_type_res = dvr_vk_find_memory_type(mem_reqs.memoryTypeBits, desc->properties);
    DVR_BUBBLE_INTO(dvr_image, memory_type_res);

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = DVR_UNWRAP(memory_type_res),
    };

    if (vkAllocateMemory(DVR_DEVICE, &alloc_info, NULL, &memory) != VK_SUCCESS) {
        return DVR_ERROR(dvr_image, "failed to allocate image memory");
    }

    vkBindImageMemory(DVR_DEVICE, image, memory, 0);

    DVR_RESULT(VkImageView)
    view_res = dvr_vk_create_image_view(image, desc->format, mip_levels);
    DVR_BUBBLE_INTO(dvr_image, view_res);

    dvr_image_data img = {
        .vk.image = image,
        .vk.memory = memory,
        .vk.view = DVR_UNWRAP(view_res),
        .vk.format = desc->format,
        .width = desc->width,
        .height = desc->height,
        .mip_level = mip_levels,
    };

    if (has_data) {
        VkBuffer staging_buffer;
        VkDeviceMemory staging_memory;

        DVR_RESULT(dvr_none)
        result = dvr_vk_create_buffer(
            desc->data.size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            &staging_buffer,
            &staging_memory
        );
        DVR_BUBBLE_INTO(dvr_image, result);

        void* mapped;
        vkMapMemory(DVR_DEVICE, staging_memory, 0, desc->data.size, 0, &mapped);
        memcpy(mapped, desc->data.base, desc->data.size);
        vkUnmapMemory(DVR_DEVICE, staging_memory);

        dvr_vk_transition_image_layout(
            image,
            desc->format,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            mip_levels
        );

        dvr_vk_copy_buffer_to_image(staging_buffer, image, desc->width, desc->height);

        vkDestroyBuffer(DVR_DEVICE, staging_buffer, NULL);
        vkFreeMemory(DVR_DEVICE, staging_memory, NULL);

        if (desc->generate_mipmaps) {
            DVR_RESULT(dvr_none)
            mipmap_res = dvr_vk_generate_image_mipmaps(&img);
            DVR_BUBBLE_INTO(dvr_image, mipmap_res);
        } else {
            dvr_vk_transition_image_layout(
                image,
                desc->format,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                mip_levels
            );
        }
    }

    u16 free_slot = dvr_find_free_slot(g_dvr_state.res.image_usage_map, DVR_MAX_IMAGES);
    g_dvr_state.res.images[free_slot] = img;
    dvr_set_slot_used(g_dvr_state.res.image_usage_map, free_slot);

    return DVR_OK(dvr_image, (dvr_image){ .id = free_slot });
}

DVR_RESULT(dvr_image) dvr_create_image(dvr_image_desc* desc) {
    return dvr_vk_create_image(desc);
}

void dvr_vk_destroy_image(dvr_image image) {
    dvr_image_data* img = dvr_get_image_data(image);
    vkDestroyImageView(DVR_DEVICE, img->vk.view, NULL);
    vkDestroyImage(DVR_DEVICE, img->vk.image, NULL);
    vkFreeMemory(DVR_DEVICE, img->vk.memory, NULL);
}

void dvr_destroy_image(dvr_image image) {
    dvr_vk_destroy_image(image);

    dvr_set_slot_free(g_dvr_state.res.image_usage_map, image.id);
}

// DVR GLOBAL STATE FUNCTIONS

static const char* dvr_validation_layers[] = {
    "VK_LAYER_KHRONOS_validation",
};

static bool dvr_check_validation_layer_support(void) {
    u32 layer_count;
    vkEnumerateInstanceLayerProperties(&layer_count, NULL);

    VkLayerProperties* available_layers = malloc(layer_count * sizeof(VkLayerProperties));
    vkEnumerateInstanceLayerProperties(&layer_count, available_layers);

    for (u32 i = 0; i < sizeof(dvr_validation_layers) / sizeof(dvr_validation_layers[0]); i++) {
        bool layer_found = false;

        for (u32 j = 0; j < layer_count; j++) {
            if (strcmp(dvr_validation_layers[i], available_layers[j].layerName) == 0) {
                layer_found = true;
                break;
            }
        }

        if (!layer_found) {
            free(available_layers);
            return false;
        }
    }

    free(available_layers);
    return true;
}

static const char** dvr_get_required_instance_extensions(u32* count) {
    u32 glfw_extension_count = 0;
    const char** glfw_extensions = glfwGetRequiredInstanceExtensions(&glfw_extension_count);

    *count = glfw_extension_count;
    const char** extensions = NULL;
    arrsetlen(extensions, glfw_extension_count);
    memcpy(extensions, glfw_extensions, glfw_extension_count * sizeof(const char*));

    if (DVR_ENABLE_VALIDATION_LAYERS) {
        arrput(extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        (*count)++;
    }

    return extensions;
}

static DVR_RESULT(dvr_none) dvr_vk_create_instance(void) {
    if (DVR_ENABLE_VALIDATION_LAYERS && !dvr_check_validation_layer_support()) {
        return DVR_ERROR(dvr_none, "validation layers requested, but not available!");
    }

    VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = PROJECT_NAME,
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "dvr",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_0,
    };

    VkInstanceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
    };

    u32 extension_count = 0;
    const char** extensions = dvr_get_required_instance_extensions(&extension_count);
    create_info.enabledExtensionCount = extension_count;
    create_info.ppEnabledExtensionNames = extensions;
    create_info.enabledLayerCount = 0;

    if (DVR_ENABLE_VALIDATION_LAYERS) {
        create_info.enabledLayerCount =
            sizeof(dvr_validation_layers) / sizeof(dvr_validation_layers[0]);
        create_info.ppEnabledLayerNames = dvr_validation_layers;
    }

    VkResult result = vkCreateInstance(&create_info, NULL, &g_dvr_state.vk.instance);
    if (result != VK_SUCCESS) {
        arrfree(extensions);
        return DVR_ERROR(dvr_none, "vkCreateInstance failed!");
    }

    arrfree(extensions);
    return DVR_OK(dvr_none, DVR_NONE);
}

static VKAPI_ATTR VkBool32 VKAPI_CALL dvr_debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT message_severity,
    VkDebugUtilsMessageTypeFlagsEXT message_type,
    const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
    void* user_data
) {
    (void)user_data;
    const char* message_type_str;
    switch (message_type) {
        case VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT:
            message_type_str = "general";
            break;
        case VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT:
            message_type_str = "validation";
            break;
        case VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT:
            message_type_str = "performance";
            break;
        default:
            message_type_str = "unknown";
            break;
    }
    switch (message_severity) {
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
            DVRLOG_DEBUG("%s: %s", message_type_str, callback_data->pMessage);
            break;
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
            DVRLOG_INFO("%s: %s", message_type_str, callback_data->pMessage);
            break;
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
            DVRLOG_WARNING("%s: %s", message_type_str, callback_data->pMessage);
            break;
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
            DVRLOG_ERROR("%s: %s", message_type_str, callback_data->pMessage);
            break;
        default:
            DVRLOG_ERROR("unknown: %s", callback_data->pMessage);
            break;
    }

    return VK_FALSE;
}

static void dvr_vk_create_debug_messenger(void) {
    if (!DVR_ENABLE_VALIDATION_LAYERS) {
        return;
    }

    VkDebugUtilsMessengerCreateInfoEXT create_info = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = dvr_debug_callback,
    };

    PFN_vkCreateDebugUtilsMessengerEXT vkCreateDebugUtilsMessengerEXT =
        (PFN_vkCreateDebugUtilsMessengerEXT
        )vkGetInstanceProcAddr(g_dvr_state.vk.instance, "vkCreateDebugUtilsMessengerEXT");
    if (vkCreateDebugUtilsMessengerEXT == NULL) {
        DVRLOG_ERROR("vkCreateDebugUtilsMessengerEXT is NULL");
        return;
    }

    VkResult result = vkCreateDebugUtilsMessengerEXT(
        g_dvr_state.vk.instance,
        &create_info,
        NULL,
        &g_dvr_state.vk.debug_messenger
    );
    if (result != VK_SUCCESS) {
        DVRLOG_ERROR("vkCreateDebugUtilsMessengerEXT failed: %d", result);
    }
}

DVR_RESULT(dvr_none) dvr_vk_create_surface(void) {
    if (glfwCreateWindowSurface(
            g_dvr_state.vk.instance,
            g_dvr_state.window.window,
            NULL,
            &g_dvr_state.vk.surface
        ) != VK_SUCCESS) {
        return DVR_ERROR(dvr_none, "failed to create window surface");
    }

    return DVR_OK(dvr_none, DVR_NONE);
}

static void dvr_glfw_resize(GLFWwindow* window, int width, int height) {
    (void)window;
    (void)width;
    (void)height;
    g_dvr_state.window.just_resized = true;
}

static DVR_RESULT(dvr_none) dvr_init_window(dvr_setup_desc* desc) {
    glfwInit();

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    glfwWindowHint(GLFW_DECORATED, GLFW_TRUE);
    if (glfwPlatformSupported(GLFW_PLATFORM_WAYLAND)) {
        DVRLOG_INFO("using wayland");
        glfwWindowHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);
    }
    g_dvr_state.window.window = glfwCreateWindow(
        (i32)desc->initial_width,
        (i32)desc->initial_height,
        desc->app_name,
        NULL,
        NULL
    );

    glfwSetFramebufferSizeCallback(g_dvr_state.window.window, dvr_glfw_resize);

    return DVR_OK(dvr_none, DVR_NONE);
}

typedef struct queue_family_indices {
    u32 graphics_family;
    bool graphics_family_found;
    u32 present_family;
    bool present_family_found;
} queue_family_indices;

static queue_family_indices find_queue_families(VkPhysicalDevice dev) {
    queue_family_indices indices;
    indices.graphics_family_found = false;

    u32 queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &queue_family_count, NULL);

    VkQueueFamilyProperties* queue_families =
        malloc(sizeof(VkQueueFamilyProperties) * queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &queue_family_count, queue_families);

    for (u32 i = 0; i < queue_family_count; i++) {
        if (queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            indices.graphics_family = i;
            indices.graphics_family_found = true;
        }
        VkBool32 present_support = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, g_dvr_state.vk.surface, &present_support);
        if (present_support) {
            indices.present_family = i;
            indices.present_family_found = true;
        }
    }

    return indices;
}

typedef struct swapchain_support_details {
    VkSurfaceCapabilitiesKHR capabilities;
    VkSurfaceFormatKHR* formats;
    VkPresentModeKHR* present_modes;
} swapchain_support_details;

static swapchain_support_details query_swapchain_support(VkPhysicalDevice dev) {
    swapchain_support_details details;
    memset(&details, 0, sizeof(details));
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        dev,
        g_dvr_state.vk.surface,
        &details.capabilities
    );

    u32 format_count;
    vkGetPhysicalDeviceSurfaceFormatsKHR(dev, g_dvr_state.vk.surface, &format_count, NULL);

    if (format_count == 0) {
        DVRLOG_WARNING("no surface formats supported");
    } else {
        arrsetlen(details.formats, format_count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(
            dev,
            g_dvr_state.vk.surface,
            &format_count,
            details.formats
        );
    }

    u32 present_mode_count;
    vkGetPhysicalDeviceSurfacePresentModesKHR(
        dev,
        g_dvr_state.vk.surface,
        &present_mode_count,
        NULL
    );

    if (present_mode_count == 0) {
        DVRLOG_WARNING("no present modes supported");
    } else {
        arrsetlen(details.present_modes, present_mode_count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(
            dev,
            g_dvr_state.vk.surface,
            &present_mode_count,
            details.present_modes
        );
    }

    return details;
}

static VkSurfaceFormatKHR choose_swapchain_format(const VkSurfaceFormatKHR* formats, usize n) {
    for (usize i = 0; i < n; i++) {
        if (formats[i].format == VK_FORMAT_B8G8R8_SRGB &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return formats[i];
        }
    }

    return formats[0];
}

static const VkPresentModeKHR dvr_preferred_present_mode_order[] = {
    VK_PRESENT_MODE_IMMEDIATE_KHR,
    VK_PRESENT_MODE_MAILBOX_KHR,
    VK_PRESENT_MODE_FIFO_KHR,
    VK_PRESENT_MODE_FIFO_RELAXED_KHR,
};

static VkPresentModeKHR choose_present_mode(const VkPresentModeKHR* present_modes, usize n) {
    for (usize i = 0; i < sizeof(dvr_preferred_present_mode_order) /
                              sizeof(dvr_preferred_present_mode_order[0]);
         i++) {
        for (usize j = 0; j < n; j++) {
            if (present_modes[j] == dvr_preferred_present_mode_order[i]) {
                return present_modes[j];
            }
        }
    }

    return present_modes[0];
}

static VkExtent2D choose_swap_extent(VkSurfaceCapabilitiesKHR capabilities) {
    if (capabilities.currentExtent.width != UINT32_MAX) {
        return capabilities.currentExtent;
    } else {
        i32 width, height;
        glfwGetFramebufferSize(g_dvr_state.window.window, &width, &height);

        VkExtent2D extent = {
            .width = dvr_clampu(
                (u32)width,
                capabilities.minImageExtent.width,
                capabilities.maxImageExtent.width
            ),
            .height = dvr_clampu(
                (u32)height,
                capabilities.minImageExtent.height,
                capabilities.maxImageExtent.height
            ),
        };

        return extent;
    }
}

const char* dvr_required_device_extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

static bool check_device_extension_support(VkPhysicalDevice dev) {
    u32 extension_count;
    vkEnumerateDeviceExtensionProperties(dev, NULL, &extension_count, NULL);

    VkExtensionProperties* available_extensions =
        malloc(sizeof(VkExtensionProperties) * extension_count);
    vkEnumerateDeviceExtensionProperties(dev, NULL, &extension_count, available_extensions);

    for (usize i = 0;
         i < sizeof(dvr_required_device_extensions) / sizeof(dvr_required_device_extensions[0]);
         i++) {
        bool found = false;
        for (usize e = 0; e < extension_count; e++) {
            VkExtensionProperties* extension = &available_extensions[e];
            if (strncmp(dvr_required_device_extensions[i], extension->extensionName, 256) ==
                0) {
                found = true;
                break;
            }
        }

        if (!found) {
            free(available_extensions);
            return false;
        }
    }

    free(available_extensions);
    return true;
}

static usize rate_device(VkPhysicalDevice dev) {
    VkPhysicalDeviceProperties device_properties;
    vkGetPhysicalDeviceProperties(dev, &device_properties);

    VkPhysicalDeviceFeatures device_features;
    vkGetPhysicalDeviceFeatures(dev, &device_features);

    usize score = 0;

    if (device_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
        score += 1000;
    }

    score += device_properties.limits.maxImageDimension2D;

    if (!device_features.geometryShader) {
        DVRLOG_WARNING("%s does not support geometry shaders", device_properties.deviceName);
        return 0;
    }

    if (!device_features.samplerAnisotropy) {
        DVRLOG_WARNING("%s does not support anisotropy", device_properties.deviceName);
        return 0;
    }

    queue_family_indices indices = find_queue_families(dev);
    if (!indices.graphics_family_found) {
        DVRLOG_WARNING(
            "%s does not support all required queue families",
            device_properties.deviceName
        );
        return 0;
    }
    swapchain_support_details details = query_swapchain_support(dev);
    bool swapchain_ok = arrlen(details.present_modes) != 0 && arrlen(details.formats) != 0;
    arrfree(details.formats);
    arrfree(details.present_modes);
    if (!swapchain_ok) {
        return 0;
    }

    if (!check_device_extension_support(dev)) {
        DVRLOG_WARNING(
            "%s does not support all required device extensions",
            device_properties.deviceName
        );
        return 0;
    }

    DVRLOG_INFO("%s score: %zu", device_properties.deviceName, score);
    return score;
}

static VkSampleCountFlagBits _dvr_vk_get_max_usable_sample_count() {
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(g_dvr_state.vk.physical_device, &properties);

    VkSampleCountFlags counts = properties.limits.framebufferColorSampleCounts &
                                properties.limits.framebufferDepthSampleCounts;

    if (counts & VK_SAMPLE_COUNT_64_BIT)
        return VK_SAMPLE_COUNT_64_BIT;
    if (counts & VK_SAMPLE_COUNT_32_BIT)
        return VK_SAMPLE_COUNT_32_BIT;
    if (counts & VK_SAMPLE_COUNT_16_BIT)
        return VK_SAMPLE_COUNT_16_BIT;
    if (counts & VK_SAMPLE_COUNT_8_BIT)
        return VK_SAMPLE_COUNT_8_BIT;
    if (counts & VK_SAMPLE_COUNT_4_BIT)
        return VK_SAMPLE_COUNT_4_BIT;
    if (counts & VK_SAMPLE_COUNT_2_BIT)
        return VK_SAMPLE_COUNT_2_BIT;
    if (counts & VK_SAMPLE_COUNT_1_BIT)
        return VK_SAMPLE_COUNT_1_BIT;

    return VK_SAMPLE_COUNT_1_BIT;
}

static DVR_RESULT(dvr_none) dvr_vk_pick_physical_device(void) {
    u32 device_count = 0;
    vkEnumeratePhysicalDevices(g_dvr_state.vk.instance, &device_count, NULL);

    if (device_count == 0) {
        return DVR_ERROR(dvr_none, "no GPUs with Vulkan support detected.");
    }

    VkPhysicalDevice* devices = malloc(sizeof(VkPhysicalDevice) * device_count);
    vkEnumeratePhysicalDevices(g_dvr_state.vk.instance, &device_count, devices);

    usize* device_scores = malloc(sizeof(usize) * device_count);
    for (usize i = 0; i < device_count; i++) {
        device_scores[i] = rate_device(devices[i]);
    }

    usize best_index = 0;
    usize best_score = 0;
    for (usize i = 0; i < device_count; i++) {
        if (device_scores[i] > best_score) {
            best_index = i;
            best_score = device_scores[i];
        }
    }

    g_dvr_state.vk.physical_device = devices[best_index];
    g_dvr_state.vk.msaa_samples = _dvr_vk_get_max_usable_sample_count();

    free(devices);
    free(device_scores);

    return DVR_OK(dvr_none, DVR_NONE);
}

static DVR_RESULT(dvr_none) dvr_vk_create_logical_device(void) {
    queue_family_indices indices = find_queue_families(g_dvr_state.vk.physical_device);

    u32* unique_queue_families = NULL;
    arrput(unique_queue_families, indices.graphics_family);
    if (indices.present_family != indices.graphics_family) {
        arrput(unique_queue_families, indices.present_family);
    }

    VkDeviceQueueCreateInfo* queue_create_infos = NULL;

    for (u32 i = 0; i < arrlenu(unique_queue_families); i++) {
        u32 qf = unique_queue_families[i];
        VkDeviceQueueCreateInfo qci = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = qf,
            .queueCount = 1,
            .pQueuePriorities = &(f32){ 1.0f }, // compound literals!
        };
        arrput(queue_create_infos, qci);
    }

    VkPhysicalDeviceFeatures device_features = {
        .samplerAnisotropy = VK_TRUE,
    };

    VkDeviceCreateInfo device_create_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pQueueCreateInfos = queue_create_infos,
        .queueCreateInfoCount = (u32)arrlenu(queue_create_infos),
        .pEnabledFeatures = &device_features,
        .enabledExtensionCount =
            sizeof(dvr_required_device_extensions) / sizeof(dvr_required_device_extensions[0]),
        .ppEnabledExtensionNames = dvr_required_device_extensions,
    };

    if (DVR_ENABLE_VALIDATION_LAYERS) {
        device_create_info.enabledLayerCount =
            sizeof(dvr_validation_layers) / sizeof(dvr_validation_layers[0]);
        device_create_info.ppEnabledLayerNames = dvr_validation_layers;
    } else {
        device_create_info.enabledLayerCount = 0;
    }

    if (vkCreateDevice(
            g_dvr_state.vk.physical_device,
            &device_create_info,
            NULL,
            &DVR_DEVICE
        ) != VK_SUCCESS) {
        return DVR_ERROR(dvr_none, "failed to create logical device");
    }

    vkGetDeviceQueue(DVR_DEVICE, indices.graphics_family, 0, &g_dvr_state.vk.graphics_queue);
    vkGetDeviceQueue(DVR_DEVICE, indices.present_family, 0, &g_dvr_state.vk.present_queue);

    return DVR_OK(dvr_none, DVR_NONE);
}

static DVR_RESULT(dvr_none) dvr_vk_create_swapchain(void) {
    swapchain_support_details swapchain_support =
        query_swapchain_support(g_dvr_state.vk.physical_device);

    VkSurfaceFormatKHR surface_format =
        choose_swapchain_format(swapchain_support.formats, arrlenu(swapchain_support.formats));
    DVRLOG_DEBUG("surface format: %d", surface_format.format);
    DVRLOG_DEBUG("color space: %d", surface_format.colorSpace);
    VkPresentModeKHR present_mode = choose_present_mode(
        swapchain_support.present_modes,
        arrlenu(swapchain_support.formats)
    );
    DVRLOG_DEBUG("present mode: %d", present_mode);
    VkExtent2D extent = choose_swap_extent(swapchain_support.capabilities);
    DVRLOG_DEBUG("extent: %d x %d", extent.width, extent.height);

    arrfree(swapchain_support.formats);
    arrfree(swapchain_support.present_modes);

    u32 image_count = swapchain_support.capabilities.minImageCount + 1;
    if (swapchain_support.capabilities.maxImageCount > 0 &&
        swapchain_support.capabilities.maxImageCount < image_count) {
        image_count = swapchain_support.capabilities.maxImageCount;
    }

    DVRLOG_DEBUG("image count: %d", image_count);

    VkSwapchainCreateInfoKHR create_info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = g_dvr_state.vk.surface,
        .minImageCount = image_count,
        .imageFormat = surface_format.format,
        .imageColorSpace = surface_format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .preTransform = swapchain_support.capabilities.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = present_mode,
        .clipped = VK_TRUE,
        .oldSwapchain = VK_NULL_HANDLE,
    };

    queue_family_indices indices = find_queue_families(g_dvr_state.vk.physical_device);
    u32 qf_indices[] = { indices.graphics_family, indices.present_family };
    if (indices.graphics_family != indices.present_family) {
        create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        create_info.queueFamilyIndexCount = 2;
        create_info.pQueueFamilyIndices = qf_indices;
    } else {
        create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        create_info.queueFamilyIndexCount = 0;
        create_info.pQueueFamilyIndices = NULL;
    }

    if (vkCreateSwapchainKHR(DVR_DEVICE, &create_info, NULL, &g_dvr_state.vk.swapchain) !=
        VK_SUCCESS) {
        return DVR_ERROR(dvr_none, "failed to create swapchain");
    }

    vkGetSwapchainImagesKHR(DVR_DEVICE, g_dvr_state.vk.swapchain, &image_count, NULL);
    arrsetlen(g_dvr_state.vk.swapchain_images, image_count);
    vkGetSwapchainImagesKHR(
        DVR_DEVICE,
        g_dvr_state.vk.swapchain,
        &image_count,
        g_dvr_state.vk.swapchain_images
    );

    g_dvr_state.vk.swapchain_format = surface_format.format;
    g_dvr_state.vk.swapchain_extent = extent;

    return DVR_OK(dvr_none, DVR_NONE);
}

static DVR_RESULT(dvr_none) dvr_vk_create_swapchain_image_views(void) {
    arrsetlen(g_dvr_state.vk.swapchain_image_views, arrlen(g_dvr_state.vk.swapchain_images));

    for (usize i = 0; i < arrlenu(g_dvr_state.vk.swapchain_images); i++) {
        DVR_RESULT(VkImageView)
        image_view_res = dvr_vk_create_image_view(
            g_dvr_state.vk.swapchain_images[i],
            g_dvr_state.vk.swapchain_format,
            1
        );
        DVR_BUBBLE_INTO(dvr_none, image_view_res);

        g_dvr_state.vk.swapchain_image_views[i] = DVR_UNWRAP(image_view_res);
    }

    return DVR_OK(dvr_none, DVR_NONE);
}
