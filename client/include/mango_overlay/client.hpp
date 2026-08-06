#pragma once

#include "mango_overlay/client.h"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mango_overlay::client {

class Error : public std::runtime_error {
public:
    Error(mango_overlay_result result, const std::string& message)
        : std::runtime_error(message)
        , result_(result)
    {
    }

    mango_overlay_result result() const noexcept { return result_; }

private:
    mango_overlay_result result_;
};

class Provider {
public:
    explicit Provider(
        const std::string& client_version,
        const std::string& socket_path = {},
        std::uint32_t timeout_ms = 2000)
    {
        mango_overlay_client_config config {};
        config.struct_size = sizeof(config);
        config.socket_path = socket_path.empty() ? nullptr : socket_path.c_str();
        config.client_version = client_version.c_str();
        config.timeout_ms = timeout_ms;
        const auto result = mango_overlay_client_open(&config, &handle_);
        if (result != MANGO_OVERLAY_OK) {
            throw Error(result, "Could not connect to mango-overlayd");
        }
    }

    ~Provider() { mango_overlay_client_close(handle_); }

    Provider(const Provider&) = delete;
    Provider& operator=(const Provider&) = delete;

    Provider(Provider&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr))
    {
    }

    Provider& operator=(Provider&& other) noexcept
    {
        if (this != &other) {
            mango_overlay_client_close(handle_);
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    void register_provider(const mango_overlay_provider_info& provider)
    {
        check(mango_overlay_client_register_provider(handle_, &provider));
    }

    void begin_transaction()
    {
        check(mango_overlay_client_begin_transaction(handle_));
    }

    void upload_resource(std::uint64_t resource_id, const std::vector<std::uint8_t>& encoded)
    {
        if (encoded.empty()
            || encoded.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument("Resource size is outside the client ABI range");
        }
        check(mango_overlay_client_upload_resource(
            handle_,
            resource_id,
            encoded.data(),
            static_cast<std::uint32_t>(encoded.size())));
    }

    void upload_resource_fd(
        std::uint64_t resource_id,
        int descriptor,
        std::uint32_t encoded_size)
    {
        if (descriptor < 0 || encoded_size == 0) {
            throw std::invalid_argument("Resource descriptor and size are required");
        }
        check(mango_overlay_client_upload_resource_fd(
            handle_, resource_id, descriptor, encoded_size));
    }

    void release_resource(std::uint64_t resource_id)
    {
        check(mango_overlay_client_release_resource(handle_, resource_id));
    }

    void upsert(const mango_overlay_group_element& element)
    {
        check(mango_overlay_client_upsert_group(handle_, &element));
    }

    void upsert(const mango_overlay_text_element& element)
    {
        check(mango_overlay_client_upsert_text(handle_, &element));
    }

    void upsert(const mango_overlay_rectangle_element& element)
    {
        check(mango_overlay_client_upsert_rectangle(handle_, &element));
    }

    void upsert(const mango_overlay_line_element& element)
    {
        check(mango_overlay_client_upsert_line(handle_, &element));
    }

    void upsert(const mango_overlay_polyline_element& element)
    {
        check(mango_overlay_client_upsert_polyline(handle_, &element));
    }

    void upsert(const mango_overlay_circle_element& element)
    {
        check(mango_overlay_client_upsert_circle(handle_, &element));
    }

    void upsert(const mango_overlay_image_element& element)
    {
        check(mango_overlay_client_upsert_image(handle_, &element));
    }

    void upsert(const mango_overlay_gif_element& element)
    {
        check(mango_overlay_client_upsert_gif(handle_, &element));
    }

    void remove(std::uint64_t element_id)
    {
        check(mango_overlay_client_remove_element(handle_, element_id));
    }

    void commit() { check(mango_overlay_client_commit_transaction(handle_)); }
    void abort() { check(mango_overlay_client_abort_transaction(handle_)); }

private:
    void check(mango_overlay_result result) const
    {
        if (result != MANGO_OVERLAY_OK) {
            throw Error(result, mango_overlay_client_last_error(handle_));
        }
    }

    mango_overlay_client* handle_ = nullptr;
};

} // namespace mango_overlay::client
