#define main mango_overlay_single_vulkan_main
#include "vulkan-test-window.c"
#undef main

static int create_shared_surface(
    struct application* app,
    VkInstance instance)
{
    const VkXlibSurfaceCreateInfoKHR surface_info = {
        .sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
        .dpy = app->display,
        .window = app->window,
    };
    return vk_success(
        vkCreateXlibSurfaceKHR(
            instance,
            &surface_info,
            NULL,
            &app->surface),
        "vkCreateXlibSurfaceKHR(second surface)");
}

static void destroy_swapchain_resources(struct application* app)
{
    if (app->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(app->device);
        if (app->frame_finished != VK_NULL_HANDLE) {
            vkDestroyFence(app->device, app->frame_finished, NULL);
            app->frame_finished = VK_NULL_HANDLE;
        }
        if (app->render_finished != VK_NULL_HANDLE) {
            vkDestroySemaphore(app->device, app->render_finished, NULL);
            app->render_finished = VK_NULL_HANDLE;
        }
        if (app->image_available != VK_NULL_HANDLE) {
            vkDestroySemaphore(app->device, app->image_available, NULL);
            app->image_available = VK_NULL_HANDLE;
        }
        if (app->command_pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(app->device, app->command_pool, NULL);
            app->command_pool = VK_NULL_HANDLE;
        }
        if (app->swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(app->device, app->swapchain, NULL);
            app->swapchain = VK_NULL_HANDLE;
        }
    }
    free(app->images);
    free(app->image_initialized);
    app->images = NULL;
    app->image_initialized = NULL;
    if (app->surface != VK_NULL_HANDLE && app->instance != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(app->instance, app->surface, NULL);
        app->surface = VK_NULL_HANDLE;
    }
    if (app->display != NULL) {
        if (app->window != None) {
            XDestroyWindow(app->display, app->window);
            app->window = None;
        }
        XCloseDisplay(app->display);
        app->display = NULL;
    }
}

static int poll_window(struct application* app)
{
    int open = 1;
    while (XPending(app->display) > 0) {
        XEvent event;
        XNextEvent(app->display, &event);
        if (event.type == ConfigureNotify) {
            app->width = event.xconfigure.width;
            app->height = event.xconfigure.height;
            app->resized = 1;
        } else if (event.type == KeyPress
            && XLookupKeysym(&event.xkey, 0) == XK_Escape) {
            open = 0;
        } else if (event.type == ClientMessage
            && (Atom)event.xclient.data.l[0] == app->wm_delete) {
            open = 0;
        }
    }
    return open;
}

int main(int argc, char** argv)
{
    const char* title_a = argc > 1
        ? argv[1]
        : "Mango Overlay Two Swapchains A";
    const char* title_b = argc > 2
        ? argv[2]
        : "Mango Overlay Two Swapchains B";
    struct application first;
    struct application second;
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));

    if (!create_window(&first, title_a)
        || !create_instance_and_surface(&first)
        || !select_device_and_queue(&first)
        || !create_swapchain(&first)
        || !create_frame_resources(&first)) {
        destroy_application(&first);
        return 1;
    }

    if (!create_window(&second, title_b)) {
        destroy_application(&first);
        return 1;
    }
    second.instance = first.instance;
    second.physical_device = first.physical_device;
    second.queue_family = first.queue_family;
    second.device = first.device;
    second.queue = first.queue;
    if (!create_shared_surface(&second, first.instance)
        || !create_swapchain(&second)
        || !create_frame_resources(&second)) {
        destroy_swapchain_resources(&second);
        destroy_application(&first);
        return 1;
    }

    XMoveWindow(first.display, first.window, 20, 40);
    XMoveWindow(second.display, second.window, 1000, 40);
    XFlush(first.display);
    XFlush(second.display);

    int first_open = 1;
    int second_open = 1;
    uint64_t frame_number = 0;
    const struct timespec event_delay = { .tv_sec = 0, .tv_nsec = 1000000L };
    while (first_open || second_open) {
        if (first_open && !poll_window(&first)) {
            destroy_swapchain_resources(&first);
            first_open = 0;
        }
        if (second_open && !poll_window(&second)) {
            destroy_swapchain_resources(&second);
            second_open = 0;
        }
        if (first_open && !render_frame(&first, frame_number)) {
            destroy_swapchain_resources(&first);
            first_open = 0;
        }
        if (second_open && !render_frame(&second, frame_number)) {
            destroy_swapchain_resources(&second);
            second_open = 0;
        }
        ++frame_number;
        nanosleep(&event_delay, NULL);
    }

    const VkDevice shared_device = first.device;
    const VkInstance shared_instance = first.instance;
    if (first_open) {
        destroy_swapchain_resources(&first);
    }
    if (second_open) {
        destroy_swapchain_resources(&second);
    }
    if (shared_device != VK_NULL_HANDLE) {
        vkDestroyDevice(shared_device, NULL);
    }
    if (shared_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(shared_instance, NULL);
    }
    return 0;
}
