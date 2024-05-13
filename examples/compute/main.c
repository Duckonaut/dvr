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
#define APP_WINDOW_NAME "compute"
#else
#define APP_WINDOW_NAME "dev: compute"
#endif

static inline f32 randf(void) {
    return (f32)rand() / (f32)RAND_MAX;
}

static inline f32 randfr(f32 min, f32 max) {
    return min + (max - min) * randf();
}

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
    srand((u32)time(NULL));
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
    dvr_image compute_targets[2];

    dvr_buffer particle_buffers[2];

    dvr_sampler sampler;

    dvr_descriptor_set_layout compute_descriptor_set_layout;
    dvr_descriptor_set compute_descriptor_sets[2];

    dvr_compute_pipeline particle_update_pipeline;
    dvr_compute_pipeline diffuse_pipeline;

    dvr_descriptor_set_layout descriptor_set_layout;
    dvr_descriptor_set descriptor_sets[2];
    dvr_pipeline pipeline;

    f64 start_time;
    f64 total_time;
    f64 delta_time;

    f64 frame_times[FRAMETIME_SAMPLES];
    u32 frame_time_index;
    u32 frame_count;

    f32 blur_strength;
    f32 decay;

    f32 speed;
    f32 turn_speed;
    f32 random_steer;
    f32 sensor_angle;
    f32 sensor_distance;

    f32 hue;
} app_state;

static app_state g_app_state;

typedef struct particle {
    vec2 position;
    float angle;
    float _padding; // std140 padding
} particle;

typedef struct diffuse_push_constants {
    f32 delta_time;
    f32 blur_strength;
    f32 decay;
} diffuse_push_constants;

typedef struct particle_push_constants {
    f32 world_size[2];
    u32 num_particles;
    f32 delta_time;
    f32 speed;
    f32 turn_speed;
    f32 random_steer;
    f32 sensor_angle;
    f32 sensor_distance;
} particle_push_constants;

typedef struct render_push_constants {
    f32 hue;
} render_push_constants;

#define NUM_PARTICLES 0x100000

static DVR_RESULT(dvr_none) app_setup(void) {
    g_app_state.decay = 0.001f;
    g_app_state.blur_strength = 0.01f;
    for (u8 i = 0; i < 2; i++) {
        DVR_RESULT(dvr_image)
        texture_res = dvr_create_image(&(dvr_image_desc){
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            .width = APP_WINDOW_WIDTH,
            .height = APP_WINDOW_HEIGHT,
            .format = VK_FORMAT_R32_SFLOAT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .properties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        });

        DVR_BUBBLE_INTO(dvr_none, texture_res);
        g_app_state.compute_targets[i] = DVR_UNWRAP(texture_res);
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

    particle* particle_data = malloc(sizeof(particle) * NUM_PARTICLES);

    vec2 center = { APP_WINDOW_WIDTH / 2.0f, APP_WINDOW_HEIGHT / 2.0f };
    for (u32 i = 0; i < NUM_PARTICLES; i++) {
        particle_data[i].angle = randfr(0.0f, 6.28f);
        f32 radius = randfr(0.0f, 100.0f);
        particle_data[i].position[0] = center[0] + cosf(particle_data[i].angle) * radius;
        particle_data[i].position[1] = center[1] + sinf(particle_data[i].angle) * radius;
    }

    DVR_RESULT(dvr_buffer)
    storage_buffer_res = dvr_create_buffer(&(dvr_buffer_desc){
        .usage = DVR_BUFFER_USAGE_STORAGE | DVR_BUFFER_USAGE_TRANSFER_DST,
        .data =
            (dvr_range){
                .base = particle_data,
                .size = sizeof(particle) * NUM_PARTICLES,
            },
        .lifecycle = DVR_BUFFER_LIFECYCLE_STATIC,
    });
    DVR_BUBBLE_INTO(dvr_none, storage_buffer_res);
    g_app_state.particle_buffers[0] = DVR_UNWRAP(storage_buffer_res);

    free(particle_data);

    DVR_RESULT(dvr_buffer)
    storage_buffer_res_2 = dvr_create_buffer(&(dvr_buffer_desc){
        .usage = DVR_BUFFER_USAGE_STORAGE | DVR_BUFFER_USAGE_TRANSFER_DST,
        .data = (dvr_range){ .size = sizeof(particle) * NUM_PARTICLES },
        .lifecycle = DVR_BUFFER_LIFECYCLE_STATIC,
    });
    DVR_BUBBLE_INTO(dvr_none, storage_buffer_res_2);
    g_app_state.particle_buffers[1] = DVR_UNWRAP(storage_buffer_res_2);

    DVR_RESULT(dvr_descriptor_set_layout)
    descriptor_set_layout_res =
        dvr_create_descriptor_set_layout(&(dvr_descriptor_set_layout_desc){
            .num_bindings = 4,
            .bindings =
                (dvr_descriptor_set_layout_binding_desc[]){
                    (dvr_descriptor_set_layout_binding_desc){
                        .binding = 0,
                        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                        .stage_flags = VK_SHADER_STAGE_COMPUTE_BIT,
                        .count = 1,
                    },
                    (dvr_descriptor_set_layout_binding_desc){
                        .binding = 1,
                        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                        .stage_flags = VK_SHADER_STAGE_COMPUTE_BIT,
                        .count = 1,
                    },
                    (dvr_descriptor_set_layout_binding_desc){
                        .binding = 2,
                        .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                        .stage_flags = VK_SHADER_STAGE_COMPUTE_BIT,
                        .count = 1,
                    },
                    (dvr_descriptor_set_layout_binding_desc){
                        .binding = 3,
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
        .num_bindings = 4,
        .bindings =
            (dvr_descriptor_set_binding_desc[]){
                (dvr_descriptor_set_binding_desc){
                    .binding = 0,
                    .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    .buffer = {
                        .buffer = g_app_state.particle_buffers[0],
                        .offset = 0,
                        .size = sizeof(particle) * NUM_PARTICLES,
                    },
                },
                (dvr_descriptor_set_binding_desc){
                    .binding = 1,
                    .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    .buffer = {
                        .buffer = g_app_state.particle_buffers[1],
                        .offset = 0,
                        .size = sizeof(particle) * NUM_PARTICLES,
                    },
                },
                (dvr_descriptor_set_binding_desc){
                    .binding = 2,
                    .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    .image = {
                        .image = g_app_state.compute_targets[0],
                        .sampler = g_app_state.sampler,
                    },
                },
                (dvr_descriptor_set_binding_desc){
                    .binding = 3,
                    .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    .image = {
                        .image = g_app_state.compute_targets[1],
                        .sampler = g_app_state.sampler,
                    },
                },
            },
    });
    DVR_BUBBLE_INTO(dvr_none, descriptor_set_res);
    g_app_state.compute_descriptor_sets[0] = DVR_UNWRAP(descriptor_set_res);

    descriptor_set_res = dvr_create_descriptor_set(&(dvr_descriptor_set_desc){
        .layout = g_app_state.compute_descriptor_set_layout,
        .num_bindings = 4,
        .bindings =
            (dvr_descriptor_set_binding_desc[]){
                (dvr_descriptor_set_binding_desc){
                    .binding = 0,
                    .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    .buffer = {
                        .buffer = g_app_state.particle_buffers[1],
                        .offset = 0,
                        .size = sizeof(particle) * NUM_PARTICLES,
                    },
                },
                (dvr_descriptor_set_binding_desc){
                    .binding = 1,
                    .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    .buffer = {
                        .buffer = g_app_state.particle_buffers[0],
                        .offset = 0,
                        .size = sizeof(particle) * NUM_PARTICLES,
                    },
                },
                (dvr_descriptor_set_binding_desc){
                    .binding = 2,
                    .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    .image = {
                        .image = g_app_state.compute_targets[1],
                        .sampler = g_app_state.sampler,
                    },
                },
                (dvr_descriptor_set_binding_desc){
                    .binding = 3,
                    .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    .image = {
                        .image = g_app_state.compute_targets[0],
                        .sampler = g_app_state.sampler,
                    },
                },
            },
    });
    DVR_BUBBLE_INTO(dvr_none, descriptor_set_res);
    g_app_state.compute_descriptor_sets[1] = DVR_UNWRAP(descriptor_set_res);

    DVR_RESULT(dvr_range) compute_spv_res = dvr_read_file("particle_update_cs.spv");
    DVR_BUBBLE_INTO(dvr_none, compute_spv_res);

    dvr_range compute_spv = DVR_UNWRAP(compute_spv_res);

    DVR_RESULT(dvr_shader_module)
    compute_shader_res = dvr_create_shader_module(&(dvr_shader_module_desc){
        .code = compute_spv,
    });
    dvr_free_file(compute_spv);
    DVR_BUBBLE_INTO(dvr_none, compute_shader_res);

    dvr_shader_module particle_update_shader = DVR_UNWRAP(compute_shader_res);

    compute_spv_res = dvr_read_file("diffuse_cs.spv");
    DVR_BUBBLE_INTO(dvr_none, compute_spv_res);

    compute_spv = DVR_UNWRAP(compute_spv_res);

    compute_shader_res = dvr_create_shader_module(&(dvr_shader_module_desc){
        .code = compute_spv,
    });
    dvr_free_file(compute_spv);
    DVR_BUBBLE_INTO(dvr_none, compute_shader_res);

    dvr_shader_module diffuse_shader = DVR_UNWRAP(compute_shader_res);

    DVR_RESULT(dvr_compute_pipeline)
    comp_pipeline_res = dvr_create_compute_pipeline(&(dvr_compute_pipeline_desc){
        .shader_module = particle_update_shader,
        .entry_point = "main",
        .num_desc_set_layouts = 1,
        .desc_set_layouts = &g_app_state.compute_descriptor_set_layout,
        .num_push_constant_ranges = 1,
        .push_constant_ranges =
            (VkPushConstantRange[]){
                (VkPushConstantRange){
                    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                    .offset = 0,
                    .size = sizeof(particle_push_constants),
                },
            },
    });
    DVR_BUBBLE_INTO(dvr_none, comp_pipeline_res);

    g_app_state.particle_update_pipeline = DVR_UNWRAP(comp_pipeline_res);

    comp_pipeline_res = dvr_create_compute_pipeline(&(dvr_compute_pipeline_desc){
        .shader_module = diffuse_shader,
        .entry_point = "main",
        .num_desc_set_layouts = 1,
        .desc_set_layouts = &g_app_state.compute_descriptor_set_layout,
        .num_push_constant_ranges = 1,
        .push_constant_ranges =
            (VkPushConstantRange[]){
                (VkPushConstantRange){
                    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                    .offset = 0,
                    .size = sizeof(diffuse_push_constants),
                },
            },
    });
    DVR_BUBBLE_INTO(dvr_none, comp_pipeline_res);

    g_app_state.diffuse_pipeline = DVR_UNWRAP(comp_pipeline_res);

    dvr_destroy_shader_module(particle_update_shader);
    dvr_destroy_shader_module(diffuse_shader);

    DVR_RESULT(dvr_range) vert_spv_res = dvr_read_file("rt_vs.spv");
    DVR_BUBBLE_INTO(dvr_none, vert_spv_res);

    dvr_range vert_spv = DVR_UNWRAP(vert_spv_res);

    DVR_RESULT(dvr_range) frag_spv_res = dvr_read_file("rt_fs.spv");
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
                            .image = g_app_state.compute_targets[i],
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

static void app_compute(void) {
    dvr_bind_compute_pipeline(g_app_state.diffuse_pipeline);
    dvr_bind_descriptor_set_compute(
        g_app_state.diffuse_pipeline,
        g_app_state.compute_descriptor_sets[(g_app_state.frame_count - 1) % 2]
    );

    dvr_push_constants_compute(
        g_app_state.diffuse_pipeline,
        0,
        (dvr_range){
            .base =
                &(diffuse_push_constants){
                    .delta_time = (f32)g_app_state.delta_time,
                    .blur_strength = g_app_state.blur_strength,
                    .decay = g_app_state.decay,
                },
            .size = sizeof(diffuse_push_constants),
        }
    );

    dvr_dispatch_compute(
        (u32)ceilf((f32)APP_WINDOW_WIDTH / 32.0f),
        (u32)ceilf((f32)APP_WINDOW_HEIGHT / 32.0f),
        1
    );

    VkMemoryBarrier memory_barrier = {
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
        &memory_barrier,
        0,
        NULL,
        0,
        NULL
    );

    dvr_bind_compute_pipeline(g_app_state.particle_update_pipeline);
    dvr_bind_descriptor_set_compute(
        g_app_state.particle_update_pipeline,
        g_app_state.compute_descriptor_sets[(g_app_state.frame_count - 1) % 2]
    );

    dvr_push_constants_compute(
        g_app_state.particle_update_pipeline,
        0,
        (dvr_range){
            .base =
                &(particle_push_constants){
                    .world_size[0] = APP_WINDOW_WIDTH,
                    .world_size[1] = APP_WINDOW_HEIGHT,
                    .num_particles = NUM_PARTICLES,
                    .delta_time = (f32)g_app_state.delta_time,
                    .speed = g_app_state.speed,
                    .turn_speed = g_app_state.turn_speed,
                    .random_steer = g_app_state.random_steer,
                    .sensor_angle = g_app_state.sensor_angle,
                    .sensor_distance = g_app_state.sensor_distance,
                },
            .size = sizeof(particle_push_constants),
        }
    );

    dvr_dispatch_compute(NUM_PARTICLES / 256, 1, 1);
}

static void reset_particles(void) {
    particle* particle_data = malloc(sizeof(particle) * NUM_PARTICLES);

    vec2 center = { APP_WINDOW_WIDTH / 2.0f, APP_WINDOW_HEIGHT / 2.0f };

    for (u32 i = 0; i < NUM_PARTICLES; i++) {
        particle_data[i].angle = randfr(0.0f, 6.28f);
        f32 radius = randfr(0.0f, 100.0f);
        particle_data[i].position[0] = center[0] + cosf(particle_data[i].angle) * radius;
        particle_data[i].position[1] = center[1] + sinf(particle_data[i].angle) * radius;
    }

    DVR_RESULT(dvr_buffer)
    copy_src_res = dvr_create_buffer(&(dvr_buffer_desc){
        .usage = DVR_BUFFER_USAGE_TRANSFER_SRC,
        .data =
            (dvr_range){
                .base = particle_data,
                .size = sizeof(particle) * NUM_PARTICLES,
            },
        .lifecycle = DVR_BUFFER_LIFECYCLE_STATIC,
    });
    DVR_EXIT_ON_ERROR(copy_src_res);

    dvr_buffer copy_src = DVR_UNWRAP(copy_src_res);

    for (u8 i = 0; i < 2; i++) {
        dvr_copy_buffer(copy_src, g_app_state.particle_buffers[i], 0, 0, sizeof(particle) * NUM_PARTICLES);
    }

    dvr_destroy_buffer(copy_src);
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

    if (igCollapsingHeader_TreeNodeFlags("particles", 0)) {
        igSliderFloat("speed", &g_app_state.speed, 0.0f, 200.0f, "%.3f", 1.0f);
        igSliderFloat("turn speed", &g_app_state.turn_speed, 0.0f, 200.0f, "%.3f", 1.0f);
        igSliderFloat("random steer", &g_app_state.random_steer, 0.0f, 200.0f, "%.3f", 1.0f);
        igSliderFloat("sensor angle", &g_app_state.sensor_angle, 0.0f, 6.28f, "%.3f", 1.0f);
        igSliderFloat(
            "sensor distance",
            &g_app_state.sensor_distance,
            0.0f,
            100.0f,
            "%.3f",
            1.0f
        );
    }

    if (igCollapsingHeader_TreeNodeFlags("diffuse", 0)) {
        igSliderFloat("blur strength", &g_app_state.blur_strength, 0.0f, 0.25f, "%.3f", 1.0f);
        igSliderFloat("decay", &g_app_state.decay, 0.0f, 0.025f, "%.4f", 1.0f);
    }

    if (igCollapsingHeader_TreeNodeFlags("render", 0)) {
        igSliderFloat("hue", &g_app_state.hue, 0.0f, 360.0f, "%.1f", 1.0f);
    }

    if (igButton("reset", (ImVec2){ 0, 0 })) {
        reset_particles();
    }

    igEnd();
}

static void app_shutdown(void) {
    dvr_wait_idle();

    for (u8 i = 0; i < 2; i++) {
        dvr_destroy_image(g_app_state.compute_targets[i]);
        dvr_destroy_buffer(g_app_state.particle_buffers[i]);
        dvr_destroy_descriptor_set(g_app_state.compute_descriptor_sets[i]);
        dvr_destroy_descriptor_set(g_app_state.descriptor_sets[i]);
    }
    dvr_destroy_sampler(g_app_state.sampler);
    dvr_destroy_descriptor_set_layout(g_app_state.compute_descriptor_set_layout);
    dvr_destroy_descriptor_set_layout(g_app_state.descriptor_set_layout);
    dvr_destroy_compute_pipeline(g_app_state.particle_update_pipeline);
    dvr_destroy_compute_pipeline(g_app_state.diffuse_pipeline);
    dvr_destroy_pipeline(g_app_state.pipeline);
}
