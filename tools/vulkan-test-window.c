#define _POSIX_C_SOURCE 200809L
#define VK_USE_PLATFORM_XLIB_KHR

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <vulkan/vulkan.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct application {
    Display* display;
    Window window;
    Atom wm_delete;
    int width;
    int height;
    int resized;
    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice physical_device;
    uint32_t queue_family;
    VkDevice device;
    VkQueue queue;
    VkSwapchainKHR swapchain;
    VkFormat swapchain_format;
    VkExtent2D swapchain_extent;
    uint32_t image_count;
    VkImage* images;
    unsigned char* image_initialized;
    VkCommandPool command_pool;
    VkCommandBuffer command_buffer;
    VkSemaphore image_available;
    VkSemaphore render_finished;
    VkFence frame_finished;
};

static int environment_dimension(const char* name, int fallback)
{
    const char* text = getenv(name);
    if (text == NULL || *text == '\0') {
        return fallback;
    }
    char* end = NULL;
    const long value = strtol(text, &end, 10);
    if (*end != '\0' || value < 64 || value > 4096) {
        return fallback;
    }
    return (int)value;
}

static int vk_success(VkResult result, const char* action)
{
    if (result == VK_SUCCESS) {
        return 1;
    }
    fprintf(stderr, "%s failed with VkResult %d.\n", action, (int)result);
    return 0;
}

static uint32_t clamp_u32(uint32_t value, uint32_t minimum, uint32_t maximum)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

static int create_window(struct application* app, const char* title)
{
    app->display = XOpenDisplay(NULL);
    if (app->display == NULL) {
        fprintf(stderr, "Could not open the X11 display.\n");
        return 0;
    }

    app->width = environment_dimension("MANGO_OVERLAY_DESKTOP_WIDTH", 960);
    app->height = environment_dimension("MANGO_OVERLAY_DESKTOP_HEIGHT", 600);
    app->window = XCreateSimpleWindow(
        app->display,
        RootWindow(app->display, DefaultScreen(app->display)),
        80,
        80,
        (unsigned int)app->width,
        (unsigned int)app->height,
        0,
        BlackPixel(app->display, DefaultScreen(app->display)),
        BlackPixel(app->display, DefaultScreen(app->display)));
    XSelectInput(
        app->display,
        app->window,
        KeyPressMask | StructureNotifyMask);
    XStoreName(app->display, app->window, title);
    const Atom pid_atom = XInternAtom(
        app->display,
        "_NET_WM_PID",
        False);
    const unsigned long process_id = (unsigned long)getpid();
    XChangeProperty(
        app->display,
        app->window,
        pid_atom,
        XA_CARDINAL,
        32,
        PropModeReplace,
        (const unsigned char*)&process_id,
        1);
    app->wm_delete = XInternAtom(app->display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(app->display, app->window, &app->wm_delete, 1);
    XMapWindow(app->display, app->window);
    XFlush(app->display);
    return 1;
}

static int create_instance_and_surface(struct application* app)
{
    const char* extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_XLIB_SURFACE_EXTENSION_NAME,
    };
    const VkApplicationInfo application_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Mango Overlay Vulkan i686",
        .applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0),
        .pEngineName = "mango-overlay-test",
        .engineVersion = VK_MAKE_API_VERSION(0, 1, 0, 0),
        .apiVersion = VK_API_VERSION_1_0,
    };
    const VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application_info,
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = extensions,
    };
    if (!vk_success(
            vkCreateInstance(&instance_info, NULL, &app->instance),
            "vkCreateInstance")) {
        return 0;
    }

    const VkXlibSurfaceCreateInfoKHR surface_info = {
        .sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
        .dpy = app->display,
        .window = app->window,
    };
    return vk_success(
        vkCreateXlibSurfaceKHR(
            app->instance,
            &surface_info,
            NULL,
            &app->surface),
        "vkCreateXlibSurfaceKHR");
}

static int select_device_and_queue(struct application* app)
{
    uint32_t device_count = 0;
    if (!vk_success(
            vkEnumeratePhysicalDevices(app->instance, &device_count, NULL),
            "vkEnumeratePhysicalDevices(count)")
        || device_count == 0) {
        fprintf(stderr, "No Vulkan physical device is available.\n");
        return 0;
    }
    VkPhysicalDevice* devices = calloc(device_count, sizeof(*devices));
    if (devices == NULL) {
        return 0;
    }
    if (!vk_success(
            vkEnumeratePhysicalDevices(app->instance, &device_count, devices),
            "vkEnumeratePhysicalDevices")) {
        free(devices);
        return 0;
    }

    int found = 0;
    for (uint32_t device_index = 0; device_index < device_count && !found;
         ++device_index) {
        uint32_t queue_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(
            devices[device_index],
            &queue_count,
            NULL);
        VkQueueFamilyProperties* queues = calloc(queue_count, sizeof(*queues));
        if (queues == NULL) {
            continue;
        }
        vkGetPhysicalDeviceQueueFamilyProperties(
            devices[device_index],
            &queue_count,
            queues);
        for (uint32_t queue_index = 0; queue_index < queue_count; ++queue_index) {
            VkBool32 present_supported = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(
                devices[device_index],
                queue_index,
                app->surface,
                &present_supported);
            if ((queues[queue_index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0
                && present_supported) {
                app->physical_device = devices[device_index];
                app->queue_family = queue_index;
                found = 1;
                break;
            }
        }
        free(queues);
    }
    free(devices);
    if (!found) {
        fprintf(stderr, "No Vulkan graphics/present queue is available.\n");
        return 0;
    }

    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(app->physical_device, &properties);
    printf("Using Vulkan device: %s\n", properties.deviceName);

    const float queue_priority = 1.0F;
    const VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = app->queue_family,
        .queueCount = 1,
        .pQueuePriorities = &queue_priority,
    };
    const char* extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    const VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = extensions,
    };
    if (!vk_success(
            vkCreateDevice(app->physical_device, &device_info, NULL, &app->device),
            "vkCreateDevice")) {
        return 0;
    }
    vkGetDeviceQueue(app->device, app->queue_family, 0, &app->queue);
    return 1;
}

static VkCompositeAlphaFlagBitsKHR choose_composite_alpha(
    VkCompositeAlphaFlagsKHR supported)
{
    const VkCompositeAlphaFlagBitsKHR candidates[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
    };
    for (size_t index = 0; index < sizeof(candidates) / sizeof(candidates[0]);
         ++index) {
        if ((supported & candidates[index]) != 0) {
            return candidates[index];
        }
    }
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

static int create_swapchain(struct application* app)
{
    VkSurfaceCapabilitiesKHR capabilities;
    if (!vk_success(
            vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
                app->physical_device,
                app->surface,
                &capabilities),
            "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) {
        return 0;
    }
    if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0
        || (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
            == 0) {
        fprintf(stderr, "Surface images do not support the required usage flags.\n");
        return 0;
    }

    uint32_t format_count = 0;
    if (!vk_success(
            vkGetPhysicalDeviceSurfaceFormatsKHR(
                app->physical_device,
                app->surface,
                &format_count,
                NULL),
            "vkGetPhysicalDeviceSurfaceFormatsKHR(count)")
        || format_count == 0) {
        return 0;
    }
    VkSurfaceFormatKHR* formats = calloc(format_count, sizeof(*formats));
    if (formats == NULL) {
        return 0;
    }
    if (!vk_success(
            vkGetPhysicalDeviceSurfaceFormatsKHR(
                app->physical_device,
                app->surface,
                &format_count,
                formats),
            "vkGetPhysicalDeviceSurfaceFormatsKHR")) {
        free(formats);
        return 0;
    }
    VkSurfaceFormatKHR selected_format = formats[0];
    for (uint32_t index = 0; index < format_count; ++index) {
        if (formats[index].format == VK_FORMAT_B8G8R8A8_UNORM
            && formats[index].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            selected_format = formats[index];
            break;
        }
    }
    free(formats);

    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == UINT32_MAX) {
        XWindowAttributes window_attributes;
        XGetWindowAttributes(app->display, app->window, &window_attributes);
        extent.width = clamp_u32(
            (uint32_t)window_attributes.width,
            capabilities.minImageExtent.width,
            capabilities.maxImageExtent.width);
        extent.height = clamp_u32(
            (uint32_t)window_attributes.height,
            capabilities.minImageExtent.height,
            capabilities.maxImageExtent.height);
    }

    uint32_t image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0
        && image_count > capabilities.maxImageCount) {
        image_count = capabilities.maxImageCount;
    }
    const VkSwapchainCreateInfoKHR swapchain_info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = app->surface,
        .minImageCount = image_count,
        .imageFormat = selected_format.format,
        .imageColorSpace = selected_format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT
            | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = capabilities.currentTransform,
        .compositeAlpha = choose_composite_alpha(
            capabilities.supportedCompositeAlpha),
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
        .oldSwapchain = app->swapchain,
    };
    VkSwapchainKHR next_swapchain = VK_NULL_HANDLE;
    if (!vk_success(
            vkCreateSwapchainKHR(
                app->device,
                &swapchain_info,
                NULL,
                &next_swapchain),
            "vkCreateSwapchainKHR")) {
        return 0;
    }

    uint32_t next_image_count = 0;
    if (!vk_success(
            vkGetSwapchainImagesKHR(
                app->device,
                next_swapchain,
                &next_image_count,
                NULL),
            "vkGetSwapchainImagesKHR(count)")) {
        vkDestroySwapchainKHR(app->device, next_swapchain, NULL);
        return 0;
    }
    VkImage* next_images = calloc(next_image_count, sizeof(*next_images));
    unsigned char* next_initialized = calloc(
        next_image_count,
        sizeof(*next_initialized));
    if (next_images == NULL || next_initialized == NULL) {
        free(next_images);
        free(next_initialized);
        vkDestroySwapchainKHR(app->device, next_swapchain, NULL);
        return 0;
    }
    if (!vk_success(
            vkGetSwapchainImagesKHR(
                app->device,
                next_swapchain,
                &next_image_count,
                next_images),
            "vkGetSwapchainImagesKHR")) {
        free(next_images);
        free(next_initialized);
        vkDestroySwapchainKHR(app->device, next_swapchain, NULL);
        return 0;
    }

    if (app->swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(app->device, app->swapchain, NULL);
    }
    free(app->images);
    free(app->image_initialized);
    app->swapchain = next_swapchain;
    app->swapchain_format = selected_format.format;
    app->swapchain_extent = extent;
    app->image_count = next_image_count;
    app->images = next_images;
    app->image_initialized = next_initialized;
    app->resized = 0;
    return 1;
}

static int create_frame_resources(struct application* app)
{
    const VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = app->queue_family,
    };
    if (!vk_success(
            vkCreateCommandPool(
                app->device,
                &pool_info,
                NULL,
                &app->command_pool),
            "vkCreateCommandPool")) {
        return 0;
    }
    const VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = app->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (!vk_success(
            vkAllocateCommandBuffers(
                app->device,
                &command_info,
                &app->command_buffer),
            "vkAllocateCommandBuffers")) {
        return 0;
    }
    const VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    if (!vk_success(
            vkCreateSemaphore(
                app->device,
                &semaphore_info,
                NULL,
                &app->image_available),
            "vkCreateSemaphore(image available)")) {
        return 0;
    }
    if (!vk_success(
            vkCreateSemaphore(
                app->device,
                &semaphore_info,
                NULL,
                &app->render_finished),
            "vkCreateSemaphore(render finished)")) {
        return 0;
    }
    const VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    return vk_success(
        vkCreateFence(app->device, &fence_info, NULL, &app->frame_finished),
        "vkCreateFence");
}

static int recreate_swapchain(struct application* app)
{
    if (!vk_success(vkDeviceWaitIdle(app->device), "vkDeviceWaitIdle")) {
        return 0;
    }
    return create_swapchain(app);
}

static int render_frame(struct application* app, uint64_t frame_number)
{
    if (!vk_success(
            vkWaitForFences(
                app->device,
                1,
                &app->frame_finished,
                VK_TRUE,
                UINT64_MAX),
            "vkWaitForFences")) {
        return 0;
    }
    if (app->resized && !recreate_swapchain(app)) {
        return 0;
    }

    uint32_t image_index = 0;
    VkResult result = vkAcquireNextImageKHR(
        app->device,
        app->swapchain,
        UINT64_MAX,
        app->image_available,
        VK_NULL_HANDLE,
        &image_index);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        return recreate_swapchain(app);
    }
    const int suboptimal = result == VK_SUBOPTIMAL_KHR;
    if (result != VK_SUCCESS && !suboptimal) {
        return vk_success(result, "vkAcquireNextImageKHR");
    }

    if (!vk_success(
            vkResetFences(app->device, 1, &app->frame_finished),
            "vkResetFences")
        || !vk_success(
            vkResetCommandBuffer(app->command_buffer, 0),
            "vkResetCommandBuffer")) {
        return 0;
    }
    const VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (!vk_success(
            vkBeginCommandBuffer(app->command_buffer, &begin_info),
            "vkBeginCommandBuffer")) {
        return 0;
    }

    const VkImageSubresourceRange range = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
    VkImageMemoryBarrier to_clear = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = app->image_initialized[image_index]
            ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
            : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = app->images[image_index],
        .subresourceRange = range,
    };
    vkCmdPipelineBarrier(
        app->command_buffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0,
        NULL,
        0,
        NULL,
        1,
        &to_clear);

    const float pulse = (float)(frame_number % 120U) / 2400.0F;
    const VkClearColorValue clear_color = {
        .float32 = { 0.035F, 0.045F + pulse, 0.075F, 1.0F },
    };
    vkCmdClearColorImage(
        app->command_buffer,
        app->images[image_index],
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        &clear_color,
        1,
        &range);

    const VkImageMemoryBarrier to_present = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = 0,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = app->images[image_index],
        .subresourceRange = range,
    };
    vkCmdPipelineBarrier(
        app->command_buffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0,
        0,
        NULL,
        0,
        NULL,
        1,
        &to_present);
    if (!vk_success(
            vkEndCommandBuffer(app->command_buffer),
            "vkEndCommandBuffer")) {
        return 0;
    }
    app->image_initialized[image_index] = 1;

    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &app->image_available,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &app->command_buffer,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &app->render_finished,
    };
    if (!vk_success(
            vkQueueSubmit(app->queue, 1, &submit_info, app->frame_finished),
            "vkQueueSubmit")) {
        return 0;
    }

    const VkPresentInfoKHR present_info = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &app->render_finished,
        .swapchainCount = 1,
        .pSwapchains = &app->swapchain,
        .pImageIndices = &image_index,
    };
    result = vkQueuePresentKHR(app->queue, &present_info);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR
        || suboptimal) {
        return recreate_swapchain(app);
    }
    return vk_success(result, "vkQueuePresentKHR");
}

static void destroy_application(struct application* app)
{
    if (app->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(app->device);
        if (app->frame_finished != VK_NULL_HANDLE) {
            vkDestroyFence(app->device, app->frame_finished, NULL);
        }
        if (app->render_finished != VK_NULL_HANDLE) {
            vkDestroySemaphore(app->device, app->render_finished, NULL);
        }
        if (app->image_available != VK_NULL_HANDLE) {
            vkDestroySemaphore(app->device, app->image_available, NULL);
        }
        if (app->command_pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(app->device, app->command_pool, NULL);
        }
        if (app->swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(app->device, app->swapchain, NULL);
        }
        vkDestroyDevice(app->device, NULL);
    }
    free(app->images);
    free(app->image_initialized);
    if (app->surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(app->instance, app->surface, NULL);
    }
    if (app->instance != VK_NULL_HANDLE) {
        vkDestroyInstance(app->instance, NULL);
    }
    if (app->display != NULL) {
        if (app->window != None) {
            XDestroyWindow(app->display, app->window);
        }
        XCloseDisplay(app->display);
    }
}

int main(int argc, char** argv)
{
    const char* title = argc > 1 ? argv[1] : "Mango Overlay Vulkan Test";
    struct application app;
    memset(&app, 0, sizeof(app));
    if (!create_window(&app, title)
        || !create_instance_and_surface(&app)
        || !select_device_and_queue(&app)
        || !create_swapchain(&app)
        || !create_frame_resources(&app)) {
        destroy_application(&app);
        return 1;
    }

    int running = 1;
    uint64_t frame_number = 0;
    const struct timespec event_delay = { .tv_sec = 0, .tv_nsec = 1000000L };
    while (running) {
        while (XPending(app.display) > 0) {
            XEvent event;
            XNextEvent(app.display, &event);
            if (event.type == ConfigureNotify) {
                app.width = event.xconfigure.width;
                app.height = event.xconfigure.height;
                app.resized = 1;
            } else if (event.type == KeyPress
                && XLookupKeysym(&event.xkey, 0) == XK_Escape) {
                running = 0;
            } else if (event.type == ClientMessage
                && (Atom)event.xclient.data.l[0] == app.wm_delete) {
                running = 0;
            }
        }
        if (running && !render_frame(&app, frame_number++)) {
            destroy_application(&app);
            return 1;
        }
        nanosleep(&event_delay, NULL);
    }

    destroy_application(&app);
    return 0;
}
