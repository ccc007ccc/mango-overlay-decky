#ifndef MANGO_OVERLAY_CLIENT_H
#define MANGO_OVERLAY_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MANGO_OVERLAY_CLIENT_ABI_VERSION 1u

typedef struct mango_overlay_client mango_overlay_client;

typedef enum mango_overlay_result {
    MANGO_OVERLAY_OK = 0,
    MANGO_OVERLAY_INVALID_ARGUMENT = 1,
    MANGO_OVERLAY_INVALID_STATE = 2,
    MANGO_OVERLAY_CONNECTION_FAILED = 3,
    MANGO_OVERLAY_IO_ERROR = 4,
    MANGO_OVERLAY_PROTOCOL_ERROR = 5,
    MANGO_OVERLAY_SERVER_REJECTED = 6,
    MANGO_OVERLAY_OUT_OF_MEMORY = 7
} mango_overlay_result;

typedef enum mango_overlay_visibility {
    MANGO_OVERLAY_VISIBILITY_GAME_ONLY = 0,
    MANGO_OVERLAY_VISIBILITY_STEAM_ONLY = 1,
    MANGO_OVERLAY_VISIBILITY_ALWAYS = 2
} mango_overlay_visibility;

typedef struct mango_overlay_color {
    float red;
    float green;
    float blue;
    float alpha;
} mango_overlay_color;

typedef struct mango_overlay_vec2 {
    float x;
    float y;
} mango_overlay_vec2;

typedef enum mango_overlay_anchor {
    MANGO_OVERLAY_ANCHOR_TOP_LEFT = 0,
    MANGO_OVERLAY_ANCHOR_TOP_CENTER = 1,
    MANGO_OVERLAY_ANCHOR_TOP_RIGHT = 2,
    MANGO_OVERLAY_ANCHOR_CENTER_LEFT = 3,
    MANGO_OVERLAY_ANCHOR_CENTER = 4,
    MANGO_OVERLAY_ANCHOR_CENTER_RIGHT = 5,
    MANGO_OVERLAY_ANCHOR_BOTTOM_LEFT = 6,
    MANGO_OVERLAY_ANCHOR_BOTTOM_CENTER = 7,
    MANGO_OVERLAY_ANCHOR_BOTTOM_RIGHT = 8
} mango_overlay_anchor;

typedef struct mango_overlay_clip_rect {
    float x;
    float y;
    float width;
    float height;
} mango_overlay_clip_rect;

typedef struct mango_overlay_element_layout {
    uint32_t struct_size;
    uint64_t parent_id;
    mango_overlay_vec2 translation;
    mango_overlay_vec2 scale;
    float rotation_degrees;
    float opacity;
    mango_overlay_anchor anchor;
    uint8_t hidden;
    uint8_t clip_enabled;
    uint16_t reserved;
    mango_overlay_clip_rect clip;
} mango_overlay_element_layout;

#define MANGO_OVERLAY_ELEMENT_LAYOUT_IDENTITY                                   \
    {                                                                           \
        sizeof(mango_overlay_element_layout), 0, { 0.0F, 0.0F },               \
            { 1.0F, 1.0F }, 0.0F, 1.0F, MANGO_OVERLAY_ANCHOR_TOP_LEFT, 0, 0,  \
            0, { 0.0F, 0.0F, 0.0F, 0.0F }                                     \
    }

typedef struct mango_overlay_client_config {
    uint32_t struct_size;
    const char* socket_path;
    const char* client_version;
    uint32_t timeout_ms;
} mango_overlay_client_config;

typedef struct mango_overlay_provider_info {
    uint32_t struct_size;
    const char* application_id;
    const char* instance_id;
    const char* display_name;
    uint16_t canvas_width;
    uint16_t canvas_height;
    mango_overlay_visibility visibility;
} mango_overlay_provider_info;

typedef struct mango_overlay_text_element {
    uint32_t struct_size;
    uint64_t element_id;
    int32_t z_index;
    float x;
    float y;
    const char* text;
    float font_size;
    mango_overlay_color color;
    const mango_overlay_element_layout* layout;
} mango_overlay_text_element;

typedef struct mango_overlay_rectangle_element {
    uint32_t struct_size;
    uint64_t element_id;
    int32_t z_index;
    float x;
    float y;
    float width;
    float height;
    float corner_radius;
    mango_overlay_color color;
    const mango_overlay_element_layout* layout;
} mango_overlay_rectangle_element;

typedef struct mango_overlay_line_element {
    uint32_t struct_size;
    uint64_t element_id;
    int32_t z_index;
    mango_overlay_vec2 start;
    mango_overlay_vec2 end;
    float thickness;
    mango_overlay_color color;
    const mango_overlay_element_layout* layout;
} mango_overlay_line_element;

typedef struct mango_overlay_polyline_element {
    uint32_t struct_size;
    uint64_t element_id;
    int32_t z_index;
    const mango_overlay_vec2* points;
    uint32_t point_count;
    float thickness;
    mango_overlay_color color;
    const mango_overlay_element_layout* layout;
} mango_overlay_polyline_element;

typedef struct mango_overlay_circle_element {
    uint32_t struct_size;
    uint64_t element_id;
    int32_t z_index;
    mango_overlay_vec2 center;
    float radius;
    mango_overlay_color color;
    const mango_overlay_element_layout* layout;
} mango_overlay_circle_element;

typedef struct mango_overlay_group_element {
    uint32_t struct_size;
    uint64_t element_id;
    int32_t z_index;
    const mango_overlay_element_layout* layout;
} mango_overlay_group_element;

typedef struct mango_overlay_image_element {
    uint32_t struct_size;
    uint64_t element_id;
    int32_t z_index;
    float x;
    float y;
    float width;
    float height;
    uint64_t resource_id;
    mango_overlay_color tint;
    const mango_overlay_element_layout* layout;
} mango_overlay_image_element;

typedef struct mango_overlay_gif_element {
    uint32_t struct_size;
    uint64_t element_id;
    int32_t z_index;
    float x;
    float y;
    float width;
    float height;
    uint64_t resource_id;
    mango_overlay_color tint;
    float playback_rate;
    uint32_t frame_index;
    uint8_t paused;
    uint8_t reserved[3];
    const mango_overlay_element_layout* layout;
} mango_overlay_gif_element;

uint32_t mango_overlay_client_abi_version(void);

mango_overlay_result mango_overlay_client_open(
    const mango_overlay_client_config* config,
    mango_overlay_client** out_client);

void mango_overlay_client_close(mango_overlay_client* client);

mango_overlay_result mango_overlay_client_register_provider(
    mango_overlay_client* client,
    const mango_overlay_provider_info* provider);

mango_overlay_result mango_overlay_client_upload_resource(
    mango_overlay_client* client,
    uint64_t resource_id,
    const void* encoded_data,
    uint32_t encoded_size);

mango_overlay_result mango_overlay_client_upload_resource_fd(
    mango_overlay_client* client,
    uint64_t resource_id,
    int descriptor,
    uint32_t encoded_size);

mango_overlay_result mango_overlay_client_release_resource(
    mango_overlay_client* client,
    uint64_t resource_id);

mango_overlay_result mango_overlay_client_begin_transaction(
    mango_overlay_client* client);

mango_overlay_result mango_overlay_client_upsert_group(
    mango_overlay_client* client,
    const mango_overlay_group_element* element);

mango_overlay_result mango_overlay_client_upsert_text(
    mango_overlay_client* client,
    const mango_overlay_text_element* element);

mango_overlay_result mango_overlay_client_upsert_rectangle(
    mango_overlay_client* client,
    const mango_overlay_rectangle_element* element);

mango_overlay_result mango_overlay_client_upsert_line(
    mango_overlay_client* client,
    const mango_overlay_line_element* element);

mango_overlay_result mango_overlay_client_upsert_polyline(
    mango_overlay_client* client,
    const mango_overlay_polyline_element* element);

mango_overlay_result mango_overlay_client_upsert_circle(
    mango_overlay_client* client,
    const mango_overlay_circle_element* element);

mango_overlay_result mango_overlay_client_upsert_image(
    mango_overlay_client* client,
    const mango_overlay_image_element* element);

mango_overlay_result mango_overlay_client_upsert_gif(
    mango_overlay_client* client,
    const mango_overlay_gif_element* element);

mango_overlay_result mango_overlay_client_remove_element(
    mango_overlay_client* client,
    uint64_t element_id);

mango_overlay_result mango_overlay_client_commit_transaction(
    mango_overlay_client* client);

mango_overlay_result mango_overlay_client_abort_transaction(
    mango_overlay_client* client);

const char* mango_overlay_client_last_error(const mango_overlay_client* client);

#ifdef __cplusplus
}
#endif

#endif
