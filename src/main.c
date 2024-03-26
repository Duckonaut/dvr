#include "dvr.h"
#include "dvr_utils.h"

#include <cglm/cglm.h>

#include <stb/stb_ds.h>
#include <stb/stb_image.h>

#include <assimp/cimport.h>
#include <assimp/scene.h>
#include <assimp/anim.h>
#include <assimp/defs.h>
#include <assimp/postprocess.h>

#include <stdint.h>
#include <signal.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <time.h>
#endif

#define APP_WINDOW_WIDTH 640
#define APP_WINDOW_HEIGHT 480
#ifdef RELEASE
#define APP_WINDOW_NAME PROJECT_NAME
#else
#define APP_WINDOW_NAME "dev: " PROJECT_NAME
#endif

void term_handler(int signum) {
    (void)signum;
    dvr_close();
}

static void get_executable_path(char* path, usize max_len) {
#ifdef _WIN32
    // set the executable path as the working directory
    memset(path, 0, max_len);
    GetModuleFileNameA(NULL, path, max_len);
#else
    // set the executable path as the working directory
    memset(path, 0, max_len);
    isize ret = readlink("/proc/self/exe", path, max_len);
    if (ret == -1) {
        DVRLOG_ERROR("readlink failed");
        return;
    }
#endif
}

static void get_executable_directory(char* path, usize max_len) {
    get_executable_path(path, max_len);
    usize last_slash = 0;
    usize path_len = strnlen(path, max_len);
    for (usize i = 0; i < path_len; i++) {
        if (path[i] == '/') {
            last_slash = i;
        }
    }
    path[last_slash] = '\0';
}

static void set_executable_directory(const char* path) {
#ifdef _WIN32
    SetCurrentDirectoryA(path);
#else
    chdir(path);
#endif
}

static DVR_RESULT(dvr_none) app_setup();
static void app_draw();
static void app_shutdown();

int main(void) {
    signal(SIGTERM, term_handler);
    signal(SIGINT, term_handler);

    char executable_directory[1024];
    get_executable_directory(executable_directory, sizeof(executable_directory));
    set_executable_directory(executable_directory);

    DVR_RESULT(dvr_none)
    result = dvr_setup(&(dvr_setup_desc){
        .app_name = APP_WINDOW_NAME,
        .initial_width = APP_WINDOW_WIDTH,
        .initial_height = APP_WINDOW_HEIGHT,
    });
    DVR_EXIT_ON_ERROR(result);

    result = app_setup();
    DVR_EXIT_ON_ERROR(result);

    while (!dvr_should_close()) {
        dvr_poll_events();
        result = dvr_begin_frame();
        DVR_EXIT_ON_ERROR(result);

        app_draw();

        result = dvr_end_frame();
        DVR_EXIT_ON_ERROR(result);
    }

    app_shutdown();

    dvr_shutdown();

    return 0;
}

// APP CODE

typedef struct app_state {
    dvr_image texture;
    dvr_sampler sampler;

    dvr_buffer vertex_buffer;
    dvr_buffer index_buffer;
    dvr_buffer uniform_buffer;

    dvr_descriptor_set_layout descriptor_set_layout;
    dvr_descriptor_set descriptor_set;

    dvr_pipeline pipeline;
} app_state;

static app_state g_app_state;

typedef struct vertex {
    vec3 pos;
    vec3 color;
    vec2 uv;
} vertex;

static const vertex k_vertices[] = {
    {
        .pos = { -0.5f, -0.5f, 0.0f },
        .color = { 1.0f, 0.0f, 0.0f },
        .uv = { 0.0f, 0.0f },
    },
    {
        .pos = { 0.5f, -0.5f, 0.0f },
        .color = { 0.0f, 1.0f, 0.0f },
        .uv = { 1.0f, 0.0f },
    },
    {
        .pos = { 0.5f, 0.5f, 0.0f },
        .color = { 0.0f, 0.0f, 1.0f },
        .uv = { 1.0f, 1.0f },
    },
    {
        .pos = { -0.5f, 0.5f, 0.0f },
        .color = { 1.0f, 1.0f, 1.0f },
        .uv = { 0.0f, 1.0f },
    },
};

static const u32 k_indices[] = {
    0, 1, 2, 2, 3, 0,
};

static const VkVertexInputBindingDescription k_vertex_binding_description = {
    .binding = 0,
    .stride = sizeof(vertex),
    .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
};

static const VkVertexInputAttributeDescription k_vertex_attribute_descriptions[] = {
    {
        .location = 0,
        .binding = 0,
        .format = VK_FORMAT_R32G32B32_SFLOAT,
        .offset = offsetof(vertex, pos),
    },
    {
        .location = 1,
        .binding = 0,
        .format = VK_FORMAT_R32G32B32_SFLOAT,
        .offset = offsetof(vertex, color),
    },
    {
        .location = 2,
        .binding = 0,
        .format = VK_FORMAT_R32G32_SFLOAT,
        .offset = offsetof(vertex, uv),
    },
};

typedef struct app_view_uniform {
    mat4 model;
    mat4 view;
    mat4 proj;
} app_view_uniform;

static DVR_RESULT(dvr_none) app_setup(void) {
    DVR_RESULT(dvr_range) texture_data_res = dvr_read_file("texture.png");
    DVR_BUBBLE_INTO(dvr_none, texture_data_res);

    dvr_range texture_data_range = DVR_UNWRAP(texture_data_res);

    i32 width, height, channels;
    u8* texture_data = stbi_load_from_memory(
        texture_data_range.base,
        (i32)texture_data_range.size,
        &width,
        &height,
        &channels,
        STBI_rgb_alpha
    );

    DVR_RESULT(dvr_image)
    texture_res = dvr_create_image(&(dvr_image_desc){
        .render_target = false,
        .data =
            (dvr_range){
                .base = texture_data,
                .size = (usize)(width * height * 4),
            },
        .generate_mipmaps = true,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
        .width = (u32)width,
        .height = (u32)height,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .properties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
    });

    stbi_image_free(texture_data);

    dvr_free_file(texture_data_range);

    DVR_BUBBLE_INTO(dvr_none, texture_res);
    g_app_state.texture = DVR_UNWRAP(texture_res);

    DVR_RESULT(dvr_sampler)
    sampler_res = dvr_create_sampler(&(dvr_sampler_desc){
        .min_filter = VK_FILTER_LINEAR,
        .mag_filter = VK_FILTER_LINEAR,
        .mipmap_mode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .address_mode_u = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .address_mode_v = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .address_mode_w = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .mip_lod_bias = 0.0f,
        .anisotropy_enable = true,
        .max_anisotropy = 1.0f,
        .compare_enable = false,
        .compare_op = VK_COMPARE_OP_ALWAYS,
        .min_lod = 0.0f,
        .max_lod = 0.0f,
        .border_color = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
        .unnormalized_coordinates = false,
    });
    DVR_BUBBLE_INTO(dvr_none, sampler_res);
    g_app_state.sampler = DVR_UNWRAP(sampler_res);

    DVR_RESULT(dvr_buffer)
    vertex_buffer_res = dvr_create_buffer(&(dvr_buffer_desc){
        .data =
            (dvr_range){
                .base = (u8*)k_vertices,
                .size = sizeof(k_vertices),
            },
        .usage = DVR_BUFFER_USAGE_VERTEX,
        .lifecycle = DVR_BUFFER_LIFECYCLE_STATIC,
    });
    DVR_BUBBLE_INTO(dvr_none, vertex_buffer_res);
    g_app_state.vertex_buffer = DVR_UNWRAP(vertex_buffer_res);

    DVR_RESULT(dvr_buffer)
    index_buffer_res = dvr_create_buffer(&(dvr_buffer_desc){
        .data =
            (dvr_range){
                .base = (u8*)k_indices,
                .size = sizeof(k_indices),
            },
        .usage = DVR_BUFFER_USAGE_INDEX,
        .lifecycle = DVR_BUFFER_LIFECYCLE_STATIC,
    });
    DVR_BUBBLE_INTO(dvr_none, index_buffer_res);
    g_app_state.index_buffer = DVR_UNWRAP(index_buffer_res);

    DVR_RESULT(dvr_buffer)
    uniform_buffer_res = dvr_create_buffer(&(dvr_buffer_desc){
        .data = {
            .base = NULL,
            .size = sizeof(app_view_uniform),
        },
        .usage = DVR_BUFFER_USAGE_UNIFORM,
        .lifecycle = DVR_BUFFER_LIFECYCLE_DYNAMIC,
    });
    DVR_BUBBLE_INTO(dvr_none, uniform_buffer_res);
    g_app_state.uniform_buffer = DVR_UNWRAP(uniform_buffer_res);

    DVR_RESULT(dvr_descriptor_set_layout)
    descriptor_set_layout_res =
        dvr_create_descriptor_set_layout(&(dvr_descriptor_set_layout_desc){
            .num_bindings = 2,
            .bindings =
                (dvr_descriptor_set_layout_binding_desc[]){
                    (dvr_descriptor_set_layout_binding_desc){
                        .binding = 0,
                        .array_element = 0,
                        .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                        .count = 1,
                        .stage_flags = VK_SHADER_STAGE_VERTEX_BIT,
                    },
                    (dvr_descriptor_set_layout_binding_desc){
                        .binding = 1,
                        .array_element = 0,
                        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                        .count = 1,
                        .stage_flags = VK_SHADER_STAGE_FRAGMENT_BIT,
                    },
                },
        });
    DVR_BUBBLE_INTO(dvr_none, descriptor_set_layout_res);
    g_app_state.descriptor_set_layout = DVR_UNWRAP(descriptor_set_layout_res);

    DVR_RESULT(dvr_descriptor_set)
    descriptor_set_res = dvr_create_descriptor_set(&(dvr_descriptor_set_desc){
        .layout = g_app_state.descriptor_set_layout,
        .num_bindings = 2,
        .bindings =
            (dvr_descriptor_set_binding_desc[]){
                (dvr_descriptor_set_binding_desc){
                    .binding = 0,
                    .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                    .buffer = {
                        .buffer = g_app_state.uniform_buffer,
                        .offset = 0,
                        .size = sizeof(app_view_uniform),
                    },
                },
                (dvr_descriptor_set_binding_desc){
                    .binding = 1,
                    .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    .image = {
                        .image = g_app_state.texture,
                        .sampler = g_app_state.sampler,
                    },
                },
            },
    });
    DVR_BUBBLE_INTO(dvr_none, descriptor_set_res);
    g_app_state.descriptor_set = DVR_UNWRAP(descriptor_set_res);

    DVR_RESULT(dvr_range) vert_spv_res = dvr_read_file("default_vs.spv");
    DVR_BUBBLE_INTO(dvr_none, vert_spv_res);

    DVR_RESULT(dvr_range) frag_spv_res = dvr_read_file("default_fs.spv");
    DVR_BUBBLE_INTO(dvr_none, frag_spv_res);

    dvr_range vert_spv_range = DVR_UNWRAP(vert_spv_res);
    dvr_range frag_spv_range = DVR_UNWRAP(frag_spv_res);

    DVR_RESULT(dvr_shader_module)
    vertex_module_res = dvr_create_shader_module(&(dvr_shader_module_desc){
        .code = vert_spv_range,
    });
    dvr_free_file(vert_spv_range);
    DVR_BUBBLE_INTO(dvr_none, vertex_module_res);
    dvr_shader_module vertex_module = DVR_UNWRAP(vertex_module_res);

    DVR_RESULT(dvr_shader_module)
    fragment_module_res = dvr_create_shader_module(&(dvr_shader_module_desc){
        .code = frag_spv_range,
    });
    dvr_free_file(frag_spv_range);
    DVR_BUBBLE_INTO(dvr_none, fragment_module_res);
    dvr_shader_module fragment_module = DVR_UNWRAP(fragment_module_res);

    DVR_RESULT(dvr_pipeline)
    pipeline_res = dvr_create_pipeline(&(dvr_pipeline_desc){
        .render_pass = dvr_swapchain_render_pass(),
        .subpass = 0,
        .layout = {
            .num_desc_set_layouts = 1,
            .desc_set_layouts = &g_app_state.descriptor_set_layout,
            .num_push_constant_ranges = 0,
        },
        .num_stages = 2,
        .stages = (dvr_pipeline_stage_desc[]){
            {
                .stage = VK_SHADER_STAGE_VERTEX_BIT,
                .entry_point = "main",
                .shader_module = vertex_module,
            },
            {
                .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                .entry_point = "main",
                .shader_module = fragment_module,
            },
        },
        .scissor = {
            .offset = { 0, 0 },
            .extent = { APP_WINDOW_WIDTH, APP_WINDOW_HEIGHT },
        },
        .viewport = {
            .x = 0,
            .y = 0,
            .width = APP_WINDOW_WIDTH,
            .height = APP_WINDOW_HEIGHT,
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        },
        .color_blend = {
            .blend_enable = true,
            .alpha_blend_op = VK_BLEND_OP_ADD,
            .color_blend_op = VK_BLEND_OP_ADD,
            .num_attachments = 1,
            .src_color_blend_factor = VK_BLEND_FACTOR_SRC_ALPHA,
            .dst_color_blend_factor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .src_alpha_blend_factor = VK_BLEND_FACTOR_ONE,
            .dst_alpha_blend_factor = VK_BLEND_FACTOR_ZERO,
        },
        .multisample = {
            .rasterization_samples = VK_SAMPLE_COUNT_1_BIT,
            .sample_shading_enable = false,
            .alpha_to_one_enable = false,
            .alpha_to_coverage_enable = false,
        },
        .vertex_input = {
            .num_bindings = 1,
            .bindings = (VkVertexInputBindingDescription*)&k_vertex_binding_description,
            .num_attributes = sizeof(k_vertex_attribute_descriptions) / sizeof(VkVertexInputAttributeDescription),
            .attributes = (VkVertexInputAttributeDescription*)k_vertex_attribute_descriptions,
        },
        .depth_stencil = {
            .depth_test_enable = false,
            .depth_write_enable = false,
            .depth_compare_op = VK_COMPARE_OP_LESS,
            .depth_bounds_test_enable = false,
            .stencil_test_enable = false,
        },
        .rasterization = {
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .polygon_mode = VK_POLYGON_MODE_FILL,
            .cull_mode = VK_CULL_MODE_NONE,
            .front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .line_width = 1.0f,
            .primitive_restart_enable = false,
            .rasterizer_discard_enable = false,
        },
    });
    DVR_BUBBLE_INTO(dvr_none, pipeline_res);
    g_app_state.pipeline = DVR_UNWRAP(pipeline_res);

    dvr_destroy_shader_module(vertex_module);
    dvr_destroy_shader_module(fragment_module);

    return DVR_OK(dvr_none, DVR_NONE);
}

static void app_draw(void) {
    dvr_begin_render_pass(
        dvr_swapchain_render_pass(),
        dvr_swapchain_framebuffer(),
        &(VkClearValue){
            .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } },
        },
        1
    );

    dvr_bind_pipeline(g_app_state.pipeline);

    dvr_bind_vertex_buffer(g_app_state.vertex_buffer, 0);
    dvr_bind_index_buffer(g_app_state.index_buffer, VK_INDEX_TYPE_UINT32);

    app_view_uniform view_uniform = {
        .model = GLM_MAT4_IDENTITY_INIT,
        .view = GLM_MAT4_IDENTITY_INIT,
        .proj = GLM_MAT4_IDENTITY_INIT,
    };
    u32 width, height;
    dvr_get_window_size(&width, &height);
    glm_perspective(
        (f32)GLM_PI_4,
        (f32)width / (f32)height,
        0.1f,
        100.0f,
        view_uniform.proj
    );
    glm_lookat(
        (vec3){ 0.0f, 0.0f, 2.0f },
        (vec3){ 0.0f, 0.0f, 0.0f },
        (vec3){ 0.0f, 1.0f, 0.0f },
        view_uniform.view
    );

    dvr_write_buffer(
        g_app_state.uniform_buffer,
        DVR_RANGE(view_uniform),
        0
    );
    dvr_bind_descriptor_set(g_app_state.pipeline, g_app_state.descriptor_set);

    vkCmdDrawIndexed(dvr_command_buffer(), 6, 1, 0, 0, 0);

    dvr_end_render_pass();
}

static void app_shutdown(void) {
    dvr_wait_idle();

    dvr_destroy_image(g_app_state.texture);
    dvr_destroy_sampler(g_app_state.sampler);
    dvr_destroy_buffer(g_app_state.vertex_buffer);
    dvr_destroy_buffer(g_app_state.index_buffer);
    dvr_destroy_buffer(g_app_state.uniform_buffer);
    dvr_destroy_descriptor_set(g_app_state.descriptor_set);
    dvr_destroy_descriptor_set_layout(g_app_state.descriptor_set_layout);
    dvr_destroy_pipeline(g_app_state.pipeline);
}
