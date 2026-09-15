#!/usr/bin/env python3
"""ctypes adapter for the pixel-only CHERS C ABI; standard library only.

Set CHERS_CLIENT_LIB to libchers_client.so if it is not in ../build.
This is a transport example, not a trained model. `frame()` returns a reusable
RGBA buffer: copy it if keeping an observation while reading the next frame.
"""

import ctypes as C
import os
from pathlib import Path


class FrameInfo(C.Structure):
    _fields_ = [("abi_version", C.c_uint32), ("width", C.c_uint32),
                ("height", C.c_uint32), ("stride", C.c_uint32),
                ("sequence", C.c_uint64)]


class Client:
    FRAME_BYTES = 640 * 360 * 4

    def __init__(self, endpoint=None, library=None):
        candidate = Path(__file__).resolve().parent.parent / "build" / "libchers_client.so"
        self.lib = C.CDLL(library or os.environ.get("CHERS_CLIENT_LIB", os.environ.get("CHRONOLANE_CLIENT_LIB", str(candidate))))
        self.lib.cl_connect.argtypes = [C.c_char_p, C.c_int, C.POINTER(C.c_void_p)]
        self.lib.cl_connect.restype = C.c_int
        self.lib.cl_read_frame.argtypes = [C.c_void_p, C.c_void_p, C.c_size_t,
                                           C.POINTER(FrameInfo), C.c_int]
        self.lib.cl_read_frame.restype = C.c_int
        self.lib.cl_tap.argtypes = [C.c_void_p, C.c_uint32]
        self.lib.cl_tap.restype = C.c_int
        for name in ("cl_start", "cl_stop"):
            getattr(self.lib, name).argtypes = [C.c_void_p]
            getattr(self.lib, name).restype = C.c_int
        self.lib.cl_close.argtypes = [C.c_void_p]
        self.lib.cl_close.restype = None
        self.lib.cl_result_string.argtypes = [C.c_int]
        self.lib.cl_result_string.restype = C.c_char_p
        self.handle = C.c_void_p()
        self.pixels = (C.c_ubyte * self.FRAME_BYTES)()
        self._check(self.lib.cl_connect(os.fsencode(endpoint) if endpoint else None,
                                        2000, C.byref(self.handle)))

    def _check(self, result):
        if result != 0:
            raise RuntimeError(self.lib.cl_result_string(result).decode())

    def frame(self, timeout_ms=1000):
        info = FrameInfo()
        result = self.lib.cl_read_frame(self.handle, self.pixels, self.FRAME_BYTES,
                                       C.byref(info), timeout_ms)
        if result == 1:
            return None
        self._check(result)
        return info, memoryview(self.pixels).cast("B")

    def start(self):
        self._check(self.lib.cl_start(self.handle))

    def tap(self, lane):
        """lane is A-P or integer 0-15; one call is one discrete input."""
        if isinstance(lane, str):
            if len(lane) != 1 or not "A" <= lane.upper() <= "P":
                raise ValueError("lane must be A-P")
            lane = ord(lane.upper()) - ord("A")
        if not isinstance(lane, int) or not 0 <= lane <= 15:
            raise ValueError("lane must be 0-15")
        self._check(self.lib.cl_tap(self.handle, lane))

    def stop(self):
        self._check(self.lib.cl_stop(self.handle))

    def close(self):
        if self.handle:
            self.lib.cl_close(self.handle)
            self.handle = C.c_void_p()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


if __name__ == "__main__":
    import sys
    with Client(sys.argv[1] if len(sys.argv) > 1 else None) as client:
        observation = client.frame()
        if observation is None:
            raise SystemExit("No frame available within one second")
        info, rgba = observation
        output = Path("chers-frame.ppm")
        # PPM is a portable example artifact; models can use rgba directly.
        rgb = bytearray(640 * 360 * 3)
        rgb[0::3] = rgba[0::4]
        rgb[1::3] = rgba[1::4]
        rgb[2::3] = rgba[2::4]
        output.write_bytes(b"P6\n640 360\n255\n" + rgb)
        print(f"Saved frame {info.sequence} to {output}")
