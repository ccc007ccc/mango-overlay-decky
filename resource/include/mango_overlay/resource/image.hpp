#pragma once

#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

namespace mango_overlay::resource {

enum class ImageFormat : std::uint8_t {
    png,
    jpeg,
    webp,
    gif,
};

struct ImageLimits {
    std::size_t maximum_encoded_bytes = 8 * 1024 * 1024;
    std::uint32_t maximum_dimension = 4096;
    std::size_t maximum_pixels_per_frame = 4 * 1024 * 1024;
    std::size_t maximum_frames = 120;
    std::size_t maximum_decoded_bytes = 32 * 1024 * 1024;
};

struct EncodedView {
    const std::uint8_t* data;
    std::size_t size;
};

struct DecodedImage {
    ImageFormat format;
    std::uint32_t width;
    std::uint32_t height;
    std::vector<std::uint32_t> frame_durations_ms;
    std::vector<std::uint8_t> rgba;

    std::size_t frame_count() const;
    std::size_t frame_stride() const;
};

enum class DecodeError {
    unsupported_format,
    invalid_data,
    resource_too_large,
};

using DecodeResult = std::variant<DecodedImage, DecodeError>;

DecodeResult decode_image(EncodedView encoded, const ImageLimits& limits = {});

} // namespace mango_overlay::resource
