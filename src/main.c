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
        GLFWwindow* window;
        bool just_resized;
    } window;
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

static DVR_RESULT(dvr_none) dvr_vk_create_render_pass(void) {
    VkAttachmentDescription color_attachment = {
        .format = g_dvr_state.vk.swapchain_format,
        .samples = g_dvr_state.vk.msaa_samples,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };

    VkAttachmentReference color_attachment_ref = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };

    VkAttachmentDescription depth_attachment = {
        .format = VK_FORMAT_D32_SFLOAT,
        .samples = g_dvr_state.vk.msaa_samples,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    };

    VkAttachmentReference depth_attachment_ref = {
        .attachment = 1,
        .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    };

    VkAttachmentDescription color_resolve_attachment = {
        .format = g_dvr_state.vk.swapchain_format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };

    VkAttachmentReference color_resolve_attachment_ref = {
        .attachment = 2,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };

    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_attachment_ref,
        .pDepthStencilAttachment = &depth_attachment_ref,
        .pResolveAttachments = &color_resolve_attachment_ref,
    };

    VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        .srcAccessMask =
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .dstAccessMask =
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
    };

    VkRenderPassCreateInfo render_pass_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 3,
        .pAttachments =
            (VkAttachmentDescription[]){
                color_attachment,
                depth_attachment,
                color_resolve_attachment,
            },
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    };

    if (vkCreateRenderPass(DVR_DEVICE, &render_pass_info, NULL, &g_dvr_state.vk.render_pass) !=
        VK_SUCCESS) {
        return DVR_ERROR(dvr_none, "failed to create render pass");
    }

    return DVR_OK(dvr_none, DVR_NONE);
}

static DVR_RESULT(dvr_none) dvr_vk_create_descriptor_set_layout() {
    VkDescriptorSetLayoutBinding ubo_layout_binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .pImmutableSamplers = NULL,
    };

    VkDescriptorSetLayoutBinding sampler_layout_binding = {
        .binding = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .pImmutableSamplers = NULL,
    };

    VkDescriptorSetLayoutBinding bindings[] = {
        ubo_layout_binding,
        sampler_layout_binding,
    };

    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = bindings,
    };

    if (vkCreateDescriptorSetLayout(
            DVR_DEVICE,
            &layout_info,
            NULL,
            &g_dvr_state.vk.descriptor_set_layout
        ) != VK_SUCCESS) {
        return DVR_ERROR(dvr_none, "failed to create descriptor set layout");
    }

    return DVR_OK(dvr_none, DVR_NONE);
}

static DVR_RESULT(dvr_range) dvr_read_file(const char* path) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        return DVR_ERROR(dvr_range, "failed to open file");
    }

    fseek(file, 0, SEEK_END);
    usize file_size = (usize)ftell(file);
    rewind(file);

    char* buffer = malloc(file_size);
    if (buffer == NULL) {
        fclose(file);
        return DVR_ERROR(dvr_range, "failed to allocate buffer");
    }

    if (fread(buffer, 1, file_size, file) != file_size) {
        fclose(file);
        free(buffer);
        return DVR_ERROR(dvr_range, "failed to read file");
    }

    fclose(file);

    dvr_range range = {
        .base = buffer,
        .size = file_size,
    };

    return DVR_OK(dvr_range, range);
}

DVR_RESULT_DEF(VkShaderModule);

static DVR_RESULT(VkShaderModule) dvr_create_shader_module(dvr_range code) {
    VkShaderModule module = {};

    if (vkCreateShaderModule(
            DVR_DEVICE,
            &(VkShaderModuleCreateInfo){
                .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .codeSize = code.size,
                .pCode = code.base,
            },
            NULL,
            &module
        ) != VK_SUCCESS) {
        return DVR_ERROR(VkShaderModule, "failed to create vertex shader module");
    }

    return DVR_OK(VkShaderModule, module);
}

static DVR_RESULT(dvr_none) dvr_vk_create_graphics_pipeline(void) {
    DVR_RESULT(dvr_range) vs_code_res = dvr_read_file("default_vs.spv");
    DVR_BUBBLE_INTO(dvr_none, vs_code_res);
    dvr_range vs_code = DVR_UNWRAP(vs_code_res);

    DVR_RESULT(dvr_range) fs_code_res = dvr_read_file("default_fs.spv");
    DVR_BUBBLE_INTO(dvr_none, fs_code_res);
    dvr_range fs_code = DVR_UNWRAP(fs_code_res);

    DVR_RESULT(VkShaderModule) vs_shader_module_res = dvr_create_shader_module(vs_code);
    DVR_RESULT(VkShaderModule) fs_shader_module_res = dvr_create_shader_module(fs_code);
    VkShaderModule vs_shader_module = DVR_UNWRAP(vs_shader_module_res);
    VkShaderModule fs_shader_module = DVR_UNWRAP(fs_shader_module_res);

    VkPipelineShaderStageCreateInfo vs_create_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_VERTEX_BIT,
        .module = vs_shader_module,
        .pName = "main",
    };

    VkPipelineShaderStageCreateInfo fs_create_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
        .module = fs_shader_module,
        .pName = "main",
    };

    VkPipelineShaderStageCreateInfo stages[] = {
        vs_create_info,
        fs_create_info,
    };

    VkDynamicState dynamic_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };

    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = (u32)(sizeof(dynamic_states) / sizeof(dynamic_states[0])),
        .pDynamicStates = dynamic_states,
    };

    VkPipelineVertexInputStateCreateInfo vertex_input_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &dvr_vertex_binding_description,
        .vertexAttributeDescriptionCount = sizeof(dvr_vertex_attribute_descriptions) /
                                           sizeof(dvr_vertex_attribute_descriptions[0]),
        .pVertexAttributeDescriptions = dvr_vertex_attribute_descriptions,
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE,
    };

    VkViewport viewport = {
        .x = 0.0f,
        .y = 0.0f,
        .width = (f32)g_dvr_state.vk.swapchain_extent.width,
        .height = (f32)g_dvr_state.vk.swapchain_extent.height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };

    VkRect2D scissor = {
        .offset = { 0, 0 },
        .extent = g_dvr_state.vk.swapchain_extent,
    };

    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
        .pViewports = &viewport,
        .pScissors = &scissor,
    };

    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .lineWidth = 1.0f,
        .cullMode = VK_CULL_MODE_BACK_BIT,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
        .depthBiasConstantFactor = 0.0f,
        .depthBiasClamp = 0.0f,
        .depthBiasSlopeFactor = 0.0f,
    };

    VkPipelineDepthStencilStateCreateInfo depth_stencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
        .depthBoundsTestEnable = VK_FALSE,
        .minDepthBounds = 0.0f,
        .maxDepthBounds = 1.0f,
        .stencilTestEnable = VK_FALSE,
    };

    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .sampleShadingEnable = VK_FALSE,
        .rasterizationSamples = g_dvr_state.vk.msaa_samples,
        .minSampleShading = 1.0f,
        .pSampleMask = NULL,
        .alphaToCoverageEnable = VK_FALSE,
        .alphaToOneEnable = VK_FALSE,
    };

    VkPipelineColorBlendAttachmentState color_blend_attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD,
    };

    VkPipelineColorBlendStateCreateInfo color_blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = 1,
        .pAttachments = &color_blend_attachment,
        .blendConstants = { 0.0f, 0.0f, 0.0f, 0.0f },
    };

    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &g_dvr_state.vk.descriptor_set_layout,
        .pushConstantRangeCount = 0,
        .pPushConstantRanges = NULL,
    };

    if (vkCreatePipelineLayout(
            DVR_DEVICE,
            &pipeline_layout_info,
            NULL,
            &g_dvr_state.vk.pipeline_layout
        ) != VK_SUCCESS) {
        return DVR_ERROR(dvr_none, "failed to create pipeline layout");
    }

    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vertex_input_info,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pDepthStencilState = &depth_stencil,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state,
        .pMultisampleState = &multisampling,
        .layout = g_dvr_state.vk.pipeline_layout,
        .renderPass = g_dvr_state.vk.render_pass,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = -1,
    };

    if (vkCreateGraphicsPipelines(
            DVR_DEVICE,
            VK_NULL_HANDLE,
            1,
            &pipeline_info,
            NULL,
            &g_dvr_state.vk.pipeline
        ) != VK_SUCCESS) {
        return DVR_ERROR(dvr_none, "failed to create graphics pipeline");
    }

    vkDestroyShaderModule(DVR_DEVICE, vs_shader_module, NULL);
    vkDestroyShaderModule(DVR_DEVICE, fs_shader_module, NULL);

    return DVR_OK(dvr_none, DVR_NONE);
}

static DVR_RESULT(dvr_none) dvr_vk_create_framebuffers(void) {
    arrsetlen(
        g_dvr_state.vk.swapchain_framebuffers,
        arrlen(g_dvr_state.vk.swapchain_image_views)
    );

    for (usize i = 0; i < arrlenu(g_dvr_state.vk.swapchain_image_views); i++) {
        VkImageView attachments[] = {
            g_dvr_state.vk.render_image.vk.view,
            g_dvr_state.vk.depth_image.vk.view,
            g_dvr_state.vk.swapchain_image_views[i],
        };

        VkFramebufferCreateInfo framebuffer_info = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = g_dvr_state.vk.render_pass,
            .attachmentCount = 3,
            .pAttachments = attachments,
            .width = g_dvr_state.vk.swapchain_extent.width,
            .height = g_dvr_state.vk.swapchain_extent.height,
            .layers = 1,
        };

        if (vkCreateFramebuffer(
                DVR_DEVICE,
                &framebuffer_info,
                NULL,
                &g_dvr_state.vk.swapchain_framebuffers[i]
            ) != VK_SUCCESS) {
            return DVR_ERROR(dvr_none, "failed to create framebuffer");
        }
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

static DVR_RESULT(dvr_none) dvr_vk_create_render_image(void) {
    i32 depth_width, depth_height;
    glfwGetFramebufferSize(g_dvr_state.window.window, &depth_width, &depth_height);

    DVR_RESULT(dvr_image)
    image_res = dvr_create_image(&(dvr_image_desc){
        .width = (u32)depth_width,
        .height = (u32)depth_height,
        .num_samples = g_dvr_state.vk.msaa_samples,
        .format = g_dvr_state.vk.swapchain_format,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .properties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
    });
    DVR_BUBBLE_INTO(dvr_none, image_res);

    dvr_image image = DVR_UNWRAP(image_res);

    dvr_vk_transition_image_layout(
        image.vk.image,
        g_dvr_state.vk.swapchain_format,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        1
    );

    g_dvr_state.vk.render_image = image;

    return DVR_OK(dvr_none, DVR_NONE);
}

static DVR_RESULT(dvr_none) dvr_vk_create_depth_image(void) {
    i32 depth_width, depth_height;
    glfwGetFramebufferSize(g_dvr_state.window.window, &depth_width, &depth_height);

    DVR_RESULT(dvr_image)
    image_res = dvr_create_image(&(dvr_image_desc){
        .width = (u32)depth_width,
        .height = (u32)depth_height,
        .num_samples = g_dvr_state.vk.msaa_samples,
        .format = VK_FORMAT_D32_SFLOAT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .properties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
    });
    DVR_BUBBLE_INTO(dvr_none, image_res);

    dvr_image image = DVR_UNWRAP(image_res);

    dvr_vk_transition_image_layout(
        image.vk.image,
        VK_FORMAT_D32_SFLOAT,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        1
    );

    g_dvr_state.vk.depth_image = image;

    return DVR_OK(dvr_none, DVR_NONE);
}

static DVR_RESULT(dvr_none) dvr_vk_create_texture_image(void) {
    i32 tex_width, tex_height, tex_channels;
    u8* pixels =
        stbi_load("texture.png", &tex_width, &tex_height, &tex_channels, STBI_rgb_alpha);

    VkDeviceSize image_size = (usize)tex_width * (usize)tex_height * 4;

    if (pixels == NULL) {
        return DVR_ERROR(dvr_none, "failed to load texture image");
    }

    VkBuffer staging_buffer;
    VkDeviceMemory staging_buffer_memory;

    DVR_RESULT(dvr_none)
    result = dvr_vk_create_buffer(
        image_size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &staging_buffer,
        &staging_buffer_memory
    );
    DVR_BUBBLE(result);

    void* data;
    vkMapMemory(DVR_DEVICE, staging_buffer_memory, 0, image_size, 0, &data);
    memcpy(data, pixels, (usize)image_size);
    vkUnmapMemory(DVR_DEVICE, staging_buffer_memory);

    stbi_image_free(pixels);

    DVR_RESULT(dvr_image)
    image_res = dvr_create_image(&(dvr_image_desc){
        .width = (u32)tex_width,
        .height = (u32)tex_height,
        .generate_mipmaps = true,
        .format = VK_FORMAT_R8G8B8A8_SRGB,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT,
        .properties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
    });
    DVR_BUBBLE_INTO(dvr_none, image_res);

    dvr_image image = DVR_UNWRAP(image_res);

    g_dvr_state.vk.texture_image = image;

    return DVR_OK(dvr_none, DVR_NONE);
}

static DVR_RESULT(dvr_none) dvr_vk_create_texture_sampler(void) {
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(g_dvr_state.vk.physical_device, &properties);

    VkSamplerCreateInfo sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .anisotropyEnable = VK_TRUE,
        .maxAnisotropy = properties.limits.maxSamplerAnisotropy,
        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .mipLodBias = 0.0f,
        .minLod = 0.0f,
        .maxLod = VK_LOD_CLAMP_NONE,
    };

    if (vkCreateSampler(DVR_DEVICE, &sampler_info, NULL, &g_dvr_state.vk.sampler) !=
        VK_SUCCESS) {
        return DVR_ERROR(dvr_none, "failed to create texture sampler");
    }

    return DVR_OK(dvr_none, DVR_NONE);
}

static DVR_RESULT(dvr_none) dvr_vk_create_mesh(void) {
    const struct aiScene* scene = aiImportFile(
        "viking_room.obj",
        aiProcess_Triangulate | aiProcess_GenNormals | aiProcess_FlipUVs |
            aiProcess_FlipWindingOrder
    );

    if (!scene) {
        return DVR_ERROR(dvr_none, "failed to load mesh");
    }

    if (scene->mNumMeshes != 1) {
        aiReleaseImport(scene);
        return DVR_ERROR(dvr_none, "more than one mesh");
    }

    const struct aiMesh* mesh = scene->mMeshes[0];

    usize vertex_count = mesh->mNumVertices;
    usize index_count = mesh->mNumFaces * 3;

    usize vertex_size = vertex_count * sizeof(dvr_vertex);
    dvr_vertex* vertex_data = malloc(vertex_size);
    usize index_size = index_count * sizeof(u32);
    u32* index_data = malloc(index_size);

    for (usize i = 0; i < vertex_count; i++) {
        vertex_data[i] = (dvr_vertex) {
            .pos = {
                mesh->mVertices[i].x,
                -mesh->mVertices[i].y,
                mesh->mVertices[i].z,
            },
            .color = { 1.0f, 1.0f, 1.0f },
            .uv = {
                mesh->mTextureCoords[0][i].x,
                mesh->mTextureCoords[0][i].y,
            },
        };
    }

    for (usize i = 0; i < mesh->mNumFaces; i++) {
        index_data[i * 3 + 0] = (u16)mesh->mFaces[i].mIndices[0];
        index_data[i * 3 + 1] = (u16)mesh->mFaces[i].mIndices[1];
        index_data[i * 3 + 2] = (u16)mesh->mFaces[i].mIndices[2];
    }

    DVRLOG_DEBUG("total vertices: %zu", vertex_count);
    DVRLOG_DEBUG("total indices: %zu", index_count);

    DVR_RESULT(dvr_buffer)
    vbuf = dvr_create_buffer(&(dvr_buffer_desc){
        .data = (dvr_range){ .base = vertex_data, .size = vertex_size },
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .lifecycle = DVR_BUFFER_LIFECYCLE_STATIC,
    });
    DVR_BUBBLE_INTO(dvr_none, vbuf);

    DVR_RESULT(dvr_buffer)
    ibuf = dvr_create_buffer(&(dvr_buffer_desc){
        .data = (dvr_range){ .base = index_data, .size = index_size },
        .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        .lifecycle = DVR_BUFFER_LIFECYCLE_STATIC,
    });
    DVR_BUBBLE_INTO(dvr_none, ibuf);

    g_dvr_state.vk.index_count = (u32)index_count;

    free(vertex_data);
    free(index_data);
    aiReleaseImport(scene);

    g_dvr_state.vk.vertex_buffer = DVR_UNWRAP(vbuf);
    g_dvr_state.vk.index_buffer = DVR_UNWRAP(ibuf);

    return DVR_OK(dvr_none, DVR_NONE);
}

static DVR_RESULT(dvr_none) dvr_vk_create_uniform_buffers(void) {
    for (usize i = 0; i < DVR_MAX_FRAMES_IN_FLIGHT; i++) {
        DVR_RESULT(dvr_buffer)
        ibuf = dvr_create_buffer(&(dvr_buffer_desc){
            .data =
                (dvr_range){
                    .base = NULL,
                    .size = sizeof(dvr_view_uniform),
                },
            .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            .lifecycle = DVR_BUFFER_LIFECYCLE_DYNAMIC,
        });
        DVR_BUBBLE_INTO(dvr_none, ibuf);

        g_dvr_state.vk.uniform_buffers[i] = DVR_UNWRAP(ibuf);
    }
    return DVR_OK(dvr_none, DVR_NONE);
}

static DVR_RESULT(dvr_none) dvr_vk_create_descriptor_pool(void) {
    VkDescriptorPoolSize ubo_size = {
        .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = DVR_MAX_FRAMES_IN_FLIGHT,
    };

    VkDescriptorPoolSize image_sampler_size = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = DVR_MAX_FRAMES_IN_FLIGHT,
    };

    VkDescriptorPoolSize pool_sizes[] = { ubo_size, image_sampler_size };

    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .poolSizeCount = 2,
        .pPoolSizes = pool_sizes,
        .maxSets = DVR_MAX_FRAMES_IN_FLIGHT,
    };

    if (vkCreateDescriptorPool(DVR_DEVICE, &pool_info, NULL, &g_dvr_state.vk.descriptor_pool) !=
        VK_SUCCESS) {
        return DVR_ERROR(dvr_none, "failed to create descriptor pool");
    }

    return DVR_OK(dvr_none, DVR_NONE);
}

static DVR_RESULT(dvr_none) dvr_vk_create_descriptor_sets(void) {
    VkDescriptorSetLayout layouts[DVR_MAX_FRAMES_IN_FLIGHT];
    for (usize i = 0; i < DVR_MAX_FRAMES_IN_FLIGHT; i++) {
        layouts[i] = g_dvr_state.vk.descriptor_set_layout;
    }

    VkDescriptorSetAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = g_dvr_state.vk.descriptor_pool,
        .descriptorSetCount = DVR_MAX_FRAMES_IN_FLIGHT,
        .pSetLayouts = layouts,
    };

    if (vkAllocateDescriptorSets(DVR_DEVICE, &alloc_info, g_dvr_state.vk.descriptor_sets) !=
        VK_SUCCESS) {
        return DVR_ERROR(dvr_none, "failed to allocate descriptor sets");
    }

    for (usize i = 0; i < DVR_MAX_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorBufferInfo buffer_info = {
            .buffer = g_dvr_state.vk.uniform_buffers[i].vk.buffer,
            .offset = 0,
            .range = sizeof(dvr_view_uniform),
        };

        VkDescriptorImageInfo image_info = {
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .imageView = g_dvr_state.vk.texture_image.vk.view,
            .sampler = g_dvr_state.vk.sampler,
        };

        VkWriteDescriptorSet descriptor_writes[2] = {
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = g_dvr_state.vk.descriptor_sets[i],
                .dstBinding = 0,
                .dstArrayElement = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .descriptorCount = 1,
                .pBufferInfo = &buffer_info,
                .pImageInfo = NULL,
                .pTexelBufferView = NULL,
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = g_dvr_state.vk.descriptor_sets[i],
                .dstBinding = 1,
                .dstArrayElement = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .pBufferInfo = NULL,
                .pImageInfo = &image_info,
                .pTexelBufferView = NULL,
            },
        };

        vkUpdateDescriptorSets(DVR_DEVICE, 2, descriptor_writes, 0, NULL);
    }

    return DVR_OK(dvr_none, DVR_NONE);
}

static DVR_RESULT(dvr_none) dvr_vk_create_command_buffer(void) {
    VkCommandBufferAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = g_dvr_state.vk.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = DVR_MAX_FRAMES_IN_FLIGHT,
    };

    if (vkAllocateCommandBuffers(DVR_DEVICE, &alloc_info, g_dvr_state.vk.command_buffers) !=
        VK_SUCCESS) {
        return DVR_ERROR(dvr_none, "failed to allocate command buffers");
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

    for (usize i = 0; i < DVR_MAX_FRAMES_IN_FLIGHT; i++) {
        if (vkCreateSemaphore(
                DVR_DEVICE,
                &sem_info,
                nullptr,
                &g_dvr_state.vk.image_available_sems[i]
            ) != VK_SUCCESS ||
            vkCreateSemaphore(
                DVR_DEVICE,
                &sem_info,
                nullptr,
                &g_dvr_state.vk.render_finished_sems[i]
            ) != VK_SUCCESS ||
            vkCreateFence(
                DVR_DEVICE,
                &fence_info,
                nullptr,
                &g_dvr_state.vk.in_flight_fences[i]
            ) != VK_SUCCESS) {
            return DVR_ERROR(dvr_none, "failed to create sync objects");
        }
    }

    return DVR_OK(dvr_none, DVR_NONE);
}

static DVR_RESULT(dvr_none) dvr_init_vulkan(void) {
    DVR_RESULT(dvr_none) result = dvr_vk_create_instance();
    DVR_BUBBLE(result);

    dvr_vk_create_debug_messenger();

    result = dvr_vk_create_surface();
    DVR_BUBBLE(result);

    result = dvr_vk_pick_physical_device();
    DVR_BUBBLE(result);

    result = dvr_vk_create_logical_device();
    DVR_BUBBLE(result);

    result = dvr_vk_create_swapchain();
    DVR_BUBBLE(result);

    result = dvr_vk_create_swapchain_image_views();
    DVR_BUBBLE(result);

    result = dvr_vk_create_render_pass();
    DVR_BUBBLE(result);

    result = dvr_vk_create_descriptor_set_layout();
    DVR_BUBBLE(result);

    result = dvr_vk_create_graphics_pipeline();
    DVR_BUBBLE(result);

    result = dvr_vk_create_command_pool();
    DVR_BUBBLE(result);

    result = dvr_vk_create_render_image();
    DVR_BUBBLE(result);

    result = dvr_vk_create_depth_image();
    DVR_BUBBLE(result);

    result = dvr_vk_create_framebuffers();
    DVR_BUBBLE(result);

    result = dvr_vk_create_texture_image();
    DVR_BUBBLE(result);

    result = dvr_vk_create_texture_sampler();
    DVR_BUBBLE(result);

    result = dvr_vk_create_mesh();
    DVR_BUBBLE(result);

    result = dvr_vk_create_uniform_buffers();
    DVR_BUBBLE(result);

    result = dvr_vk_create_descriptor_pool();
    DVR_BUBBLE(result);

    result = dvr_vk_create_descriptor_sets();
    DVR_BUBBLE(result);

    result = dvr_vk_create_command_buffer();
    DVR_BUBBLE(result);

    result = dvr_vk_create_sync_objects();
    DVR_BUBBLE(result);

    return DVR_OK(dvr_none, DVR_NONE);
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
    vkWaitForFences(
        DVR_DEVICE,
        1,
        &g_dvr_state.vk.in_flight_fences[g_dvr_state.vk.current_frame],
        VK_TRUE,
        UINT64_MAX
    );

    u32 image_index;
    VkResult result = vkAcquireNextImageKHR(
        DVR_DEVICE,
        g_dvr_state.vk.swapchain,
        UINT64_MAX,
        g_dvr_state.vk.image_available_sems[g_dvr_state.vk.current_frame],
        VK_NULL_HANDLE,
        &image_index
    );

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        DVR_RESULT(dvr_none) res = dvr_vk_recreate_swapchain();
        DVR_BUBBLE(res);
        return DVR_OK(dvr_none, DVR_NONE);
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        return DVR_ERROR(dvr_none, "failed to acquire swapchain image");
    }

    vkResetFences(
        DVR_DEVICE,
        1,
        &g_dvr_state.vk.in_flight_fences[g_dvr_state.vk.current_frame]
    );

    vkResetCommandBuffer(g_dvr_state.vk.command_buffers[g_dvr_state.vk.current_frame], 0);

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

    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pWaitDstStageMask =
            (VkPipelineStageFlags[]){ VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT },
        .commandBufferCount = 1,
        .pCommandBuffers = &g_dvr_state.vk.command_buffers[g_dvr_state.vk.current_frame],
        .waitSemaphoreCount = 1,
        .pWaitSemaphores =
            (VkSemaphore[]){
                g_dvr_state.vk.image_available_sems[g_dvr_state.vk.current_frame],
            },
        .signalSemaphoreCount = 1,
        .pSignalSemaphores =
            (VkSemaphore[]){
                g_dvr_state.vk.render_finished_sems[g_dvr_state.vk.current_frame],
            },
    };

    if (vkQueueSubmit(
            g_dvr_state.vk.graphics_queue,
            1,
            &submit_info,
            g_dvr_state.vk.in_flight_fences[g_dvr_state.vk.current_frame]
        ) != VK_SUCCESS) {
        return DVR_ERROR(dvr_none, "failed to submit draw command buffer");
    }

    VkPresentInfoKHR present_info = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores =
            (VkSemaphore[]){
                g_dvr_state.vk.render_finished_sems[g_dvr_state.vk.current_frame],
            },
        .swapchainCount = 1,
        .pSwapchains =
            (VkSwapchainKHR[]){
                g_dvr_state.vk.swapchain,
            },
        .pImageIndices = &image_index,
        .pResults = NULL
    };

    result = vkQueuePresentKHR(g_dvr_state.vk.present_queue, &present_info);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
        g_dvr_state.window.just_resized) {
        g_dvr_state.window.just_resized = false;
        DVR_RESULT(dvr_none) res = dvr_vk_recreate_swapchain();
        DVR_BUBBLE(res);
        return DVR_OK(dvr_none, DVR_NONE);
    } else if (result != VK_SUCCESS) {
        return DVR_ERROR(dvr_none, "failed to present swapchain image");
    }

    g_dvr_state.vk.current_frame =
        (g_dvr_state.vk.current_frame + 1) % DVR_MAX_FRAMES_IN_FLIGHT;

    return DVR_OK(dvr_none, DVR_NONE);
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
