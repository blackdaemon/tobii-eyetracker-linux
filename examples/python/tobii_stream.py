#!/usr/bin/env python3
"""Minimal Tobii Stream Engine 3.0.4 binding via ctypes (no dependencies).

    python3 tobii_stream.py            # print gaze points, Ctrl+C to stop

Import it to use from your own code:

    from tobii_stream import EyeTracker
    with EyeTracker() as et:
        for t_us, x, y in et.gaze():     # x, y normalised 0..1 on the display area
            ...
"""
import ctypes as C
import ctypes.util
import os
import queue
import threading

# Override with TOBII_STREAM_ENGINE=/path/to/libtobii_stream_engine.so
_lib = C.CDLL(os.environ.get("TOBII_STREAM_ENGINE")
              or ctypes.util.find_library("tobii_stream_engine")
              or "/usr/local/lib64/tobii/libtobii_stream_engine.so")

TOBII_ERROR_NO_ERROR = 0
TOBII_ERROR_TIMED_OUT = 6
TOBII_ERROR_CONNECTION_FAILED = 5
TOBII_VALIDITY_VALID = 1


class GazePoint(C.Structure):
    _fields_ = [("timestamp_us", C.c_int64),
                ("validity", C.c_int),
                ("position_xy", C.c_float * 2)]


class GazeOrigin(C.Structure):
    _fields_ = [("timestamp_us", C.c_int64),
                ("left_validity", C.c_int), ("left_xyz", C.c_float * 3),
                ("right_validity", C.c_int), ("right_xyz", C.c_float * 3)]


URL_CB = C.CFUNCTYPE(None, C.c_char_p, C.c_void_p)
GAZE_CB = C.CFUNCTYPE(None, C.POINTER(GazePoint), C.c_void_p)
ORIGIN_CB = C.CFUNCTYPE(None, C.POINTER(GazeOrigin), C.c_void_p)

_lib.tobii_error_message.restype = C.c_char_p
_lib.tobii_api_create.argtypes = [C.POINTER(C.c_void_p), C.c_void_p, C.c_void_p]
_lib.tobii_api_destroy.argtypes = [C.c_void_p]
_lib.tobii_enumerate_local_device_urls.argtypes = [C.c_void_p, URL_CB, C.c_void_p]
_lib.tobii_device_create.argtypes = [C.c_void_p, C.c_char_p, C.POINTER(C.c_void_p)]
_lib.tobii_device_destroy.argtypes = [C.c_void_p]
_lib.tobii_device_reconnect.argtypes = [C.c_void_p]
_lib.tobii_wait_for_callbacks.argtypes = [C.c_int, C.POINTER(C.c_void_p)]
_lib.tobii_device_process_callbacks.argtypes = [C.c_void_p]
_lib.tobii_gaze_point_subscribe.argtypes = [C.c_void_p, GAZE_CB, C.c_void_p]
_lib.tobii_gaze_point_unsubscribe.argtypes = [C.c_void_p]
_lib.tobii_gaze_origin_subscribe.argtypes = [C.c_void_p, ORIGIN_CB, C.c_void_p]
_lib.tobii_gaze_origin_unsubscribe.argtypes = [C.c_void_p]


class TobiiError(RuntimeError):
    def __init__(self, func, code):
        super().__init__(f"{func}: {_lib.tobii_error_message(code).decode()}")
        self.code = code


def _check(func, code):
    if code != TOBII_ERROR_NO_ERROR:
        raise TobiiError(func, code)


class EyeTracker:
    """Connects to the first tracker (or `url`) served by tobiiusb.service."""

    def __init__(self, url=None):
        self._api = C.c_void_p()
        _check("tobii_api_create", _lib.tobii_api_create(C.byref(self._api), None, None))
        if url is None:
            urls = []
            cb = URL_CB(lambda u, _: urls.append(u.decode()))
            _check("enumerate", _lib.tobii_enumerate_local_device_urls(self._api, cb, None))
            if not urls:
                _lib.tobii_api_destroy(self._api)
                raise RuntimeError("no eye tracker found (is tobiiusb.service running?)")
            url = urls[0]
        self.url = url
        self._dev = C.c_void_p()
        _check("tobii_device_create",
               _lib.tobii_device_create(self._api, url.encode(), C.byref(self._dev)))
        self._callbacks = []          # keep ctypes callbacks alive
        self._stop = threading.Event()
        self._thread = None

    def close(self):
        # Stop the worker before destroying the device it is waiting on.
        self._stop.set()
        if self._thread is not None:
            self._thread.join()
            self._thread = None
        if self._dev:
            _lib.tobii_device_destroy(self._dev)
            self._dev = C.c_void_p()
        if self._api:
            _lib.tobii_api_destroy(self._api)
            self._api = C.c_void_p()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def subscribe_gaze(self, fn):
        """fn(timestamp_us, x, y) for every valid gaze point."""
        def cb(p, _):
            g = p.contents
            if g.validity == TOBII_VALIDITY_VALID:
                fn(g.timestamp_us, g.position_xy[0], g.position_xy[1])
        c = GAZE_CB(cb)
        self._callbacks.append(c)
        _check("tobii_gaze_point_subscribe", _lib.tobii_gaze_point_subscribe(self._dev, c, None))

    def subscribe_eye_position(self, fn):
        """fn(timestamp_us, left_xyz_mm | None, right_xyz_mm | None) in tracker coordinates."""
        def cb(p, _):
            o = p.contents
            fn(o.timestamp_us,
               tuple(o.left_xyz) if o.left_validity == TOBII_VALIDITY_VALID else None,
               tuple(o.right_xyz) if o.right_validity == TOBII_VALIDITY_VALID else None)
        c = ORIGIN_CB(cb)
        self._callbacks.append(c)
        _check("tobii_gaze_origin_subscribe", _lib.tobii_gaze_origin_subscribe(self._dev, c, None))

    def process(self):
        """Block until data arrives (or a short timeout) and run callbacks."""
        devs = (C.c_void_p * 1)(self._dev)
        e = _lib.tobii_wait_for_callbacks(1, devs)
        if e == TOBII_ERROR_TIMED_OUT:
            return
        if e == TOBII_ERROR_NO_ERROR:
            e = _lib.tobii_device_process_callbacks(self._dev)
        if e == TOBII_ERROR_CONNECTION_FAILED:
            _lib.tobii_device_reconnect(self._dev)
        elif e != TOBII_ERROR_NO_ERROR:
            raise TobiiError("process", e)

    def gaze(self):
        """Generator of (timestamp_us, x, y) valid gaze points."""
        q = []
        self.subscribe_gaze(lambda t, x, y: q.append((t, x, y)))
        while True:
            self.process()
            while q:
                yield q.pop(0)

    def gaze_in_thread(self, maxsize=1):
        """Start a background thread; returns a queue holding the latest gaze point(s).

        Handy for GUI / game loops: call q.get() or q.get_nowait() from your loop.
        Stream Engine is not thread-safe: do all subscribe_*() calls before this,
        and don't call process() yourself afterwards.
        """
        q = queue.Queue(maxsize=maxsize)

        def push(t, x, y):
            if q.full():
                try:
                    q.get_nowait()
                except queue.Empty:
                    pass
            q.put_nowait((t, x, y))

        self.subscribe_gaze(push)
        def run():
            while not self._stop.is_set():
                self.process()

        self._thread = threading.Thread(target=run, name="tobii", daemon=True)
        self._thread.start()
        return q


if __name__ == "__main__":
    with EyeTracker() as et:
        print("connected to", et.url)
        try:
            for t, x, y in et.gaze():
                print(f"{t} {x:.4f} {y:.4f}")
        except KeyboardInterrupt:
            pass
