#pragma once

/// DVR: Duckonaut's Vulkan Renderer
///
/// This provides a layer of abstraction over Vulkan to make it easier to use.

#include "dvr_types.h"
#include "dvr_result.h"
#include "dvr_utils.h"

#include <vulkan/vulkan.h>

typedef struct dvr_setup_desc {
    u32 initial_width;
    u32 initial_height;
    const char* app_name;
} dvr_setup_desc;

typedef enum dvr_buffer_lifecycle {
    DVR_BUFFER_LIFECYCLE_STATIC,
    DVR_BUFFER_LIFECYCLE_DYNAMIC,
} dvr_buffer_lifecycle;

typedef enum dvr_buffer_usage {
    DVR_BUFFER_USAGE_NONE = 0,
    DVR_BUFFER_USAGE_VERTEX = 1 << 0,
    DVR_BUFFER_USAGE_INDEX = 1 << 1,
    DVR_BUFFER_USAGE_UNIFORM = 1 << 2,
    DVR_BUFFER_USAGE_TRANSFER_SRC = 1 << 3,
    DVR_BUFFER_USAGE_TRANSFER_DST = 1 << 4,
} dvr_buffer_usage;

typedef struct dvr_buffer_desc {
    dvr_range data;
    dvr_buffer_usage usage;
    dvr_buffer_lifecycle lifecycle;
} dvr_buffer_desc;

typedef struct dvr_buffer {
    u16 id;
} dvr_buffer;
DVR_RESULT_DEF(dvr_buffer);

DVR_RESULT(dvr_buffer) dvr_create_buffer(dvr_buffer_desc* desc);
void dvr_destroy_buffer(dvr_buffer buffer);
void dvr_write_buffer(dvr_buffer buffer, dvr_range data);

typedef struct dvr_image_desc {
    u32 width;
    u32 height;
    dvr_range data;
    bool generate_mipmaps;
    VkSampleCountFlagBits num_samples;
    VkFormat format;
    VkImageTiling tiling;
    VkImageUsageFlags usage;
    VkMemoryPropertyFlags properties;
} dvr_image_desc;

typedef struct dvr_image {
    u16 id;
} dvr_image;
DVR_RESULT_DEF(dvr_image);

DVR_RESULT(dvr_image) dvr_create_image(dvr_image_desc* desc);
void dvr_destroy_image(dvr_image image);

typedef struct dvr_render_pass_attachment_desc {
    bool enable;
    VkFormat format;
    VkSampleCountFlagBits num_samples;
    VkAttachmentLoadOp load_op;
    VkAttachmentStoreOp store_op;
    VkAttachmentLoadOp stencil_load_op;
    VkAttachmentStoreOp stencil_store_op;
    VkImageLayout initial_layout;
    VkImageLayout final_layout;
} dvr_render_pass_attachment_desc;

#define DVR_MAX_RENDER_PASS_COLOR_ATTACHMENTS 8
typedef struct dvr_render_pass_desc {
    u32 num_color_attachments;
    dvr_render_pass_attachment_desc color_attachments[DVR_MAX_RENDER_PASS_COLOR_ATTACHMENTS];
    dvr_render_pass_attachment_desc depth_stencil_attachment;
    dvr_render_pass_attachment_desc resolve_attachment;
} dvr_render_pass_desc;

typedef struct dvr_render_pass {
    u16 id;
} dvr_render_pass;
DVR_RESULT_DEF(dvr_render_pass);

DVR_RESULT(dvr_render_pass) dvr_create_render_pass(dvr_render_pass_desc* desc);
void dvr_destroy_render_pass(dvr_render_pass render_pass);
