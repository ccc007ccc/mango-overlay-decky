#include <cstdlib>
#include <functional>
#include <thread>
#include <string>
#include <iostream>
#include <sstream>
#include <memory>
#include <unistd.h>
#include <spdlog/spdlog.h>
#include <imgui.h>
#ifdef __linux__
#include <implot.h>
#endif
#include "imgui_utils.h"
#include "gl_hud.h"
#include "file_utils.h"
#include "notify.h"
#include "blacklist.h"

#ifdef MANGO_OVERLAY_DECKY
#include "mango_overlay/renderer/canvas.hpp"
#include "mango_overlay/renderer/imgui_scene.hpp"
#include "mango_overlay/renderer/opengl_texture_backend.hpp"
#include "mango_overlay/renderer/scene_client.hpp"
#endif

#include <glad/glad.h>


#define GLX_RENDERER_VENDOR_ID_MESA                      0x8183
#define GLX_RENDERER_DEVICE_ID_MESA                      0x8184

#ifdef HAVE_X11
bool glx_mesa_queryInteger(int attrib, unsigned int *value);
#endif

namespace MangoHud { namespace GL {

struct GLVec
{
    GLint v[4];

    GLint operator[] (size_t i)
    {
        return v[i];
    }

    bool operator== (const GLVec& r)
    {
        return v[0] == r.v[0]
            && v[1] == r.v[1]
            && v[2] == r.v[2]
            && v[3] == r.v[3];
    }

    bool operator!= (const GLVec& r)
    {
        return !(*this == r);
    }
};

static GLVec last_vp {}, last_sb {};
swapchain_stats sw_stats {};
static struct imgui_contexts imgui_contexts;
static uint32_t vendorID;
static std::string deviceName;

static notify_thread notifier;
static bool cfg_inited = false;
static ImVec2 window_size;
static bool inited = false;
overlay_params params {};

#ifdef MANGO_OVERLAY_DECKY
static std::unique_ptr<mango_overlay::renderer::SceneClient> provider_scene_client;
static std::unique_ptr<mango_overlay::renderer::ImGuiSceneRenderer> provider_scene_renderer;
static ImFont* provider_scene_font = nullptr;
#endif

// seems to quit by itself though
static std::unique_ptr<notify_thread, std::function<void(notify_thread *)>>
    stop_it(&notifier, [](notify_thread *n){ stop_notifier(*n); });

void imgui_init()
{
    if (cfg_inited)
        return;

    init_spdlog();
    if (is_blacklisted())
        return;

    parse_overlay_config(&params, getenv("MANGOHUD_CONFIG"), false);

   //check for blacklist item in the config file
   for (auto& item : params.blacklist) {
      add_blacklist(item);
   }

    if (sw_stats.engine != EngineTypes::ZINK){
        sw_stats.engine = OPENGL;
        if (lib_loaded("wined3d", HUDElements.g_gamescopePid))
            sw_stats.engine = WINED3D;
        if (lib_loaded("libtogl.so", HUDElements.g_gamescopePid) || lib_loaded("libtogl_client.so", HUDElements.g_gamescopePid))
            sw_stats.engine = TOGL;
    }

    is_blacklisted(true);
    notifier.params = &params;
    start_notifier(notifier);
    window_size = ImVec2(params.width, params.height);
    init_system_info();
    cfg_inited = true;
    init_cpu_stats(params);
}

void imgui_create(gl_context *ctx, const gl_wsi plat)
{
    //SPDLOG_DEBUG("ctx {}", (void *)ctx);
    if (!ctx)
        return;

    if (inited)
        return;

    imgui_init();

    if (!gladLoadGL())
        spdlog::error("Failed to initialize OpenGL context, crash incoming");

    deviceName = (char*)glGetString(GL_RENDERER);
    // If we're running zink we want to rely on the vulkan loader for the hud instead.
    if (deviceName.find("zink") != std::string::npos)
        return;

    GetOpenGLVersion(sw_stats.version_gl.major,
        sw_stats.version_gl.minor,
        sw_stats.version_gl.is_gles);

    std::string vendor = (char*)glGetString(GL_VENDOR);
    SPDLOG_DEBUG("vendor: {}, deviceName: {}", vendor, deviceName);
    sw_stats.deviceName = deviceName;
    if (vendor.find("AMD") != std::string::npos
    || deviceName.find("AMD") != std::string::npos
    || deviceName.find("Radeon") != std::string::npos
    || deviceName.find("NAVI") != std::string::npos) {
        vendorID = 0x1002;
    } else if (vendor.find("Intel") != std::string::npos
    || deviceName.find("Intel") != std::string::npos) {
        vendorID = 0x8086;
    } else if (vendor.find("freedreno") != std::string::npos) {
        vendorID = 0x5143;
    }  else {
        vendorID = 0x10de;
    }

    HUDElements.vendorID = vendorID;

    uint32_t device_id = 0;
#ifdef HAVE_X11
    if (plat == gl_wsi::GL_WSI_GLX)
        glx_mesa_queryInteger(GLX_RENDERER_DEVICE_ID_MESA, &device_id);
#endif
    SPDLOG_DEBUG("GL device id: {:04X}", device_id);
    sw_stats.gpuName = gpu = remove_parentheses(deviceName);
    SPDLOG_DEBUG("gpu: {}", gpu);
    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();

    if (!imgui_contexts.imgui)
        imgui_contexts =  create_imgui_contexts();

    auto saved_imgui_contexts = get_current_imgui_contexts();
    make_imgui_contexts_current(imgui_contexts);

    ImGuiIO& io = ImGui::GetIO(); (void)io;
    //io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    //io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    //ImGui::StyleColorsClassic();
    HUDElements.convert_colors(false, params);

    glGetIntegerv (GL_VIEWPORT, last_vp.v);
    glGetIntegerv (GL_SCISSOR_BOX, last_sb.v);

    ImGui::GetIO().IniFilename = NULL;
    ImGui::GetIO().DisplaySize = ImVec2(last_vp[2], last_vp[3]);

    ImGui_ImplOpenGL3_Init(ctx);

#ifdef MANGO_OVERLAY_DECKY
    create_fonts(
        nullptr,
        params,
        sw_stats.font_small,
        sw_stats.font_text,
        sw_stats.font_secondary,
        &provider_scene_font);
    provider_scene_renderer
        = std::make_unique<mango_overlay::renderer::ImGuiSceneRenderer>(
            mango_overlay::renderer::make_opengl_texture_backend());
    provider_scene_renderer->set_text_font(provider_scene_font);
    provider_scene_client
        = std::make_unique<mango_overlay::renderer::SceneClient>();
    provider_scene_client->start();
#else
    create_fonts(nullptr, params, sw_stats.font_small, sw_stats.font_text, sw_stats.font_secondary);
#endif
    sw_stats.font_params_hash = params.font_params_hash;
    inited = true;

    // Restore global context or ours might clash with apps that use Dear ImGui
    make_imgui_contexts_current(saved_imgui_contexts);
}

void imgui_shutdown(gl_context *ctx, bool last)
{
    //SPDLOG_DEBUG("destroying ctx {}, imgui_ctx {}, last {}", (void *)ctx, (void *)state.imgui_ctx, last);
    if (imgui_contexts.imgui) {
        auto saved_imgui_contexts = get_current_imgui_contexts();
        make_imgui_contexts_current(imgui_contexts);
#ifdef MANGO_OVERLAY_DECKY
        if (last) {
            provider_scene_renderer.reset();
            if (provider_scene_client) {
                provider_scene_client->stop();
                provider_scene_client.reset();
            }
            provider_scene_font = nullptr;
        }
#endif
        ImGui_ImplOpenGL3_Shutdown(ctx);
        make_imgui_contexts_current(saved_imgui_contexts);
        if (last)
        {
            destroy_imgui_contexts(imgui_contexts);
            inited = false;
        }
    }
}

void imgui_render(gl_context *ctx, unsigned int width, unsigned int height)
{
    //SPDLOG_DEBUG("imgui_ctx {}", (void *)state.imgui_ctx);
    if (!imgui_contexts.imgui)
        return;

    static int control_client = -1;
    if (params.control >= 0) {
        control_client_check(params.control, control_client, deviceName);
        process_control_socket(control_client, params);
    }

    check_keybinds(params);
    update_hud_info(sw_stats, params, vendorID);

    auto saved_imgui_contexts = get_current_imgui_contexts();
    make_imgui_contexts_current(imgui_contexts);

    ImGui::GetIO().DisplaySize = ImVec2(width, height);
    if (HUDElements.colors.update)
        HUDElements.convert_colors(params);

    ImGui_ImplOpenGL3_NewFrame(ctx);
    ImGui::NewFrame();
    {
        std::lock_guard<std::mutex> lk(notifier.mutex);
        overlay_new_frame(params);
        position_layer(sw_stats, params, window_size);
        render_imgui(sw_stats, params, window_size, false);
#ifdef MANGO_OVERLAY_DECKY
        if (provider_scene_client && provider_scene_renderer
            && provider_scene_client->connected()) {
            const auto snapshot = provider_scene_client->snapshot();
            if (mango_overlay::renderer::snapshot_has_visible_provider(
                    *snapshot, false)) {
                provider_scene_renderer->draw(
                    *snapshot,
                    static_cast<float>(width),
                    static_cast<float>(height),
                    false);
            }
        }
#endif
        overlay_end_frame();
    }

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    if (sw_stats.font_params_hash != params.font_params_hash)
    {
        sw_stats.font_params_hash = params.font_params_hash;
#ifdef MANGO_OVERLAY_DECKY
        create_fonts(
            nullptr,
            params,
            sw_stats.font_small,
            sw_stats.font_text,
            sw_stats.font_secondary,
            &provider_scene_font);
        if (provider_scene_renderer)
            provider_scene_renderer->set_text_font(provider_scene_font);
#else
        create_fonts(nullptr, params, sw_stats.font_small, sw_stats.font_text, sw_stats.font_secondary);
#endif
        // Previus texture is created in ImGui_ImplOpenGL3_CreateDeviceObjects in first call of ImGui_ImplOpenGL3_NewFrame
        ImGui_ImplOpenGL3_DestroyFontsTexture(ctx);
        ImGui_ImplOpenGL3_CreateFontsTexture(ctx);
    }

    make_imgui_contexts_current(saved_imgui_contexts);
}

}} // namespaces
