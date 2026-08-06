#include "mango_overlay/client.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fcntl.h>
#include <signal.h>
#include <gif_lib.h>
#include <png.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct BrokerProcess {
    pid_t pid = -1;
    std::string directory;
    std::string socket_path;

    ~BrokerProcess()
    {
        if (pid > 0) {
            kill(pid, SIGTERM);
            waitpid(pid, nullptr, 0);
        }
        if (!socket_path.empty()) {
            unlink(socket_path.c_str());
        }
        if (!directory.empty()) {
            rmdir(directory.c_str());
        }
    }
};

bool start_broker(const char* executable, BrokerProcess& process)
{
    char directory[] = "/tmp/mango-overlay-client-test-XXXXXX";
    if (mkdtemp(directory) == nullptr) {
        return false;
    }
    process.directory = directory;
    process.socket_path = process.directory + "/broker.sock";

    const int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::memcpy(
        address.sun_path,
        process.socket_path.c_str(),
        process.socket_path.size() + 1);
    const auto address_size = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + process.socket_path.size() + 1);
    if (listener < 0
        || bind(listener, reinterpret_cast<sockaddr*>(&address), address_size) != 0
        || listen(listener, 4) != 0) {
        if (listener >= 0) {
            close(listener);
        }
        return false;
    }

    process.pid = fork();
    if (process.pid < 0) {
        close(listener);
        return false;
    }
    if (process.pid == 0) {
        if (fcntl(listener, F_SETFD, 0) != 0) {
            _exit(126);
        }
        const std::string descriptor = std::to_string(listener);
        execl(executable, executable, "--listen-fd", descriptor.c_str(), nullptr);
        _exit(127);
    }
    close(listener);
    return true;
}

bool expect(
    mango_overlay_result actual,
    mango_overlay_result expected,
    const char* operation,
    const mango_overlay_client* client = nullptr)
{
    if (actual == expected) {
        return true;
    }
    std::fprintf(
        stderr,
        "%s returned %d instead of %d: %s\n",
        operation,
        static_cast<int>(actual),
        static_cast<int>(expected),
        client == nullptr ? "" : mango_overlay_client_last_error(client));
    return false;
}

std::vector<std::uint8_t> make_png()
{
    const std::uint8_t pixels[] = {
        255, 0, 0, 255,
        0, 255, 0, 255,
    };
    png_image image {};
    image.version = PNG_IMAGE_VERSION;
    image.width = 2;
    image.height = 1;
    image.format = PNG_FORMAT_RGBA;
    png_alloc_size_t size = 0;
    png_image_write_to_memory(&image, nullptr, &size, 0, pixels, 0, nullptr);
    std::vector<std::uint8_t> output(size);
    if (png_image_write_to_memory(
            &image, output.data(), &size, 0, pixels, 0, nullptr)
        == 0) {
        return {};
    }
    output.resize(size);
    return output;
}

int write_gif(GifFileType* gif, const GifByteType* bytes, int count)
{
    auto* output = static_cast<std::vector<std::uint8_t>*>(gif->UserData);
    try {
        output->insert(output->end(), bytes, bytes + count);
        return count;
    } catch (...) {
        return 0;
    }
}

std::vector<std::uint8_t> make_gif()
{
    std::vector<std::uint8_t> output;
    const GifColorType colors[] = { { 255, 0, 0 }, { 0, 255, 0 } };
    ColorMapObject* map = GifMakeMapObject(2, colors);
    int error = 0;
    GifFileType* gif = EGifOpen(&output, write_gif, &error);
    if (map == nullptr || gif == nullptr) {
        GifFreeMapObject(map);
        return {};
    }
    EGifSetGifVersion(gif, true);
    if (EGifPutScreenDesc(gif, 2, 1, 8, 0, map) != GIF_OK) {
        EGifCloseFile(gif, &error);
        GifFreeMapObject(map);
        return {};
    }
    for (int frame = 0; frame < 2; ++frame) {
        GraphicsControlBlock control {
            DISPOSE_DO_NOT, false, frame + 2, NO_TRANSPARENT_COLOR };
        GifByteType extension[4] {};
        EGifGCBToExtension(&control, extension);
        GifPixelType row[] = {
            static_cast<GifPixelType>(frame),
            static_cast<GifPixelType>(1 - frame),
        };
        if (EGifPutExtension(
                gif, GRAPHICS_EXT_FUNC_CODE, sizeof(extension), extension)
                != GIF_OK
            || EGifPutImageDesc(gif, 0, 0, 2, 1, false, nullptr) != GIF_OK
            || EGifPutLine(gif, row, 2) != GIF_OK) {
            EGifCloseFile(gif, &error);
            GifFreeMapObject(map);
            return {};
        }
    }
    EGifCloseFile(gif, &error);
    GifFreeMapObject(map);
    return output;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fputs("broker executable path is required\n", stderr);
        return 1;
    }

    BrokerProcess broker;
    if (!start_broker(argv[1], broker)) {
        std::perror("start broker");
        return 1;
    }

    mango_overlay_client_config config {};
    config.struct_size = sizeof(config);
    config.socket_path = broker.socket_path.c_str();
    config.client_version = "provider-client-process-test/1.0";
    config.timeout_ms = 2000;

    mango_overlay_client* client = nullptr;
    if (!expect(
            mango_overlay_client_open(&config, &client),
            MANGO_OVERLAY_OK,
            "open",
            client)
        || client == nullptr) {
        return 1;
    }

    mango_overlay_element_layout group_layout
        = MANGO_OVERLAY_ELEMENT_LAYOUT_IDENTITY;
    group_layout.translation = { 100.0F, 50.0F };
    group_layout.scale = { 1.25F, 1.25F };
    group_layout.opacity = 0.8F;
    group_layout.clip_enabled = 1;
    group_layout.clip = { 0.0F, 0.0F, 400.0F, 200.0F };
    mango_overlay_group_element group {};
    group.struct_size = sizeof(group);
    group.element_id = 9;
    group.z_index = 0;
    group.layout = &group_layout;

    mango_overlay_element_layout child_layout
        = MANGO_OVERLAY_ELEMENT_LAYOUT_IDENTITY;
    child_layout.parent_id = 9;

    mango_overlay_text_element text {};
    text.struct_size = sizeof(text);
    text.element_id = 10;
    text.z_index = 2;
    text.x = 20.0F;
    text.y = 30.0F;
    text.text = "atomic scene";
    text.font_size = 24.0F;
    text.color = { 1.0F, 1.0F, 1.0F, 1.0F };
    text.layout = &child_layout;
    if (!expect(
            mango_overlay_client_upsert_text(client, &text),
            MANGO_OVERLAY_INVALID_STATE,
            "upsert outside transaction",
            client)) {
        mango_overlay_client_close(client);
        return 1;
    }

    mango_overlay_provider_info provider {};
    provider.struct_size = sizeof(provider);
    provider.application_id = "dev.mango-overlay.client-test";
    provider.instance_id = "primary";
    provider.display_name = "Provider Client Test";
    provider.canvas_width = 1280;
    provider.canvas_height = 800;
    provider.visibility = MANGO_OVERLAY_VISIBILITY_ALWAYS;
    const auto png = make_png();
    const auto gif_data = make_gif();
    if (!expect(
            mango_overlay_client_register_provider(client, &provider),
            MANGO_OVERLAY_OK,
            "register",
            client)
        || png.empty() || gif_data.empty()
        || !expect(
            mango_overlay_client_upload_resource(
                client, 100, png.data(), static_cast<std::uint32_t>(png.size())),
            MANGO_OVERLAY_OK,
            "upload PNG",
            client)
        || !expect(
            mango_overlay_client_upload_resource(
                client,
                101,
                gif_data.data(),
                static_cast<std::uint32_t>(gif_data.size())),
            MANGO_OVERLAY_OK,
            "upload GIF",
            client)
        || !expect(
            mango_overlay_client_begin_transaction(client),
            MANGO_OVERLAY_OK,
            "begin",
            client)
        || !expect(
            mango_overlay_client_upsert_group(client, &group),
            MANGO_OVERLAY_OK,
            "upsert group",
            client)
        || !expect(
            mango_overlay_client_upsert_text(client, &text),
            MANGO_OVERLAY_OK,
            "upsert text",
            client)) {
        mango_overlay_client_close(client);
        return 1;
    }

    mango_overlay_rectangle_element rectangle {};
    rectangle.struct_size = sizeof(rectangle);
    rectangle.element_id = 11;
    rectangle.z_index = 1;
    rectangle.x = 12.0F;
    rectangle.y = 18.0F;
    rectangle.width = 320.0F;
    rectangle.height = 96.0F;
    rectangle.corner_radius = 8.0F;
    rectangle.color = { 0.02F, 0.03F, 0.04F, 0.9F };
    mango_overlay_line_element line {};
    line.struct_size = sizeof(line);
    line.element_id = 12;
    line.z_index = 2;
    line.start = { 24.0F, 120.0F };
    line.end = { 280.0F, 120.0F };
    line.thickness = 3.0F;
    line.color = { 0.2F, 0.8F, 0.5F, 1.0F };
    const mango_overlay_vec2 points[] = {
        { 30.0F, 160.0F },
        { 80.0F, 140.0F },
        { 130.0F, 170.0F },
    };
    mango_overlay_polyline_element polyline {};
    polyline.struct_size = sizeof(polyline);
    polyline.element_id = 13;
    polyline.z_index = 3;
    polyline.points = points;
    polyline.point_count = 3;
    polyline.thickness = 2.0F;
    polyline.color = { 0.9F, 0.7F, 0.2F, 1.0F };
    mango_overlay_circle_element circle {};
    circle.struct_size = sizeof(circle);
    circle.element_id = 14;
    circle.z_index = 4;
    circle.center = { 360.0F, 80.0F };
    circle.radius = 24.0F;
    circle.color = { 0.3F, 0.5F, 1.0F, 1.0F };
    mango_overlay_image_element image {};
    image.struct_size = sizeof(image);
    image.element_id = 15;
    image.z_index = 5;
    image.x = 20.0F;
    image.y = 200.0F;
    image.width = 64.0F;
    image.height = 32.0F;
    image.resource_id = 100;
    image.tint = { 1.0F, 1.0F, 1.0F, 1.0F };
    mango_overlay_gif_element gif {};
    gif.struct_size = sizeof(gif);
    gif.element_id = 16;
    gif.z_index = 6;
    gif.x = 100.0F;
    gif.y = 200.0F;
    gif.width = 64.0F;
    gif.height = 32.0F;
    gif.resource_id = 101;
    gif.tint = { 1.0F, 1.0F, 1.0F, 1.0F };
    gif.playback_rate = 1.0F;
    if (!expect(
            mango_overlay_client_upsert_rectangle(client, &rectangle),
            MANGO_OVERLAY_OK,
            "upsert rectangle",
            client)
        || !expect(
            mango_overlay_client_upsert_line(client, &line),
            MANGO_OVERLAY_OK,
            "upsert line",
            client)
        || !expect(
            mango_overlay_client_upsert_polyline(client, &polyline),
            MANGO_OVERLAY_OK,
            "upsert polyline",
            client)
        || !expect(
            mango_overlay_client_upsert_circle(client, &circle),
            MANGO_OVERLAY_OK,
            "upsert circle",
            client)
        || !expect(
            mango_overlay_client_upsert_image(client, &image),
            MANGO_OVERLAY_OK,
            "upsert image",
            client)
        || !expect(
            mango_overlay_client_upsert_gif(client, &gif),
            MANGO_OVERLAY_OK,
            "upsert GIF",
            client)
        || !expect(
            mango_overlay_client_commit_transaction(client),
            MANGO_OVERLAY_OK,
            "commit",
            client)
        || !expect(
            mango_overlay_client_begin_transaction(client),
            MANGO_OVERLAY_OK,
            "resource removal begin",
            client)
        || !expect(
            mango_overlay_client_remove_element(client, 15),
            MANGO_OVERLAY_OK,
            "remove image",
            client)
        || !expect(
            mango_overlay_client_remove_element(client, 16),
            MANGO_OVERLAY_OK,
            "remove GIF",
            client)
        || !expect(
            mango_overlay_client_commit_transaction(client),
            MANGO_OVERLAY_OK,
            "resource removal commit",
            client)
        || !expect(
            mango_overlay_client_release_resource(client, 100),
            MANGO_OVERLAY_OK,
            "release PNG",
            client)
        || !expect(
            mango_overlay_client_release_resource(client, 101),
            MANGO_OVERLAY_OK,
            "release GIF",
            client)
        || !expect(
            mango_overlay_client_begin_transaction(client),
            MANGO_OVERLAY_OK,
            "second begin",
            client)
        || !expect(
            mango_overlay_client_remove_element(client, 10),
            MANGO_OVERLAY_OK,
            "remove",
            client)
        || !expect(
            mango_overlay_client_abort_transaction(client),
            MANGO_OVERLAY_OK,
            "abort",
            client)) {
        mango_overlay_client_close(client);
        return 1;
    }

    mango_overlay_client_close(client);
    return 0;
}
