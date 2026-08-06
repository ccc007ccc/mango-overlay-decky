#include "mango_overlay/resource/image.hpp"

#include <cstdio>

#include <gif_lib.h>
#include <jpeglib.h>
#include <png.h>
#include <webp/decode.h>

#include <algorithm>
#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace mango_overlay::resource {

namespace {

bool starts_with(EncodedView encoded, const std::uint8_t* prefix, std::size_t size)
{
    return encoded.size >= size && std::memcmp(encoded.data, prefix, size) == 0;
}

bool allowed_dimensions(
    std::uint32_t width,
    std::uint32_t height,
    std::size_t frames,
    const ImageLimits& limits)
{
    if (width == 0 || height == 0 || width > limits.maximum_dimension
        || height > limits.maximum_dimension || frames == 0
        || frames > limits.maximum_frames) {
        return false;
    }
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    if (pixels > limits.maximum_pixels_per_frame
        || pixels > std::numeric_limits<std::size_t>::max() / 4) {
        return false;
    }
    const std::size_t frame_bytes = pixels * 4;
    return frames <= limits.maximum_decoded_bytes / frame_bytes;
}

DecodeResult decode_png(EncodedView encoded, const ImageLimits& limits)
{
    png_image image {};
    image.version = PNG_IMAGE_VERSION;
    if (png_image_begin_read_from_memory(&image, encoded.data, encoded.size) == 0) {
        return DecodeError::invalid_data;
    }
    if (!allowed_dimensions(image.width, image.height, 1, limits)) {
        png_image_free(&image);
        return DecodeError::resource_too_large;
    }
    image.format = PNG_FORMAT_RGBA;
    DecodedImage result {
        ImageFormat::png,
        image.width,
        image.height,
        { 0 },
        std::vector<std::uint8_t>(PNG_IMAGE_SIZE(image)),
    };
    if (png_image_finish_read(
            &image, nullptr, result.rgba.data(), 0, nullptr)
        == 0) {
        png_image_free(&image);
        return DecodeError::invalid_data;
    }
    png_image_free(&image);
    return result;
}

struct JpegErrorContext {
    jpeg_error_mgr manager;
    std::jmp_buf jump;
    std::uint8_t* pixels = nullptr;
    std::uint8_t* row = nullptr;
};

void jpeg_error_exit(j_common_ptr common)
{
    auto* context = reinterpret_cast<JpegErrorContext*>(common->err);
    std::longjmp(context->jump, 1);
}

DecodeResult decode_jpeg(EncodedView encoded, const ImageLimits& limits)
{
    jpeg_decompress_struct decoder {};
    JpegErrorContext error {};
    decoder.err = jpeg_std_error(&error.manager);
    error.manager.error_exit = jpeg_error_exit;
    if (setjmp(error.jump) != 0) {
        std::free(error.pixels);
        std::free(error.row);
        jpeg_destroy_decompress(&decoder);
        return DecodeError::invalid_data;
    }

    jpeg_create_decompress(&decoder);
    jpeg_mem_src(
        &decoder,
        const_cast<unsigned char*>(encoded.data),
        static_cast<unsigned long>(encoded.size));
    if (jpeg_read_header(&decoder, TRUE) != JPEG_HEADER_OK
        || !allowed_dimensions(decoder.image_width, decoder.image_height, 1, limits)) {
        const bool too_large = decoder.image_width > limits.maximum_dimension
            || decoder.image_height > limits.maximum_dimension
            || static_cast<std::size_t>(decoder.image_width) * decoder.image_height
                > limits.maximum_pixels_per_frame;
        jpeg_destroy_decompress(&decoder);
        return too_large ? DecodeError::resource_too_large : DecodeError::invalid_data;
    }

    decoder.out_color_space = JCS_RGB;
    jpeg_start_decompress(&decoder);
    if (decoder.output_components != 3
        || !allowed_dimensions(decoder.output_width, decoder.output_height, 1, limits)) {
        jpeg_destroy_decompress(&decoder);
        return DecodeError::invalid_data;
    }
    const std::size_t pixels
        = static_cast<std::size_t>(decoder.output_width) * decoder.output_height;
    error.pixels = static_cast<std::uint8_t*>(std::malloc(pixels * 4));
    if (error.pixels == nullptr) {
        jpeg_destroy_decompress(&decoder);
        return DecodeError::resource_too_large;
    }
    error.row = static_cast<std::uint8_t*>(
        std::malloc(static_cast<std::size_t>(decoder.output_width) * 3));
    if (error.row == nullptr) {
        std::free(error.pixels);
        error.pixels = nullptr;
        jpeg_destroy_decompress(&decoder);
        return DecodeError::resource_too_large;
    }
    while (decoder.output_scanline < decoder.output_height) {
        JSAMPROW row_pointer = error.row;
        const auto output_row = decoder.output_scanline;
        if (jpeg_read_scanlines(&decoder, &row_pointer, 1) != 1) {
            std::free(error.pixels);
            error.pixels = nullptr;
            std::free(error.row);
            error.row = nullptr;
            jpeg_destroy_decompress(&decoder);
            return DecodeError::invalid_data;
        }
        for (std::uint32_t x = 0; x < decoder.output_width; ++x) {
            const std::size_t source = static_cast<std::size_t>(x) * 3;
            const std::size_t target
                = (static_cast<std::size_t>(output_row) * decoder.output_width + x) * 4;
            error.pixels[target] = error.row[source];
            error.pixels[target + 1] = error.row[source + 1];
            error.pixels[target + 2] = error.row[source + 2];
            error.pixels[target + 3] = 255;
        }
    }
    jpeg_finish_decompress(&decoder);
    const std::uint32_t width = decoder.output_width;
    const std::uint32_t height = decoder.output_height;
    jpeg_destroy_decompress(&decoder);
    std::free(error.row);
    error.row = nullptr;
    DecodedImage result {
        ImageFormat::jpeg,
        width,
        height,
        { 0 },
        std::vector<std::uint8_t>(error.pixels, error.pixels + pixels * 4),
    };
    std::free(error.pixels);
    error.pixels = nullptr;
    return result;
}

DecodeResult decode_webp(EncodedView encoded, const ImageLimits& limits)
{
    WebPBitstreamFeatures features {};
    if (WebPGetFeatures(encoded.data, encoded.size, &features) != VP8_STATUS_OK) {
        return DecodeError::invalid_data;
    }
    if (features.has_animation != 0) {
        return DecodeError::invalid_data;
    }
    if (!allowed_dimensions(features.width, features.height, 1, limits)) {
        return DecodeError::resource_too_large;
    }
    DecodedImage result {
        ImageFormat::webp,
        static_cast<std::uint32_t>(features.width),
        static_cast<std::uint32_t>(features.height),
        { 0 },
        std::vector<std::uint8_t>(
            static_cast<std::size_t>(features.width) * features.height * 4),
    };
    if (WebPDecodeRGBAInto(
            encoded.data,
            encoded.size,
            result.rgba.data(),
            result.rgba.size(),
            features.width * 4)
        == nullptr) {
        return DecodeError::invalid_data;
    }
    return result;
}

struct GifReader {
    EncodedView encoded;
    std::size_t offset = 0;
};

int read_gif(GifFileType* gif, GifByteType* output, int requested)
{
    auto* reader = static_cast<GifReader*>(gif->UserData);
    if (reader == nullptr || requested < 0) {
        return 0;
    }
    const std::size_t available = reader->encoded.size - reader->offset;
    const std::size_t count = std::min<std::size_t>(available, requested);
    if (count != 0) {
        std::memcpy(output, reader->encoded.data + reader->offset, count);
        reader->offset += count;
    }
    return static_cast<int>(count);
}

void clear_gif_rectangle(
    std::vector<std::uint8_t>& canvas,
    std::uint32_t canvas_width,
    const GifImageDesc& rectangle)
{
    for (int y = 0; y < rectangle.Height; ++y) {
        for (int x = 0; x < rectangle.Width; ++x) {
            const std::size_t target
                = (static_cast<std::size_t>(rectangle.Top + y) * canvas_width
                      + rectangle.Left + x)
                * 4;
            std::fill_n(canvas.begin() + static_cast<std::ptrdiff_t>(target), 4, 0);
        }
    }
}

DecodeResult decode_gif(EncodedView encoded, const ImageLimits& limits)
{
    GifReader reader { encoded };
    int error = 0;
    GifFileType* gif = DGifOpen(&reader, read_gif, &error);
    if (gif == nullptr) {
        return DecodeError::invalid_data;
    }
    if (DGifSlurp(gif) != GIF_OK) {
        DGifCloseFile(gif, &error);
        return DecodeError::invalid_data;
    }
    const std::uint32_t width = static_cast<std::uint32_t>(gif->SWidth);
    const std::uint32_t height = static_cast<std::uint32_t>(gif->SHeight);
    const std::size_t frame_count = static_cast<std::size_t>(gif->ImageCount);
    if (!allowed_dimensions(width, height, frame_count, limits)) {
        DGifCloseFile(gif, &error);
        return DecodeError::resource_too_large;
    }

    const std::size_t frame_stride = static_cast<std::size_t>(width) * height * 4;
    DecodedImage result {
        ImageFormat::gif,
        width,
        height,
        {},
        {},
    };
    result.frame_durations_ms.reserve(frame_count);
    result.rgba.reserve(frame_stride * frame_count);
    std::vector<std::uint8_t> canvas(frame_stride, 0);
    std::vector<std::uint8_t> previous_canvas;
    int previous_disposal = DISPOSAL_UNSPECIFIED;
    GifImageDesc previous_rectangle {};

    for (int frame_index = 0; frame_index < gif->ImageCount; ++frame_index) {
        if (frame_index != 0) {
            if (previous_disposal == DISPOSE_BACKGROUND) {
                clear_gif_rectangle(canvas, width, previous_rectangle);
            } else if (previous_disposal == DISPOSE_PREVIOUS) {
                canvas = previous_canvas;
            }
        }

        const SavedImage& frame = gif->SavedImages[frame_index];
        const auto& rectangle = frame.ImageDesc;
        if (rectangle.Left < 0 || rectangle.Top < 0 || rectangle.Width <= 0
            || rectangle.Height <= 0
            || rectangle.Left + rectangle.Width > gif->SWidth
            || rectangle.Top + rectangle.Height > gif->SHeight
            || frame.RasterBits == nullptr) {
            DGifCloseFile(gif, &error);
            return DecodeError::invalid_data;
        }
        const ColorMapObject* colors
            = rectangle.ColorMap != nullptr ? rectangle.ColorMap : gif->SColorMap;
        if (colors == nullptr) {
            DGifCloseFile(gif, &error);
            return DecodeError::invalid_data;
        }

        GraphicsControlBlock control {
            DISPOSAL_UNSPECIFIED, false, 0, NO_TRANSPARENT_COLOR };
        DGifSavedExtensionToGCB(gif, frame_index, &control);
        previous_canvas = canvas;
        for (int y = 0; y < rectangle.Height; ++y) {
            for (int x = 0; x < rectangle.Width; ++x) {
                const int palette_index
                    = frame.RasterBits[y * rectangle.Width + x];
                if (palette_index == control.TransparentColor) {
                    continue;
                }
                if (palette_index < 0 || palette_index >= colors->ColorCount) {
                    DGifCloseFile(gif, &error);
                    return DecodeError::invalid_data;
                }
                const auto& color = colors->Colors[palette_index];
                const std::size_t target
                    = (static_cast<std::size_t>(rectangle.Top + y) * width
                          + rectangle.Left + x)
                    * 4;
                canvas[target] = color.Red;
                canvas[target + 1] = color.Green;
                canvas[target + 2] = color.Blue;
                canvas[target + 3] = 255;
            }
        }
        result.rgba.insert(result.rgba.end(), canvas.begin(), canvas.end());
        result.frame_durations_ms.push_back(
            std::max<std::uint32_t>(10, static_cast<std::uint32_t>(control.DelayTime) * 10));
        previous_disposal = control.DisposalMode;
        previous_rectangle = rectangle;
    }

    DGifCloseFile(gif, &error);
    return result;
}

} // namespace

std::size_t DecodedImage::frame_count() const
{
    return frame_durations_ms.size();
}

std::size_t DecodedImage::frame_stride() const
{
    return static_cast<std::size_t>(width) * height * 4;
}

DecodeResult decode_image(EncodedView encoded, const ImageLimits& limits)
{
    if (encoded.data == nullptr || encoded.size == 0) {
        return DecodeError::invalid_data;
    }
    if (encoded.size > limits.maximum_encoded_bytes) {
        return DecodeError::resource_too_large;
    }
    static constexpr std::uint8_t png_magic[] = {
        0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    static constexpr std::uint8_t jpeg_magic[] = { 0xff, 0xd8 };
    static constexpr std::uint8_t gif87_magic[] = { 'G', 'I', 'F', '8', '7', 'a' };
    static constexpr std::uint8_t gif89_magic[] = { 'G', 'I', 'F', '8', '9', 'a' };

    if (starts_with(encoded, png_magic, sizeof(png_magic))) {
        return decode_png(encoded, limits);
    }
    if (starts_with(encoded, jpeg_magic, sizeof(jpeg_magic))) {
        return decode_jpeg(encoded, limits);
    }
    if (starts_with(encoded, gif87_magic, sizeof(gif87_magic))
        || starts_with(encoded, gif89_magic, sizeof(gif89_magic))) {
        return decode_gif(encoded, limits);
    }
    if (encoded.size >= 12 && std::memcmp(encoded.data, "RIFF", 4) == 0
        && std::memcmp(encoded.data + 8, "WEBP", 4) == 0) {
        return decode_webp(encoded, limits);
    }
    return DecodeError::unsupported_format;
}

} // namespace mango_overlay::resource
