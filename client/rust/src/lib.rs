//! Safe, single-threaded Rust bindings for Mango Overlay providers.

use std::ffi::{CStr, CString};
use std::fmt;
use std::marker::PhantomData;
use std::os::fd::RawFd;
use std::os::raw::{c_char, c_int, c_void};
use std::ptr;
use std::rc::Rc;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(i32)]
pub enum ResultCode {
    InvalidArgument = 1,
    InvalidState = 2,
    ConnectionFailed = 3,
    IoError = 4,
    ProtocolError = 5,
    ServerRejected = 6,
    OutOfMemory = 7,
}

#[derive(Debug)]
pub struct Error {
    pub code: ResultCode,
    pub message: String,
}

impl fmt::Display for Error {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.message)
    }
}

impl std::error::Error for Error {}

pub type Result<T> = std::result::Result<T, Error>;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(i32)]
pub enum Visibility {
    GameOnly = 0,
    SteamOnly = 1,
    Always = 2,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(i32)]
pub enum Anchor {
    TopLeft = 0,
    TopCenter = 1,
    TopRight = 2,
    CenterLeft = 3,
    Center = 4,
    CenterRight = 5,
    BottomLeft = 6,
    BottomCenter = 7,
    BottomRight = 8,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct Vec2 {
    pub x: f32,
    pub y: f32,
}

impl Vec2 {
    pub const fn new(x: f32, y: f32) -> Self {
        Self { x, y }
    }
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct Color {
    pub red: f32,
    pub green: f32,
    pub blue: f32,
    pub alpha: f32,
}

impl Color {
    pub const WHITE: Self = Self {
        red: 1.0,
        green: 1.0,
        blue: 1.0,
        alpha: 1.0,
    };
}

impl Default for Color {
    fn default() -> Self {
        Self::WHITE
    }
}

#[derive(Clone, Copy, Debug)]
pub struct ClipRect {
    pub position: Vec2,
    pub size: Vec2,
}

#[derive(Clone, Copy, Debug)]
pub struct Layout {
    pub parent_id: u64,
    pub translation: Vec2,
    pub scale: Vec2,
    pub rotation_degrees: f32,
    pub opacity: f32,
    pub anchor: Anchor,
    pub clip: Option<ClipRect>,
    pub hidden: bool,
}

impl Default for Layout {
    fn default() -> Self {
        Self {
            parent_id: 0,
            translation: Vec2::new(0.0, 0.0),
            scale: Vec2::new(1.0, 1.0),
            rotation_degrees: 0.0,
            opacity: 1.0,
            anchor: Anchor::TopLeft,
            clip: None,
            hidden: false,
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub struct Group<'a> {
    pub id: u64,
    pub z_index: i32,
    pub layout: Option<&'a Layout>,
}

#[derive(Clone, Copy, Debug)]
pub struct Text<'a> {
    pub id: u64,
    pub z_index: i32,
    pub position: Vec2,
    pub text: &'a str,
    pub font_size: f32,
    pub color: Color,
    pub layout: Option<&'a Layout>,
}

#[derive(Clone, Copy, Debug)]
pub struct Rectangle<'a> {
    pub id: u64,
    pub z_index: i32,
    pub position: Vec2,
    pub size: Vec2,
    pub corner_radius: f32,
    pub color: Color,
    pub layout: Option<&'a Layout>,
}

#[derive(Clone, Copy, Debug)]
pub struct Line<'a> {
    pub id: u64,
    pub z_index: i32,
    pub start: Vec2,
    pub end: Vec2,
    pub thickness: f32,
    pub color: Color,
    pub layout: Option<&'a Layout>,
}

#[derive(Clone, Copy, Debug)]
pub struct Polyline<'a> {
    pub id: u64,
    pub z_index: i32,
    pub points: &'a [Vec2],
    pub thickness: f32,
    pub color: Color,
    pub layout: Option<&'a Layout>,
}

#[derive(Clone, Copy, Debug)]
pub struct Circle<'a> {
    pub id: u64,
    pub z_index: i32,
    pub center: Vec2,
    pub radius: f32,
    pub color: Color,
    pub layout: Option<&'a Layout>,
}

#[derive(Clone, Copy, Debug)]
pub struct Image<'a> {
    pub id: u64,
    pub z_index: i32,
    pub position: Vec2,
    pub size: Vec2,
    pub resource_id: u64,
    pub tint: Color,
    pub layout: Option<&'a Layout>,
}

#[derive(Clone, Copy, Debug)]
pub struct Gif<'a> {
    pub id: u64,
    pub z_index: i32,
    pub position: Vec2,
    pub size: Vec2,
    pub resource_id: u64,
    pub tint: Color,
    pub playback_rate: f32,
    pub frame_index: u32,
    pub paused: bool,
    pub layout: Option<&'a Layout>,
}

pub struct Provider {
    handle: *mut ffi::Client,
    _single_threaded: PhantomData<Rc<()>>,
}

impl Provider {
    pub fn connect(
        client_version: &str,
        socket_path: Option<&str>,
        timeout_ms: u32,
    ) -> Result<Self> {
        let version = c_string(client_version)?;
        let socket = socket_path.map(c_string).transpose()?;
        let config = ffi::ClientConfig {
            struct_size: std::mem::size_of::<ffi::ClientConfig>() as u32,
            socket_path: socket.as_ref().map_or(ptr::null(), |value| value.as_ptr()),
            client_version: version.as_ptr(),
            timeout_ms,
        };
        let mut handle = ptr::null_mut();
        let code = unsafe { ffi::mango_overlay_client_open(&config, &mut handle) };
        if code != 0 || handle.is_null() {
            return Err(error_from_code(
                handle,
                code,
                "Could not connect to mango-overlayd",
            ));
        }
        Ok(Self {
            handle,
            _single_threaded: PhantomData,
        })
    }

    pub fn register(
        &mut self,
        application_id: &str,
        instance_id: &str,
        display_name: &str,
        canvas_size: (u16, u16),
        visibility: Visibility,
    ) -> Result<()> {
        let application_id = c_string(application_id)?;
        let instance_id = c_string(instance_id)?;
        let display_name = c_string(display_name)?;
        let provider = ffi::ProviderInfo {
            struct_size: std::mem::size_of::<ffi::ProviderInfo>() as u32,
            application_id: application_id.as_ptr(),
            instance_id: instance_id.as_ptr(),
            display_name: display_name.as_ptr(),
            canvas_width: canvas_size.0,
            canvas_height: canvas_size.1,
            visibility: visibility as i32,
        };
        self.call(unsafe { ffi::mango_overlay_client_register_provider(self.handle, &provider) })
    }

    pub fn upload_resource(&mut self, resource_id: u64, encoded: &[u8]) -> Result<()> {
        let size =
            u32::try_from(encoded.len()).map_err(|_| invalid_argument("Resource is too large"))?;
        self.call(unsafe {
            ffi::mango_overlay_client_upload_resource(
                self.handle,
                resource_id,
                encoded.as_ptr().cast(),
                size,
            )
        })
    }

    pub fn upload_resource_fd(
        &mut self,
        resource_id: u64,
        descriptor: RawFd,
        encoded_size: u32,
    ) -> Result<()> {
        self.call(unsafe {
            ffi::mango_overlay_client_upload_resource_fd(
                self.handle,
                resource_id,
                descriptor,
                encoded_size,
            )
        })
    }

    pub fn release_resource(&mut self, resource_id: u64) -> Result<()> {
        self.call(unsafe { ffi::mango_overlay_client_release_resource(self.handle, resource_id) })
    }

    pub fn begin(&mut self) -> Result<Transaction<'_>> {
        self.call(unsafe { ffi::mango_overlay_client_begin_transaction(self.handle) })?;
        Ok(Transaction {
            provider: self,
            active: true,
        })
    }

    fn call(&self, code: c_int) -> Result<()> {
        if code == 0 {
            Ok(())
        } else {
            Err(error_from_code(
                self.handle,
                code,
                "Mango Overlay request failed",
            ))
        }
    }
}

impl Drop for Provider {
    fn drop(&mut self) {
        unsafe { ffi::mango_overlay_client_close(self.handle) };
        self.handle = ptr::null_mut();
    }
}

pub struct Transaction<'a> {
    provider: &'a mut Provider,
    active: bool,
}

impl Transaction<'_> {
    pub fn group(&mut self, element: Group<'_>) -> Result<()> {
        with_layout(element.layout, |layout| {
            let native = ffi::GroupElement {
                struct_size: std::mem::size_of::<ffi::GroupElement>() as u32,
                element_id: element.id,
                z_index: element.z_index,
                layout,
            };
            self.call(unsafe {
                ffi::mango_overlay_client_upsert_group(self.provider.handle, &native)
            })
        })
    }

    pub fn text(&mut self, element: Text<'_>) -> Result<()> {
        let text = c_string(element.text)?;
        with_layout(element.layout, |layout| {
            let native = ffi::TextElement {
                struct_size: std::mem::size_of::<ffi::TextElement>() as u32,
                element_id: element.id,
                z_index: element.z_index,
                x: element.position.x,
                y: element.position.y,
                text: text.as_ptr(),
                font_size: element.font_size,
                color: element.color,
                layout,
            };
            self.call(unsafe {
                ffi::mango_overlay_client_upsert_text(self.provider.handle, &native)
            })
        })
    }

    pub fn rectangle(&mut self, element: Rectangle<'_>) -> Result<()> {
        with_layout(element.layout, |layout| {
            let native = ffi::RectangleElement {
                struct_size: std::mem::size_of::<ffi::RectangleElement>() as u32,
                element_id: element.id,
                z_index: element.z_index,
                x: element.position.x,
                y: element.position.y,
                width: element.size.x,
                height: element.size.y,
                corner_radius: element.corner_radius,
                color: element.color,
                layout,
            };
            self.call(unsafe {
                ffi::mango_overlay_client_upsert_rectangle(self.provider.handle, &native)
            })
        })
    }

    pub fn line(&mut self, element: Line<'_>) -> Result<()> {
        with_layout(element.layout, |layout| {
            let native = ffi::LineElement {
                struct_size: std::mem::size_of::<ffi::LineElement>() as u32,
                element_id: element.id,
                z_index: element.z_index,
                start: element.start,
                end: element.end,
                thickness: element.thickness,
                color: element.color,
                layout,
            };
            self.call(unsafe {
                ffi::mango_overlay_client_upsert_line(self.provider.handle, &native)
            })
        })
    }

    pub fn polyline(&mut self, element: Polyline<'_>) -> Result<()> {
        let point_count = u32::try_from(element.points.len())
            .map_err(|_| invalid_argument("Polyline has too many points"))?;
        with_layout(element.layout, |layout| {
            let native = ffi::PolylineElement {
                struct_size: std::mem::size_of::<ffi::PolylineElement>() as u32,
                element_id: element.id,
                z_index: element.z_index,
                points: element.points.as_ptr(),
                point_count,
                thickness: element.thickness,
                color: element.color,
                layout,
            };
            self.call(unsafe {
                ffi::mango_overlay_client_upsert_polyline(self.provider.handle, &native)
            })
        })
    }

    pub fn circle(&mut self, element: Circle<'_>) -> Result<()> {
        with_layout(element.layout, |layout| {
            let native = ffi::CircleElement {
                struct_size: std::mem::size_of::<ffi::CircleElement>() as u32,
                element_id: element.id,
                z_index: element.z_index,
                center: element.center,
                radius: element.radius,
                color: element.color,
                layout,
            };
            self.call(unsafe {
                ffi::mango_overlay_client_upsert_circle(self.provider.handle, &native)
            })
        })
    }

    pub fn image(&mut self, element: Image<'_>) -> Result<()> {
        with_layout(element.layout, |layout| {
            let native = ffi::ImageElement {
                struct_size: std::mem::size_of::<ffi::ImageElement>() as u32,
                element_id: element.id,
                z_index: element.z_index,
                x: element.position.x,
                y: element.position.y,
                width: element.size.x,
                height: element.size.y,
                resource_id: element.resource_id,
                tint: element.tint,
                layout,
            };
            self.call(unsafe {
                ffi::mango_overlay_client_upsert_image(self.provider.handle, &native)
            })
        })
    }

    pub fn gif(&mut self, element: Gif<'_>) -> Result<()> {
        with_layout(element.layout, |layout| {
            let native = ffi::GifElement {
                struct_size: std::mem::size_of::<ffi::GifElement>() as u32,
                element_id: element.id,
                z_index: element.z_index,
                x: element.position.x,
                y: element.position.y,
                width: element.size.x,
                height: element.size.y,
                resource_id: element.resource_id,
                tint: element.tint,
                playback_rate: element.playback_rate,
                frame_index: element.frame_index,
                paused: u8::from(element.paused),
                reserved: [0; 3],
                layout,
            };
            self.call(unsafe {
                ffi::mango_overlay_client_upsert_gif(self.provider.handle, &native)
            })
        })
    }

    pub fn remove(&mut self, element_id: u64) -> Result<()> {
        self.call(unsafe {
            ffi::mango_overlay_client_remove_element(self.provider.handle, element_id)
        })
    }

    pub fn commit(mut self) -> Result<()> {
        let result = self
            .call(unsafe { ffi::mango_overlay_client_commit_transaction(self.provider.handle) });
        if result.is_ok() {
            self.active = false;
        }
        result
    }

    pub fn abort(mut self) -> Result<()> {
        self.active = false;
        self.call(unsafe { ffi::mango_overlay_client_abort_transaction(self.provider.handle) })
    }

    fn call(&self, code: c_int) -> Result<()> {
        self.provider.call(code)
    }
}

impl Drop for Transaction<'_> {
    fn drop(&mut self) {
        if self.active {
            unsafe { ffi::mango_overlay_client_abort_transaction(self.provider.handle) };
            self.active = false;
        }
    }
}

fn with_layout<T>(
    layout: Option<&Layout>,
    callback: impl FnOnce(*const ffi::ElementLayout) -> T,
) -> T {
    let native = layout.map(ffi::ElementLayout::from);
    callback(
        native
            .as_ref()
            .map_or(ptr::null(), |value| value as *const ffi::ElementLayout),
    )
}

fn c_string(value: &str) -> Result<CString> {
    CString::new(value).map_err(|_| invalid_argument("String contains a NUL byte"))
}

fn invalid_argument(message: &str) -> Error {
    Error {
        code: ResultCode::InvalidArgument,
        message: message.to_owned(),
    }
}

fn error_from_code(handle: *mut ffi::Client, raw: c_int, fallback: &str) -> Error {
    let code = match raw {
        1 => ResultCode::InvalidArgument,
        2 => ResultCode::InvalidState,
        3 => ResultCode::ConnectionFailed,
        4 => ResultCode::IoError,
        6 => ResultCode::ServerRejected,
        7 => ResultCode::OutOfMemory,
        _ => ResultCode::ProtocolError,
    };
    let message = if handle.is_null() {
        fallback.to_owned()
    } else {
        let pointer = unsafe { ffi::mango_overlay_client_last_error(handle) };
        if pointer.is_null() {
            fallback.to_owned()
        } else {
            unsafe { CStr::from_ptr(pointer) }
                .to_string_lossy()
                .into_owned()
        }
    };
    Error { code, message }
}

mod ffi {
    use super::{c_char, c_int, c_void, ClipRect, Color, Layout, Vec2};

    #[repr(C)]
    pub struct Client {
        _private: [u8; 0],
    }

    #[repr(C)]
    pub struct ClientConfig {
        pub struct_size: u32,
        pub socket_path: *const c_char,
        pub client_version: *const c_char,
        pub timeout_ms: u32,
    }

    #[repr(C)]
    pub struct ProviderInfo {
        pub struct_size: u32,
        pub application_id: *const c_char,
        pub instance_id: *const c_char,
        pub display_name: *const c_char,
        pub canvas_width: u16,
        pub canvas_height: u16,
        pub visibility: i32,
    }

    #[repr(C)]
    pub struct NativeClipRect {
        pub x: f32,
        pub y: f32,
        pub width: f32,
        pub height: f32,
    }

    #[repr(C)]
    pub struct ElementLayout {
        pub struct_size: u32,
        pub parent_id: u64,
        pub translation: Vec2,
        pub scale: Vec2,
        pub rotation_degrees: f32,
        pub opacity: f32,
        pub anchor: i32,
        pub hidden: u8,
        pub clip_enabled: u8,
        pub reserved: u16,
        pub clip: NativeClipRect,
    }

    impl From<&Layout> for ElementLayout {
        fn from(layout: &Layout) -> Self {
            let clip = layout.clip.unwrap_or(ClipRect {
                position: Vec2::new(0.0, 0.0),
                size: Vec2::new(0.0, 0.0),
            });
            Self {
                struct_size: std::mem::size_of::<Self>() as u32,
                parent_id: layout.parent_id,
                translation: layout.translation,
                scale: layout.scale,
                rotation_degrees: layout.rotation_degrees,
                opacity: layout.opacity,
                anchor: layout.anchor as i32,
                hidden: u8::from(layout.hidden),
                clip_enabled: u8::from(layout.clip.is_some()),
                reserved: 0,
                clip: NativeClipRect {
                    x: clip.position.x,
                    y: clip.position.y,
                    width: clip.size.x,
                    height: clip.size.y,
                },
            }
        }
    }

    #[repr(C)]
    pub struct GroupElement {
        pub struct_size: u32,
        pub element_id: u64,
        pub z_index: i32,
        pub layout: *const ElementLayout,
    }
    #[repr(C)]
    pub struct TextElement {
        pub struct_size: u32,
        pub element_id: u64,
        pub z_index: i32,
        pub x: f32,
        pub y: f32,
        pub text: *const c_char,
        pub font_size: f32,
        pub color: Color,
        pub layout: *const ElementLayout,
    }
    #[repr(C)]
    pub struct RectangleElement {
        pub struct_size: u32,
        pub element_id: u64,
        pub z_index: i32,
        pub x: f32,
        pub y: f32,
        pub width: f32,
        pub height: f32,
        pub corner_radius: f32,
        pub color: Color,
        pub layout: *const ElementLayout,
    }
    #[repr(C)]
    pub struct LineElement {
        pub struct_size: u32,
        pub element_id: u64,
        pub z_index: i32,
        pub start: Vec2,
        pub end: Vec2,
        pub thickness: f32,
        pub color: Color,
        pub layout: *const ElementLayout,
    }
    #[repr(C)]
    pub struct PolylineElement {
        pub struct_size: u32,
        pub element_id: u64,
        pub z_index: i32,
        pub points: *const Vec2,
        pub point_count: u32,
        pub thickness: f32,
        pub color: Color,
        pub layout: *const ElementLayout,
    }
    #[repr(C)]
    pub struct CircleElement {
        pub struct_size: u32,
        pub element_id: u64,
        pub z_index: i32,
        pub center: Vec2,
        pub radius: f32,
        pub color: Color,
        pub layout: *const ElementLayout,
    }
    #[repr(C)]
    pub struct ImageElement {
        pub struct_size: u32,
        pub element_id: u64,
        pub z_index: i32,
        pub x: f32,
        pub y: f32,
        pub width: f32,
        pub height: f32,
        pub resource_id: u64,
        pub tint: Color,
        pub layout: *const ElementLayout,
    }
    #[repr(C)]
    pub struct GifElement {
        pub struct_size: u32,
        pub element_id: u64,
        pub z_index: i32,
        pub x: f32,
        pub y: f32,
        pub width: f32,
        pub height: f32,
        pub resource_id: u64,
        pub tint: Color,
        pub playback_rate: f32,
        pub frame_index: u32,
        pub paused: u8,
        pub reserved: [u8; 3],
        pub layout: *const ElementLayout,
    }

    extern "C" {
        pub fn mango_overlay_client_open(
            config: *const ClientConfig,
            client: *mut *mut Client,
        ) -> c_int;
        pub fn mango_overlay_client_close(client: *mut Client);
        pub fn mango_overlay_client_register_provider(
            client: *mut Client,
            provider: *const ProviderInfo,
        ) -> c_int;
        pub fn mango_overlay_client_upload_resource(
            client: *mut Client,
            id: u64,
            data: *const c_void,
            size: u32,
        ) -> c_int;
        pub fn mango_overlay_client_upload_resource_fd(
            client: *mut Client,
            id: u64,
            descriptor: c_int,
            size: u32,
        ) -> c_int;
        pub fn mango_overlay_client_release_resource(client: *mut Client, id: u64) -> c_int;
        pub fn mango_overlay_client_begin_transaction(client: *mut Client) -> c_int;
        pub fn mango_overlay_client_upsert_group(
            client: *mut Client,
            element: *const GroupElement,
        ) -> c_int;
        pub fn mango_overlay_client_upsert_text(
            client: *mut Client,
            element: *const TextElement,
        ) -> c_int;
        pub fn mango_overlay_client_upsert_rectangle(
            client: *mut Client,
            element: *const RectangleElement,
        ) -> c_int;
        pub fn mango_overlay_client_upsert_line(
            client: *mut Client,
            element: *const LineElement,
        ) -> c_int;
        pub fn mango_overlay_client_upsert_polyline(
            client: *mut Client,
            element: *const PolylineElement,
        ) -> c_int;
        pub fn mango_overlay_client_upsert_circle(
            client: *mut Client,
            element: *const CircleElement,
        ) -> c_int;
        pub fn mango_overlay_client_upsert_image(
            client: *mut Client,
            element: *const ImageElement,
        ) -> c_int;
        pub fn mango_overlay_client_upsert_gif(
            client: *mut Client,
            element: *const GifElement,
        ) -> c_int;
        pub fn mango_overlay_client_remove_element(client: *mut Client, element_id: u64) -> c_int;
        pub fn mango_overlay_client_commit_transaction(client: *mut Client) -> c_int;
        pub fn mango_overlay_client_abort_transaction(client: *mut Client) -> c_int;
        pub fn mango_overlay_client_last_error(client: *const Client) -> *const c_char;
    }
}
