#include "mango_overlay/client.hpp"

#include <signal.h>
#include <gif_lib.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> running { true };

void stop(int)
{
    running.store(false, std::memory_order_relaxed);
}

const std::vector<std::uint8_t> png {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
    0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0xf0,
    0x1f, 0x00, 0x05, 0x00, 0x01, 0xff, 0x89, 0x99, 0x3d, 0x1d, 0x00, 0x00,
    0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

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
    const GifColorType colors[] = { { 0, 220, 150 }, { 40, 90, 255 } };
    ColorMapObject* color_map = GifMakeMapObject(2, colors);
    int error = 0;
    GifFileType* gif = EGifOpen(&output, write_gif, &error);
    if (color_map == nullptr || gif == nullptr) {
        GifFreeMapObject(color_map);
        return {};
    }
    EGifSetGifVersion(gif, true);
    if (EGifPutScreenDesc(gif, 8, 8, 8, 0, color_map) != GIF_OK) {
        EGifCloseFile(gif, &error);
        GifFreeMapObject(color_map);
        return {};
    }
    for (int frame = 0; frame < 2; ++frame) {
        GraphicsControlBlock control {
            DISPOSE_DO_NOT, false, 20 + frame * 10, NO_TRANSPARENT_COLOR };
        GifByteType extension[4] {};
        EGifGCBToExtension(&control, extension);
        if (EGifPutExtension(
                gif, GRAPHICS_EXT_FUNC_CODE, sizeof(extension), extension)
                != GIF_OK
            || EGifPutImageDesc(gif, 0, 0, 8, 8, false, nullptr) != GIF_OK) {
            EGifCloseFile(gif, &error);
            GifFreeMapObject(color_map);
            return {};
        }
        for (int y = 0; y < 8; ++y) {
            GifPixelType row[8] {};
            for (int x = 0; x < 8; ++x) {
                row[x] = static_cast<GifPixelType>((x + y + frame) % 2);
            }
            if (EGifPutLine(gif, row, 8) != GIF_OK) {
                EGifCloseFile(gif, &error);
                GifFreeMapObject(color_map);
                return {};
            }
        }
    }
    EGifCloseFile(gif, &error);
    GifFreeMapObject(color_map);
    return output;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2
        && std::string_view(argv[1]) == "--mango-overlay-self-test") {
        std::puts("mango-overlay-test-provider version=" MANGO_OVERLAY_VERSION " protocol=1.0 status=ok");
        return 0;
    }
    if (argc != 1) {
        return 64;
    }
    signal(SIGINT, stop);
    signal(SIGTERM, stop);

    try {
        mango_overlay::client::Provider client(
            "mango-overlay-test-provider/" MANGO_OVERLAY_VERSION);

        mango_overlay_provider_info provider {};
        provider.struct_size = sizeof(provider);
        provider.application_id = "dev.mango-overlay.test";
        provider.instance_id = "decky";
        provider.display_name = "Test Canvas";
        provider.canvas_width = 1280;
        provider.canvas_height = 800;
        provider.visibility = MANGO_OVERLAY_VISIBILITY_ALWAYS;
        client.register_provider(provider);
        const auto gif = make_gif();
        if (gif.empty()) {
            throw std::runtime_error("Could not create the demo GIF");
        }
        client.upload_resource(100, png);
        client.upload_resource(101, gif);

        unsigned int frame = 0;
        while (running.load(std::memory_order_relaxed)) {
            const std::string label = "Mango Overlay Decky  |  frame "
                + std::to_string(frame);

            mango_overlay_element_layout group_layout
                = MANGO_OVERLAY_ELEMENT_LAYOUT_IDENTITY;
            group_layout.translation = { -600.0F, 36.0F };
            group_layout.anchor = MANGO_OVERLAY_ANCHOR_TOP_RIGHT;
            group_layout.clip_enabled = 1;
            group_layout.clip = { 0.0F, 0.0F, 560.0F, 184.0F };
            mango_overlay_group_element group {};
            group.struct_size = sizeof(group);
            group.element_id = 1;
            group.z_index = 0;
            group.layout = &group_layout;

            mango_overlay_element_layout child_layout
                = MANGO_OVERLAY_ELEMENT_LAYOUT_IDENTITY;
            child_layout.parent_id = 1;

            mango_overlay_rectangle_element panel {};
            panel.struct_size = sizeof(panel);
            panel.element_id = 2;
            panel.z_index = 0;
            panel.x = 0.0F;
            panel.y = 0.0F;
            panel.width = 560.0F;
            panel.height = 184.0F;
            panel.corner_radius = 12.0F;
            panel.color = { 0.03F, 0.04F, 0.05F, 0.88F };
            panel.layout = &child_layout;

            mango_overlay_text_element text {};
            text.struct_size = sizeof(text);
            text.element_id = 3;
            text.z_index = 1;
            text.x = 28.0F;
            text.y = 24.0F;
            text.text = label.c_str();
            text.font_size = 26.0F;
            text.color = { 0.95F, 0.97F, 1.0F, 1.0F };
            text.layout = &child_layout;

            mango_overlay_rectangle_element progress {};
            progress.struct_size = sizeof(progress);
            progress.element_id = 4;
            progress.z_index = 1;
            progress.x = 28.0F;
            progress.y = 92.0F;
            progress.width = 24.0F + static_cast<float>(frame % 90U) * 4.2F;
            progress.height = 18.0F;
            progress.corner_radius = 5.0F;
            progress.color = { 0.12F, 0.78F, 0.54F, 1.0F };
            progress.layout = &child_layout;

            const mango_overlay_vec2 sparkline_points[] = {
                { 28.0F, 72.0F },
                { 70.0F, 58.0F },
                { 112.0F, 67.0F },
                { 154.0F, 46.0F },
                { 196.0F, 60.0F },
            };
            mango_overlay_polyline_element sparkline {};
            sparkline.struct_size = sizeof(sparkline);
            sparkline.element_id = 5;
            sparkline.z_index = 1;
            sparkline.points = sparkline_points;
            sparkline.point_count = 5;
            sparkline.thickness = 3.0F;
            sparkline.color = { 0.28F, 0.62F, 1.0F, 1.0F };
            sparkline.layout = &child_layout;

            mango_overlay_circle_element status {};
            status.struct_size = sizeof(status);
            status.element_id = 6;
            status.z_index = 1;
            status.center = { 528.0F, 38.0F };
            status.radius = 8.0F;
            status.color = { 0.12F, 0.78F, 0.54F, 1.0F };
            status.layout = &child_layout;

            mango_overlay_image_element image {};
            image.struct_size = sizeof(image);
            image.element_id = 7;
            image.z_index = 2;
            image.x = 426.0F;
            image.y = 70.0F;
            image.width = 72.0F;
            image.height = 72.0F;
            image.resource_id = 100;
            image.tint = { 1.0F, 1.0F, 1.0F, 1.0F };
            image.layout = &child_layout;

            mango_overlay_gif_element gif_image {};
            gif_image.struct_size = sizeof(gif_image);
            gif_image.element_id = 8;
            gif_image.z_index = 3;
            gif_image.x = 502.0F;
            gif_image.y = 70.0F;
            gif_image.width = 42.0F;
            gif_image.height = 72.0F;
            gif_image.resource_id = 101;
            gif_image.tint = { 1.0F, 1.0F, 1.0F, 1.0F };
            gif_image.playback_rate = 1.0F;
            gif_image.layout = &child_layout;

            client.begin_transaction();
            client.upsert(group);
            client.upsert(panel);
            client.upsert(text);
            client.upsert(progress);
            client.upsert(sparkline);
            client.upsert(status);
            client.upsert(image);
            client.upsert(gif_image);
            client.commit();

            ++frame;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    } catch (const mango_overlay::client::Error& error) {
        std::fprintf(
            stderr,
            "provider demo failed (%d): %s\n",
            static_cast<int>(error.result()),
            error.what());
        return 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "provider demo failed: %s\n", error.what());
        return 1;
    }
    return 0;
}
