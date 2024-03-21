#include "dvr.h"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

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

DVR_RESULT(dvr_none) dvr_init(void);
void dvr_main_loop(void);
void dvr_cleanup(void);

void dvr_term_handler(int signum);

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

int main(void) {
    signal(SIGTERM, dvr_term_handler);
    signal(SIGINT, dvr_term_handler);

    char executable_directory[1024];
    get_executable_directory(executable_directory, sizeof(executable_directory));
    set_executable_directory(executable_directory);

    DVR_RESULT(dvr_none) result = dvr_init();
    DVR_EXIT_ON_ERROR(result);

    dvr_main_loop();
    dvr_cleanup();

    return 0;
}

#define DVR_WINDOW_WIDTH 640
#define DVR_WINDOW_HEIGHT 480
#ifdef RELEASE
#define DVR_WINDOW_NAME PROJECT_NAME
#else
#define DVR_WINDOW_NAME "dev: " PROJECT_NAME
#endif

typedef struct dvr_state {
    struct {
        u32 current_frame;
        dvr_buffer vertex_buffer;
        dvr_buffer index_buffer;
        dvr_buffer uniform_buffers[DVR_MAX_FRAMES_IN_FLIGHT];
        VkDescriptorSet descriptor_sets[DVR_MAX_FRAMES_IN_FLIGHT];
        dvr_image texture_image;
        dvr_image render_image;
        dvr_image depth_image;
        VkSampler sampler;
        u32 index_count;
    } dvr;
    struct {
        f32 total;
        f32 delta;
        u64 frames;
        u64 frames_in_second;

        f64 start;
        f64 last;
    } time;
} dvr_state;

static dvr_state g_dvr_state;

#define DVR_DEVICE g_dvr_state.vk.device

typedef struct dvr_vertex {
    vec3 pos;
    vec3 color;
    vec2 uv;
} dvr_vertex;

static const VkVertexInputBindingDescription dvr_vertex_binding_description = {
    .binding = 0,
    .stride = sizeof(dvr_vertex),
    .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
};

static const VkVertexInputAttributeDescription dvr_vertex_attribute_descriptions[] = {
    {
        .location = 0,
        .binding = 0,
        .format = VK_FORMAT_R32G32B32_SFLOAT,
        .offset = offsetof(dvr_vertex, pos),
    },
    {
        .location = 1,
        .binding = 0,
        .format = VK_FORMAT_R32G32B32_SFLOAT,
        .offset = offsetof(dvr_vertex, color),
    },
    {
        .location = 2,
        .binding = 0,
        .format = VK_FORMAT_R32G32_SFLOAT,
        .offset = offsetof(dvr_vertex, uv),
    },
};

typedef struct dvr_view_uniform {
    mat4 model;
    mat4 view;
    mat4 proj;
} dvr_view_uniform;



// DVR LIFECYCLE FUNCTIONS

void dvr_term_handler(int signum) {
    (void)signum;
    glfwSetWindowShouldClose(g_dvr_state.window.window, GLFW_TRUE);
}


DVR_RESULT(dvr_none) dvr_init(void) {
    dvr_log_init();

    DVRLOG_INFO("initializing %s", PROJECT_NAME);
    DVRLOG_INFO("version: %s", PROJECT_VERSION);

    memset(&g_dvr_state, 0, sizeof(g_dvr_state));

    DVR_RESULT(dvr_none) result = dvr_init_window();
    DVR_BUBBLE(result);

    DVRLOG_INFO("window initialized");

    result = dvr_init_vulkan();
    DVR_BUBBLE(result);

    DVRLOG_INFO("vulkan initialized");

#ifdef __linux__
    // set the starting time for the frame timer
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    g_dvr_state.time.start = (f64)ts.tv_sec + (f64)ts.tv_nsec / 1e9;
    g_dvr_state.time.last = g_dvr_state.time.start;
#endif

    return DVR_OK(dvr_none, DVR_NONE);
}

static void dvr_vk_cleanup_swapchain(void);

static DVR_RESULT(dvr_none) dvr_vk_recreate_swapchain(void) {
    i32 width = 0;
    i32 height = 0;
    glfwGetFramebufferSize(g_dvr_state.window.window, &width, &height);
    while (width == 0 || height == 0) {
        glfwGetFramebufferSize(g_dvr_state.window.window, &width, &height);
        glfwWaitEvents();
    }

    vkDeviceWaitIdle(DVR_DEVICE);

    VkFormat old_format = g_dvr_state.vk.swapchain_format;

    dvr_vk_cleanup_swapchain();

    DVR_RESULT(dvr_none) result = dvr_vk_create_swapchain();
    DVR_BUBBLE(result);

    bool swapchain_format_changed = old_format != g_dvr_state.vk.swapchain_format;

    result = dvr_vk_create_swapchain_image_views();
    DVR_BUBBLE(result);

    if (swapchain_format_changed) {
        vkDestroyRenderPass(DVR_DEVICE, g_dvr_state.vk.render_pass, NULL);

        result = dvr_vk_create_render_pass();
        DVR_BUBBLE(result);

        vkDestroyPipeline(DVR_DEVICE, g_dvr_state.vk.pipeline, NULL);
        vkDestroyPipelineLayout(DVR_DEVICE, g_dvr_state.vk.pipeline_layout, NULL);

        result = dvr_vk_create_graphics_pipeline();
        DVR_BUBBLE(result);
    }

    result = dvr_vk_create_render_image();
    DVR_BUBBLE(result);

    result = dvr_vk_create_depth_image();
    DVR_BUBBLE(result);

    result = dvr_vk_create_framebuffers();
    DVR_BUBBLE(result);

    return DVR_OK(dvr_none, DVR_NONE);
}

static DVR_RESULT(dvr_none)
    dvr_vk_record_command_buffer(VkCommandBuffer command_buffer, u32 image_index) {
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = 0,
        .pInheritanceInfo = NULL,
    };

    if (vkBeginCommandBuffer(command_buffer, &begin_info) != VK_SUCCESS) {
        return DVR_ERROR(dvr_none, "failed to begin recording command buffer");
    }

    VkRenderPassBeginInfo render_pass_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = g_dvr_state.vk.render_pass,
        .framebuffer = g_dvr_state.vk.swapchain_framebuffers[image_index],
        .renderArea = {
            .offset = { 0, 0 },
            .extent = g_dvr_state.vk.swapchain_extent,
        },
        .clearValueCount = 2,
        .pClearValues = (VkClearValue[]) {
            {
                .color = {
                    .float32 = { 0.0f, 0.0f, 0.0f, 1.0f },
                },
            },
            {
                .depthStencil = { 1.0f, 0 },
            },
        },
    };

    vkCmdBeginRenderPass(command_buffer, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, g_dvr_state.vk.pipeline);
    vkCmdSetViewport(
        command_buffer,
        0,
        1,
        &(VkViewport){
            .x = 0.0f,
            .y = 0.0f,
            .width = (f32)g_dvr_state.vk.swapchain_extent.width,
            .height = (f32)g_dvr_state.vk.swapchain_extent.height,
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        }
    );

    vkCmdSetScissor(
        command_buffer,
        0,
        1,
        &(VkRect2D){
            .offset = { 0, 0 },
            .extent = g_dvr_state.vk.swapchain_extent,
        }
    );

    VkBuffer vertex_buffers[] = { g_dvr_state.vk.vertex_buffer.vk.buffer };
    VkDeviceSize offsets[] = { 0 };
    vkCmdBindVertexBuffers(command_buffer, 0, 1, vertex_buffers, offsets);
    vkCmdBindIndexBuffer(
        command_buffer,
        g_dvr_state.vk.index_buffer.vk.buffer,
        0,
        VK_INDEX_TYPE_UINT32
    );

    vkCmdBindDescriptorSets(
        command_buffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        g_dvr_state.vk.pipeline_layout,
        0,
        1,
        &g_dvr_state.vk.descriptor_sets[g_dvr_state.vk.current_frame],
        0,
        NULL
    );
    vkCmdDrawIndexed(command_buffer, g_dvr_state.vk.index_count, 1, 0, 0, 0);

    vkCmdEndRenderPass(command_buffer);

    if (vkEndCommandBuffer(command_buffer) != VK_SUCCESS) {
        return DVR_ERROR(dvr_none, "failed to record command buffer");
    }

    return DVR_OK(dvr_none, DVR_NONE);
}

static DVR_RESULT(dvr_none) dvr_draw_frame(void) {

    DVR_RESULT(dvr_none)
    res = dvr_vk_record_command_buffer(
        g_dvr_state.vk.command_buffers[g_dvr_state.vk.current_frame],
        image_index
    );
    DVR_BUBBLE(res);

#define RAD_TO_DEG 57.2957795131f
#define DEG_TO_RAD 0.0174532925f

    i32 width, height;
    glfwGetFramebufferSize(g_dvr_state.window.window, &width, &height);
    dvr_view_uniform uniform = {};
    glm_perspective(
        50.0f * DEG_TO_RAD,
        (f32)width / (f32)height,
        0.0001f,
        1000.0f,
        uniform.proj
    );
    // uniform.proj[1][1] = -1;
    glm_mat4_identity(uniform.view);
    glm_translate_z(uniform.view, -2.0f);
    glm_mat4_identity(uniform.model);
    glm_translate_y(uniform.view, 0.5f);
    glm_rotate_y(uniform.model, -2.0f, uniform.model);

    dvr_write_buffer(
        g_dvr_state.vk.uniform_buffers[g_dvr_state.vk.current_frame],
        DVR_RANGE(uniform)
    );

}

void dvr_main_loop(void) {
    while (!glfwWindowShouldClose(g_dvr_state.window.window)) {
        glfwPollEvents();
        DVR_RESULT(dvr_none) res = dvr_draw_frame();

        if (DVR_RESULT_IS_ERROR(res)) {
            DVR_SHOW_ERROR(res);
            glfwSetWindowShouldClose(g_dvr_state.window.window, GLFW_TRUE);
        }

        f64 last_total = g_dvr_state.time.total;
#ifdef __linux__
        // update the time for the frame timer
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);

        g_dvr_state.time.delta =
            (f32)((f64)ts.tv_sec + (f64)ts.tv_nsec / 1e9 - g_dvr_state.time.last);
        g_dvr_state.time.last = (f64)ts.tv_sec + (f64)ts.tv_nsec / 1e9;
        g_dvr_state.time.total =
            (f32)((f64)ts.tv_sec + (f64)ts.tv_nsec / 1e9 - g_dvr_state.time.start);
#endif

        g_dvr_state.time.frames++;
        g_dvr_state.time.frames_in_second++;

        if ((i32)last_total != (i32)g_dvr_state.time.total) {
            DVRLOG_INFO("fps: %zu", g_dvr_state.time.frames_in_second);
            g_dvr_state.time.frames_in_second = 0;
        }
    }

    vkDeviceWaitIdle(DVR_DEVICE);
}

static void dvr_vk_cleanup_swapchain(void) {
    for (usize i = 0; i < arrlenu(g_dvr_state.vk.swapchain_framebuffers); i++) {
        vkDestroyFramebuffer(DVR_DEVICE, g_dvr_state.vk.swapchain_framebuffers[i], NULL);
    }
    for (usize i = 0; i < arrlenu(g_dvr_state.vk.swapchain_image_views); i++) {
        vkDestroyImageView(DVR_DEVICE, g_dvr_state.vk.swapchain_image_views[i], NULL);
    }
    dvr_destroy_image(g_dvr_state.vk.depth_image);
    dvr_destroy_image(g_dvr_state.vk.render_image);
    vkDestroySwapchainKHR(DVR_DEVICE, g_dvr_state.vk.swapchain, NULL);
}

void dvr_cleanup_vulkan(void) {
    dvr_vk_cleanup_swapchain();

    dvr_destroy_buffer(g_dvr_state.vk.vertex_buffer);
    dvr_destroy_buffer(g_dvr_state.vk.index_buffer);
    dvr_destroy_image(g_dvr_state.vk.texture_image);
    vkDestroySampler(DVR_DEVICE, g_dvr_state.vk.sampler, NULL);

    for (usize i = 0; i < DVR_MAX_FRAMES_IN_FLIGHT; i++) {
        dvr_destroy_buffer(g_dvr_state.vk.uniform_buffers[i]);
    }

    vkDestroyDescriptorSetLayout(DVR_DEVICE, g_dvr_state.vk.descriptor_set_layout, NULL);
    vkDestroyDescriptorPool(DVR_DEVICE, g_dvr_state.vk.descriptor_pool, NULL);

    for (usize i = 0; i < DVR_MAX_FRAMES_IN_FLIGHT; i++) {
        vkDestroySemaphore(DVR_DEVICE, g_dvr_state.vk.image_available_sems[i], NULL);
        vkDestroySemaphore(DVR_DEVICE, g_dvr_state.vk.render_finished_sems[i], NULL);
        vkDestroyFence(DVR_DEVICE, g_dvr_state.vk.in_flight_fences[i], NULL);
    }
    vkDestroyCommandPool(DVR_DEVICE, g_dvr_state.vk.command_pool, NULL);

    vkDestroyPipeline(DVR_DEVICE, g_dvr_state.vk.pipeline, NULL);
    vkDestroyRenderPass(DVR_DEVICE, g_dvr_state.vk.render_pass, NULL);
    vkDestroyPipelineLayout(DVR_DEVICE, g_dvr_state.vk.pipeline_layout, NULL);
    vkDestroySurfaceKHR(g_dvr_state.vk.instance, g_dvr_state.vk.surface, NULL);
    if (DVR_ENABLE_VALIDATION_LAYERS) {
        PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT =
            (PFN_vkDestroyDebugUtilsMessengerEXT
            )vkGetInstanceProcAddr(g_dvr_state.vk.instance, "vkDestroyDebugUtilsMessengerEXT");
        if (vkDestroyDebugUtilsMessengerEXT == NULL) {
            DVRLOG_ERROR("vkDestroyDebugUtilsMessengerEXT is NULL");
            return;
        }
        vkDestroyDebugUtilsMessengerEXT(
            g_dvr_state.vk.instance,
            g_dvr_state.vk.debug_messenger,
            NULL
        );
    }
    vkDestroyDevice(DVR_DEVICE, NULL);
    vkDestroyInstance(g_dvr_state.vk.instance, NULL);
    DVRLOG_INFO("vulkan instance destroyed");
}

void dvr_cleanup_window(void) {
    glfwDestroyWindow(g_dvr_state.window.window);
    DVRLOG_INFO("window destroyed");

    glfwTerminate();
    DVRLOG_INFO("glfw terminated");
}

void dvr_cleanup(void) {
    DVRLOG_INFO("cleaning up");
    dvr_cleanup_vulkan();
    dvr_cleanup_window();
    dvr_log_close();
}
