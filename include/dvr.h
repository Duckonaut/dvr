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
    DVR_BUFFER_USAGE_STORAGE = 1 << 5,
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
void dvr_write_buffer(dvr_buffer buffer, dvr_range data, u32 offset);
void dvr_bind_vertex_buffer(dvr_buffer buffer, u32 binding);
void dvr_bind_index_buffer(dvr_buffer buffer, VkIndexType index_type);
void dvr_bind_uniform_buffer(dvr_buffer buffer, u32 binding);

typedef struct dvr_image_desc {
    u32 width;
    u32 height;
    bool render_target;
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

typedef struct dvr_sampler_desc {
    VkFilter mag_filter;
    VkFilter min_filter;
    VkSamplerMipmapMode mipmap_mode;
    VkSamplerAddressMode address_mode_u;
    VkSamplerAddressMode address_mode_v;
    VkSamplerAddressMode address_mode_w;
    f32 mip_lod_bias;
    bool anisotropy_enable;
    f32 max_anisotropy;
    bool compare_enable;
    VkCompareOp compare_op;
    f32 min_lod;
    f32 max_lod;
    VkBorderColor border_color;
    bool unnormalized_coordinates;
} dvr_sampler_desc;

typedef struct dvr_sampler {
    u16 id;
} dvr_sampler;
DVR_RESULT_DEF(dvr_sampler);

DVR_RESULT(dvr_sampler) dvr_create_sampler(dvr_sampler_desc* desc);
void dvr_destroy_sampler(dvr_sampler sampler);

typedef struct dvr_render_pass_attachment_desc {
    bool enable;
    VkFormat format;
    VkSampleCountFlagBits samples;
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
    u32 num_resolve_attachments;
    dvr_render_pass_attachment_desc resolve_attachments[DVR_MAX_RENDER_PASS_COLOR_ATTACHMENTS];
    dvr_render_pass_attachment_desc depth_stencil_attachment;
} dvr_render_pass_desc;

typedef struct dvr_render_pass {
    u16 id;
} dvr_render_pass;
DVR_RESULT_DEF(dvr_render_pass);

DVR_RESULT(dvr_render_pass) dvr_create_render_pass(dvr_render_pass_desc* desc);
void dvr_destroy_render_pass(dvr_render_pass render_pass);

typedef struct dvr_framebuffer dvr_framebuffer;

void dvr_begin_render_pass(
    dvr_render_pass render_pass,
    dvr_framebuffer framebuffer,
    VkClearValue* clear_values,
    u32 num_clear_values
);
void dvr_end_render_pass();

typedef struct dvr_descriptor_set_layout_binding_desc {
    u32 binding;
    u32 array_element;
    VkDescriptorType type;
    u32 count;
    VkShaderStageFlags stage_flags;
} dvr_descriptor_set_layout_binding_desc;

typedef struct dvr_descriptor_set_layout_desc {
    u32 num_bindings;
    dvr_descriptor_set_layout_binding_desc* bindings;
} dvr_descriptor_set_layout_desc;

typedef struct dvr_descriptor_set_layout {
    u16 id;
} dvr_descriptor_set_layout;
DVR_RESULT_DEF(dvr_descriptor_set_layout);

DVR_RESULT(dvr_descriptor_set_layout)
dvr_create_descriptor_set_layout(dvr_descriptor_set_layout_desc* desc);
void dvr_destroy_descriptor_set_layout(dvr_descriptor_set_layout desc_set_layout);

typedef struct dvr_descriptor_set_binding_desc {
    u32 binding;
    VkDescriptorType type;
    union {
        struct {
            dvr_buffer buffer;
            u32 offset;
            u32 size;
        } buffer;
        struct {
            dvr_image image;
            dvr_sampler sampler;
            VkImageLayout layout;
        } image;
    };
} dvr_descriptor_set_binding_desc;

typedef struct dvr_descriptor_set_desc {
    dvr_descriptor_set_layout layout;
    u32 num_bindings;
    dvr_descriptor_set_binding_desc* bindings;
} dvr_descriptor_set_desc;

typedef struct dvr_descriptor_set {
    u16 id;
} dvr_descriptor_set;
DVR_RESULT_DEF(dvr_descriptor_set);

DVR_RESULT(dvr_descriptor_set) dvr_create_descriptor_set(dvr_descriptor_set_desc* desc);
void dvr_destroy_descriptor_set(dvr_descriptor_set desc_set);

typedef struct dvr_pipeline dvr_pipeline;
void dvr_bind_descriptor_set(dvr_pipeline pipeline, dvr_descriptor_set desc_set);
typedef struct dvr_compute_pipeline dvr_compute_pipeline;
void dvr_bind_descriptor_set_compute(dvr_compute_pipeline pipeline, dvr_descriptor_set desc_set);

typedef struct dvr_shader_module_desc {
    dvr_range code;
} dvr_shader_module_desc;

typedef struct dvr_shader_module {
    u16 id;
} dvr_shader_module;
DVR_RESULT_DEF(dvr_shader_module);

DVR_RESULT(dvr_shader_module) dvr_create_shader_module(dvr_shader_module_desc* desc);
void dvr_destroy_shader_module(dvr_shader_module shader_module);

typedef struct dvr_pipeline_stage_desc {
    VkShaderStageFlagBits stage;
    dvr_shader_module shader_module;
    const char* entry_point;
} dvr_pipeline_stage_desc;

typedef struct dvr_vertex_input_state_desc {
    u32 num_bindings;
    VkVertexInputBindingDescription* bindings;
    u32 num_attributes;
    VkVertexInputAttributeDescription* attributes;
} dvr_vertex_input_state_desc;

typedef struct dvr_pipeline_desc {
    u32 num_stages;
    dvr_pipeline_stage_desc* stages;
    dvr_render_pass render_pass;
    u32 subpass;
    VkViewport viewport;
    VkRect2D scissor;

    dvr_vertex_input_state_desc vertex_input;
    struct {
        VkPrimitiveTopology topology;
        VkPolygonMode polygon_mode;
        bool primitive_restart_enable;
        bool rasterizer_discard_enable;
        f32 line_width;
        VkCullModeFlags cull_mode;
        VkFrontFace front_face;
    } rasterization;

    struct {
        bool depth_test_enable;
        bool depth_write_enable;
        bool depth_bias_enable;
        bool depth_clamp_enable;
        f32 depth_bias_constant_factor;
        f32 depth_bias_clamp;
        f32 depth_bias_slope_factor;
        VkCompareOp depth_compare_op;
        bool depth_bounds_test_enable;
        f32 min_depth_bounds;
        f32 max_depth_bounds;
        bool stencil_test_enable;
        VkStencilOpState front;
        VkStencilOpState back;
    } depth_stencil;

    struct {
        bool sample_shading_enable;
        f32 min_sample_shading;
        VkSampleCountFlagBits rasterization_samples;
        VkSampleMask* sample_mask;
        bool alpha_to_coverage_enable;
        bool alpha_to_one_enable;
    } multisample;

    struct {
        bool blend_enable;
        u32 num_attachments;
        VkBlendFactor src_color_blend_factor;
        VkBlendFactor dst_color_blend_factor;
        VkBlendOp color_blend_op;
        VkBlendFactor src_alpha_blend_factor;
        VkBlendFactor dst_alpha_blend_factor;
        VkBlendOp alpha_blend_op;
    } color_blend;

    struct {
        u32 num_desc_set_layouts;
        dvr_descriptor_set_layout* desc_set_layouts;
        u32 num_push_constant_ranges;
        VkPushConstantRange* push_constant_ranges;
    } layout;
} dvr_pipeline_desc;

typedef struct dvr_pipeline {
    u16 id;
} dvr_pipeline;
DVR_RESULT_DEF(dvr_pipeline);

DVR_RESULT(dvr_pipeline) dvr_create_pipeline(dvr_pipeline_desc* desc);
void dvr_destroy_pipeline(dvr_pipeline pipeline);

void dvr_bind_pipeline(dvr_pipeline pipeline);

typedef struct dvr_framebuffer_desc {
    dvr_render_pass render_pass;
    u32 num_attachments;
    dvr_image* attachments;
    u32 width;
    u32 height;
} dvr_framebuffer_desc;

typedef struct dvr_framebuffer {
    u16 id;
} dvr_framebuffer;
DVR_RESULT_DEF(dvr_framebuffer);

DVR_RESULT(dvr_framebuffer) dvr_create_framebuffer(dvr_framebuffer_desc* desc);
void dvr_destroy_framebuffer(dvr_framebuffer framebuffer);

typedef struct dvr_compute_pipeline {
    u16 id;
} dvr_compute_pipeline;
DVR_RESULT_DEF(dvr_compute_pipeline);

typedef struct dvr_compute_pipeline_desc {
    dvr_shader_module shader_module;
    const char* entry_point;
    u32 num_desc_set_layouts;
    dvr_descriptor_set_layout* desc_set_layouts;
    u32 num_push_constant_ranges;
    VkPushConstantRange* push_constant_ranges;
} dvr_compute_pipeline_desc;

DVR_RESULT(dvr_compute_pipeline) dvr_create_compute_pipeline(dvr_compute_pipeline_desc* desc);
void dvr_destroy_compute_pipeline(dvr_compute_pipeline pipeline);

void dvr_bind_compute_pipeline(dvr_compute_pipeline pipeline);
void dvr_dispatch_compute(u32 group_count_x, u32 group_count_y, u32 group_count_z);
void dvr_push_constants_compute(dvr_compute_pipeline pipeline, u32 offset, dvr_range data);

DVR_RESULT(dvr_none) dvr_setup(dvr_setup_desc* desc);
void dvr_shutdown();

VkFormat dvr_swapchain_format();
VkSampleCountFlags dvr_max_msaa_samples();
dvr_framebuffer dvr_swapchain_framebuffer();
dvr_render_pass dvr_swapchain_render_pass();
void dvr_begin_swapchain_render_pass();

VkDevice dvr_device();
VkCommandBuffer dvr_command_buffer();
#define DVR_COMMAND_BUFFER dvr_command_buffer()

VkCommandBuffer dvr_compute_command_buffer();
#define DVR_COMPUTE_COMMAND_BUFFER dvr_compute_command_buffer()

DVR_RESULT(dvr_none) dvr_begin_frame();
DVR_RESULT(dvr_none) dvr_end_frame();

DVR_RESULT(dvr_none) dvr_begin_compute();
DVR_RESULT(dvr_none) dvr_end_compute();

bool dvr_should_close(void);
void dvr_poll_events(void);
void dvr_close(void);
void dvr_wait_idle(void);
void dvr_get_window_size(u32* width, u32* height);

#ifdef DVR_ENABLE_IMGUI
DVR_RESULT(dvr_none) dvr_imgui_setup();
void dvr_imgui_shutdown();

void dvr_imgui_begin_frame();
void dvr_imgui_render();

void dvr_imgui_info(void);
#endif

