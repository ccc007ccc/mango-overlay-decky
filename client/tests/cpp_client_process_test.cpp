#include "mango_overlay/client.hpp"

#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

const std::vector<std::uint8_t> png {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
    0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0xf0,
    0x1f, 0x00, 0x05, 0x00, 0x01, 0xff, 0x89, 0x99, 0x3d, 0x1d, 0x00, 0x00,
    0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

} // namespace

int main()
{
    const char* socket_path = std::getenv("MANGO_OVERLAY_TEST_SOCKET");
    if (socket_path == nullptr || socket_path[0] == '\0') {
        std::fputs("MANGO_OVERLAY_TEST_SOCKET is required\n", stderr);
        return 1;
    }

    try {
        mango_overlay::client::Provider provider(
            "cpp-client-process-test/1.0", socket_path);
        mango_overlay_provider_info identity {};
        identity.struct_size = sizeof(identity);
        identity.application_id = "dev.mango-overlay.cpp-test";
        identity.instance_id = "primary";
        identity.display_name = "C++ Client Test";
        identity.canvas_width = 1280;
        identity.canvas_height = 800;
        identity.visibility = MANGO_OVERLAY_VISIBILITY_ALWAYS;
        provider.register_provider(identity);

        try {
            provider.upload_resource(99, {});
            std::fputs("an empty resource was accepted\n", stderr);
            return 1;
        } catch (const std::invalid_argument&) {
        }
        provider.upload_resource(100, png);

        mango_overlay_text_element text {};
        text.struct_size = sizeof(text);
        text.element_id = 1;
        text.z_index = 1;
        text.x = 20.0F;
        text.y = 24.0F;
        text.text = "C++ scene";
        text.font_size = 24.0F;
        text.color = { 1.0F, 1.0F, 1.0F, 1.0F };
        try {
            provider.upsert(text);
            std::fputs("an element outside a transaction was accepted\n", stderr);
            return 1;
        } catch (const mango_overlay::client::Error& error) {
            if (error.result() != MANGO_OVERLAY_INVALID_STATE) {
                throw;
            }
        }

        mango_overlay_rectangle_element panel {};
        panel.struct_size = sizeof(panel);
        panel.element_id = 2;
        panel.z_index = 0;
        panel.x = 8.0F;
        panel.y = 12.0F;
        panel.width = 320.0F;
        panel.height = 100.0F;
        panel.corner_radius = 8.0F;
        panel.color = { 0.02F, 0.03F, 0.04F, 0.9F };

        mango_overlay_image_element image {};
        image.struct_size = sizeof(image);
        image.element_id = 3;
        image.z_index = 2;
        image.x = 260.0F;
        image.y = 24.0F;
        image.width = 48.0F;
        image.height = 48.0F;
        image.resource_id = 100;
        image.tint = { 1.0F, 1.0F, 1.0F, 1.0F };

        provider.begin_transaction();
        provider.upsert(panel);
        provider.upsert(text);
        provider.upsert(image);
        provider.commit();

        provider.begin_transaction();
        provider.remove(3);
        provider.commit();
        provider.release_resource(100);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "C++ SDK process test failed: %s\n", error.what());
        return 1;
    }
    return 0;
}
