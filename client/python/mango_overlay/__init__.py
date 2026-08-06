"""Python bindings for the Mango Overlay provider client."""

from __future__ import annotations

import contextlib
import ctypes
import ctypes.util
import enum
import os
from collections.abc import Iterator, Sequence
from dataclasses import dataclass


class Result(enum.IntEnum):
    OK = 0
    INVALID_ARGUMENT = 1
    INVALID_STATE = 2
    CONNECTION_FAILED = 3
    IO_ERROR = 4
    PROTOCOL_ERROR = 5
    SERVER_REJECTED = 6
    OUT_OF_MEMORY = 7


class Visibility(enum.IntEnum):
    GAME_ONLY = 0
    STEAM_ONLY = 1
    ALWAYS = 2


class Anchor(enum.IntEnum):
    TOP_LEFT = 0
    TOP_CENTER = 1
    TOP_RIGHT = 2
    CENTER_LEFT = 3
    CENTER = 4
    CENTER_RIGHT = 5
    BOTTOM_LEFT = 6
    BOTTOM_CENTER = 7
    BOTTOM_RIGHT = 8


@dataclass(frozen=True)
class ClipRect:
    x: float
    y: float
    width: float
    height: float


@dataclass(frozen=True)
class Layout:
    parent_id: int = 0
    translation: tuple[float, float] = (0.0, 0.0)
    scale: tuple[float, float] = (1.0, 1.0)
    rotation_degrees: float = 0.0
    opacity: float = 1.0
    anchor: Anchor = Anchor.TOP_LEFT
    clip: ClipRect | None = None
    hidden: bool = False


class OverlayError(RuntimeError):
    def __init__(self, result: Result, message: str) -> None:
        super().__init__(message)
        self.result = result


class _Color(ctypes.Structure):
    _fields_ = [
        ("red", ctypes.c_float),
        ("green", ctypes.c_float),
        ("blue", ctypes.c_float),
        ("alpha", ctypes.c_float),
    ]


class _Vec2(ctypes.Structure):
    _fields_ = [("x", ctypes.c_float), ("y", ctypes.c_float)]


class _ClipRect(ctypes.Structure):
    _fields_ = [
        ("x", ctypes.c_float),
        ("y", ctypes.c_float),
        ("width", ctypes.c_float),
        ("height", ctypes.c_float),
    ]


class _ElementLayout(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("parent_id", ctypes.c_uint64),
        ("translation", _Vec2),
        ("scale", _Vec2),
        ("rotation_degrees", ctypes.c_float),
        ("opacity", ctypes.c_float),
        ("anchor", ctypes.c_int),
        ("hidden", ctypes.c_uint8),
        ("clip_enabled", ctypes.c_uint8),
        ("reserved", ctypes.c_uint16),
        ("clip", _ClipRect),
    ]


class _ClientConfig(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("socket_path", ctypes.c_char_p),
        ("client_version", ctypes.c_char_p),
        ("timeout_ms", ctypes.c_uint32),
    ]


class _ProviderInfo(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("application_id", ctypes.c_char_p),
        ("instance_id", ctypes.c_char_p),
        ("display_name", ctypes.c_char_p),
        ("canvas_width", ctypes.c_uint16),
        ("canvas_height", ctypes.c_uint16),
        ("visibility", ctypes.c_int),
    ]


class _TextElement(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("element_id", ctypes.c_uint64),
        ("z_index", ctypes.c_int32),
        ("x", ctypes.c_float),
        ("y", ctypes.c_float),
        ("text", ctypes.c_char_p),
        ("font_size", ctypes.c_float),
        ("color", _Color),
        ("layout", ctypes.POINTER(_ElementLayout)),
    ]


class _RectangleElement(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("element_id", ctypes.c_uint64),
        ("z_index", ctypes.c_int32),
        ("x", ctypes.c_float),
        ("y", ctypes.c_float),
        ("width", ctypes.c_float),
        ("height", ctypes.c_float),
        ("corner_radius", ctypes.c_float),
        ("color", _Color),
        ("layout", ctypes.POINTER(_ElementLayout)),
    ]


class _LineElement(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("element_id", ctypes.c_uint64),
        ("z_index", ctypes.c_int32),
        ("start", _Vec2),
        ("end", _Vec2),
        ("thickness", ctypes.c_float),
        ("color", _Color),
        ("layout", ctypes.POINTER(_ElementLayout)),
    ]


class _PolylineElement(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("element_id", ctypes.c_uint64),
        ("z_index", ctypes.c_int32),
        ("points", ctypes.POINTER(_Vec2)),
        ("point_count", ctypes.c_uint32),
        ("thickness", ctypes.c_float),
        ("color", _Color),
        ("layout", ctypes.POINTER(_ElementLayout)),
    ]


class _CircleElement(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("element_id", ctypes.c_uint64),
        ("z_index", ctypes.c_int32),
        ("center", _Vec2),
        ("radius", ctypes.c_float),
        ("color", _Color),
        ("layout", ctypes.POINTER(_ElementLayout)),
    ]


class _GroupElement(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("element_id", ctypes.c_uint64),
        ("z_index", ctypes.c_int32),
        ("layout", ctypes.POINTER(_ElementLayout)),
    ]


class _ImageElement(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("element_id", ctypes.c_uint64),
        ("z_index", ctypes.c_int32),
        ("x", ctypes.c_float),
        ("y", ctypes.c_float),
        ("width", ctypes.c_float),
        ("height", ctypes.c_float),
        ("resource_id", ctypes.c_uint64),
        ("tint", _Color),
        ("layout", ctypes.POINTER(_ElementLayout)),
    ]


class _GifElement(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("element_id", ctypes.c_uint64),
        ("z_index", ctypes.c_int32),
        ("x", ctypes.c_float),
        ("y", ctypes.c_float),
        ("width", ctypes.c_float),
        ("height", ctypes.c_float),
        ("resource_id", ctypes.c_uint64),
        ("tint", _Color),
        ("playback_rate", ctypes.c_float),
        ("frame_index", ctypes.c_uint32),
        ("paused", ctypes.c_uint8),
        ("reserved", ctypes.c_uint8 * 3),
        ("layout", ctypes.POINTER(_ElementLayout)),
    ]


def _encode(value: str) -> bytes:
    return value.encode("utf-8")


def _load_library(path: str | None) -> ctypes.CDLL:
    selected = path or os.environ.get("MANGO_OVERLAY_CLIENT_LIBRARY")
    if not selected:
        selected = ctypes.util.find_library("mango-overlay-client")
    if not selected:
        raise RuntimeError("libmango-overlay-client.so was not found")

    library = ctypes.CDLL(selected)
    handle_pointer = ctypes.POINTER(ctypes.c_void_p)
    library.mango_overlay_client_open.argtypes = [
        ctypes.POINTER(_ClientConfig),
        handle_pointer,
    ]
    library.mango_overlay_client_open.restype = ctypes.c_int
    library.mango_overlay_client_close.argtypes = [ctypes.c_void_p]
    library.mango_overlay_client_close.restype = None
    library.mango_overlay_client_register_provider.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(_ProviderInfo),
    ]
    library.mango_overlay_client_register_provider.restype = ctypes.c_int
    library.mango_overlay_client_upload_resource.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint64,
        ctypes.c_void_p,
        ctypes.c_uint32,
    ]
    library.mango_overlay_client_upload_resource.restype = ctypes.c_int
    library.mango_overlay_client_upload_resource_fd.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint64,
        ctypes.c_int,
        ctypes.c_uint32,
    ]
    library.mango_overlay_client_upload_resource_fd.restype = ctypes.c_int
    library.mango_overlay_client_release_resource.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint64,
    ]
    library.mango_overlay_client_release_resource.restype = ctypes.c_int
    library.mango_overlay_client_begin_transaction.argtypes = [ctypes.c_void_p]
    library.mango_overlay_client_begin_transaction.restype = ctypes.c_int
    library.mango_overlay_client_upsert_group.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(_GroupElement),
    ]
    library.mango_overlay_client_upsert_group.restype = ctypes.c_int
    library.mango_overlay_client_upsert_text.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(_TextElement),
    ]
    library.mango_overlay_client_upsert_text.restype = ctypes.c_int
    library.mango_overlay_client_upsert_rectangle.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(_RectangleElement),
    ]
    library.mango_overlay_client_upsert_rectangle.restype = ctypes.c_int
    library.mango_overlay_client_upsert_line.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(_LineElement),
    ]
    library.mango_overlay_client_upsert_line.restype = ctypes.c_int
    library.mango_overlay_client_upsert_polyline.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(_PolylineElement),
    ]
    library.mango_overlay_client_upsert_polyline.restype = ctypes.c_int
    library.mango_overlay_client_upsert_circle.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(_CircleElement),
    ]
    library.mango_overlay_client_upsert_circle.restype = ctypes.c_int
    library.mango_overlay_client_upsert_image.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(_ImageElement),
    ]
    library.mango_overlay_client_upsert_image.restype = ctypes.c_int
    library.mango_overlay_client_upsert_gif.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(_GifElement),
    ]
    library.mango_overlay_client_upsert_gif.restype = ctypes.c_int
    library.mango_overlay_client_remove_element.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint64,
    ]
    library.mango_overlay_client_remove_element.restype = ctypes.c_int
    library.mango_overlay_client_commit_transaction.argtypes = [ctypes.c_void_p]
    library.mango_overlay_client_commit_transaction.restype = ctypes.c_int
    library.mango_overlay_client_abort_transaction.argtypes = [ctypes.c_void_p]
    library.mango_overlay_client_abort_transaction.restype = ctypes.c_int
    library.mango_overlay_client_last_error.argtypes = [ctypes.c_void_p]
    library.mango_overlay_client_last_error.restype = ctypes.c_char_p
    return library


def _native_layout(
    layout: Layout | None,
) -> tuple[_ElementLayout | None, ctypes.POINTER(_ElementLayout) | None]:
    if layout is None:
        return None, None
    clip = layout.clip
    native = _ElementLayout(
        ctypes.sizeof(_ElementLayout),
        layout.parent_id,
        _Vec2(*layout.translation),
        _Vec2(*layout.scale),
        layout.rotation_degrees,
        layout.opacity,
        int(layout.anchor),
        int(layout.hidden),
        int(clip is not None),
        0,
        _ClipRect(0.0, 0.0, 0.0, 0.0)
        if clip is None
        else _ClipRect(clip.x, clip.y, clip.width, clip.height),
    )
    return native, ctypes.pointer(native)


class Provider:
    """A single-threaded retained-scene provider connection."""

    def __init__(
        self,
        client_version: str,
        *,
        socket_path: str | None = None,
        timeout_ms: int = 2000,
        library_path: str | None = None,
    ) -> None:
        self._library = _load_library(library_path)
        self._handle = ctypes.c_void_p()
        config = _ClientConfig(
            ctypes.sizeof(_ClientConfig),
            None if socket_path is None else _encode(socket_path),
            _encode(client_version),
            timeout_ms,
        )
        result = Result(
            self._library.mango_overlay_client_open(
                ctypes.byref(config), ctypes.byref(self._handle)
            )
        )
        if result is not Result.OK:
            raise OverlayError(result, "Could not connect to mango-overlayd")

    def close(self) -> None:
        if self._handle:
            self._library.mango_overlay_client_close(self._handle)
            self._handle = ctypes.c_void_p()

    def __enter__(self) -> Provider:
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass

    def _check(self, raw_result: int) -> None:
        result = Result(raw_result)
        if result is Result.OK:
            return
        message = self._library.mango_overlay_client_last_error(self._handle)
        detail = message.decode("utf-8", errors="replace") if message else result.name
        raise OverlayError(result, detail)

    def register(
        self,
        application_id: str,
        instance_id: str,
        display_name: str,
        *,
        canvas_width: int = 1280,
        canvas_height: int = 800,
        visibility: Visibility = Visibility.GAME_ONLY,
    ) -> None:
        provider = _ProviderInfo(
            ctypes.sizeof(_ProviderInfo),
            _encode(application_id),
            _encode(instance_id),
            _encode(display_name),
            canvas_width,
            canvas_height,
            int(visibility),
        )
        self._check(
            self._library.mango_overlay_client_register_provider(
                self._handle, ctypes.byref(provider)
            )
        )

    def begin(self) -> None:
        self._check(self._library.mango_overlay_client_begin_transaction(self._handle))

    def upload_resource(self, resource_id: int, encoded: bytes) -> None:
        if not encoded:
            raise ValueError("encoded resource must not be empty")
        data = ctypes.create_string_buffer(encoded)
        self._check(
            self._library.mango_overlay_client_upload_resource(
                self._handle, resource_id, data, len(encoded)
            )
        )

    def upload_resource_fd(
        self, resource_id: int, descriptor: int, encoded_size: int
    ) -> None:
        self._check(
            self._library.mango_overlay_client_upload_resource_fd(
                self._handle, resource_id, descriptor, encoded_size
            )
        )

    def release_resource(self, resource_id: int) -> None:
        self._check(
            self._library.mango_overlay_client_release_resource(
                self._handle, resource_id
            )
        )

    def group(
        self,
        element_id: int,
        *,
        z_index: int = 0,
        layout: Layout | None = None,
    ) -> None:
        native_layout, layout_pointer = _native_layout(layout)
        element = _GroupElement(
            ctypes.sizeof(_GroupElement), element_id, z_index, layout_pointer
        )
        self._check(
            self._library.mango_overlay_client_upsert_group(
                self._handle, ctypes.byref(element)
            )
        )
        del native_layout

    def text(
        self,
        element_id: int,
        x: float,
        y: float,
        value: str,
        *,
        font_size: float = 24.0,
        color: tuple[float, float, float, float] = (1.0, 1.0, 1.0, 1.0),
        z_index: int = 0,
        layout: Layout | None = None,
    ) -> None:
        native_layout, layout_pointer = _native_layout(layout)
        element = _TextElement(
            ctypes.sizeof(_TextElement),
            element_id,
            z_index,
            x,
            y,
            _encode(value),
            font_size,
            _Color(*color),
            layout_pointer,
        )
        self._check(
            self._library.mango_overlay_client_upsert_text(
                self._handle, ctypes.byref(element)
            )
        )
        del native_layout

    def rectangle(
        self,
        element_id: int,
        x: float,
        y: float,
        width: float,
        height: float,
        *,
        corner_radius: float = 0.0,
        color: tuple[float, float, float, float] = (1.0, 1.0, 1.0, 1.0),
        z_index: int = 0,
        layout: Layout | None = None,
    ) -> None:
        native_layout, layout_pointer = _native_layout(layout)
        element = _RectangleElement(
            ctypes.sizeof(_RectangleElement),
            element_id,
            z_index,
            x,
            y,
            width,
            height,
            corner_radius,
            _Color(*color),
            layout_pointer,
        )
        self._check(
            self._library.mango_overlay_client_upsert_rectangle(
                self._handle, ctypes.byref(element)
            )
        )
        del native_layout

    def remove(self, element_id: int) -> None:
        self._check(
            self._library.mango_overlay_client_remove_element(
                self._handle, element_id
            )
        )

    def line(
        self,
        element_id: int,
        start: tuple[float, float],
        end: tuple[float, float],
        *,
        thickness: float = 1.0,
        color: tuple[float, float, float, float] = (1.0, 1.0, 1.0, 1.0),
        z_index: int = 0,
        layout: Layout | None = None,
    ) -> None:
        native_layout, layout_pointer = _native_layout(layout)
        element = _LineElement(
            ctypes.sizeof(_LineElement),
            element_id,
            z_index,
            _Vec2(*start),
            _Vec2(*end),
            thickness,
            _Color(*color),
            layout_pointer,
        )
        self._check(
            self._library.mango_overlay_client_upsert_line(
                self._handle, ctypes.byref(element)
            )
        )
        del native_layout

    def polyline(
        self,
        element_id: int,
        points: Sequence[tuple[float, float]],
        *,
        thickness: float = 1.0,
        color: tuple[float, float, float, float] = (1.0, 1.0, 1.0, 1.0),
        z_index: int = 0,
        layout: Layout | None = None,
    ) -> None:
        native_layout, layout_pointer = _native_layout(layout)
        point_array = (_Vec2 * len(points))(*(_Vec2(*point) for point in points))
        element = _PolylineElement(
            ctypes.sizeof(_PolylineElement),
            element_id,
            z_index,
            point_array,
            len(points),
            thickness,
            _Color(*color),
            layout_pointer,
        )
        self._check(
            self._library.mango_overlay_client_upsert_polyline(
                self._handle, ctypes.byref(element)
            )
        )
        del native_layout

    def circle(
        self,
        element_id: int,
        center: tuple[float, float],
        radius: float,
        *,
        color: tuple[float, float, float, float] = (1.0, 1.0, 1.0, 1.0),
        z_index: int = 0,
        layout: Layout | None = None,
    ) -> None:
        native_layout, layout_pointer = _native_layout(layout)
        element = _CircleElement(
            ctypes.sizeof(_CircleElement),
            element_id,
            z_index,
            _Vec2(*center),
            radius,
            _Color(*color),
            layout_pointer,
        )
        self._check(
            self._library.mango_overlay_client_upsert_circle(
                self._handle, ctypes.byref(element)
            )
        )
        del native_layout

    def image(
        self,
        element_id: int,
        resource_id: int,
        x: float,
        y: float,
        width: float,
        height: float,
        *,
        tint: tuple[float, float, float, float] = (1.0, 1.0, 1.0, 1.0),
        z_index: int = 0,
        layout: Layout | None = None,
    ) -> None:
        native_layout, layout_pointer = _native_layout(layout)
        element = _ImageElement(
            ctypes.sizeof(_ImageElement),
            element_id,
            z_index,
            x,
            y,
            width,
            height,
            resource_id,
            _Color(*tint),
            layout_pointer,
        )
        self._check(
            self._library.mango_overlay_client_upsert_image(
                self._handle, ctypes.byref(element)
            )
        )
        del native_layout

    def gif(
        self,
        element_id: int,
        resource_id: int,
        x: float,
        y: float,
        width: float,
        height: float,
        *,
        tint: tuple[float, float, float, float] = (1.0, 1.0, 1.0, 1.0),
        playback_rate: float = 1.0,
        paused: bool = False,
        frame_index: int = 0,
        z_index: int = 0,
        layout: Layout | None = None,
    ) -> None:
        native_layout, layout_pointer = _native_layout(layout)
        element = _GifElement(
            ctypes.sizeof(_GifElement),
            element_id,
            z_index,
            x,
            y,
            width,
            height,
            resource_id,
            _Color(*tint),
            playback_rate,
            frame_index,
            int(paused),
            (ctypes.c_uint8 * 3)(0, 0, 0),
            layout_pointer,
        )
        self._check(
            self._library.mango_overlay_client_upsert_gif(
                self._handle, ctypes.byref(element)
            )
        )
        del native_layout

    def commit(self) -> None:
        self._check(self._library.mango_overlay_client_commit_transaction(self._handle))

    def abort(self) -> None:
        self._check(self._library.mango_overlay_client_abort_transaction(self._handle))

    @contextlib.contextmanager
    def transaction(self) -> Iterator[Provider]:
        self.begin()
        try:
            yield self
        except BaseException:
            self.abort()
            raise
        else:
            self.commit()


__all__ = [
    "Anchor",
    "ClipRect",
    "Layout",
    "OverlayError",
    "Provider",
    "Result",
    "Visibility",
]
