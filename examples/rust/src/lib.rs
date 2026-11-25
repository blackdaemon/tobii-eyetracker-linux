//! Minimal safe wrapper around Tobii Stream Engine 3.0.4 for Linux.
//!
//! ```no_run
//! let api = tobii_stream_engine::Api::new()?;
//! let mut dev = api.connect_first()?;
//! dev.subscribe_gaze(|g| println!("{} {:.3} {:.3}", g.timestamp_us, g.x, g.y))?;
//! loop { dev.process()?; }
//! # Ok::<(), tobii_stream_engine::Error>(())
//! ```
//!
//! Gaze is normalised to the display area stored on the tracker: (0,0) is the
//! top-left corner, (1,1) bottom-right; values slightly outside 0..1 mean you
//! are looking just off-screen.

use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::fmt;
use std::marker::PhantomData;

mod ffi {
    use super::*;

    pub type TobiiError = c_int;
    pub const NO_ERROR: TobiiError = 0;
    pub const CONNECTION_FAILED: TobiiError = 5;
    pub const TIMED_OUT: TobiiError = 6;
    pub const VALID: c_int = 1;

    #[repr(C)]
    pub struct Api {
        _p: [u8; 0],
    }
    #[repr(C)]
    pub struct Device {
        _p: [u8; 0],
    }

    #[repr(C)]
    pub struct GazePoint {
        pub timestamp_us: i64,
        pub validity: c_int,
        pub position_xy: [f32; 2],
    }

    #[repr(C)]
    pub struct GazeOrigin {
        pub timestamp_us: i64,
        pub left_validity: c_int,
        pub left_xyz: [f32; 3],
        pub right_validity: c_int,
        pub right_xyz: [f32; 3],
    }

    #[repr(C)]
    pub struct DeviceInfo {
        pub serial_number: [c_char; 256],
        pub model: [c_char; 256],
        pub generation: [c_char; 256],
        pub firmware_version: [c_char; 256],
        pub integration_id: [c_char; 128],
        pub hw_calibration_version: [c_char; 128],
        pub hw_calibration_date: [c_char; 128],
        pub lot_id: [c_char; 128],
        pub integration_type: [c_char; 256],
        pub runtime_build_version: [c_char; 256],
    }

    pub type UrlCb = unsafe extern "C" fn(*const c_char, *mut c_void);
    pub type GazeCb = unsafe extern "C" fn(*const GazePoint, *mut c_void);
    pub type OriginCb = unsafe extern "C" fn(*const GazeOrigin, *mut c_void);

    extern "C" {
        pub fn tobii_error_message(e: TobiiError) -> *const c_char;
        pub fn tobii_api_create(api: *mut *mut Api, alloc: *const c_void, log: *const c_void) -> TobiiError;
        pub fn tobii_api_destroy(api: *mut Api) -> TobiiError;
        pub fn tobii_enumerate_local_device_urls(api: *mut Api, cb: UrlCb, user: *mut c_void) -> TobiiError;
        pub fn tobii_device_create(api: *mut Api, url: *const c_char, dev: *mut *mut Device) -> TobiiError;
        pub fn tobii_device_destroy(dev: *mut Device) -> TobiiError;
        pub fn tobii_device_reconnect(dev: *mut Device) -> TobiiError;
        pub fn tobii_get_device_info(dev: *mut Device, info: *mut DeviceInfo) -> TobiiError;
        pub fn tobii_wait_for_callbacks(count: c_int, devs: *const *mut Device) -> TobiiError;
        pub fn tobii_device_process_callbacks(dev: *mut Device) -> TobiiError;
        pub fn tobii_gaze_point_subscribe(dev: *mut Device, cb: GazeCb, user: *mut c_void) -> TobiiError;
        pub fn tobii_gaze_point_unsubscribe(dev: *mut Device) -> TobiiError;
        pub fn tobii_gaze_origin_subscribe(dev: *mut Device, cb: OriginCb, user: *mut c_void) -> TobiiError;
        pub fn tobii_gaze_origin_unsubscribe(dev: *mut Device) -> TobiiError;
    }
}

/// A Stream Engine error code with its message.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Error {
    pub code: i32,
    pub context: &'static str,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let msg = unsafe { CStr::from_ptr(ffi::tobii_error_message(self.code)) };
        write!(f, "{}: {}", self.context, msg.to_string_lossy())
    }
}
impl std::error::Error for Error {}

fn check(context: &'static str, code: ffi::TobiiError) -> Result<(), Error> {
    if code == ffi::NO_ERROR { Ok(()) } else { Err(Error { code, context }) }
}

/// A valid gaze point (invalid samples are filtered out).
#[derive(Debug, Clone, Copy)]
pub struct Gaze {
    pub timestamp_us: i64,
    pub x: f32,
    pub y: f32,
}

/// Eye positions in mm, tracker coordinates (x right, y up, z towards the user).
#[derive(Debug, Clone, Copy)]
pub struct EyePosition {
    pub timestamp_us: i64,
    pub left_mm: Option<[f32; 3]>,
    pub right_mm: Option<[f32; 3]>,
}

#[derive(Debug, Clone)]
pub struct DeviceInfo {
    pub serial_number: String,
    pub model: String,
    pub generation: String,
    pub firmware_version: String,
}

/// Stream Engine API context. Devices borrow it, so it outlives them.
pub struct Api(*mut ffi::Api);

impl Api {
    pub fn new() -> Result<Self, Error> {
        let mut p = std::ptr::null_mut();
        check("tobii_api_create", unsafe { ffi::tobii_api_create(&mut p, std::ptr::null(), std::ptr::null()) })?;
        Ok(Api(p))
    }

    /// URLs of trackers served by tobiiusb.service, e.g. `tobii-ttp://IS404-...`.
    pub fn device_urls(&self) -> Result<Vec<String>, Error> {
        unsafe extern "C" fn cb(url: *const c_char, user: *mut c_void) {
            let v = &mut *(user as *mut Vec<String>);
            v.push(CStr::from_ptr(url).to_string_lossy().into_owned());
        }
        let mut urls = Vec::new();
        check("tobii_enumerate_local_device_urls", unsafe {
            ffi::tobii_enumerate_local_device_urls(self.0, cb, &mut urls as *mut _ as *mut c_void)
        })?;
        Ok(urls)
    }

    pub fn connect(&self, url: &str) -> Result<Device<'_>, Error> {
        let url = CString::new(url).expect("url contains NUL");
        let mut p = std::ptr::null_mut();
        check("tobii_device_create", unsafe { ffi::tobii_device_create(self.0, url.as_ptr(), &mut p) })?;
        Ok(Device { ptr: p, gaze: None, origin: None, _api: PhantomData })
    }

    /// Connect to the first enumerated tracker.
    pub fn connect_first(&self) -> Result<Device<'_>, Error> {
        let urls = self.device_urls()?;
        let url = urls.first().ok_or(Error { code: ffi::CONNECTION_FAILED, context: "no eye tracker found" })?;
        self.connect(url)
    }
}

impl Drop for Api {
    fn drop(&mut self) {
        unsafe { ffi::tobii_api_destroy(self.0) };
    }
}

type GazeFn = Box<dyn FnMut(Gaze)>;
type OriginFn = Box<dyn FnMut(EyePosition)>;

/// A connected tracker. Not `Send`: Stream Engine calls for one device must
/// stay on one thread. Callbacks run inside [`Device::process`].
pub struct Device<'a> {
    ptr: *mut ffi::Device,
    // Double-boxed so the thin pointer handed to C stays stable.
    gaze: Option<Box<GazeFn>>,
    origin: Option<Box<OriginFn>>,
    _api: PhantomData<&'a Api>,
}

fn cstr(buf: &[c_char]) -> String {
    unsafe { CStr::from_ptr(buf.as_ptr()) }.to_string_lossy().into_owned()
}

impl Device<'_> {
    pub fn info(&self) -> Result<DeviceInfo, Error> {
        let mut i: ffi::DeviceInfo = unsafe { std::mem::zeroed() };
        check("tobii_get_device_info", unsafe { ffi::tobii_get_device_info(self.ptr, &mut i) })?;
        Ok(DeviceInfo {
            serial_number: cstr(&i.serial_number),
            model: cstr(&i.model),
            generation: cstr(&i.generation),
            firmware_version: cstr(&i.firmware_version),
        })
    }

    /// Call `f` for every valid gaze point.
    pub fn subscribe_gaze(&mut self, f: impl FnMut(Gaze) + 'static) -> Result<(), Error> {
        unsafe extern "C" fn tramp(p: *const ffi::GazePoint, user: *mut c_void) {
            let g = &*p;
            if g.validity == ffi::VALID {
                let f = &mut *(user as *mut GazeFn);
                f(Gaze { timestamp_us: g.timestamp_us, x: g.position_xy[0], y: g.position_xy[1] });
            }
        }
        let mut boxed: Box<GazeFn> = Box::new(Box::new(f));
        let user = &mut *boxed as *mut GazeFn as *mut c_void;
        check("tobii_gaze_point_subscribe", unsafe { ffi::tobii_gaze_point_subscribe(self.ptr, tramp, user) })?;
        self.gaze = Some(boxed);
        Ok(())
    }

    /// Call `f` for every eye-position sample (either eye may be missing).
    pub fn subscribe_eye_position(&mut self, f: impl FnMut(EyePosition) + 'static) -> Result<(), Error> {
        unsafe extern "C" fn tramp(p: *const ffi::GazeOrigin, user: *mut c_void) {
            let o = &*p;
            let f = &mut *(user as *mut OriginFn);
            f(EyePosition {
                timestamp_us: o.timestamp_us,
                left_mm: (o.left_validity == ffi::VALID).then_some(o.left_xyz),
                right_mm: (o.right_validity == ffi::VALID).then_some(o.right_xyz),
            });
        }
        let mut boxed: Box<OriginFn> = Box::new(Box::new(f));
        let user = &mut *boxed as *mut OriginFn as *mut c_void;
        check("tobii_gaze_origin_subscribe", unsafe { ffi::tobii_gaze_origin_subscribe(self.ptr, tramp, user) })?;
        self.origin = Some(boxed);
        Ok(())
    }

    /// Block until data arrives (or a short internal timeout), then run
    /// callbacks. Reconnects automatically if the tracker was replugged.
    pub fn process(&mut self) -> Result<(), Error> {
        let mut e = unsafe { ffi::tobii_wait_for_callbacks(1, &self.ptr) };
        if e == ffi::TIMED_OUT {
            return Ok(());
        }
        if e == ffi::NO_ERROR {
            e = unsafe { ffi::tobii_device_process_callbacks(self.ptr) };
        }
        if e == ffi::CONNECTION_FAILED {
            unsafe { ffi::tobii_device_reconnect(self.ptr) };
            return Ok(());
        }
        check("process", e)
    }
}

impl Drop for Device<'_> {
    fn drop(&mut self) {
        unsafe {
            // Unsubscribe before the closures are freed, then destroy.
            if self.gaze.is_some() {
                ffi::tobii_gaze_point_unsubscribe(self.ptr);
            }
            if self.origin.is_some() {
                ffi::tobii_gaze_origin_unsubscribe(self.ptr);
            }
            ffi::tobii_device_destroy(self.ptr);
        }
    }
}
