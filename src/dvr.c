#include "dvr.h"
#include "dvr_log.h"

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

typedef struct dvr_sampler_data {
    struct {
        VkSampler sampler;
    } vk;
} dvr_sampler_data;

typedef struct dvr_render_pass_data {
    struct {
        VkRenderPass render_pass;
    } vk;
} dvr_render_pass_data;

typedef struct dvr_shader_module_data {
    struct {
        VkShaderModule module;
    } vk;
} dvr_shader_module_data;

typedef struct dvr_pipeline_data {
    struct {
        VkPipelineLayout layout;
        VkPipeline pipeline;
    } vk;
} dvr_pipeline_data;

typedef struct dvr_framebuffer_data {
    struct {
        VkFramebuffer framebuffer;
    } vk;
    VkRect2D render_area;
} dvr_framebuffer_data;

typedef struct dvr_descriptor_set_layout_data {
    struct {
        VkDescriptorSetLayout layout;
    } vk;
} dvr_descriptor_set_layout_data;

typedef struct dvr_descriptor_set_data {
    struct {
        VkDescriptorSet set;
    } vk;
} dvr_descriptor_set_data;

#define DVR_MAX_BUFFERS 1024
#define DVR_MAX_IMAGES 1024
#define DVR_MAX_SAMPLERS 256
#define DVR_MAX_RENDER_PASSES 256
#define DVR_MAX_SHADER_MODULES 256
#define DVR_MAX_PIPELINES 256
#define DVR_MAX_FRAMEBUFFERS 64
#define DVR_MAX_DESCRIPTOR_SET_LAYOUTS 64
#define DVR_MAX_DESCRIPTOR_SETS 1024

#define DVR_POOL_MAX_UBOS 1024
#define DVR_POOL_MAX_SAMPLERS 1024

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
        VkFormat swapchain_format;
        VkExtent2D swapchain_extent;
        VkDescriptorPool descriptor_pool;
        VkCommandPool command_pool;
        VkCommandBuffer command_buffer;
        VkSampleCountFlagBits msaa_samples;
        VkSemaphore image_available_sem;
        VkSemaphore render_finished_sem;
        VkFence in_flight_fence;
        u32 image_index;
    } vk;
    struct {
        dvr_buffer_data buffers[DVR_MAX_BUFFERS];
        u64 buffer_usage_map[DVR_MAX_BUFFERS / 64];
        dvr_image_data images[DVR_MAX_IMAGES];
        u64 image_usage_map[DVR_MAX_IMAGES / 64];
        dvr_sampler_data samplers[DVR_MAX_SAMPLERS];
        u64 sampler_usage_map[DVR_MAX_SAMPLERS / 64];
        dvr_render_pass_data render_passes[DVR_MAX_RENDER_PASSES];
        u64 render_pass_usage_map[DVR_MAX_RENDER_PASSES / 64];
        dvr_shader_module_data shader_modules[DVR_MAX_SHADER_MODULES];
        u64 shader_module_usage_map[DVR_MAX_SHADER_MODULES / 64];
        dvr_pipeline_data pipelines[DVR_MAX_PIPELINES];
        u64 pipeline_usage_map[DVR_MAX_PIPELINES / 64];
        dvr_framebuffer_data framebuffers[DVR_MAX_FRAMEBUFFERS];
        u64 framebuffer_usage_map[DVR_MAX_FRAMEBUFFERS / 64];
        dvr_descriptor_set_layout_data descriptor_set_layouts[DVR_MAX_DESCRIPTOR_SET_LAYOUTS];
        u64 descriptor_set_layout_usage_map[DVR_MAX_DESCRIPTOR_SET_LAYOUTS / 64];
        dvr_descriptor_set_data descriptor_sets[DVR_MAX_DESCRIPTOR_SETS];
        u64 descriptor_set_usage_map[DVR_MAX_DESCRIPTOR_SETS / 64];
    } res;
    struct {
        dvr_render_pass swapchain_render_pass;
        dvr_image* swapchain_images;
        dvr_framebuffer* swapchain_framebuffers;
    } defaults;
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

    VkBufferUsageFlags usage = 0;
    switch (desc->usage) {
        case DVR_BUFFER_USAGE_VERTEX:
            usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
            break;
        case DVR_BUFFER_USAGE_INDEX:
            usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
            break;
        case DVR_BUFFER_USAGE_UNIFORM:
            usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            break;
        default:
            return DVR_ERROR(dvr_buffer, "unknown buffer usage");
    }

    result = dvr_vk_create_buffer(
        desc->data.size,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | usage,
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

    VkBufferUsageFlags usage = 0;
    switch (desc->usage) {
        case DVR_BUFFER_USAGE_VERTEX:
            usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
            break;
        case DVR_BUFFER_USAGE_INDEX:
            usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
            break;
        case DVR_BUFFER_USAGE_UNIFORM:
            usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            break;
        default:
            return DVR_ERROR(dvr_buffer, "unknown buffer usage");
    }

    DVR_RESULT(dvr_none)
    result = dvr_vk_create_buffer(
        desc->data.size,
        usage,
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

static void dvr_vk_destroy_buffer(dvr_buffer buffer) {
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

void dvr_write_buffer(dvr_buffer buffer, dvr_range new_data, u32 offset) {
    dvr_buffer_data* buf = dvr_get_buffer_data(buffer);
    if (buf->lifecycle != DVR_BUFFER_LIFECYCLE_DYNAMIC) {
        DVRLOG_ERROR("cannot write to buffers not marked as dynamic");
        return;
    }

    memcpy(((u8*)buf->vk.memmap + offset), new_data.base, new_data.size);
}

void dvr_bind_vertex_buffer(dvr_buffer buffer, u32 binding) {
    dvr_buffer_data* buf = dvr_get_buffer_data(buffer);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(g_dvr_state.vk.command_buffer, binding, 1, &buf->vk.buffer, &offset);
}

void dvr_bind_index_buffer(dvr_buffer buffer, VkIndexType index_type) {
    dvr_buffer_data* buf = dvr_get_buffer_data(buffer);
    vkCmdBindIndexBuffer(g_dvr_state.vk.command_buffer, buf->vk.buffer, 0, index_type);
}

void dvr_bind_uniform_buffer(dvr_buffer buffer, u32 binding) {
    dvr_buffer_data* buf = dvr_get_buffer_data(buffer);
    VkDescriptorBufferInfo buffer_info = {
        .buffer = buf->vk.buffer,
        .offset = 0,
        .range = VK_WHOLE_SIZE,
    };

    VkWriteDescriptorSet descriptor_write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = g_dvr_state.res.descriptor_sets[binding].vk.set,
        .dstBinding = binding,
        .dstArrayElement = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1,
        .pBufferInfo = &buffer_info,
    };

    vkUpdateDescriptorSets(DVR_DEVICE, 1, &descriptor_write, 0, NULL);
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
    // validate desc (only for debug builds)

    bool has_data = desc->data.base != NULL;
    if (desc->num_samples == 0) {
        desc->num_samples = VK_SAMPLE_COUNT_1_BIT;
    }
#ifndef NDEBUG
    if (desc->width == 0 || desc->height == 0) {
        return DVR_ERROR(dvr_image, "image width and height must be greater than 0");
    }
    if (desc->format == VK_FORMAT_UNDEFINED) {
        return DVR_ERROR(dvr_image, "image format must be specified");
    }

    if (has_data) {
        if (desc->render_target) {
            return DVR_ERROR(dvr_image, "image cannot be a render target and have data");
        }
        if (desc->num_samples != VK_SAMPLE_COUNT_1_BIT) {
            return DVR_ERROR(dvr_image, "image cannot have data and be multisampled");
        }
    } else {
        if (desc->generate_mipmaps) {
            return DVR_ERROR(dvr_image, "cannot generate mipmaps for image with no data");
        }
    }
#endif

    u32 mip_levels = 1;
    if (desc->generate_mipmaps) {
        mip_levels = (u32)log2(fmax(desc->width, desc->height)) + 1;
    }

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
        .usage = desc->usage | (has_data ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0) |
                 (desc->render_target ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0) |
                 (desc->generate_mipmaps
                      ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                      : 0),
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

    if (desc->render_target) {
        dvr_vk_transition_image_layout(
            image,
            desc->format,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            mip_levels
        );
    }

    u16 free_slot = dvr_find_free_slot(g_dvr_state.res.image_usage_map, DVR_MAX_IMAGES);
    g_dvr_state.res.images[free_slot] = img;
    dvr_set_slot_used(g_dvr_state.res.image_usage_map, free_slot);

    return DVR_OK(dvr_image, (dvr_image){ .id = free_slot });
}

DVR_RESULT(dvr_image) dvr_create_image(dvr_image_desc* desc) {
    return dvr_vk_create_image(desc);
}

static void dvr_vk_destroy_image(dvr_image image) {
    dvr_image_data* img = dvr_get_image_data(image);
    vkDestroyImageView(DVR_DEVICE, img->vk.view, NULL);
    vkDestroyImage(DVR_DEVICE, img->vk.image, NULL);
    vkFreeMemory(DVR_DEVICE, img->vk.memory, NULL);
}

void dvr_destroy_image(dvr_image image) {
    dvr_vk_destroy_image(image);

    dvr_set_slot_free(g_dvr_state.res.image_usage_map, image.id);
}

// DVR_SAMPLER FUNCTIONS

static dvr_sampler_data* dvr_get_sampler_data(dvr_sampler sampler) {
    if (sampler.id >= DVR_MAX_SAMPLERS) {
        DVRLOG_ERROR("sampler id out of range: %u", sampler.id);
        return NULL;
    }

    return &g_dvr_state.res.samplers[sampler.id];
}

DVR_RESULT(dvr_sampler) dvr_create_sampler(dvr_sampler_desc* desc) {
    VkSampler sampler;

    VkSamplerCreateInfo sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = desc->mag_filter,
        .minFilter = desc->min_filter,
        .addressModeU = desc->address_mode_u,
        .addressModeV = desc->address_mode_v,
        .addressModeW = desc->address_mode_w,
        .anisotropyEnable = desc->anisotropy_enable,
        .maxAnisotropy = desc->max_anisotropy,
        .borderColor = desc->border_color,
        .unnormalizedCoordinates = desc->unnormalized_coordinates,
        .compareEnable = desc->compare_enable,
        .compareOp = desc->compare_op,
        .mipmapMode = desc->mipmap_mode,
        .mipLodBias = desc->mip_lod_bias,
        .minLod = desc->min_lod,
        .maxLod = desc->max_lod,
    };

    if (vkCreateSampler(DVR_DEVICE, &sampler_info, NULL, &sampler) != VK_SUCCESS) {
        return DVR_ERROR(dvr_sampler, "failed to create sampler");
    }

    dvr_sampler_data samp = {
        .vk.sampler = sampler,
    };

    u16 free_slot = dvr_find_free_slot(g_dvr_state.res.sampler_usage_map, DVR_MAX_SAMPLERS);
    g_dvr_state.res.samplers[free_slot] = samp;
    dvr_set_slot_used(g_dvr_state.res.sampler_usage_map, free_slot);

    return DVR_OK(dvr_sampler, (dvr_sampler){ .id = free_slot });
}

static void dvr_vk_destroy_sampler(dvr_sampler sampler) {
    dvr_sampler_data* samp = dvr_get_sampler_data(sampler);
    vkDestroySampler(DVR_DEVICE, samp->vk.sampler, NULL);
}

void dvr_destroy_sampler(dvr_sampler sampler) {
    dvr_vk_destroy_sampler(sampler);

    dvr_set_slot_free(g_dvr_state.res.sampler_usage_map, sampler.id);
}

// DVR_RENDER_PASS FUNCTIONS

static dvr_render_pass_data* dvr_get_render_pass_data(dvr_render_pass pass) {
    if (pass.id >= DVR_MAX_RENDER_PASSES) {
        DVRLOG_ERROR("render pass id out of range: %u", pass.id);
        return NULL;
    }

    return &g_dvr_state.res.render_passes[pass.id];
}

DVR_RESULT(dvr_render_pass) dvr_create_render_pass(dvr_render_pass_desc* desc) {
    VkAttachmentDescription attachments
        [desc->num_color_attachments + desc->num_resolve_attachments +
         (i32)desc->depth_stencil_attachment.enable];

    VkAttachmentDescription color_attachments[desc->num_color_attachments];
    VkAttachmentReference color_attachment_refs[desc->num_color_attachments];

    for (u32 i = 0; i < desc->num_color_attachments; i++) {
        if (!desc->color_attachments[i].enable) {
            DVRLOG_WARNING("color attachment %u is not enabled, but is required", i);
            continue;
        }
        color_attachments[i] = (VkAttachmentDescription){
            .format = desc->color_attachments[i].format,
            .samples = desc->color_attachments[i].samples,
            .loadOp = desc->color_attachments[i].load_op,
            .storeOp = desc->color_attachments[i].store_op,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = desc->color_attachments[i].initial_layout,
            .finalLayout = desc->color_attachments[i].final_layout,
        };

        color_attachment_refs[i] = (VkAttachmentReference){
            .attachment = i,
            .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        };

        attachments[i] = color_attachments[i];
    }

    VkAttachmentDescription resolve_attachments[desc->num_resolve_attachments];
    VkAttachmentReference resolve_attachment_refs[desc->num_resolve_attachments];

    for (u32 i = 0; i < desc->num_resolve_attachments; i++) {
        if (!desc->resolve_attachments[i].enable) {
            continue;
        }
        resolve_attachments[i] = (VkAttachmentDescription){
            .format = desc->resolve_attachments[i].format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = desc->resolve_attachments[i].load_op,
            .storeOp = desc->resolve_attachments[i].store_op,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        };

        resolve_attachment_refs[i] = (VkAttachmentReference){
            .attachment = i + desc->num_color_attachments,
            .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        };

        attachments[i + desc->num_color_attachments] = resolve_attachments[i];
    }

    VkAttachmentDescription depth_attachment = { 0 };
    VkAttachmentReference depth_attachment_ref = { 0 };

    if (desc->depth_stencil_attachment.enable) {
        depth_attachment = (VkAttachmentDescription){
            .format = desc->depth_stencil_attachment.format,
            .samples = desc->depth_stencil_attachment.samples,
            .loadOp = desc->depth_stencil_attachment.load_op,
            .storeOp = desc->depth_stencil_attachment.store_op,
            .stencilLoadOp = desc->depth_stencil_attachment.stencil_load_op,
            .stencilStoreOp = desc->depth_stencil_attachment.stencil_store_op,
            .initialLayout = desc->depth_stencil_attachment.initial_layout,
            .finalLayout = desc->depth_stencil_attachment.final_layout,
        };

        depth_attachment_ref = (VkAttachmentReference){
            .attachment = desc->num_color_attachments + desc->num_resolve_attachments,
            .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        };

        attachments[desc->num_color_attachments + desc->num_resolve_attachments] =
            depth_attachment;
    }

    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = desc->num_color_attachments,
        .pColorAttachments = color_attachment_refs,
        .pResolveAttachments = desc->num_resolve_attachments ? resolve_attachment_refs : NULL,
        .pDepthStencilAttachment =
            desc->depth_stencil_attachment.enable ? &depth_attachment_ref : NULL,
    };

    VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask =
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };

    VkRenderPassCreateInfo render_pass_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = desc->num_color_attachments + desc->num_resolve_attachments +
                           (u32)desc->depth_stencil_attachment.enable,
        .pAttachments = attachments,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    };

    VkRenderPass render_pass;
    if (vkCreateRenderPass(DVR_DEVICE, &render_pass_info, NULL, &render_pass) != VK_SUCCESS) {
        return DVR_ERROR(dvr_render_pass, "failed to create render pass");
    }

    dvr_render_pass_data pass = {
        .vk.render_pass = render_pass,
    };

    u16 free_slot =
        dvr_find_free_slot(g_dvr_state.res.render_pass_usage_map, DVR_MAX_RENDER_PASSES);
    g_dvr_state.res.render_passes[free_slot] = pass;
    dvr_set_slot_used(g_dvr_state.res.render_pass_usage_map, free_slot);

    return DVR_OK(dvr_render_pass, (dvr_render_pass){ .id = free_slot });
}

static void dvr_vk_destroy_render_pass(dvr_render_pass pass) {
    dvr_render_pass_data* data = dvr_get_render_pass_data(pass);
    vkDestroyRenderPass(DVR_DEVICE, data->vk.render_pass, NULL);
}

void dvr_destroy_render_pass(dvr_render_pass pass) {
    dvr_vk_destroy_render_pass(pass);

    dvr_set_slot_free(g_dvr_state.res.render_pass_usage_map, pass.id);
}

// DVR_DESCRIPTOR_SET_LAYOUT FUNCTIONS

static dvr_descriptor_set_layout_data*
dvr_get_descriptor_set_layout_data(dvr_descriptor_set_layout layout) {
    if (layout.id >= DVR_MAX_DESCRIPTOR_SET_LAYOUTS) {
        DVRLOG_ERROR("descriptor set layout id out of range: %u", layout.id);
        return NULL;
    }

    return &g_dvr_state.res.descriptor_set_layouts[layout.id];
}

DVR_RESULT(dvr_descriptor_set_layout)
dvr_create_descriptor_set_layout(dvr_descriptor_set_layout_desc* desc) {
    VkDescriptorSetLayoutBinding bindings[desc->num_bindings];
    for (u32 i = 0; i < desc->num_bindings; i++) {
        bindings[i] = (VkDescriptorSetLayoutBinding){
            .binding = desc->bindings[i].binding,
            .descriptorType = desc->bindings[i].type,
            .descriptorCount = desc->bindings[i].count,
            .stageFlags = desc->bindings[i].stage_flags,
            .pImmutableSamplers = NULL,
        };
    }

    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = desc->num_bindings,
        .pBindings = bindings,
    };

    VkDescriptorSetLayout layout;
    if (vkCreateDescriptorSetLayout(DVR_DEVICE, &layout_info, NULL, &layout) != VK_SUCCESS) {
        return DVR_ERROR(dvr_descriptor_set_layout, "failed to create descriptor set layout");
    }

    dvr_descriptor_set_layout_data layout_data = {
        .vk.layout = layout,
    };

    u16 free_slot = dvr_find_free_slot(
        g_dvr_state.res.descriptor_set_layout_usage_map,
        DVR_MAX_DESCRIPTOR_SET_LAYOUTS
    );
    g_dvr_state.res.descriptor_set_layouts[free_slot] = layout_data;
    dvr_set_slot_used(g_dvr_state.res.descriptor_set_layout_usage_map, free_slot);

    return DVR_OK(dvr_descriptor_set_layout, (dvr_descriptor_set_layout){ .id = free_slot });
}

static void dvr_vk_destroy_descriptor_set_layout(dvr_descriptor_set_layout layout) {
    dvr_descriptor_set_layout_data* data = dvr_get_descriptor_set_layout_data(layout);
    vkDestroyDescriptorSetLayout(DVR_DEVICE, data->vk.layout, NULL);
}

void dvr_destroy_descriptor_set_layout(dvr_descriptor_set_layout layout) {
    dvr_vk_destroy_descriptor_set_layout(layout);

    dvr_set_slot_free(g_dvr_state.res.descriptor_set_layout_usage_map, layout.id);
}

static dvr_framebuffer_data* dvr_get_framebuffer_data(dvr_framebuffer framebuffer);

void dvr_begin_render_pass(
    dvr_render_pass pass,
    dvr_framebuffer framebuffer,
    VkClearValue* clear_values,
    u32 num_clear_values
) {
    dvr_render_pass_data* pass_data = dvr_get_render_pass_data(pass);
    dvr_framebuffer_data* framebuffer_data = dvr_get_framebuffer_data(framebuffer);

    VkRenderPassBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = pass_data->vk.render_pass,
        .framebuffer = framebuffer_data->vk.framebuffer,
        .renderArea = framebuffer_data->render_area,
        .clearValueCount = num_clear_values,
        .pClearValues = clear_values,
    };

    vkCmdBeginRenderPass(DVR_COMMAND_BUFFER, &begin_info, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(
        DVR_COMMAND_BUFFER,
        0,
        1,
        &(VkViewport){
            .x = 0.0f,
            .y = 0.0f,
            .width = (f32)framebuffer_data->render_area.extent.width,
            .height = (f32)framebuffer_data->render_area.extent.height,
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        }
    );
    vkCmdSetScissor(DVR_COMMAND_BUFFER, 0, 1, &framebuffer_data->render_area);
}

void dvr_end_render_pass() {
    vkCmdEndRenderPass(DVR_COMMAND_BUFFER);
}

// DVR_DESCRIPTOR_SET FUNCTIONS

static dvr_descriptor_set_data* dvr_get_descriptor_set_data(dvr_descriptor_set set) {
    if (set.id >= DVR_MAX_DESCRIPTOR_SETS) {
        DVRLOG_ERROR("descriptor set id out of range: %u", set.id);
        return NULL;
    }

    return &g_dvr_state.res.descriptor_sets[set.id];
}

DVR_RESULT(dvr_descriptor_set) dvr_create_descriptor_set(dvr_descriptor_set_desc* desc) {
    dvr_descriptor_set_layout_data* layout_data =
        dvr_get_descriptor_set_layout_data(desc->layout);

    VkDescriptorSetLayout* layouts = &layout_data->vk.layout;

    VkDescriptorSetAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = g_dvr_state.vk.descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = layouts,
    };

    VkDescriptorSet set;
    if (vkAllocateDescriptorSets(DVR_DEVICE, &alloc_info, &set) != VK_SUCCESS) {
        return DVR_ERROR(dvr_descriptor_set, "failed to allocate descriptor set");
    }

    u32 i_buffer = 0;
    u32 i_image = 0;
    VkDescriptorBufferInfo buffer_infos[desc->num_bindings];
    VkDescriptorImageInfo image_infos[desc->num_bindings];
    VkWriteDescriptorSet writes[desc->num_bindings];

    for (u32 i = 0; i < desc->num_bindings; i++) {
        if (desc->bindings[i].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
            desc->bindings[i].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            buffer_infos[i_buffer] = (VkDescriptorBufferInfo){
                .buffer = dvr_get_buffer_data(desc->bindings[i].buffer.buffer)->vk.buffer,
                .offset = desc->bindings[i].buffer.offset,
                .range = desc->bindings[i].buffer.size,
            };

            writes[i] = (VkWriteDescriptorSet){
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = set,
                .dstBinding = desc->bindings[i].binding,
                .dstArrayElement = 0,
                .descriptorType = desc->bindings[i].type,
                .descriptorCount = 1,
                .pBufferInfo = &buffer_infos[i_buffer],
            };

            i_buffer++;
        } else if (desc->bindings[i].type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                   desc->bindings[i].type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
            image_infos[i_image] = (VkDescriptorImageInfo){
                .sampler = dvr_get_sampler_data(desc->bindings[i].image.sampler)->vk.sampler,
                .imageView = dvr_get_image_data(desc->bindings[i].image.image)->vk.view,
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            };

            writes[i] = (VkWriteDescriptorSet){
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = set,
                .dstBinding = desc->bindings[i].binding,
                .dstArrayElement = 0,
                .descriptorType = desc->bindings[i].type,
                .descriptorCount = 1,
                .pImageInfo = &image_infos[i_image],
            };

            i_image++;
        }
    }

    vkUpdateDescriptorSets(DVR_DEVICE, desc->num_bindings, writes, 0, NULL);

    dvr_descriptor_set_data set_data = {
        .vk.set = set,
    };

    u16 free_slot =
        dvr_find_free_slot(g_dvr_state.res.descriptor_set_usage_map, DVR_MAX_DESCRIPTOR_SETS);
    g_dvr_state.res.descriptor_sets[free_slot] = set_data;
    dvr_set_slot_used(g_dvr_state.res.descriptor_set_usage_map, free_slot);

    return DVR_OK(dvr_descriptor_set, (dvr_descriptor_set){ .id = free_slot });
}

static void dvr_vk_destroy_descriptor_set(dvr_descriptor_set set) {
    dvr_descriptor_set_data* data = dvr_get_descriptor_set_data(set);
    vkFreeDescriptorSets(DVR_DEVICE, g_dvr_state.vk.descriptor_pool, 1, &data->vk.set);
}

void dvr_destroy_descriptor_set(dvr_descriptor_set set) {
    dvr_vk_destroy_descriptor_set(set);

    dvr_set_slot_free(g_dvr_state.res.descriptor_set_usage_map, set.id);
}

static dvr_pipeline_data* dvr_get_pipeline_data(dvr_pipeline pipeline);

void dvr_bind_descriptor_set(dvr_pipeline pipeline, dvr_descriptor_set set) {
    dvr_pipeline_data* pipeline_data = dvr_get_pipeline_data(pipeline);
    dvr_descriptor_set_data* set_data = dvr_get_descriptor_set_data(set);

    vkCmdBindDescriptorSets(
        DVR_COMMAND_BUFFER,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipeline_data->vk.layout,
        0,
        1,
        &set_data->vk.set,
        0,
        NULL
    );
}

// DVR_SHADER_MODULE FUNCTIONS

static dvr_shader_module_data* dvr_get_shader_module_data(dvr_shader_module module) {
    if (module.id >= DVR_MAX_SHADER_MODULES) {
        DVRLOG_ERROR("shader module id out of range: %u", module.id);
        return NULL;
    }

    return &g_dvr_state.res.shader_modules[module.id];
}

DVR_RESULT(dvr_shader_module) dvr_create_shader_module(dvr_shader_module_desc* desc) {
    VkShaderModuleCreateInfo create_info = { .sType =
                                                 VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                             .codeSize = desc->code.size,
                                             .pCode = (u32*)desc->code.base };

    VkShaderModule shader_module;
    if (vkCreateShaderModule(DVR_DEVICE, &create_info, NULL, &shader_module) != VK_SUCCESS) {
        return DVR_ERROR(dvr_shader_module, "failed to create shader module");
    }

    dvr_shader_module_data mod = {
        .vk.module = shader_module,
    };

    u16 free_slot =
        dvr_find_free_slot(g_dvr_state.res.shader_module_usage_map, DVR_MAX_SHADER_MODULES);
    g_dvr_state.res.shader_modules[free_slot] = mod;
    dvr_set_slot_used(g_dvr_state.res.shader_module_usage_map, free_slot);

    return DVR_OK(dvr_shader_module, (dvr_shader_module){ .id = free_slot });
}

static void dvr_vk_destroy_shader_module(dvr_shader_module module) {
    dvr_shader_module_data* data = dvr_get_shader_module_data(module);
    vkDestroyShaderModule(DVR_DEVICE, data->vk.module, NULL);
}

void dvr_destroy_shader_module(dvr_shader_module module) {
    dvr_vk_destroy_shader_module(module);

    dvr_set_slot_free(g_dvr_state.res.shader_module_usage_map, module.id);
}

// DVR_PIPELINE FUNCTIONS

static dvr_pipeline_data* dvr_get_pipeline_data(dvr_pipeline pipeline) {
    if (pipeline.id >= DVR_MAX_PIPELINES) {
        DVRLOG_ERROR("pipeline id out of range: %u", pipeline.id);
        return NULL;
    }

    return &g_dvr_state.res.pipelines[pipeline.id];
}

DVR_RESULT(dvr_pipeline) dvr_create_pipeline(dvr_pipeline_desc* desc) {
    VkPipelineShaderStageCreateInfo shader_stages[desc->num_stages];
    for (u32 i = 0; i < desc->num_stages; i++) {
        shader_stages[i] = (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = desc->stages[i].stage,
            .module = dvr_get_shader_module_data(desc->stages[i].shader_module)->vk.module,
            .pName = desc->stages[i].entry_point,
        };
    }

    VkDynamicState dynamic_states[2] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };

    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamic_states,
    };

    VkPipelineVertexInputStateCreateInfo vertex_input_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = desc->vertex_input.num_bindings,
        .pVertexBindingDescriptions = desc->vertex_input.bindings,
        .vertexAttributeDescriptionCount = desc->vertex_input.num_attributes,
        .pVertexAttributeDescriptions = desc->vertex_input.attributes,
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = desc->rasterization.topology,
        .primitiveRestartEnable =
            desc->rasterization.primitive_restart_enable ? VK_TRUE : VK_FALSE,
    };

    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
        .pViewports = &desc->viewport,
        .pScissors = &desc->scissor,
    };

    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = desc->depth_stencil.depth_clamp_enable ? VK_TRUE : VK_FALSE,
        .rasterizerDiscardEnable =
            desc->rasterization.rasterizer_discard_enable ? VK_TRUE : VK_FALSE,
        .polygonMode = desc->rasterization.polygon_mode,
        .cullMode = desc->rasterization.cull_mode,
        .frontFace = desc->rasterization.front_face,
        .depthBiasEnable = desc->depth_stencil.depth_bias_enable ? VK_TRUE : VK_FALSE,
        .depthBiasConstantFactor = desc->depth_stencil.depth_bias_constant_factor,
        .depthBiasClamp = desc->depth_stencil.depth_bias_clamp,
        .depthBiasSlopeFactor = desc->depth_stencil.depth_bias_slope_factor,
        .lineWidth = desc->rasterization.line_width,
    };

    VkPipelineDepthStencilStateCreateInfo depth_stencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = desc->depth_stencil.depth_test_enable ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = desc->depth_stencil.depth_write_enable ? VK_TRUE : VK_FALSE,
        .depthCompareOp = desc->depth_stencil.depth_compare_op,
        .depthBoundsTestEnable =
            desc->depth_stencil.depth_bounds_test_enable ? VK_TRUE : VK_FALSE,
        .minDepthBounds = desc->depth_stencil.min_depth_bounds,
        .maxDepthBounds = desc->depth_stencil.max_depth_bounds,
        .stencilTestEnable = desc->depth_stencil.stencil_test_enable ? VK_TRUE : VK_FALSE,
        .front = desc->depth_stencil.front,
        .back = desc->depth_stencil.back
    };

    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = desc->multisample.rasterization_samples,
        .sampleShadingEnable = desc->multisample.sample_shading_enable ? VK_TRUE : VK_FALSE,
        .minSampleShading = desc->multisample.min_sample_shading,
        .pSampleMask = desc->multisample.sample_mask,
        .alphaToCoverageEnable =
            desc->multisample.alpha_to_coverage_enable ? VK_TRUE : VK_FALSE,
        .alphaToOneEnable = desc->multisample.alpha_to_one_enable ? VK_TRUE : VK_FALSE,
    };

    VkPipelineColorBlendAttachmentState
        color_blend_attachments[desc->color_blend.num_attachments];

    for (u32 i = 0; i < desc->color_blend.num_attachments; i++) {
        color_blend_attachments[i] = (VkPipelineColorBlendAttachmentState){
            .blendEnable = desc->color_blend.blend_enable,
            .srcColorBlendFactor = desc->color_blend.src_color_blend_factor,
            .dstColorBlendFactor = desc->color_blend.dst_color_blend_factor,
            .colorBlendOp = desc->color_blend.color_blend_op,
            .srcAlphaBlendFactor = desc->color_blend.src_alpha_blend_factor,
            .dstAlphaBlendFactor = desc->color_blend.dst_alpha_blend_factor,
            .alphaBlendOp = desc->color_blend.alpha_blend_op,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        };
    }

    VkPipelineColorBlendStateCreateInfo color_blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = desc->color_blend.num_attachments,
        .pAttachments = color_blend_attachments,
        .blendConstants = { 0.0f, 0.0f, 0.0f, 0.0f },
    };

    VkDescriptorSetLayout layouts[desc->layout.num_desc_set_layouts];

    for (u32 i = 0; i < desc->layout.num_desc_set_layouts; i++) {
        layouts[i] =
            dvr_get_descriptor_set_layout_data(desc->layout.desc_set_layouts[i])->vk.layout;
    }

    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = desc->layout.num_desc_set_layouts,
        .pSetLayouts = layouts,
        .pushConstantRangeCount = desc->layout.num_push_constant_ranges,
        .pPushConstantRanges = desc->layout.push_constant_ranges,
    };

    VkPipelineLayout pipeline_layout;

    if (vkCreatePipelineLayout(DVR_DEVICE, &pipeline_layout_info, NULL, &pipeline_layout) !=
        VK_SUCCESS) {
        return DVR_ERROR(dvr_pipeline, "failed to create pipeline layout");
    }

    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = desc->num_stages,
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input_info,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = &depth_stencil,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state,
        .layout = pipeline_layout,
        .renderPass = dvr_get_render_pass_data(desc->render_pass)->vk.render_pass,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
    };

    VkPipeline pipeline;
    if (vkCreateGraphicsPipelines(
            DVR_DEVICE,
            VK_NULL_HANDLE,
            1,
            &pipeline_info,
            NULL,
            &pipeline
        ) != VK_SUCCESS) {
        vkDestroyPipelineLayout(DVR_DEVICE, pipeline_layout, NULL);
        return DVR_ERROR(dvr_pipeline, "failed to create graphics pipeline");
    }

    dvr_pipeline_data pipe = {
        .vk.pipeline = pipeline,
        .vk.layout = pipeline_layout,
    };

    u16 free_slot = dvr_find_free_slot(g_dvr_state.res.pipeline_usage_map, DVR_MAX_PIPELINES);
    g_dvr_state.res.pipelines[free_slot] = pipe;
    dvr_set_slot_used(g_dvr_state.res.pipeline_usage_map, free_slot);

    return DVR_OK(dvr_pipeline, (dvr_pipeline){ .id = free_slot });
}

static void dvr_vk_destroy_pipeline(dvr_pipeline pipeline) {
    dvr_pipeline_data* data = dvr_get_pipeline_data(pipeline);
    vkDestroyPipeline(DVR_DEVICE, data->vk.pipeline, NULL);
    vkDestroyPipelineLayout(DVR_DEVICE, data->vk.layout, NULL);
}

void dvr_destroy_pipeline(dvr_pipeline pipeline) {
    dvr_vk_destroy_pipeline(pipeline);

    dvr_set_slot_free(g_dvr_state.res.pipeline_usage_map, pipeline.id);
}

void dvr_bind_pipeline(dvr_pipeline pipeline) {
    dvr_pipeline_data* data = dvr_get_pipeline_data(pipeline);
    vkCmdBindPipeline(DVR_COMMAND_BUFFER, VK_PIPELINE_BIND_POINT_GRAPHICS, data->vk.pipeline);
}

// DVR_FRAMEBUFFER FUNCTIONS

static dvr_framebuffer_data* dvr_get_framebuffer_data(dvr_framebuffer framebuffer) {
    if (framebuffer.id >= DVR_MAX_FRAMEBUFFERS) {
        DVRLOG_ERROR("framebuffer id out of range: %u", framebuffer.id);
        return NULL;
    }

    return &g_dvr_state.res.framebuffers[framebuffer.id];
}

DVR_RESULT_DEF(dvr_framebuffer_data);
static DVR_RESULT(dvr_framebuffer_data) dvr_vk_create_framebuffer(dvr_framebuffer_desc* desc) {
    VkImageView attachments[desc->num_attachments];
    for (u32 i = 0; i < desc->num_attachments; i++) {
        attachments[i] = dvr_get_image_data(desc->attachments[i])->vk.view;
    }

    VkFramebufferCreateInfo framebuffer_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = dvr_get_render_pass_data(desc->render_pass)->vk.render_pass,
        .attachmentCount = desc->num_attachments,
        .pAttachments = attachments,
        .width = desc->width,
        .height = desc->height,
        .layers = 1,
    };

    VkFramebuffer framebuffer;
    if (vkCreateFramebuffer(DVR_DEVICE, &framebuffer_info, NULL, &framebuffer) != VK_SUCCESS) {
        return DVR_ERROR(dvr_framebuffer_data, "failed to create framebuffer");
    }
    dvr_framebuffer_data fb = {
        .vk.framebuffer = framebuffer,
        .render_area =
            (VkRect2D){
                .offset = { 0, 0 },
                .extent = { desc->width, desc->height },
            },
    };

    return DVR_OK(dvr_framebuffer_data, fb);
}

DVR_RESULT(dvr_framebuffer) dvr_create_framebuffer(dvr_framebuffer_desc* desc) {
    DVR_RESULT(dvr_framebuffer_data) fb_res = dvr_vk_create_framebuffer(desc);
    DVR_BUBBLE_INTO(dvr_framebuffer, fb_res);

    u16 free_slot =
        dvr_find_free_slot(g_dvr_state.res.framebuffer_usage_map, DVR_MAX_FRAMEBUFFERS);
    g_dvr_state.res.framebuffers[free_slot] = DVR_UNWRAP(fb_res);
    dvr_set_slot_used(g_dvr_state.res.framebuffer_usage_map, free_slot);

    return DVR_OK(dvr_framebuffer, (dvr_framebuffer){ .id = free_slot });
}

static void dvr_vk_destroy_framebuffer(dvr_framebuffer framebuffer) {
    dvr_framebuffer_data* data = dvr_get_framebuffer_data(framebuffer);
    vkDestroyFramebuffer(DVR_DEVICE, data->vk.framebuffer, NULL);
}

void dvr_destroy_framebuffer(dvr_framebuffer framebuffer) {
    dvr_vk_destroy_framebuffer(framebuffer);

    dvr_set_slot_free(g_dvr_state.res.framebuffer_usage_map, framebuffer.id);
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
        if (formats[i].format == VK_FORMAT_R8G8B8A8_SRGB &&
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
    arrsetlen(g_dvr_state.defaults.swapchain_images, arrlen(g_dvr_state.vk.swapchain_images));

    for (usize i = 0; i < arrlenu(g_dvr_state.vk.swapchain_images); i++) {
        DVR_RESULT(VkImageView)
        image_view_res = dvr_vk_create_image_view(
            g_dvr_state.vk.swapchain_images[i],
            g_dvr_state.vk.swapchain_format,
            1
        );
        DVR_BUBBLE_INTO(dvr_none, image_view_res);

        g_dvr_state.vk.swapchain_image_views[i] = DVR_UNWRAP(image_view_res);

        dvr_image_data data = {
            .vk.image = g_dvr_state.vk.swapchain_images[i],
            .vk.view = g_dvr_state.vk.swapchain_image_views[i],
            .width = g_dvr_state.vk.swapchain_extent.width,
            .height = g_dvr_state.vk.swapchain_extent.height,
        };

        u16 free_slot = dvr_find_free_slot(g_dvr_state.res.image_usage_map, DVR_MAX_IMAGES);
        g_dvr_state.res.images[free_slot] = data;
        dvr_set_slot_used(g_dvr_state.res.image_usage_map, free_slot);

        g_dvr_state.defaults.swapchain_images[i] = (dvr_image){ .id = free_slot };
    }

    return DVR_OK(dvr_none, DVR_NONE);
}

static DVR_RESULT(dvr_none) dvr_vk_create_swapchain_render_pass(void) {
    DVR_RESULT(dvr_render_pass)
    rp_res = dvr_create_render_pass(&(dvr_render_pass_desc){
        .num_color_attachments = 1,
        .color_attachments[0] =
            (dvr_render_pass_attachment_desc){
                .enable = true,
                .format = g_dvr_state.vk.swapchain_format,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .store_op = VK_ATTACHMENT_STORE_OP_STORE,
                .stencil_load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                .stencil_store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                .initial_layout = VK_IMAGE_LAYOUT_UNDEFINED,
                .final_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            },
        .num_resolve_attachments = 0,
    });

    DVR_BUBBLE_INTO(dvr_none, rp_res);

    g_dvr_state.defaults.swapchain_render_pass = DVR_UNWRAP(rp_res);

    return DVR_OK(dvr_none, DVR_NONE);
}

static DVR_RESULT(dvr_none) dvr_vk_create_swapchain_framebuffers(void) {
    arrsetlen(
        g_dvr_state.defaults.swapchain_framebuffers,
        arrlen(g_dvr_state.vk.swapchain_images)
    );

    for (usize i = 0; i < arrlenu(g_dvr_state.vk.swapchain_images); i++) {
        DVR_RESULT(dvr_framebuffer)
        fb_res = dvr_create_framebuffer(&(dvr_framebuffer_desc){
            .render_pass = g_dvr_state.defaults.swapchain_render_pass,
            .width = g_dvr_state.vk.swapchain_extent.width,
            .height = g_dvr_state.vk.swapchain_extent.height,
            .num_attachments = 1,
            .attachments = &g_dvr_state.defaults.swapchain_images[i],
        });
        DVR_BUBBLE_INTO(dvr_none, fb_res);

        g_dvr_state.defaults.swapchain_framebuffers[i] = DVR_UNWRAP(fb_res);
    }

    return DVR_OK(dvr_none, DVR_NONE);
}

static DVR_RESULT(dvr_none) dvr_vk_create_command_pool(void) {
    queue_family_indices indices = find_queue_families(g_dvr_state.vk.physical_device);

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = indices.graphics_family,
    };

    if (vkCreateCommandPool(DVR_DEVICE, &pool_info, NULL, &g_dvr_state.vk.command_pool) !=
        VK_SUCCESS) {
        return DVR_OK(dvr_none, DVR_NONE);
    }

    return DVR_OK(dvr_none, DVR_NONE);
}

static DVR_RESULT(dvr_none) dvr_vk_create_command_buffer(void) {
    VkCommandBufferAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = g_dvr_state.vk.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };

    if (vkAllocateCommandBuffers(DVR_DEVICE, &alloc_info, &g_dvr_state.vk.command_buffer) !=
        VK_SUCCESS) {
        return DVR_ERROR(dvr_none, "failed to allocate command buffer");
    }

    return DVR_OK(dvr_none, DVR_NONE);
}

static DVR_RESULT(dvr_none) dvr_vk_create_descriptor_pool(void) {
    VkDescriptorPoolSize ubo_size = {
        .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = DVR_POOL_MAX_UBOS,
    };

    VkDescriptorPoolSize image_sampler_size = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = DVR_POOL_MAX_SAMPLERS,
    };

    VkDescriptorPoolSize pool_sizes[] = { ubo_size, image_sampler_size };

    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .poolSizeCount = 2,
        .pPoolSizes = pool_sizes,
        .maxSets = 1,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
    };

    if (vkCreateDescriptorPool(DVR_DEVICE, &pool_info, NULL, &g_dvr_state.vk.descriptor_pool) !=
        VK_SUCCESS) {
        return DVR_ERROR(dvr_none, "failed to create descriptor pool");
    }

    return DVR_OK(dvr_none, DVR_NONE);
}

static DVR_RESULT(dvr_none) dvr_vk_create_sync_objects(void) {
    VkSemaphoreCreateInfo sem_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };

    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };

    if (vkCreateSemaphore(
            DVR_DEVICE,
            &sem_info,
            nullptr,
            &g_dvr_state.vk.image_available_sem
        ) != VK_SUCCESS ||
        vkCreateSemaphore(
            DVR_DEVICE,
            &sem_info,
            nullptr,
            &g_dvr_state.vk.render_finished_sem
        ) != VK_SUCCESS ||
        vkCreateFence(DVR_DEVICE, &fence_info, nullptr, &g_dvr_state.vk.in_flight_fence) !=
            VK_SUCCESS) {
        return DVR_ERROR(dvr_none, "failed to create sync objects");
    }

    return DVR_OK(dvr_none, DVR_NONE);
}

static DVR_RESULT(dvr_none) dvr_vk_setup(dvr_setup_desc* desc) {
    (void)desc;
    DVR_RESULT(dvr_none) res;
    res = dvr_vk_create_instance();
    DVR_BUBBLE(res);

    dvr_vk_create_debug_messenger();

    res = dvr_vk_create_surface();
    DVR_BUBBLE(res);

    res = dvr_vk_pick_physical_device();
    DVR_BUBBLE(res);

    res = dvr_vk_create_logical_device();
    DVR_BUBBLE(res);

    res = dvr_vk_create_swapchain();
    DVR_BUBBLE(res);

    res = dvr_vk_create_swapchain_image_views();
    DVR_BUBBLE(res);

    res = dvr_vk_create_swapchain_render_pass();
    DVR_BUBBLE(res);

    res = dvr_vk_create_swapchain_framebuffers();
    DVR_BUBBLE(res);

    res = dvr_vk_create_command_pool();
    DVR_BUBBLE(res);

    res = dvr_vk_create_command_buffer();
    DVR_BUBBLE(res);

    res = dvr_vk_create_descriptor_pool();
    DVR_BUBBLE(res);

    res = dvr_vk_create_sync_objects();
    DVR_BUBBLE(res);

    return DVR_OK(dvr_none, DVR_NONE);
}

static void dvr_glfw_resize(GLFWwindow* window, int width, int height) {
    (void)window;
    (void)width;
    (void)height;
    g_dvr_state.window.just_resized = true;
}

static DVR_RESULT(dvr_none) dvr_glfw_setup(dvr_setup_desc* desc) {
    glfwInit();

    if (!glfwVulkanSupported()) {
        return DVR_ERROR(dvr_none, "glfw does not support vulkan");
    }

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

DVR_RESULT(dvr_none) dvr_setup(dvr_setup_desc* desc) {
    dvr_log_init();

    DVRLOG_INFO("initializing dvr...");

    memset(&g_dvr_state, 0, sizeof(g_dvr_state));

    DVR_RESULT(dvr_none) res;

    res = dvr_glfw_setup(desc);
    DVR_BUBBLE(res);

    DVRLOG_INFO("glfw setup complete");

    res = dvr_vk_setup(desc);
    DVR_BUBBLE(res);

    DVRLOG_INFO("vulkan initialized");

    return DVR_OK(dvr_none, DVR_NONE);
}

static void dvr_vk_cleanup_swapchain(void) {
    for (usize i = 0; i < arrlenu(g_dvr_state.defaults.swapchain_framebuffers); i++) {
        dvr_destroy_framebuffer(g_dvr_state.defaults.swapchain_framebuffers[i]);
    }
    for (usize i = 0; i < arrlenu(g_dvr_state.vk.swapchain_image_views); i++) {
        vkDestroyImageView(DVR_DEVICE, g_dvr_state.vk.swapchain_image_views[i], NULL);
    }
    dvr_destroy_render_pass(g_dvr_state.defaults.swapchain_render_pass);
    vkDestroySwapchainKHR(DVR_DEVICE, g_dvr_state.vk.swapchain, NULL);
}

static void dvr_vk_shutdown(void) {
    vkDeviceWaitIdle(DVR_DEVICE);

    dvr_vk_cleanup_swapchain();

    vkDestroySemaphore(DVR_DEVICE, g_dvr_state.vk.render_finished_sem, NULL);
    vkDestroySemaphore(DVR_DEVICE, g_dvr_state.vk.image_available_sem, NULL);
    vkDestroyFence(DVR_DEVICE, g_dvr_state.vk.in_flight_fence, NULL);

    vkDestroyDescriptorPool(DVR_DEVICE, g_dvr_state.vk.descriptor_pool, NULL);

    vkFreeCommandBuffers(
        DVR_DEVICE,
        g_dvr_state.vk.command_pool,
        1,
        &g_dvr_state.vk.command_buffer
    );
    vkDestroyCommandPool(DVR_DEVICE, g_dvr_state.vk.command_pool, NULL);

    vkDestroyDevice(DVR_DEVICE, NULL);

    if (DVR_ENABLE_VALIDATION_LAYERS) {
        PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT =
            (PFN_vkDestroyDebugUtilsMessengerEXT
            )vkGetInstanceProcAddr(g_dvr_state.vk.instance, "vkDestroyDebugUtilsMessengerEXT");
        if (vkDestroyDebugUtilsMessengerEXT != NULL) {
            vkDestroyDebugUtilsMessengerEXT(
                g_dvr_state.vk.instance,
                g_dvr_state.vk.debug_messenger,
                NULL
            );
        }
    }

    vkDestroySurfaceKHR(g_dvr_state.vk.instance, g_dvr_state.vk.surface, NULL);
    vkDestroyInstance(g_dvr_state.vk.instance, NULL);
}

static void dvr_glfw_shutdown(void) {
    glfwDestroyWindow(g_dvr_state.window.window);
    glfwTerminate();
}

void dvr_shutdown(void) {
    dvr_vk_shutdown();
    dvr_glfw_shutdown();
    dvr_log_close();
}

dvr_framebuffer dvr_swapchain_framebuffer(void) {
    return g_dvr_state.defaults.swapchain_framebuffers[g_dvr_state.vk.image_index];
}

dvr_render_pass dvr_swapchain_render_pass(void) {
    return g_dvr_state.defaults.swapchain_render_pass;
}

VkCommandBuffer dvr_command_buffer(void) {
    return g_dvr_state.vk.command_buffer;
}

static DVR_RESULT(dvr_none) dvr_vk_recreate_swapchain(void) {
    i32 width = 0;
    i32 height = 0;
    glfwGetFramebufferSize(g_dvr_state.window.window, &width, &height);
    while (width == 0 || height == 0) {
        glfwGetFramebufferSize(g_dvr_state.window.window, &width, &height);
        glfwWaitEvents();
    }

    vkDeviceWaitIdle(DVR_DEVICE);

    dvr_vk_cleanup_swapchain();

    DVR_RESULT(dvr_none) res = dvr_vk_create_swapchain();
    DVR_BUBBLE(res);

    res = dvr_vk_create_swapchain_image_views();
    DVR_BUBBLE(res);

    res = dvr_vk_create_swapchain_render_pass();
    DVR_BUBBLE(res);

    res = dvr_vk_create_swapchain_framebuffers();
    DVR_BUBBLE(res);

    return DVR_OK(dvr_none, DVR_NONE);
}

DVR_RESULT(dvr_none) dvr_begin_frame(void) {
    vkWaitForFences(DVR_DEVICE, 1, &g_dvr_state.vk.in_flight_fence, VK_TRUE, UINT64_MAX);

    VkResult result = vkAcquireNextImageKHR(
        DVR_DEVICE,
        g_dvr_state.vk.swapchain,
        UINT64_MAX,
        g_dvr_state.vk.image_available_sem,
        VK_NULL_HANDLE,
        &g_dvr_state.vk.image_index
    );

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        DVR_RESULT(dvr_none) res = dvr_vk_recreate_swapchain();
        DVR_BUBBLE(res);
        return DVR_OK(dvr_none, DVR_NONE);
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        return DVR_ERROR(dvr_none, "failed to acquire swapchain image");
    }

    vkResetFences(DVR_DEVICE, 1, &g_dvr_state.vk.in_flight_fence);

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = 0,
        .pInheritanceInfo = NULL,
    };

    if (vkBeginCommandBuffer(g_dvr_state.vk.command_buffer, &begin_info) != VK_SUCCESS) {
        return DVR_ERROR(dvr_none, "failed to begin recording command buffer");
    }

    return DVR_OK(dvr_none, DVR_NONE);
}

DVR_RESULT(dvr_none) dvr_end_frame(void) {
    if (vkEndCommandBuffer(g_dvr_state.vk.command_buffer) != VK_SUCCESS) {
        return DVR_ERROR(dvr_none, "failed to record command buffer");
    }

    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pWaitDstStageMask =
            (VkPipelineStageFlags[]){ VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT },
        .commandBufferCount = 1,
        .pCommandBuffers = &g_dvr_state.vk.command_buffer,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores =
            (VkSemaphore[]){
                g_dvr_state.vk.image_available_sem,
            },
        .signalSemaphoreCount = 1,
        .pSignalSemaphores =
            (VkSemaphore[]){
                g_dvr_state.vk.render_finished_sem,
            },
    };

    if (vkQueueSubmit(
            g_dvr_state.vk.graphics_queue,
            1,
            &submit_info,
            g_dvr_state.vk.in_flight_fence
        ) != VK_SUCCESS) {
        return DVR_ERROR(dvr_none, "failed to submit draw command buffer");
    }

    VkPresentInfoKHR present_info = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores =
            (VkSemaphore[]){
                g_dvr_state.vk.render_finished_sem,
            },
        .swapchainCount = 1,
        .pSwapchains =
            (VkSwapchainKHR[]){
                g_dvr_state.vk.swapchain,
            },
        .pImageIndices = &g_dvr_state.vk.image_index,
        .pResults = NULL,
    };

    VkResult result = vkQueuePresentKHR(g_dvr_state.vk.present_queue, &present_info);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
        g_dvr_state.window.just_resized) {
        g_dvr_state.window.just_resized = false;
        DVR_RESULT(dvr_none) res = dvr_vk_recreate_swapchain();
        DVR_BUBBLE(res);
        return DVR_OK(dvr_none, DVR_NONE);
    } else if (result != VK_SUCCESS) {
        return DVR_ERROR(dvr_none, "failed to present swapchain image");
    }

    return DVR_OK(dvr_none, DVR_NONE);
}

bool dvr_should_close(void) {
    return glfwWindowShouldClose(g_dvr_state.window.window);
}

void dvr_poll_events(void) {
    glfwPollEvents();
}

void dvr_close(void) {
    glfwSetWindowShouldClose(g_dvr_state.window.window, GLFW_TRUE);
}

void dvr_wait_idle(void) {
    vkDeviceWaitIdle(DVR_DEVICE);
}

void dvr_get_window_size(u32* width, u32* height) {
    i32 w, h;
    glfwGetFramebufferSize(g_dvr_state.window.window, &w, &h);
    *width = (u32)w;
    *height = (u32)h;
}
