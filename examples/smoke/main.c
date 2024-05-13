#include "dvr.h"
#include "dvr_utils.h"

#include <cglm/cglm.h>

#include <stb/stb_ds.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include <signal.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <time.h>
#endif

#define APP_WINDOW_WIDTH 1920
#define APP_WINDOW_HEIGHT 1080
#ifdef RELEASE
#define APP_WINDOW_NAME "smoke"
#else
#define APP_WINDOW_NAME "dev: smoke"
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
static void app_update();
static void app_compute();
static void app_draw();
static void app_draw_imgui();
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

    result = dvr_imgui_setup();
    DVR_EXIT_ON_ERROR(result);

    result = app_setup();
    DVR_EXIT_ON_ERROR(result);

    while (!dvr_should_close()) {
        dvr_poll_events();
        app_update();

        app_draw_imgui();

        result = dvr_begin_compute();
        DVR_EXIT_ON_ERROR(result);

        app_compute();

        result = dvr_end_compute();
        DVR_EXIT_ON_ERROR(result);

        result = dvr_begin_frame();
        DVR_EXIT_ON_ERROR(result);

        app_draw();

        result = dvr_end_frame();
        DVR_EXIT_ON_ERROR(result);
    }

    app_shutdown();

    dvr_imgui_shutdown();

    dvr_shutdown();

    return 0;
}

// APP CODE

#define FRAMETIME_SAMPLES 2000

typedef struct app_state {
    dvr_image smoke_images[2];

    dvr_sampler sampler;

    dvr_descriptor_set_layout compute_descriptor_set_layout;
    dvr_descriptor_set compute_descriptor_sets[2];

    dvr_compute_pipeline smoke_source_pipeline;
    dvr_compute_pipeline smoke_diffuse_pipeline;
    dvr_compute_pipeline smoke_advect_pipeline;
    dvr_compute_pipeline smoke_velocity_pipeline;

    dvr_descriptor_set_layout descriptor_set_layout;
    dvr_descriptor_set descriptor_sets[2];
    dvr_pipeline pipeline;

    f64 start_time;
    f64 total_time;
    f64 delta_time;

    f64 frame_times[FRAMETIME_SAMPLES];
    u32 frame_time_index;
    u32 frame_count;

    f32 hue;
} app_state;

static app_state g_app_state;

typedef struct particle {
    vec2 position;
    float angle;
    float _padding; // std140 padding
} particle;

typedef struct smoke_push_constants {
    f32 delta_time;
    f32 diffuse_strength;
    vec2 emitter_position;
    f32 emitter_radius;
    f32 _padding2;
} smoke_push_constants;

typedef struct render_push_constants {
    f32 hue;
} render_push_constants;

static DVR_RESULT(dvr_compute_pipeline) build_pipeline(const char* file) {
    DVR_RESULT(dvr_range) compute_spv_res = dvr_read_file(file);
    DVR_BUBBLE_INTO(dvr_compute_pipeline, compute_spv_res);

    dvr_range compute_spv = DVR_UNWRAP(compute_spv_res);

    DVR_RESULT(dvr_shader_module)
    compute_shader_res = dvr_create_shader_module(&(dvr_shader_module_desc){
        .code = compute_spv,
    });
    dvr_free_file(compute_spv);
    DVR_BUBBLE_INTO(dvr_compute_pipeline, compute_shader_res);

    dvr_shader_module smoke_shader = DVR_UNWRAP(compute_shader_res);

    DVR_RESULT(dvr_compute_pipeline)
    comp_pipeline_res = dvr_create_compute_pipeline(&(dvr_compute_pipeline_desc){
        .shader_module = smoke_shader,
        .entry_point = "main",
        .num_desc_set_layouts = 1,
        .desc_set_layouts = &g_app_state.compute_descriptor_set_layout,
        .num_push_constant_ranges = 1,
        .push_constant_ranges =
            (VkPushConstantRange[]){
                (VkPushConstantRange){
                    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                    .offset = 0,
                    .size = sizeof(smoke_push_constants),
                },
            },
    });

    dvr_destroy_shader_module(smoke_shader);

    return comp_pipeline_res;
}

static DVR_RESULT(dvr_none) app_setup(void) {
    for (u8 i = 0; i < 2; i++) {
        DVR_RESULT(dvr_image)
        texture_res = dvr_create_image(&(dvr_image_desc){
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            .width = APP_WINDOW_WIDTH,
            .height = APP_WINDOW_HEIGHT,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .properties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        });

        DVR_BUBBLE_INTO(dvr_none, texture_res);
        g_app_state.smoke_images[i] = DVR_UNWRAP(texture_res);
    }

    DVR_RESULT(dvr_sampler)
    sampler_res = dvr_create_sampler(&(dvr_sampler_desc){
        .min_filter = VK_FILTER_NEAREST,
        .mag_filter = VK_FILTER_NEAREST,
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

    DVR_RESULT(dvr_descriptor_set_layout)
    descriptor_set_layout_res =
        dvr_create_descriptor_set_layout(&(dvr_descriptor_set_layout_desc){
            .num_bindings = 2,
            .bindings =
                (dvr_descriptor_set_layout_binding_desc[]){
                    (dvr_descriptor_set_layout_binding_desc){
                        .binding = 0,
                        .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                        .stage_flags = VK_SHADER_STAGE_COMPUTE_BIT,
                        .count = 1,
                    },
                    (dvr_descriptor_set_layout_binding_desc){
                        .binding = 1,
                        .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                        .stage_flags = VK_SHADER_STAGE_COMPUTE_BIT,
                        .count = 1,
                    },
                },
        });
    DVR_BUBBLE_INTO(dvr_none, descriptor_set_layout_res);
    g_app_state.compute_descriptor_set_layout = DVR_UNWRAP(descriptor_set_layout_res);

    DVR_RESULT(dvr_descriptor_set)
    descriptor_set_res = dvr_create_descriptor_set(&(dvr_descriptor_set_desc){
        .layout = g_app_state.compute_descriptor_set_layout,
        .num_bindings = 2,
        .bindings =
            (dvr_descriptor_set_binding_desc[]){
                (dvr_descriptor_set_binding_desc){
                    .binding = 0,
                    .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    .image = {
                        .image = g_app_state.smoke_images[0],
                        .sampler = g_app_state.sampler,
                    },
                },
                (dvr_descriptor_set_binding_desc){
                    .binding = 1,
                    .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    .image = {
                        .image = g_app_state.smoke_images[1],
                        .sampler = g_app_state.sampler,
                    },
                },
            },
    });
    DVR_BUBBLE_INTO(dvr_none, descriptor_set_res);
    g_app_state.compute_descriptor_sets[0] = DVR_UNWRAP(descriptor_set_res);

    descriptor_set_res = dvr_create_descriptor_set(&(dvr_descriptor_set_desc){
        .layout = g_app_state.compute_descriptor_set_layout,
        .num_bindings = 2,
        .bindings =
            (dvr_descriptor_set_binding_desc[]){
                (dvr_descriptor_set_binding_desc){
                    .binding = 0,
                    .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    .image = {
                        .image = g_app_state.smoke_images[1],
                        .sampler = g_app_state.sampler,
                    },
                },
                (dvr_descriptor_set_binding_desc){
                    .binding = 1,
                    .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    .image = {
                        .image = g_app_state.smoke_images[0],
                        .sampler = g_app_state.sampler,
                    },
                },
            },
    });
    DVR_BUBBLE_INTO(dvr_none, descriptor_set_res);
    g_app_state.compute_descriptor_sets[1] = DVR_UNWRAP(descriptor_set_res);

    DVR_RESULT(dvr_compute_pipeline)
    comp_pipeline_res = build_pipeline("smoke_source_cs.spv");
    DVR_BUBBLE_INTO(dvr_none, comp_pipeline_res);
    g_app_state.smoke_source_pipeline = DVR_UNWRAP(comp_pipeline_res);

    comp_pipeline_res = build_pipeline("smoke_diffuse_cs.spv");
    DVR_BUBBLE_INTO(dvr_none, comp_pipeline_res);
    g_app_state.smoke_diffuse_pipeline = DVR_UNWRAP(comp_pipeline_res);

    comp_pipeline_res = build_pipeline("smoke_advect_cs.spv");
    DVR_BUBBLE_INTO(dvr_none, comp_pipeline_res);
    g_app_state.smoke_advect_pipeline = DVR_UNWRAP(comp_pipeline_res);

    comp_pipeline_res = build_pipeline("smoke_velocity_cs.spv");
    DVR_BUBBLE_INTO(dvr_none, comp_pipeline_res);
    g_app_state.smoke_velocity_pipeline = DVR_UNWRAP(comp_pipeline_res);

    DVR_RESULT(dvr_range) vert_spv_res = dvr_read_file("rt_render_vs.spv");
    DVR_BUBBLE_INTO(dvr_none, vert_spv_res);

    dvr_range vert_spv = DVR_UNWRAP(vert_spv_res);

    DVR_RESULT(dvr_range) frag_spv_res = dvr_read_file("smoke_render_fs.spv");
    DVR_BUBBLE_INTO(dvr_none, frag_spv_res);

    dvr_range frag_spv = DVR_UNWRAP(frag_spv_res);

    DVR_RESULT(dvr_shader_module)
    vert_shader_res = dvr_create_shader_module(&(dvr_shader_module_desc){
        .code = vert_spv,
    });
    dvr_free_file(vert_spv);
    DVR_BUBBLE_INTO(dvr_none, vert_shader_res);

    DVR_RESULT(dvr_shader_module)
    frag_shader_res = dvr_create_shader_module(&(dvr_shader_module_desc){
        .code = frag_spv,
    });
    dvr_free_file(frag_spv);
    DVR_BUBBLE_INTO(dvr_none, frag_shader_res);

    dvr_shader_module vert_shader = DVR_UNWRAP(vert_shader_res);
    dvr_shader_module frag_shader = DVR_UNWRAP(frag_shader_res);

    descriptor_set_layout_res =
        dvr_create_descriptor_set_layout(&(dvr_descriptor_set_layout_desc){
            .num_bindings = 1,
            .bindings =
                (dvr_descriptor_set_layout_binding_desc[]){
                    (dvr_descriptor_set_layout_binding_desc){
                        .binding = 0,
                        .array_element = 0,
                        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                        .count = 1,
                        .stage_flags = VK_SHADER_STAGE_FRAGMENT_BIT,
                    },
                },
        });
    DVR_BUBBLE_INTO(dvr_none, descriptor_set_layout_res);
    g_app_state.descriptor_set_layout = DVR_UNWRAP(descriptor_set_layout_res);

    for (u8 i = 0; i < 2; i++) {
        descriptor_set_res = dvr_create_descriptor_set(&(dvr_descriptor_set_desc){
            .layout = g_app_state.descriptor_set_layout,
            .num_bindings = 1,
            .bindings =
                (dvr_descriptor_set_binding_desc[]){
                    (dvr_descriptor_set_binding_desc){
                        .binding = 0,
                        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                        .image = {
                            .image = g_app_state.smoke_images[i],
                            .sampler = g_app_state.sampler,
                            .layout = VK_IMAGE_LAYOUT_GENERAL,
                        },
                    },
                },
        });
        DVR_BUBBLE_INTO(dvr_none, descriptor_set_res);
        g_app_state.descriptor_sets[i] = DVR_UNWRAP(descriptor_set_res);
    }

    DVR_RESULT(dvr_pipeline)
    pipeline_res = dvr_create_pipeline(&(dvr_pipeline_desc){
        .render_pass = dvr_swapchain_render_pass(),
        .subpass = 0,
        .layout = {
            .num_desc_set_layouts = 1,
            .desc_set_layouts = &g_app_state.descriptor_set_layout,
            .num_push_constant_ranges = 1,
            .push_constant_ranges =
                (VkPushConstantRange[]){
                    (VkPushConstantRange){
                        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                        .offset = 0,
                        .size = sizeof(render_push_constants),
                    },
                },
        },
        .num_stages = 2,
        .stages = (dvr_pipeline_stage_desc[]){
            {
                .stage = VK_SHADER_STAGE_VERTEX_BIT,
                .entry_point = "main",
                .shader_module = vert_shader,
            },
            {
                .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                .entry_point = "main",
                .shader_module = frag_shader,
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
            .rasterization_samples = dvr_max_msaa_samples(),
            .sample_shading_enable = false,
            .alpha_to_one_enable = false,
            .alpha_to_coverage_enable = false,
        },
        .vertex_input = {
            .num_bindings = 0,
            .bindings = NULL,
            .num_attributes = 0,
            .attributes = NULL,
        },
        .depth_stencil = {
            .depth_test_enable = true,
            .depth_write_enable = true,
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

    dvr_destroy_shader_module(vert_shader);
    dvr_destroy_shader_module(frag_shader);

    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    g_app_state.start_time = (f64)start_time.tv_sec + (f64)start_time.tv_nsec / 1.0e9;
    g_app_state.total_time = 0.0;
    g_app_state.delta_time = 0.0;
    g_app_state.frame_time_index = 0;
    g_app_state.frame_count = 0;

    return DVR_OK(dvr_none, DVR_NONE);
}

static void app_update(void) {
    struct timespec current_time;
    clock_gettime(CLOCK_MONOTONIC, &current_time);

    f64 current_time_s = (f64)current_time.tv_sec + (f64)current_time.tv_nsec / 1.0e9;
    g_app_state.delta_time = current_time_s - g_app_state.total_time - g_app_state.start_time;
    g_app_state.total_time = current_time_s - g_app_state.start_time;

    g_app_state.frame_times[g_app_state.frame_time_index] = g_app_state.delta_time;
    g_app_state.frame_time_index = (g_app_state.frame_time_index + 1) % FRAMETIME_SAMPLES;
    g_app_state.frame_count++;
}

static void barrier(void) {
    VkMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = NULL,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
    };

    vkCmdPipelineBarrier(
        dvr_compute_command_buffer(),
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        1,
        &barrier,
        0,
        NULL,
        0,
        NULL
    );
}

static void app_compute(void) {
    u32 wg_count_x = (u32)ceilf((f32)APP_WINDOW_WIDTH / 32.0f);
    u32 wg_count_y = (u32)ceilf((f32)APP_WINDOW_HEIGHT / 32.0f);

    vec2 mouse_pos = { 0.0f, 0.0f };
    dvr_get_mouse_pos(&mouse_pos[0], &mouse_pos[1]);

    u32 descriptor_set_index = g_app_state.frame_count % 2;

    smoke_push_constants push_constants = {
        .delta_time = (f32)g_app_state.delta_time,
        .diffuse_strength = 40.0f,
        .emitter_position = { mouse_pos[0], mouse_pos[1] },
        .emitter_radius = 24.0f,
    };

    dvr_bind_compute_pipeline(g_app_state.smoke_source_pipeline);
    dvr_bind_descriptor_set_compute(
        g_app_state.smoke_source_pipeline,
        g_app_state.compute_descriptor_sets[descriptor_set_index]
    );

    dvr_push_constants_compute(
        g_app_state.smoke_source_pipeline,
        0,
        (dvr_range){
            .base = &push_constants,
            .size = sizeof(smoke_push_constants),
        }
    );

    descriptor_set_index = (descriptor_set_index + 1) % 2;
    dvr_dispatch_compute(wg_count_x, wg_count_y, 1);

    barrier();

    dvr_bind_compute_pipeline(g_app_state.smoke_diffuse_pipeline);
    dvr_push_constants_compute(
        g_app_state.smoke_diffuse_pipeline,
        0,
        (dvr_range){
            .base = &push_constants,
            .size = sizeof(smoke_push_constants),
        }
    );
    // for (u32 i = 0; i < 10; i++) {
        dvr_bind_descriptor_set_compute(
            g_app_state.smoke_diffuse_pipeline,
            g_app_state.compute_descriptor_sets[descriptor_set_index]
        );

        descriptor_set_index = (descriptor_set_index + 1) % 2;
        dvr_dispatch_compute(wg_count_x, wg_count_y, 1);

        barrier();
    // }

    dvr_bind_compute_pipeline(g_app_state.smoke_advect_pipeline);
    dvr_push_constants_compute(
        g_app_state.smoke_advect_pipeline,
        0,
        (dvr_range){
            .base = &push_constants,
            .size = sizeof(smoke_push_constants),
        }
    );
    dvr_bind_descriptor_set_compute(
        g_app_state.smoke_advect_pipeline,
        g_app_state.compute_descriptor_sets[descriptor_set_index]
    );

    descriptor_set_index = (descriptor_set_index + 1) % 2;
    dvr_dispatch_compute(wg_count_x, wg_count_y, 1);

    barrier();

    dvr_bind_compute_pipeline(g_app_state.smoke_velocity_pipeline);
    dvr_push_constants_compute(
        g_app_state.smoke_velocity_pipeline,
        0,
        (dvr_range){
            .base = &push_constants,
            .size = sizeof(smoke_push_constants),
        }
    );
    dvr_bind_descriptor_set_compute(
        g_app_state.smoke_velocity_pipeline,
        g_app_state.compute_descriptor_sets[descriptor_set_index]
    );

    descriptor_set_index = (descriptor_set_index + 1) % 2;
    dvr_dispatch_compute(wg_count_x, wg_count_y, 1);
}

static void app_draw(void) {
    dvr_begin_swapchain_render_pass();

    dvr_bind_pipeline(g_app_state.pipeline);
    dvr_bind_descriptor_set(
        g_app_state.pipeline,
        g_app_state.descriptor_sets[(g_app_state.frame_count - 1) % 2]
    );
    dvr_push_constants(
        g_app_state.pipeline,
        VK_SHADER_STAGE_FRAGMENT_BIT,
        0,
        (dvr_range){
            .base =
                &(render_push_constants){
                    .hue = g_app_state.hue,
                },
            .size = sizeof(render_push_constants),
        }
    );
    vkCmdDraw(dvr_command_buffer(), 3, 1, 0, 0);

    dvr_imgui_render();

    dvr_end_render_pass();
}

static void app_draw_imgui(void) {
    dvr_imgui_begin_frame();

    f64 avg_frametime = 0.0;
    for (u32 i = 0; i < (g_app_state.frame_count < FRAMETIME_SAMPLES ? g_app_state.frame_count
                                                                     : FRAMETIME_SAMPLES);
         i++) {
        avg_frametime += g_app_state.frame_times[i];
    }
    avg_frametime /= (f64)(g_app_state.frame_count < FRAMETIME_SAMPLES ? g_app_state.frame_count
                                                                       : FRAMETIME_SAMPLES);

    igBegin(APP_WINDOW_NAME, NULL, 0);

    igText("Frame Time: %.3f ms (avg %d samples)", avg_frametime * 1000.0f, FRAMETIME_SAMPLES);
    igText("FPS: %.1f", 1.0f / avg_frametime);

    if (igCollapsingHeader_TreeNodeFlags("smoke", 0)) {}

    if (igCollapsingHeader_TreeNodeFlags("render", 0)) {
        igSliderFloat("hue", &g_app_state.hue, 0.0f, 360.0f, "%.1f", 1.0f);
    }

    igEnd();
}

static void app_shutdown(void) {
    dvr_wait_idle();

    for (u8 i = 0; i < 2; i++) {
        dvr_destroy_image(g_app_state.smoke_images[i]);
        dvr_destroy_descriptor_set(g_app_state.compute_descriptor_sets[i]);
        dvr_destroy_descriptor_set(g_app_state.descriptor_sets[i]);
    }
    dvr_destroy_sampler(g_app_state.sampler);
    dvr_destroy_descriptor_set_layout(g_app_state.compute_descriptor_set_layout);
    dvr_destroy_descriptor_set_layout(g_app_state.descriptor_set_layout);
    dvr_destroy_compute_pipeline(g_app_state.smoke_source_pipeline);
    dvr_destroy_compute_pipeline(g_app_state.smoke_diffuse_pipeline);
    dvr_destroy_compute_pipeline(g_app_state.smoke_advect_pipeline);
    dvr_destroy_compute_pipeline(g_app_state.smoke_velocity_pipeline);
    dvr_destroy_pipeline(g_app_state.pipeline);
}
