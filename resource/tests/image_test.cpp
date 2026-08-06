#include "mango_overlay/resource/image.hpp"

#include <cstdio>

#include <gif_lib.h>
#include <jpeglib.h>
#include <png.h>
#include <webp/encode.h>

#include <cstdlib>
#include <vector>

using namespace mango_overlay::resource;

namespace {

const std::uint8_t pixels[] = {
    255, 0, 0, 255,
    0, 255, 0, 255,
};

std::vector<std::uint8_t> make_png()
{
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

std::vector<std::uint8_t> make_jpeg()
{
    jpeg_compress_struct encoder {};
    jpeg_error_mgr error {};
    encoder.err = jpeg_std_error(&error);
    jpeg_create_compress(&encoder);
    unsigned char* memory = nullptr;
    unsigned long size = 0;
    jpeg_mem_dest(&encoder, &memory, &size);
    encoder.image_width = 2;
    encoder.image_height = 1;
    encoder.input_components = 3;
    encoder.in_color_space = JCS_RGB;
    jpeg_set_defaults(&encoder);
    jpeg_start_compress(&encoder, TRUE);
    std::uint8_t row[] = { 255, 0, 0, 0, 255, 0 };
    JSAMPROW row_pointer = row;
    jpeg_write_scanlines(&encoder, &row_pointer, 1);
    jpeg_finish_compress(&encoder);
    std::vector<std::uint8_t> output(memory, memory + size);
    std::free(memory);
    jpeg_destroy_compress(&encoder);
    return output;
}

std::vector<std::uint8_t> make_webp()
{
    std::uint8_t* memory = nullptr;
    const std::size_t size = WebPEncodeLosslessRGBA(pixels, 2, 1, 8, &memory);
    std::vector<std::uint8_t> output(memory, memory + size);
    WebPFree(memory);
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
    output.reserve(256);
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
        if (EGifPutExtension(
                gif, GRAPHICS_EXT_FUNC_CODE, sizeof(extension), extension)
                != GIF_OK
            || EGifPutImageDesc(gif, 0, 0, 2, 1, false, nullptr) != GIF_OK) {
            EGifCloseFile(gif, &error);
            GifFreeMapObject(map);
            return {};
        }
        GifPixelType row[] = {
            static_cast<GifPixelType>(frame),
            static_cast<GifPixelType>(1 - frame),
        };
        if (EGifPutLine(gif, row, 2) != GIF_OK) {
            EGifCloseFile(gif, &error);
            GifFreeMapObject(map);
            return {};
        }
    }
    EGifCloseFile(gif, &error);
    GifFreeMapObject(map);
    return output;
}

bool decoded_as(
    const std::vector<std::uint8_t>& encoded,
    ImageFormat format,
    std::size_t frames)
{
    const auto decoded = decode_image({ encoded.data(), encoded.size() });
    const auto* image = std::get_if<DecodedImage>(&decoded);
    return image != nullptr && image->format == format
        && image->width == 2 && image->height == 1
        && image->frame_count() == frames
        && image->rgba.size() == image->frame_stride() * frames;
}

} // namespace

int main()
{
    const auto png = make_png();
    const auto jpeg = make_jpeg();
    const auto webp = make_webp();
    const auto gif = make_gif();
    if (png.empty() || jpeg.empty() || webp.empty() || gif.empty()
        || !decoded_as(png, ImageFormat::png, 1)
        || !decoded_as(jpeg, ImageFormat::jpeg, 1)
        || !decoded_as(webp, ImageFormat::webp, 1)
        || !decoded_as(gif, ImageFormat::gif, 2)) {
        std::fputs("a supported image format did not decode\n", stderr);
        return 1;
    }

    const auto decoded_gif = std::get<DecodedImage>(
        decode_image({ gif.data(), gif.size() }));
    if (decoded_gif.frame_durations_ms[0] != 20
        || decoded_gif.frame_durations_ms[1] != 30
        || decoded_gif.rgba[0] == decoded_gif.rgba[decoded_gif.frame_stride()]) {
        std::fputs("GIF frames or timing were not preserved\n", stderr);
        return 1;
    }

    ImageLimits tight;
    tight.maximum_pixels_per_frame = 1;
    const auto oversized = decode_image({ png.data(), png.size() }, tight);
    const auto truncated = decode_image({ png.data(), 8 });
    const auto* oversized_error = std::get_if<DecodeError>(&oversized);
    const auto* truncated_error = std::get_if<DecodeError>(&truncated);
    if (oversized_error == nullptr
        || *oversized_error != DecodeError::resource_too_large
        || truncated_error == nullptr
        || *truncated_error != DecodeError::invalid_data) {
        std::fputs("invalid or oversized image input was accepted\n", stderr);
        return 1;
    }
    return 0;
}
