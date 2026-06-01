#!/usr/bin/env python3
import argparse
import ctypes
import glob
import html
import json
import math
import mmap
import os
import select
import signal
import struct
import subprocess
import sys
import threading
import time
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse


IOC_WRITE = 1
IOC_READ = 2
MEDIA_LNK_FL_ENABLED = 1
MEDIA_LNK_FL_IMMUTABLE = 2
MEDIA_LNK_FL_INTERFACE_LINK = 1 << 28
MEDIA_BUS_FMT_SGRBG10_1X10 = 0x300a
V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE = 9
V4L2_BUF_TYPE_VIDEO_OUTPUT = 2
V4L2_MEMORY_MMAP = 1
V4L2_FIELD_NONE = 1
V4L2_SUBDEV_FORMAT_ACTIVE = 1
V4L2_SEL_TGT_CROP = 0x0000
V4L2_SEL_TGT_CROP_DEFAULT = 0x0001
V4L2_SEL_TGT_CROP_BOUNDS = 0x0002
V4L2_SEL_TGT_NATIVE_SIZE = 0x0003
V4L2_FRMSIZE_TYPE_DISCRETE = 1
V4L2_FRMSIZE_TYPE_CONTINUOUS = 2
V4L2_FRMSIZE_TYPE_STEPWISE = 3
V4L2_FRMIVAL_TYPE_DISCRETE = 1
V4L2_FRMIVAL_TYPE_CONTINUOUS = 2
V4L2_FRMIVAL_TYPE_STEPWISE = 3
SELECTION_TARGET_NAMES = {
    V4L2_SEL_TGT_CROP: "crop",
    V4L2_SEL_TGT_CROP_DEFAULT: "crop_default",
    V4L2_SEL_TGT_CROP_BOUNDS: "crop_bounds",
    V4L2_SEL_TGT_NATIVE_SIZE: "native_size",
}
FRAME_SIZE_TYPE_NAMES = {
    V4L2_FRMSIZE_TYPE_DISCRETE: "discrete",
    V4L2_FRMSIZE_TYPE_CONTINUOUS: "continuous",
    V4L2_FRMSIZE_TYPE_STEPWISE: "stepwise",
}
FRAME_INTERVAL_TYPE_NAMES = {
    V4L2_FRMIVAL_TYPE_DISCRETE: "discrete",
    V4L2_FRMIVAL_TYPE_CONTINUOUS: "continuous",
    V4L2_FRMIVAL_TYPE_STEPWISE: "stepwise",
}
V4L2_CID_EXPOSURE = 0x00980911
V4L2_CID_VBLANK = 0x009e0901
V4L2_CID_ANALOGUE_GAIN = 0x009e0903
V4L2_CID_UNIT_CELL_SIZE = 0x009e0908
V4L2_CID_DIGITAL_GAIN = 0x009f0905
V4L2_CID_CAMERA_ORIENTATION = 0x009a0922
V4L2_CID_CAMERA_SENSOR_ROTATION = 0x009a0923
V4L2_CTRL_TYPE_INTEGER = 1
V4L2_CTRL_TYPE_BOOLEAN = 2
V4L2_CTRL_TYPE_MENU = 3
V4L2_CTRL_TYPE_BUTTON = 4
V4L2_CTRL_TYPE_INTEGER64 = 5
V4L2_CTRL_TYPE_CTRL_CLASS = 6
V4L2_CTRL_TYPE_STRING = 7
V4L2_CTRL_TYPE_BITMASK = 8
V4L2_CTRL_TYPE_INTEGER_MENU = 9
V4L2_CTRL_COMPOUND_TYPES = 0x0100
V4L2_CTRL_TYPE_AREA = 0x0106
V4L2_CTRL_FLAG_DISABLED = 0x0001
V4L2_CTRL_FLAG_GRABBED = 0x0002
V4L2_CTRL_FLAG_READ_ONLY = 0x0004
V4L2_CTRL_FLAG_UPDATE = 0x0008
V4L2_CTRL_FLAG_INACTIVE = 0x0010
V4L2_CTRL_FLAG_SLIDER = 0x0020
V4L2_CTRL_FLAG_WRITE_ONLY = 0x0040
V4L2_CTRL_FLAG_VOLATILE = 0x0080
V4L2_CTRL_FLAG_HAS_PAYLOAD = 0x0100
V4L2_CTRL_FLAG_EXECUTE_ON_WRITE = 0x0200
V4L2_CTRL_FLAG_MODIFY_LAYOUT = 0x0400
V4L2_CTRL_FLAG_DYNAMIC_ARRAY = 0x0800
V4L2_CTRL_FLAG_HAS_WHICH_MIN_MAX = 0x1000
V4L2_CTRL_FLAG_NEXT_COMPOUND = 0x40000000
V4L2_CTRL_FLAG_NEXT_CTRL = 0x80000000
CONTROL_NAMES = {
    V4L2_CID_VBLANK: "vblank",
    V4L2_CID_EXPOSURE: "exposure",
    V4L2_CID_ANALOGUE_GAIN: "analogue_gain",
    V4L2_CID_DIGITAL_GAIN: "digital_gain",
    V4L2_CID_CAMERA_ORIENTATION: "camera_orientation",
    V4L2_CID_CAMERA_SENSOR_ROTATION: "camera_sensor_rotation",
}
CONTROL_TYPE_NAMES = {
    V4L2_CTRL_TYPE_INTEGER: "integer",
    V4L2_CTRL_TYPE_BOOLEAN: "boolean",
    V4L2_CTRL_TYPE_MENU: "menu",
    V4L2_CTRL_TYPE_BUTTON: "button",
    V4L2_CTRL_TYPE_INTEGER64: "integer64",
    V4L2_CTRL_TYPE_CTRL_CLASS: "ctrl_class",
    V4L2_CTRL_TYPE_STRING: "string",
    V4L2_CTRL_TYPE_BITMASK: "bitmask",
    V4L2_CTRL_TYPE_INTEGER_MENU: "integer_menu",
    V4L2_CTRL_TYPE_AREA: "area",
}
CONTROL_FLAG_NAMES = {
    V4L2_CTRL_FLAG_DISABLED: "disabled",
    V4L2_CTRL_FLAG_GRABBED: "grabbed",
    V4L2_CTRL_FLAG_READ_ONLY: "read_only",
    V4L2_CTRL_FLAG_UPDATE: "update",
    V4L2_CTRL_FLAG_INACTIVE: "inactive",
    V4L2_CTRL_FLAG_SLIDER: "slider",
    V4L2_CTRL_FLAG_WRITE_ONLY: "write_only",
    V4L2_CTRL_FLAG_VOLATILE: "volatile",
    V4L2_CTRL_FLAG_HAS_PAYLOAD: "has_payload",
    V4L2_CTRL_FLAG_EXECUTE_ON_WRITE: "execute_on_write",
    V4L2_CTRL_FLAG_MODIFY_LAYOUT: "modify_layout",
    V4L2_CTRL_FLAG_DYNAMIC_ARRAY: "dynamic_array",
    V4L2_CTRL_FLAG_HAS_WHICH_MIN_MAX: "has_which_min_max",
}
CAPABILITY_NAMES = {
    0x00001000: "video_capture_mplane",
    0x01000000: "readwrite",
    0x04000000: "streaming",
    0x20000000: "io_mc",
    0x80000000: "device_caps",
}
FMT_FLAG_NAMES = {
    0x0001: "compressed",
    0x0002: "emulated",
    0x0004: "continuous_bytestream",
}


def ioc(direction, type_char, nr, size):
    return (direction << 30) | (ord(type_char) << 8) | nr | (size << 16)


def iowr(type_char, nr, size):
    return ioc(IOC_READ | IOC_WRITE, type_char, nr, size)


def iow(type_char, nr, size):
    return ioc(IOC_WRITE, type_char, nr, size)


def ior(type_char, nr, size):
    return ioc(IOC_READ, type_char, nr, size)


def fourcc(value):
    if len(value) != 4:
        raise ValueError(f"fourcc must be 4 characters: {value}")
    return ord(value[0]) | (ord(value[1]) << 8) | (ord(value[2]) << 16) | (ord(value[3]) << 24)


def fourcc_name(value):
    return bytes([
        value & 0xff,
        (value >> 8) & 0xff,
        (value >> 16) & 0xff,
        (value >> 24) & 0xff,
    ]).decode("latin1")


def decode_c_string(value):
    return bytes(value).split(b"\0", 1)[0].decode("utf-8", "replace")


def flags_to_names(value, names):
    return [name for bit, name in names.items() if value & bit]


class MediaPadDesc(ctypes.Structure):
    _fields_ = [
        ("entity", ctypes.c_uint32),
        ("index", ctypes.c_uint16),
        ("flags", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32 * 2),
    ]


class MediaLinkDesc(ctypes.Structure):
    _fields_ = [
        ("source", MediaPadDesc),
        ("sink", MediaPadDesc),
        ("flags", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32 * 2),
    ]


class MbusFrameFormat(ctypes.Structure):
    _fields_ = [
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
        ("code", ctypes.c_uint32),
        ("field", ctypes.c_uint32),
        ("colorspace", ctypes.c_uint32),
        ("ycbcr_enc", ctypes.c_uint16),
        ("quantization", ctypes.c_uint16),
        ("xfer_func", ctypes.c_uint16),
        ("flags", ctypes.c_uint16),
        ("reserved", ctypes.c_uint16 * 10),
    ]


class SubdevFormat(ctypes.Structure):
    _fields_ = [
        ("which", ctypes.c_uint32),
        ("pad", ctypes.c_uint32),
        ("format", MbusFrameFormat),
        ("stream", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32 * 7),
    ]


class V4L2Fract(ctypes.Structure):
    _fields_ = [
        ("numerator", ctypes.c_uint32),
        ("denominator", ctypes.c_uint32),
    ]


class V4L2Rect(ctypes.Structure):
    _fields_ = [
        ("left", ctypes.c_int32),
        ("top", ctypes.c_int32),
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
    ]


class V4L2Area(ctypes.Structure):
    _fields_ = [
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
    ]


class SubdevFrameInterval(ctypes.Structure):
    _fields_ = [
        ("pad", ctypes.c_uint32),
        ("interval", V4L2Fract),
        ("stream", ctypes.c_uint32),
        ("which", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32 * 7),
    ]


class SubdevFrameIntervalEnum(ctypes.Structure):
    _fields_ = [
        ("index", ctypes.c_uint32),
        ("pad", ctypes.c_uint32),
        ("code", ctypes.c_uint32),
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
        ("interval", V4L2Fract),
        ("which", ctypes.c_uint32),
        ("stream", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32 * 7),
    ]


class SubdevFrameSizeEnum(ctypes.Structure):
    _fields_ = [
        ("index", ctypes.c_uint32),
        ("pad", ctypes.c_uint32),
        ("code", ctypes.c_uint32),
        ("min_width", ctypes.c_uint32),
        ("max_width", ctypes.c_uint32),
        ("min_height", ctypes.c_uint32),
        ("max_height", ctypes.c_uint32),
        ("which", ctypes.c_uint32),
        ("stream", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32 * 7),
    ]


class V4L2FrmSizeDiscrete(ctypes.Structure):
    _fields_ = [
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
    ]


class V4L2FrmSizeStepwise(ctypes.Structure):
    _fields_ = [
        ("min_width", ctypes.c_uint32),
        ("max_width", ctypes.c_uint32),
        ("step_width", ctypes.c_uint32),
        ("min_height", ctypes.c_uint32),
        ("max_height", ctypes.c_uint32),
        ("step_height", ctypes.c_uint32),
    ]


class V4L2FrmSizeUnion(ctypes.Union):
    _fields_ = [
        ("discrete", V4L2FrmSizeDiscrete),
        ("stepwise", V4L2FrmSizeStepwise),
    ]


class V4L2FrmSizeEnum(ctypes.Structure):
    _fields_ = [
        ("index", ctypes.c_uint32),
        ("pixel_format", ctypes.c_uint32),
        ("type", ctypes.c_uint32),
        ("u", V4L2FrmSizeUnion),
        ("reserved", ctypes.c_uint32 * 2),
    ]


class V4L2FrmIvalStepwise(ctypes.Structure):
    _fields_ = [
        ("min", V4L2Fract),
        ("max", V4L2Fract),
        ("step", V4L2Fract),
    ]


class V4L2FrmIvalUnion(ctypes.Union):
    _fields_ = [
        ("discrete", V4L2Fract),
        ("stepwise", V4L2FrmIvalStepwise),
    ]


class V4L2FrmIvalEnum(ctypes.Structure):
    _fields_ = [
        ("index", ctypes.c_uint32),
        ("pixel_format", ctypes.c_uint32),
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
        ("type", ctypes.c_uint32),
        ("u", V4L2FrmIvalUnion),
        ("reserved", ctypes.c_uint32 * 2),
    ]


class SubdevSelection(ctypes.Structure):
    _fields_ = [
        ("which", ctypes.c_uint32),
        ("pad", ctypes.c_uint32),
        ("target", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("r", V4L2Rect),
        ("stream", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32 * 7),
    ]


class V4L2PlanePixFormat(ctypes.Structure):
    _layout_ = "ms"
    _pack_ = 1
    _fields_ = [
        ("sizeimage", ctypes.c_uint32),
        ("bytesperline", ctypes.c_uint32),
        ("reserved", ctypes.c_uint16 * 6),
    ]


class V4L2PixFormatMPlane(ctypes.Structure):
    _layout_ = "ms"
    _pack_ = 1
    _fields_ = [
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
        ("pixelformat", ctypes.c_uint32),
        ("field", ctypes.c_uint32),
        ("colorspace", ctypes.c_uint32),
        ("plane_fmt", V4L2PlanePixFormat * 8),
        ("num_planes", ctypes.c_uint8),
        ("flags", ctypes.c_uint8),
        ("ycbcr_enc", ctypes.c_uint8),
        ("quantization", ctypes.c_uint8),
        ("xfer_func", ctypes.c_uint8),
        ("reserved", ctypes.c_uint8 * 7),
    ]


class V4L2PixFormat(ctypes.Structure):
    _layout_ = "ms"
    _pack_ = 1
    _fields_ = [
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
        ("pixelformat", ctypes.c_uint32),
        ("field", ctypes.c_uint32),
        ("bytesperline", ctypes.c_uint32),
        ("sizeimage", ctypes.c_uint32),
        ("colorspace", ctypes.c_uint32),
        ("priv", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("enc", ctypes.c_uint32),
        ("quantization", ctypes.c_uint32),
        ("xfer_func", ctypes.c_uint32),
    ]


class FormatUnion(ctypes.Union):
    _fields_ = [
        ("pix", V4L2PixFormat),
        ("pix_mp", V4L2PixFormatMPlane),
        ("raw_data", ctypes.c_uint8 * 200),
    ]


class V4L2Format(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.c_uint32),
        ("pad0", ctypes.c_uint32),
        ("fmt", FormatUnion),
    ]


class RequestBuffers(ctypes.Structure):
    _fields_ = [
        ("count", ctypes.c_uint32),
        ("type", ctypes.c_uint32),
        ("memory", ctypes.c_uint32),
        ("capabilities", ctypes.c_uint32),
        ("flags", ctypes.c_uint8),
        ("reserved", ctypes.c_uint8 * 3),
    ]


class Timeval(ctypes.Structure):
    _fields_ = [
        ("tv_sec", ctypes.c_long),
        ("tv_usec", ctypes.c_long),
    ]


class Timecode(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("frames", ctypes.c_uint8),
        ("seconds", ctypes.c_uint8),
        ("minutes", ctypes.c_uint8),
        ("hours", ctypes.c_uint8),
        ("userbits", ctypes.c_uint8 * 4),
    ]


class PlaneMemory(ctypes.Union):
    _fields_ = [
        ("mem_offset", ctypes.c_uint32),
        ("userptr", ctypes.c_ulong),
        ("fd", ctypes.c_int32),
    ]


class V4L2Plane(ctypes.Structure):
    _fields_ = [
        ("bytesused", ctypes.c_uint32),
        ("length", ctypes.c_uint32),
        ("m", PlaneMemory),
        ("data_offset", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32 * 11),
    ]


class BufferMemory(ctypes.Union):
    _fields_ = [
        ("offset", ctypes.c_uint32),
        ("userptr", ctypes.c_ulong),
        ("planes", ctypes.POINTER(V4L2Plane)),
        ("fd", ctypes.c_int32),
    ]


class V4L2Buffer(ctypes.Structure):
    _fields_ = [
        ("index", ctypes.c_uint32),
        ("type", ctypes.c_uint32),
        ("bytesused", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("field", ctypes.c_uint32),
        ("timestamp", Timeval),
        ("timecode", Timecode),
        ("sequence", ctypes.c_uint32),
        ("memory", ctypes.c_uint32),
        ("m", BufferMemory),
        ("length", ctypes.c_uint32),
        ("reserved2", ctypes.c_uint32),
        ("request_fd", ctypes.c_int32),
    ]


class V4L2Control(ctypes.Structure):
    _fields_ = [
        ("id", ctypes.c_uint32),
        ("value", ctypes.c_int32),
    ]


class V4L2ExtControlValue(ctypes.Union):
    _fields_ = [
        ("value", ctypes.c_int32),
        ("value64", ctypes.c_int64),
        ("ptr", ctypes.c_void_p),
    ]


class V4L2ExtControl(ctypes.Structure):
    _layout_ = "ms"
    _pack_ = 1
    _fields_ = [
        ("id", ctypes.c_uint32),
        ("size", ctypes.c_uint32),
        ("reserved2", ctypes.c_uint32 * 1),
        ("value", V4L2ExtControlValue),
    ]


class V4L2ExtControls(ctypes.Structure):
    _fields_ = [
        ("ctrl_class", ctypes.c_uint32),
        ("count", ctypes.c_uint32),
        ("error_idx", ctypes.c_uint32),
        ("request_fd", ctypes.c_int32),
        ("reserved", ctypes.c_uint32 * 1),
        ("controls", ctypes.POINTER(V4L2ExtControl)),
    ]


class V4L2QueryControl(ctypes.Structure):
    _fields_ = [
        ("id", ctypes.c_uint32),
        ("type", ctypes.c_uint32),
        ("name", ctypes.c_uint8 * 32),
        ("minimum", ctypes.c_int32),
        ("maximum", ctypes.c_int32),
        ("step", ctypes.c_int32),
        ("default_value", ctypes.c_int32),
        ("flags", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32 * 2),
    ]


class V4L2QueryExtControl(ctypes.Structure):
    _fields_ = [
        ("id", ctypes.c_uint32),
        ("type", ctypes.c_uint32),
        ("name", ctypes.c_uint8 * 32),
        ("minimum", ctypes.c_int64),
        ("maximum", ctypes.c_int64),
        ("step", ctypes.c_uint64),
        ("default_value", ctypes.c_int64),
        ("flags", ctypes.c_uint32),
        ("elem_size", ctypes.c_uint32),
        ("elems", ctypes.c_uint32),
        ("nr_of_dims", ctypes.c_uint32),
        ("dims", ctypes.c_uint32 * 4),
        ("reserved", ctypes.c_uint32 * 32),
    ]


class QueryMenuUnion(ctypes.Union):
    _fields_ = [
        ("name", ctypes.c_uint8 * 32),
        ("value", ctypes.c_int64),
    ]


class V4L2QueryMenu(ctypes.Structure):
    _fields_ = [
        ("id", ctypes.c_uint32),
        ("index", ctypes.c_uint32),
        ("u", QueryMenuUnion),
        ("reserved", ctypes.c_uint32),
    ]


class V4L2Capability(ctypes.Structure):
    _fields_ = [
        ("driver", ctypes.c_uint8 * 16),
        ("card", ctypes.c_uint8 * 32),
        ("bus_info", ctypes.c_uint8 * 32),
        ("version", ctypes.c_uint32),
        ("capabilities", ctypes.c_uint32),
        ("device_caps", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32 * 3),
    ]


class V4L2FmtDesc(ctypes.Structure):
    _fields_ = [
        ("index", ctypes.c_uint32),
        ("type", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("description", ctypes.c_uint8 * 32),
        ("pixelformat", ctypes.c_uint32),
        ("mbus_code", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32 * 3),
    ]


MEDIA_IOC_SETUP_LINK = iowr("|", 3, ctypes.sizeof(MediaLinkDesc))
VIDIOC_QUERYCAP = ior("V", 0, ctypes.sizeof(V4L2Capability))
VIDIOC_ENUM_FMT = iowr("V", 2, ctypes.sizeof(V4L2FmtDesc))
VIDIOC_G_FMT = iowr("V", 4, ctypes.sizeof(V4L2Format))
VIDIOC_ENUM_FRAMESIZES = iowr("V", 74, ctypes.sizeof(V4L2FrmSizeEnum))
VIDIOC_ENUM_FRAMEINTERVALS = iowr("V", 75, ctypes.sizeof(V4L2FrmIvalEnum))
VIDIOC_SUBDEV_G_FMT = iowr("V", 4, ctypes.sizeof(SubdevFormat))
VIDIOC_SUBDEV_S_FMT = iowr("V", 5, ctypes.sizeof(SubdevFormat))
VIDIOC_SUBDEV_G_FRAME_INTERVAL = iowr("V", 21, ctypes.sizeof(SubdevFrameInterval))
VIDIOC_SUBDEV_S_FRAME_INTERVAL = iowr("V", 22, ctypes.sizeof(SubdevFrameInterval))
VIDIOC_SUBDEV_ENUM_FRAME_SIZE = iowr("V", 74, ctypes.sizeof(SubdevFrameSizeEnum))
VIDIOC_SUBDEV_ENUM_FRAME_INTERVAL = iowr("V", 75, ctypes.sizeof(SubdevFrameIntervalEnum))
VIDIOC_SUBDEV_G_SELECTION = iowr("V", 61, ctypes.sizeof(SubdevSelection))
VIDIOC_S_FMT = iowr("V", 5, ctypes.sizeof(V4L2Format))
VIDIOC_TRY_FMT = iowr("V", 64, ctypes.sizeof(V4L2Format))
VIDIOC_REQBUFS = iowr("V", 8, ctypes.sizeof(RequestBuffers))
VIDIOC_QUERYBUF = iowr("V", 9, ctypes.sizeof(V4L2Buffer))
VIDIOC_QBUF = iowr("V", 15, ctypes.sizeof(V4L2Buffer))
VIDIOC_DQBUF = iowr("V", 17, ctypes.sizeof(V4L2Buffer))
VIDIOC_STREAMON = iow("V", 18, ctypes.sizeof(ctypes.c_int))
VIDIOC_STREAMOFF = iow("V", 19, ctypes.sizeof(ctypes.c_int))
VIDIOC_G_CTRL = iowr("V", 27, ctypes.sizeof(V4L2Control))
VIDIOC_S_CTRL = iowr("V", 28, ctypes.sizeof(V4L2Control))
VIDIOC_QUERYCTRL = iowr("V", 36, ctypes.sizeof(V4L2QueryControl))
VIDIOC_QUERYMENU = iowr("V", 37, ctypes.sizeof(V4L2QueryMenu))
VIDIOC_G_EXT_CTRLS = iowr("V", 71, ctypes.sizeof(V4L2ExtControls))
VIDIOC_QUERY_EXT_CTRL = iowr("V", 103, ctypes.sizeof(V4L2QueryExtControl))

_libc = ctypes.CDLL(None, use_errno=True)


def ioctl(fd, request, obj, label):
    ret = _libc.ioctl(fd, ctypes.c_ulong(request), ctypes.byref(obj))
    if ret < 0:
        err = ctypes.get_errno()
        raise OSError(err, f"{label}: {os.strerror(err)}")
    return ret


def video4linux_nodes(prefix):
    nodes = []
    for path in glob.glob(f"/dev/{prefix}*"):
        name_path = f"/sys/class/video4linux/{os.path.basename(path)}/name"
        try:
            with open(name_path, "r", encoding="utf-8") as file:
                name = file.read().strip()
        except OSError:
            name = ""
        nodes.append((path, name))
    return sorted(nodes, key=lambda item: item[0])


def find_devnode_by_name(prefix, needle):
    matches = [(path, name) for path, name in video4linux_nodes(prefix) if needle in name]
    if not matches:
        raise RuntimeError(f"no /dev/{prefix}* node named like {needle!r}")
    if len(matches) > 1:
        names = ", ".join(f"{path}:{name}" for path, name in matches)
        raise RuntimeError(f"multiple nodes match {needle!r}: {names}")
    return matches[0][0]


def clamp(value, minimum, maximum):
    return max(minimum, min(maximum, int(value)))


def float_clamp(value, minimum, maximum):
    return max(minimum, min(maximum, float(value)))


def parse_ccm(value):
    items = [float(item.strip()) for item in value.split(",") if item.strip()]
    if len(items) != 9:
        raise argparse.ArgumentTypeError("CCM must contain 9 comma-separated numbers")
    return items


def luma_summary(values):
    if not values:
        return {
            "samples": 0,
            "mean": 0.0,
            "p50": 0.0,
            "p70": 0.0,
            "p90": 0.0,
        }
    ordered = sorted(values)
    count = len(ordered)

    def percentile(fraction):
        return ordered[min(count - 1, max(0, int(round((count - 1) * fraction))))]

    return {
        "samples": count,
        "mean": sum(ordered) / count,
        "p50": percentile(0.50),
        "p70": percentile(0.70),
        "p90": percentile(0.90),
    }


class SensorControls:
    def __init__(self, path):
        self.path = path
        self.fd = os.open(path, os.O_RDWR)
        self.info = {}
        self.refresh()

    def refresh(self, ctrl_id=None):
        ctrl_ids = [ctrl_id] if ctrl_id else CONTROL_NAMES
        for item in ctrl_ids:
            try:
                self.info[item] = self.query(item)
            except OSError:
                self.info.pop(item, None)

    def query(self, ctrl_id):
        query = V4L2QueryControl()
        query.id = ctrl_id
        ioctl(self.fd, VIDIOC_QUERYCTRL, query, f"{self.path} QUERYCTRL {CONTROL_NAMES.get(ctrl_id, hex(ctrl_id))}")
        return self.query_to_dict(query)

    def query_ext(self, ctrl_id):
        query = V4L2QueryExtControl()
        query.id = ctrl_id
        ioctl(self.fd, VIDIOC_QUERY_EXT_CTRL, query, f"{self.path} QUERY_EXT_CTRL {CONTROL_NAMES.get(ctrl_id, hex(ctrl_id))}")
        return self.query_ext_to_dict(query)

    def query_to_dict(self, query):
        return {
            "id": f"0x{query.id:08x}",
            "id_value": query.id,
            "name": decode_c_string(query.name),
            "type": query.type,
            "type_name": CONTROL_TYPE_NAMES.get(query.type, f"unknown_{query.type}"),
            "minimum": query.minimum,
            "maximum": query.maximum,
            "step": query.step,
            "default": query.default_value,
            "flags": f"0x{query.flags:x}",
            "flag_names": flags_to_names(query.flags, CONTROL_FLAG_NAMES),
        }

    def query_ext_to_dict(self, query):
        nr_of_dims = min(query.nr_of_dims, len(query.dims))
        return {
            "id": f"0x{query.id:08x}",
            "id_value": query.id,
            "name": decode_c_string(query.name),
            "type": query.type,
            "type_name": CONTROL_TYPE_NAMES.get(query.type, f"unknown_{query.type}"),
            "minimum": query.minimum,
            "maximum": query.maximum,
            "step": query.step,
            "default": query.default_value,
            "flags": f"0x{query.flags:x}",
            "flag_names": flags_to_names(query.flags, CONTROL_FLAG_NAMES),
            "elem_size": query.elem_size,
            "elems": query.elems,
            "nr_of_dims": query.nr_of_dims,
            "dims": [query.dims[index] for index in range(nr_of_dims)],
            "query_ext_ctrl": True,
        }

    def enumerate(self):
        controls = []
        next_flags = V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND
        next_id = next_flags
        seen = set()
        while True:
            query = V4L2QueryExtControl()
            query.id = next_id
            try:
                ioctl(self.fd, VIDIOC_QUERY_EXT_CTRL, query, f"{self.path} QUERY_EXT_CTRL NEXT {next_id:#x}")
            except OSError as exc:
                if exc.errno == 22:
                    break
                raise
            if query.id in seen:
                break
            seen.add(query.id)
            item = self.query_ext_to_dict(query)
            if query.type == V4L2_CTRL_TYPE_CTRL_CLASS:
                item["current_unavailable"] = "control class"
            elif query.flags & V4L2_CTRL_FLAG_DISABLED:
                item["current_unavailable"] = "disabled"
            elif query.type == V4L2_CTRL_TYPE_INTEGER64:
                try:
                    item["current"] = self.get_ext_int64(query.id)
                except OSError as exc:
                    item["current_error"] = str(exc)
            elif query.type == V4L2_CTRL_TYPE_AREA:
                try:
                    item["current"] = self.get_ext_area(query.id)
                except OSError as exc:
                    item["current_error"] = str(exc)
            elif query.type in (V4L2_CTRL_TYPE_BUTTON, V4L2_CTRL_TYPE_STRING):
                item["current_unavailable"] = "requires extended control ioctl or is write-only"
            elif query.type >= V4L2_CTRL_COMPOUND_TYPES:
                item["current_unavailable"] = "compound control type not decoded"
            else:
                try:
                    item["current"] = self.get(query.id)
                except OSError as exc:
                    item["current_error"] = str(exc)
            if query.type in (V4L2_CTRL_TYPE_MENU, V4L2_CTRL_TYPE_INTEGER_MENU):
                item["menu"] = self.query_menu_items(query)
            controls.append(item)
            next_id = query.id | next_flags
        return controls

    def query_menu_items(self, query):
        items = []
        step = max(1, query.step)
        for index in range(query.minimum, query.maximum + 1, step):
            menu = V4L2QueryMenu()
            menu.id = query.id
            menu.index = index
            try:
                ioctl(self.fd, VIDIOC_QUERYMENU, menu, f"{self.path} QUERYMENU {query.id:#x}:{index}")
            except OSError as exc:
                if exc.errno == 25:
                    return []
                if exc.errno == 22:
                    continue
                raise
            item = {"index": index}
            if query.type == V4L2_CTRL_TYPE_INTEGER_MENU:
                item["value"] = menu.u.value
            else:
                item["name"] = decode_c_string(menu.u.name)
            items.append(item)
        return items

    def get(self, ctrl_id):
        ctrl = V4L2Control()
        ctrl.id = ctrl_id
        ioctl(self.fd, VIDIOC_G_CTRL, ctrl, f"{self.path} G_CTRL {CONTROL_NAMES.get(ctrl_id, hex(ctrl_id))}")
        return ctrl.value

    def get_ext_int64(self, ctrl_id):
        controls = (V4L2ExtControl * 1)()
        controls[0].id = ctrl_id
        ctrls = V4L2ExtControls()
        ctrls.ctrl_class = ctrl_id & 0x0fff0000
        ctrls.count = 1
        ctrls.controls = ctypes.cast(controls, ctypes.POINTER(V4L2ExtControl))
        ioctl(self.fd, VIDIOC_G_EXT_CTRLS, ctrls, f"{self.path} G_EXT_CTRLS {hex(ctrl_id)}")
        return controls[0].value.value64

    def get_ext_area(self, ctrl_id):
        area = V4L2Area()
        controls = (V4L2ExtControl * 1)()
        controls[0].id = ctrl_id
        controls[0].size = ctypes.sizeof(area)
        controls[0].value.ptr = ctypes.addressof(area)
        ctrls = V4L2ExtControls()
        ctrls.ctrl_class = ctrl_id & 0x0fff0000
        ctrls.count = 1
        ctrls.controls = ctypes.cast(controls, ctypes.POINTER(V4L2ExtControl))
        ioctl(self.fd, VIDIOC_G_EXT_CTRLS, ctrls, f"{self.path} G_EXT_CTRLS {hex(ctrl_id)}")
        return {"width": area.width, "height": area.height}

    def set(self, ctrl_id, value):
        info = self.info.get(ctrl_id)
        if info:
            value = clamp(value, info["minimum"], info["maximum"])
        ctrl = V4L2Control()
        ctrl.id = ctrl_id
        ctrl.value = int(value)
        ioctl(self.fd, VIDIOC_S_CTRL, ctrl, f"{self.path} S_CTRL {CONTROL_NAMES.get(ctrl_id, hex(ctrl_id))}")
        if ctrl_id == V4L2_CID_VBLANK:
            self.refresh(V4L2_CID_VBLANK)
            self.refresh(V4L2_CID_EXPOSURE)
        return ctrl.value

    def reset(self):
        for ctrl_id in (V4L2_CID_VBLANK, V4L2_CID_EXPOSURE, V4L2_CID_ANALOGUE_GAIN, V4L2_CID_DIGITAL_GAIN):
            info = self.info.get(ctrl_id)
            if info:
                self.set(ctrl_id, info["default"])

    def status(self):
        out = {}
        for ctrl_id, name in CONTROL_NAMES.items():
            if ctrl_id not in self.info:
                continue
            try:
                out[name] = self.get(ctrl_id)
            except OSError as exc:
                out[f"{name}_error"] = str(exc)
        return out

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None


class MediaTopology:
    top_fmt = "=QIIQIIQIIQIIQ"
    ent_fmt = "=I64sII5I"
    pad_fmt = "=IIII4I"
    link_fmt = "=IIII6I"
    intf_fmt = "=III9I16I"
    ioctl_g_topology = iowr("|", 4, struct.calcsize(top_fmt))

    def __init__(self, media):
        self.media = media
        self.entities = {}
        self.entity_by_name = {}
        self.pads = {}
        self.links = []
        self.load()

    def load(self):
        import fcntl

        fd = os.open(self.media, os.O_RDWR)
        try:
            top = bytearray(struct.calcsize(self.top_fmt))
            fcntl.ioctl(fd, self.ioctl_g_topology, top, True)
            _, nents, _, _, nintfs, _, _, npads, _, _, nlinks, _, _ = struct.unpack(self.top_fmt, top)
            ents = bytearray(nents * struct.calcsize(self.ent_fmt))
            intfs = bytearray(nintfs * struct.calcsize(self.intf_fmt))
            pads = bytearray(npads * struct.calcsize(self.pad_fmt))
            links = bytearray(nlinks * struct.calcsize(self.link_fmt))
            top = bytearray(struct.calcsize(self.top_fmt))
            struct.pack_into(
                self.top_fmt,
                top,
                0,
                0,
                nents,
                0,
                ctypes.addressof(ctypes.c_char.from_buffer(ents)),
                nintfs,
                0,
                ctypes.addressof(ctypes.c_char.from_buffer(intfs)),
                npads,
                0,
                ctypes.addressof(ctypes.c_char.from_buffer(pads)),
                nlinks,
                0,
                ctypes.addressof(ctypes.c_char.from_buffer(links)),
            )
            fcntl.ioctl(fd, self.ioctl_g_topology, top, True)
        finally:
            os.close(fd)

        ent_size = struct.calcsize(self.ent_fmt)
        for index in range(nents):
            entity_id, raw_name, function, flags, *_ = struct.unpack_from(self.ent_fmt, ents, index * ent_size)
            name = raw_name.split(b"\0", 1)[0].decode("utf-8", "replace")
            self.entities[entity_id] = {"id": entity_id, "name": name, "function": function, "flags": flags}
            self.entity_by_name[name] = entity_id

        pad_size = struct.calcsize(self.pad_fmt)
        for index in range(npads):
            pad_id, entity_id, flags, pad_index, *_ = struct.unpack_from(self.pad_fmt, pads, index * pad_size)
            self.pads[pad_id] = {"id": pad_id, "entity": entity_id, "flags": flags, "index": pad_index}

        link_size = struct.calcsize(self.link_fmt)
        for index in range(nlinks):
            link_id, source_id, sink_id, flags, *_ = struct.unpack_from(self.link_fmt, links, index * link_size)
            self.links.append({"id": link_id, "source": source_id, "sink": sink_id, "flags": flags})

    def find_entity(self, name_part):
        matches = [entity for entity in self.entities.values() if name_part in entity["name"]]
        if not matches:
            raise RuntimeError(f"media entity not found: {name_part}")
        if len(matches) > 1:
            raise RuntimeError("multiple media entities match %r: %s" % (
                name_part,
                ", ".join(entity["name"] for entity in matches),
            ))
        return matches[0]

    def entity_name_for_pad(self, pad_id):
        pad = self.pads.get(pad_id)
        if not pad:
            return None
        entity = self.entities.get(pad["entity"])
        return entity["name"] if entity else None

    def route_links(self):
        out = []
        for link in self.links:
            if link["flags"] & MEDIA_LNK_FL_INTERFACE_LINK:
                continue
            source = self.pads.get(link["source"])
            sink = self.pads.get(link["sink"])
            if not source or not sink:
                continue
            out.append((link, source, sink))
        return out

    def print_summary(self):
        print(f"media: {self.media}")
        for entity in sorted(self.entities.values(), key=lambda item: item["id"]):
            name = entity["name"]
            if any(part in name for part in ("ov13", "msm_csiphy", "msm_csid", "msm_vfe")):
                print(f"  entity {entity['id']:3d}: {name}")
        print("enabled data links:")
        for link, source, sink in self.route_links():
            if not link["flags"] & MEDIA_LNK_FL_ENABLED:
                continue
            source_name = self.entities[source["entity"]]["name"]
            sink_name = self.entities[sink["entity"]]["name"]
            print(f"  {source_name}:pad{source['index']} -> {sink_name}:pad{sink['index']} flags=0x{link['flags']:x}")


def setup_link(media, source_entity, source_pad, sink_entity, sink_pad, flags):
    fd = os.open(media, os.O_RDWR)
    try:
        link = MediaLinkDesc()
        link.source.entity = source_entity
        link.source.index = source_pad
        link.sink.entity = sink_entity
        link.sink.index = sink_pad
        link.flags = flags
        ioctl(fd, MEDIA_IOC_SETUP_LINK, link, f"MEDIA_IOC_SETUP_LINK {source_entity}:{source_pad}->{sink_entity}:{sink_pad}")
    finally:
        os.close(fd)


def configure_lmi_ov13b10_route(args):
    topology = MediaTopology(args.media)
    sensor = topology.find_entity(args.sensor)
    csiphy = topology.find_entity(args.csiphy)
    csid = topology.find_entity(args.csid)
    vfe = topology.find_entity(args.vfe)

    if args.media_print:
        topology.print_summary()

    if not args.keep_links:
        for link, source, sink in topology.route_links():
            if link["flags"] & MEDIA_LNK_FL_IMMUTABLE:
                continue
            source_name = topology.entities[source["entity"]]["name"]
            sink_name = topology.entities[sink["entity"]]["name"]
            is_camss_route = (
                source_name.startswith("msm_csiphy") and sink_name.startswith("msm_csid")
            ) or (
                source_name.startswith("msm_csid") and sink_name.startswith("msm_vfe")
            )
            if is_camss_route and link["flags"] & MEDIA_LNK_FL_ENABLED:
                setup_link(args.media, source["entity"], source["index"], sink["entity"], sink["index"], 0)

    setup_link(args.media, csiphy["id"], 1, csid["id"], 0, MEDIA_LNK_FL_ENABLED)
    setup_link(args.media, csid["id"], args.csid_source_pad, vfe["id"], 0, MEDIA_LNK_FL_ENABLED)

    sensor_node = args.sensor_subdev or find_devnode_by_name("v4l-subdev", sensor["name"])
    csiphy_node = args.csiphy_subdev or find_devnode_by_name("v4l-subdev", csiphy["name"])
    csid_node = args.csid_subdev or find_devnode_by_name("v4l-subdev", csid["name"])
    vfe_node = args.vfe_subdev or find_devnode_by_name("v4l-subdev", vfe["name"])
    args.sensor_subdev = sensor_node
    if not args.control_subdev:
        args.control_subdev = sensor_node
    if not args.video:
        args.video = find_devnode_by_name("video", args.video_entity)

    set_subdev_format(sensor_node, [0], args.width, args.height, args.mbus_code)
    set_subdev_format(csiphy_node, [0, 1], args.width, args.height, args.mbus_code)
    set_subdev_format(csid_node, [0, args.csid_source_pad], args.width, args.height, args.mbus_code)
    set_subdev_format(vfe_node, [0], args.width, args.height, args.mbus_code)
    args.video_configured_format = video_set_format(args.video, args.width, args.height, args.pixelformat)

    if args.media_print:
        MediaTopology(args.media).print_summary()



def set_subdev_format(path, pads, width, height, mbus_code):
    fd = os.open(path, os.O_RDWR)
    try:
        for pad in pads:
            fmt = SubdevFormat()
            fmt.which = V4L2_SUBDEV_FORMAT_ACTIVE
            fmt.pad = pad
            fmt.format.width = width
            fmt.format.height = height
            fmt.format.code = mbus_code
            fmt.format.field = V4L2_FIELD_NONE
            ioctl(fd, VIDIOC_SUBDEV_S_FMT, fmt, f"{path} S_FMT pad{pad}")
    finally:
        os.close(fd)


class IoctlCamera:
    def __init__(self, args):
        self.args = args
        self.fd = None
        self.buffers = []
        self.streaming = False
        self.width = args.width
        self.height = args.height
        self.pixelformat = args.pixelformat
        self.stride = args.stride_bytes
        self.sizeimage = 0
        self.controls = None
        self.control_status = {}
        self.ae_frames = 0

    def start(self):
        if self.args.control_subdev:
            self.controls = SensorControls(self.args.control_subdev)
            if self.args.reset_controls or (self.args.auto_exposure and not self.args.preserve_controls):
                self.controls.reset()
            self.apply_initial_controls()
        self.fd = os.open(self.args.video, os.O_RDWR | os.O_NONBLOCK)
        fmt = V4L2Format()
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
        fmt.fmt.pix_mp.width = self.args.width
        fmt.fmt.pix_mp.height = self.args.height
        fmt.fmt.pix_mp.pixelformat = fourcc(self.args.pixelformat)
        fmt.fmt.pix_mp.field = V4L2_FIELD_NONE
        fmt.fmt.pix_mp.num_planes = 1
        ioctl(self.fd, VIDIOC_S_FMT, fmt, f"{self.args.video} S_FMT")
        self.width = fmt.fmt.pix_mp.width
        self.height = fmt.fmt.pix_mp.height
        self.pixelformat = fourcc_name(fmt.fmt.pix_mp.pixelformat)
        self.stride = self.args.stride_bytes or fmt.fmt.pix_mp.plane_fmt[0].bytesperline
        self.sizeimage = fmt.fmt.pix_mp.plane_fmt[0].sizeimage

        req = RequestBuffers()
        req.count = self.args.buffers
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
        req.memory = V4L2_MEMORY_MMAP
        ioctl(self.fd, VIDIOC_REQBUFS, req, f"{self.args.video} REQBUFS")
        if req.count < 1:
            raise RuntimeError("V4L2 driver returned zero buffers")

        for index in range(req.count):
            planes = (V4L2Plane * 1)()
            buf = V4L2Buffer()
            buf.index = index
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
            buf.memory = V4L2_MEMORY_MMAP
            buf.length = 1
            buf.m.planes = planes
            ioctl(self.fd, VIDIOC_QUERYBUF, buf, f"{self.args.video} QUERYBUF {index}")
            mapping = mmap.mmap(
                self.fd,
                planes[0].length,
                flags=mmap.MAP_SHARED,
                prot=mmap.PROT_READ | mmap.PROT_WRITE,
                offset=planes[0].m.mem_offset,
            )
            self.buffers.append(mapping)
            self.queue(index)

        buf_type = ctypes.c_int(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
        ioctl(self.fd, VIDIOC_STREAMON, buf_type, f"{self.args.video} STREAMON")
        self.streaming = True
        if self.controls:
            self.control_status = self.controls.status()

    def apply_initial_controls(self):
        for attr, ctrl_id in (
            ("vblank", V4L2_CID_VBLANK),
            ("exposure", V4L2_CID_EXPOSURE),
            ("analogue_gain", V4L2_CID_ANALOGUE_GAIN),
            ("digital_gain", V4L2_CID_DIGITAL_GAIN),
        ):
            value = getattr(self.args, attr)
            if value is None or ctrl_id not in self.controls.info:
                continue
            self.controls.set(ctrl_id, value)
        self.control_status = self.controls.status()

    def update_auto_exposure(self, luma):
        if not self.args.auto_exposure or not self.controls:
            return self.control_status
        self.ae_frames += 1
        if self.ae_frames % max(1, self.args.ae_interval):
            return self.control_status
        if abs(luma - self.args.target_luma) <= self.args.ae_deadband:
            self.control_status = self.controls.status()
            return self.control_status

        values = self.controls.status()
        ratio = max(1.02, self.args.target_luma / max(luma, 1.0)) if luma < self.args.target_luma else max(1.02, luma / max(self.args.target_luma, 1.0))
        ratio = min(ratio, self.args.ae_max_step)
        changed = False
        if luma < self.args.target_luma:
            changed = self.raise_exposure(values, ratio)
        else:
            changed = self.lower_exposure(values, ratio)
        if not changed:
            self.control_status = values
        else:
            self.control_status = self.controls.status()
        return self.control_status

    def raise_exposure(self, values, ratio):
        info = self.controls.info.get(V4L2_CID_EXPOSURE)
        value = values.get("exposure")
        if info and value is not None and value < info["maximum"]:
            new_value = max(value + info["step"], int(value * ratio))
            self.controls.set(V4L2_CID_EXPOSURE, new_value)
            return True
        if self.raise_vblank(values, ratio):
            return True
        for name, ctrl_id in (
            ("analogue_gain", V4L2_CID_ANALOGUE_GAIN),
            ("digital_gain", V4L2_CID_DIGITAL_GAIN),
        ):
            info = self.controls.info.get(ctrl_id)
            value = values.get(name)
            if not info or value is None or value >= info["maximum"]:
                continue
            new_value = max(value + info["step"], int(value * ratio))
            self.controls.set(ctrl_id, new_value)
            return True
        return False

    def raise_vblank(self, values, ratio):
        info = self.controls.info.get(V4L2_CID_VBLANK)
        exp_info = self.controls.info.get(V4L2_CID_EXPOSURE)
        value = values.get("vblank")
        if not info or not exp_info or value is None or value >= info["maximum"]:
            return False
        limit = self.auto_vblank_limit(info)
        if value >= limit:
            return False
        current_max = exp_info["maximum"]
        desired_max = max(current_max + 1, int(current_max * ratio))
        new_value = min(limit, value + max(info["step"], desired_max - current_max))
        if new_value <= value:
            return False
        self.controls.set(V4L2_CID_VBLANK, new_value)
        exp_info = self.controls.info.get(V4L2_CID_EXPOSURE)
        if exp_info and values.get("exposure") is not None:
            self.controls.set(V4L2_CID_EXPOSURE, exp_info["maximum"])
        return True

    def auto_vblank_limit(self, info):
        if self.args.max_auto_vblank is not None:
            return min(info["maximum"], self.args.max_auto_vblank)
        if self.args.ae_mode == "video":
            return min(info["maximum"], info["default"])
        if self.args.ae_mode == "balanced":
            return min(info["maximum"], max(info["default"], 2048))
        return min(info["maximum"], 16000)

    def lower_exposure(self, values, ratio):
        for name, ctrl_id in (
            ("digital_gain", V4L2_CID_DIGITAL_GAIN),
            ("analogue_gain", V4L2_CID_ANALOGUE_GAIN),
            ("exposure", V4L2_CID_EXPOSURE),
        ):
            info = self.controls.info.get(ctrl_id)
            value = values.get(name)
            if not info or value is None or value <= info["minimum"]:
                continue
            new_value = min(value - info["step"], int(value / ratio))
            self.controls.set(ctrl_id, new_value)
            return True
        return self.lower_vblank(values, ratio)

    def lower_vblank(self, values, ratio):
        info = self.controls.info.get(V4L2_CID_VBLANK)
        exp_info = self.controls.info.get(V4L2_CID_EXPOSURE)
        value = values.get("vblank")
        exposure = values.get("exposure")
        if not info or not exp_info or value is None or value <= info["minimum"]:
            return False
        floor = self.args.vblank if self.args.vblank is not None else info["default"]
        if value <= floor:
            return False
        new_value = max(floor, info["minimum"], min(value - info["step"], int(value / ratio)))
        if exposure is not None:
            height = exp_info["maximum"] - value + 8
            new_value = max(new_value, exposure - height + 8)
        if new_value >= value:
            return False
        self.controls.set(V4L2_CID_VBLANK, new_value)
        return True

    def queue(self, index):
        planes = (V4L2Plane * 1)()
        buf = V4L2Buffer()
        buf.index = index
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
        buf.memory = V4L2_MEMORY_MMAP
        buf.length = 1
        buf.m.planes = planes
        ioctl(self.fd, VIDIOC_QBUF, buf, f"{self.args.video} QBUF {index}")

    def read_frame(self):
        ready, _, _ = select.select([self.fd], [], [], self.args.capture_timeout)
        if not ready:
            raise TimeoutError("timed out waiting for a camera frame")
        planes = (V4L2Plane * 1)()
        buf = V4L2Buffer()
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
        buf.memory = V4L2_MEMORY_MMAP
        buf.length = 1
        buf.m.planes = planes
        ioctl(self.fd, VIDIOC_DQBUF, buf, f"{self.args.video} DQBUF")
        bytesused = planes[0].bytesused or self.sizeimage
        data = bytes(self.buffers[buf.index][:bytesused])
        sequence = buf.sequence
        self.queue(buf.index)
        return data, {
            "sequence": sequence,
            "bytesused": bytesused,
            "stride": self.stride,
            "width": self.width,
            "height": self.height,
            "pixelformat": self.pixelformat,
        }

    def close(self):
        if self.fd is not None:
            if self.streaming:
                try:
                    buf_type = ctypes.c_int(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
                    ioctl(self.fd, VIDIOC_STREAMOFF, buf_type, f"{self.args.video} STREAMOFF")
                except OSError as exc:
                    print(exc, file=sys.stderr)
                self.streaming = False
            for mapping in self.buffers:
                mapping.close()
            self.buffers.clear()
            try:
                req = RequestBuffers()
                req.count = 0
                req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                req.memory = V4L2_MEMORY_MMAP
                ioctl(self.fd, VIDIOC_REQBUFS, req, f"{self.args.video} REQBUFS 0")
            except OSError:
                pass
            os.close(self.fd)
            self.fd = None
        if self.controls:
            self.controls.close()
            self.controls = None

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()


class PreviewState:
    def __init__(self):
        self.lock = threading.Lock()
        self.condition = threading.Condition(self.lock)
        self.frame = None
        self.raw = None
        self.content_type = "image/png"
        self.status = {"state": "starting", "frames": 0, "error": None, "updated_at": None}
        self.stop = threading.Event()


def now():
    return time.strftime("%Y-%m-%d %H:%M:%S")


def list_devices():
    print("media devices:")
    for path in sorted(glob.glob("/dev/media*")):
        print(f"  {path}")
    print("video devices:")
    for path, name in video4linux_nodes("video"):
        print(f"  {path}  {name}")
    print("subdev devices:")
    for path, name in video4linux_nodes("v4l-subdev"):
        print(f"  {path}  {name}")


def collect_nodes():
    return {
        "media": sorted(glob.glob("/dev/media*")),
        "video": [{"node": path, "name": name} for path, name in video4linux_nodes("video")],
        "subdev": [{"node": path, "name": name} for path, name in video4linux_nodes("v4l-subdev")],
    }


def collect_enabled_links(topology):
    links = []
    for link, source, sink in topology.route_links():
        if not link["flags"] & MEDIA_LNK_FL_ENABLED:
            continue
        links.append({
            "source": topology.entities[source["entity"]]["name"],
            "source_pad": source["index"],
            "sink": topology.entities[sink["entity"]]["name"],
            "sink_pad": sink["index"],
            "flags": f"0x{link['flags']:x}",
        })
    return links


def video_querycap(path):
    fd = os.open(path, os.O_RDWR)
    try:
        cap = V4L2Capability()
        ioctl(fd, VIDIOC_QUERYCAP, cap, f"{path} QUERYCAP")
        return {
            "driver": decode_c_string(cap.driver),
            "card": decode_c_string(cap.card),
            "bus_info": decode_c_string(cap.bus_info),
            "version": cap.version,
            "capabilities": f"0x{cap.capabilities:08x}",
            "capability_names": flags_to_names(cap.capabilities, CAPABILITY_NAMES),
            "device_caps": f"0x{cap.device_caps:08x}",
            "device_capability_names": flags_to_names(cap.device_caps, CAPABILITY_NAMES),
        }
    finally:
        os.close(fd)


def video_formats(path, mbus_code=0):
    formats = []
    fd = os.open(path, os.O_RDWR)
    try:
        index = 0
        while True:
            desc = V4L2FmtDesc()
            desc.index = index
            desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
            desc.mbus_code = mbus_code
            try:
                ioctl(fd, VIDIOC_ENUM_FMT, desc, f"{path} ENUM_FMT {index}")
            except OSError as exc:
                if exc.errno == 22:
                    break
                raise
            formats.append({
                "index": index,
                "description": decode_c_string(desc.description),
                "pixelformat": fourcc_name(desc.pixelformat),
                "flags": f"0x{desc.flags:x}",
                "flag_names": flags_to_names(desc.flags, FMT_FLAG_NAMES),
                "mbus_code": f"0x{desc.mbus_code:x}",
            })
            index += 1
    finally:
        os.close(fd)
    return formats


def frame_size_to_dict(size):
    out = {
        "index": size.index,
        "pixel_format": fourcc_name(size.pixel_format),
        "type": size.type,
        "type_name": FRAME_SIZE_TYPE_NAMES.get(size.type, f"unknown_{size.type}"),
    }
    if size.type == V4L2_FRMSIZE_TYPE_DISCRETE:
        out["width"] = size.u.discrete.width
        out["height"] = size.u.discrete.height
    else:
        out["min_width"] = size.u.stepwise.min_width
        out["max_width"] = size.u.stepwise.max_width
        out["step_width"] = size.u.stepwise.step_width
        out["min_height"] = size.u.stepwise.min_height
        out["max_height"] = size.u.stepwise.max_height
        out["step_height"] = size.u.stepwise.step_height
    return out


def frame_interval_to_dict(interval):
    out = {
        "index": interval.index,
        "pixel_format": fourcc_name(interval.pixel_format),
        "width": interval.width,
        "height": interval.height,
        "type": interval.type,
        "type_name": FRAME_INTERVAL_TYPE_NAMES.get(interval.type, f"unknown_{interval.type}"),
    }
    if interval.type == V4L2_FRMIVAL_TYPE_DISCRETE:
        out["interval"] = fract_to_dict(interval.u.discrete)
    else:
        out["min"] = fract_to_dict(interval.u.stepwise.min)
        out["max"] = fract_to_dict(interval.u.stepwise.max)
        out["step"] = fract_to_dict(interval.u.stepwise.step)
    return out


def video_frame_sizes(path, pixelformat):
    out = {"pixelformat": pixelformat, "sizes": []}
    fd = os.open(path, os.O_RDWR)
    try:
        index = 0
        while True:
            size = V4L2FrmSizeEnum()
            size.index = index
            size.pixel_format = fourcc(pixelformat)
            try:
                ioctl(fd, VIDIOC_ENUM_FRAMESIZES, size, f"{path} ENUM_FRAMESIZES {index}")
            except OSError as exc:
                if exc.errno in (22, 25):
                    if index == 0:
                        out["error"] = str(exc)
                    break
                raise
            out["sizes"].append(frame_size_to_dict(size))
            index += 1
    finally:
        os.close(fd)
    return out


def video_frame_intervals(path, pixelformat, width, height):
    out = {"pixelformat": pixelformat, "width": width, "height": height, "intervals": []}
    fd = os.open(path, os.O_RDWR)
    try:
        index = 0
        while True:
            interval = V4L2FrmIvalEnum()
            interval.index = index
            interval.pixel_format = fourcc(pixelformat)
            interval.width = width
            interval.height = height
            try:
                ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, interval, f"{path} ENUM_FRAMEINTERVALS {index}")
            except OSError as exc:
                if exc.errno in (22, 25):
                    if index == 0:
                        out["error"] = str(exc)
                    break
                raise
            out["intervals"].append(frame_interval_to_dict(interval))
            index += 1
    finally:
        os.close(fd)
    return out


def pix_format_to_dict(pix):
    planes = []
    for index in range(pix.num_planes):
        plane = pix.plane_fmt[index]
        planes.append({
            "index": index,
            "bytesperline": plane.bytesperline,
            "sizeimage": plane.sizeimage,
        })
    return {
        "width": pix.width,
        "height": pix.height,
        "pixelformat": fourcc_name(pix.pixelformat),
        "field": pix.field,
        "num_planes": pix.num_planes,
        "planes": planes,
    }


def video_set_format(path, width, height, pixelformat):
    fd = os.open(path, os.O_RDWR)
    try:
        fmt = V4L2Format()
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
        fmt.fmt.pix_mp.width = width
        fmt.fmt.pix_mp.height = height
        fmt.fmt.pix_mp.pixelformat = fourcc(pixelformat)
        fmt.fmt.pix_mp.field = V4L2_FIELD_NONE
        fmt.fmt.pix_mp.num_planes = 1
        ioctl(fd, VIDIOC_S_FMT, fmt, f"{path} S_FMT")
        return pix_format_to_dict(fmt.fmt.pix_mp)
    finally:
        os.close(fd)


def video_try_format(path, width, height, pixelformat):
    fd = os.open(path, os.O_RDWR)
    try:
        fmt = V4L2Format()
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
        fmt.fmt.pix_mp.width = width
        fmt.fmt.pix_mp.height = height
        fmt.fmt.pix_mp.pixelformat = fourcc(pixelformat)
        fmt.fmt.pix_mp.field = V4L2_FIELD_NONE
        fmt.fmt.pix_mp.num_planes = 1
        ioctl(fd, VIDIOC_TRY_FMT, fmt, f"{path} TRY_FMT {pixelformat}")
        return pix_format_to_dict(fmt.fmt.pix_mp)
    finally:
        os.close(fd)


def video_try_formats(path, width, height, formats):
    probes = []
    seen = set()
    for item in formats:
        pixelformat = item["pixelformat"]
        if pixelformat in seen:
            continue
        seen.add(pixelformat)
        probe = {"requested": pixelformat}
        try:
            probe["result"] = video_try_format(path, width, height, pixelformat)
        except OSError as exc:
            probe["error"] = str(exc)
        probes.append(probe)
    return probes


def video_current_format(path):
    fd = os.open(path, os.O_RDWR)
    try:
        fmt = V4L2Format()
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
        ioctl(fd, VIDIOC_G_FMT, fmt, f"{path} G_FMT")
        return pix_format_to_dict(fmt.fmt.pix_mp)
    finally:
        os.close(fd)


def fract_to_dict(fract):
    fps = None
    if fract.numerator and fract.denominator:
        fps = fract.denominator / fract.numerator
    return {
        "numerator": fract.numerator,
        "denominator": fract.denominator,
        "fps": round(fps, 3) if fps else None,
    }


def rect_to_dict(rect):
    return {
        "left": rect.left,
        "top": rect.top,
        "width": rect.width,
        "height": rect.height,
    }


def resolve_sensor_subdev(args):
    if args.sensor_subdev:
        return args.sensor_subdev
    if args.route == "lmi-ov13b10":
        topology = MediaTopology(args.media)
        sensor = topology.find_entity(args.sensor)
        args.sensor_subdev = find_devnode_by_name("v4l-subdev", sensor["name"])
    else:
        args.sensor_subdev = find_devnode_by_name("v4l-subdev", args.sensor)
    return args.sensor_subdev


def collect_sensor_modes(args):
    sensor_subdev = resolve_sensor_subdev(args)
    out = {
        "sensor_subdev": sensor_subdev,
        "mbus_code": f"0x{args.mbus_code:x}",
        "modes": [],
    }
    fd = os.open(sensor_subdev, os.O_RDWR)
    try:
        index = 0
        while True:
            fse = SubdevFrameSizeEnum()
            fse.index = index
            fse.pad = 0
            fse.code = args.mbus_code
            fse.which = V4L2_SUBDEV_FORMAT_ACTIVE
            try:
                ioctl(fd, VIDIOC_SUBDEV_ENUM_FRAME_SIZE, fse, f"{sensor_subdev} ENUM_FRAME_SIZE {index}")
            except OSError as exc:
                if exc.errno == 22:
                    break
                raise

            mode = {
                "index": index,
                "min_width": fse.min_width,
                "max_width": fse.max_width,
                "min_height": fse.min_height,
                "max_height": fse.max_height,
            }
            if fse.min_width == fse.max_width and fse.min_height == fse.max_height:
                fie = SubdevFrameIntervalEnum()
                fie.index = 0
                fie.pad = 0
                fie.code = args.mbus_code
                fie.width = fse.min_width
                fie.height = fse.min_height
                fie.which = V4L2_SUBDEV_FORMAT_ACTIVE
                try:
                    ioctl(fd, VIDIOC_SUBDEV_ENUM_FRAME_INTERVAL, fie, f"{sensor_subdev} ENUM_FRAME_INTERVAL {index}")
                    mode["frame_interval"] = fract_to_dict(fie.interval)
                except OSError as exc:
                    mode["frame_interval_error"] = str(exc)
            out["modes"].append(mode)
            index += 1
    finally:
        os.close(fd)
    return out


def select_sensor_mode(args):
    if args.mode_index is None:
        args.selected_mode = None
        return None
    modes = collect_sensor_modes(args)["modes"]
    if args.mode_index < 0 or args.mode_index >= len(modes):
        raise SystemExit(f"--mode-index {args.mode_index} is out of range; available modes: 0..{len(modes) - 1}")
    mode = modes[args.mode_index]
    if mode["min_width"] != mode["max_width"] or mode["min_height"] != mode["max_height"]:
        raise SystemExit(f"--mode-index {args.mode_index} is not a discrete frame size")
    args.width = mode["min_width"]
    args.height = mode["min_height"]
    args.selected_mode = mode
    return mode


def list_sensor_modes(args):
    print(json.dumps(collect_sensor_modes(args), ensure_ascii=False, indent=2, sort_keys=True))


def collect_sensor_metadata(args, configure_route=True):
    if configure_route and args.route == "lmi-ov13b10":
        configure_lmi_ov13b10_route(args)
    elif not args.sensor_subdev:
        args.sensor_subdev = find_devnode_by_name("v4l-subdev", args.sensor)

    out = {
        "sensor_subdev": args.sensor_subdev,
        "width": args.width,
        "height": args.height,
        "mbus_code": f"0x{args.mbus_code:x}",
    }
    fd = os.open(args.sensor_subdev, os.O_RDWR)
    try:
        fie = SubdevFrameIntervalEnum()
        fie.index = 0
        fie.pad = 0
        fie.code = args.mbus_code
        fie.width = args.width
        fie.height = args.height
        fie.which = V4L2_SUBDEV_FORMAT_ACTIVE
        ioctl(fd, VIDIOC_SUBDEV_ENUM_FRAME_INTERVAL, fie, f"{args.sensor_subdev} ENUM_FRAME_INTERVAL")
        out["frame_interval_enum"] = fract_to_dict(fie.interval)

        fi = SubdevFrameInterval()
        fi.pad = 0
        fi.which = V4L2_SUBDEV_FORMAT_ACTIVE
        ioctl(fd, VIDIOC_SUBDEV_G_FRAME_INTERVAL, fi, f"{args.sensor_subdev} G_FRAME_INTERVAL")
        out["frame_interval"] = fract_to_dict(fi.interval)

        if args.metadata_set_interval:
            set_fi = SubdevFrameInterval()
            set_fi.pad = 0
            set_fi.which = V4L2_SUBDEV_FORMAT_ACTIVE
            set_fi.interval.numerator = fi.interval.numerator
            set_fi.interval.denominator = fi.interval.denominator
            ioctl(fd, VIDIOC_SUBDEV_S_FRAME_INTERVAL, set_fi, f"{args.sensor_subdev} S_FRAME_INTERVAL")
            out["set_frame_interval"] = fract_to_dict(set_fi.interval)

        selections = {}
        for target, name in SELECTION_TARGET_NAMES.items():
            sel = SubdevSelection()
            sel.which = V4L2_SUBDEV_FORMAT_ACTIVE
            sel.pad = 0
            sel.target = target
            ioctl(fd, VIDIOC_SUBDEV_G_SELECTION, sel, f"{args.sensor_subdev} G_SELECTION {name}")
            selections[name] = rect_to_dict(sel.r)
        out["selection"] = selections
    finally:
        os.close(fd)

    try:
        controls = SensorControls(args.sensor_subdev)
        try:
            out["controls"] = controls.status()
        finally:
            controls.close()
    except OSError as exc:
        out["controls_error"] = str(exc)
    return out


def query_sensor_metadata(args):
    print(json.dumps(collect_sensor_metadata(args), ensure_ascii=False, indent=2, sort_keys=True))


def collect_sensor_controls(args, configure_route=True):
    if configure_route and args.route == "lmi-ov13b10":
        configure_lmi_ov13b10_route(args)
    sensor_subdev = resolve_sensor_subdev(args)
    controls = SensorControls(sensor_subdev)
    try:
        return {
            "sensor_subdev": sensor_subdev,
            "controls": controls.enumerate(),
        }
    finally:
        controls.close()


def query_sensor_controls(args):
    print(json.dumps(collect_sensor_controls(args), ensure_ascii=False, indent=2, sort_keys=True))


def sampled_mean(data, max_samples=4096):
    if not data:
        return 0.0
    step = max(1, len(data) // max_samples)
    total = 0
    count = 0
    for index in range(0, len(data), step):
        total += data[index]
        count += 1
    return total / max(1, count)


def gray_luma_stats(data, max_samples=8192):
    if not data:
        return luma_summary([])
    step = max(1, len(data) // max_samples)
    return luma_summary([data[index] for index in range(0, len(data), step)])


def metered_luma(stats, metric):
    return stats.get(metric, stats.get("mean", 0.0))


def choose_raw_format(raw_format, raw_len, width, height, stride):
    if raw_format != "auto":
        return raw_format
    pixels = width * height
    packed10_stride = stride or ((width + 3) // 4) * 5
    if raw_len >= packed10_stride * height and raw_len < pixels * 2:
        return "bayer10p"
    if raw_len >= pixels * 2:
        return "bayer10le"
    return "gray8"


def raw_to_gray8(data, width, height, raw_format, stride, shift):
    raw_format = choose_raw_format(raw_format, len(data), width, height, stride)
    out = bytearray(width * height)
    if raw_format in ("gray8", "bayer8"):
        stride = stride or width
        required = stride * height
        if len(data) < required:
            raise RuntimeError(f"frame too small for {raw_format}: got {len(data)}, need {required}")
        for y in range(height):
            src = y * stride
            dst = y * width
            out[dst:dst + width] = data[src:src + width]
        return out, raw_format
    if raw_format == "bayer10le":
        stride = stride or width * 2
        required = stride * height
        if len(data) < required:
            raise RuntimeError(f"frame too small for bayer10le: got {len(data)}, need {required}")
        for y in range(height):
            src = y * stride
            dst = y * width
            for x in range(width):
                pos = src + x * 2
                value = data[pos] | (data[pos + 1] << 8)
                out[dst + x] = max(0, min(255, value >> shift))
        return out, raw_format
    if raw_format == "bayer10p":
        stride = stride or ((width + 3) // 4) * 5
        required = stride * height
        if len(data) < required:
            raise RuntimeError(f"frame too small for bayer10p: got {len(data)}, need {required}")
        for y in range(height):
            src = y * stride
            dst = y * width
            x = 0
            pos = src
            while x + 3 < width:
                out[dst + x] = data[pos]
                out[dst + x + 1] = data[pos + 1]
                out[dst + x + 2] = data[pos + 2]
                out[dst + x + 3] = data[pos + 3]
                x += 4
                pos += 5
            while x < width and pos < src + stride:
                out[dst + x] = data[pos]
                x += 1
                pos += 1
        return out, raw_format
    raise RuntimeError(f"unsupported raw format: {raw_format}")


def gray_to_bmp(gray, width, height, preview_width):
    step = max(1, math.ceil(width / preview_width)) if preview_width else 1
    out_width = (width + step - 1) // step
    out_height = (height + step - 1) // step
    row_size = (out_width * 3 + 3) & ~3
    pixels = bytearray(row_size * out_height)
    for out_y in range(out_height):
        src_y = min(height - 1, (out_height - 1 - out_y) * step)
        row = out_y * row_size
        src_row = src_y * width
        for out_x in range(out_width):
            src_x = min(width - 1, out_x * step)
            value = gray[src_row + src_x]
            dst = row + out_x * 3
            pixels[dst] = value
            pixels[dst + 1] = value
            pixels[dst + 2] = value
    header_size = 14 + 40
    file_size = header_size + len(pixels)
    file_header = struct.pack("<2sIHHI", b"BM", file_size, 0, 0, header_size)
    info_header = struct.pack(
        "<IiiHHIIiiII",
        40,
        out_width,
        out_height,
        1,
        24,
        0,
        len(pixels),
        2835,
        2835,
        0,
        0,
    )
    return file_header + info_header + pixels, out_width, out_height


def png_chunk(name, data):
    name = name.encode("ascii")
    return struct.pack(">I", len(data)) + name + data + struct.pack(">I", zlib.crc32(name + data) & 0xffffffff)


def rgb_to_png(rgb, width, height, level):
    rows = bytearray()
    stride = width * 3
    for y in range(height):
        rows.append(0)
        start = y * stride
        rows.extend(rgb[start:start + stride])
    return b"\x89PNG\r\n\x1a\n" + png_chunk(
        "IHDR",
        struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0),
    ) + png_chunk("IDAT", zlib.compress(bytes(rows), max(0, min(9, level)))) + png_chunk("IEND", b"")


def rgb_to_nv12(rgb, width, height):
    out_width = width & ~1
    out_height = height & ~1
    if out_width < 2 or out_height < 2:
        raise RuntimeError(f"preview too small for NV12: {width}x{height}")

    y_plane = bytearray(out_width * out_height)
    uv_plane = bytearray(out_width * (out_height // 2))
    for y in range(out_height):
        src_row = y * width * 3
        dst_row = y * out_width
        for x in range(out_width):
            src = src_row + x * 3
            r = rgb[src]
            g = rgb[src + 1]
            b = rgb[src + 2]
            y_plane[dst_row + x] = clamp((77 * r + 150 * g + 29 * b) >> 8, 0, 255)

    for y in range(0, out_height, 2):
        uv_row = (y // 2) * out_width
        for x in range(0, out_width, 2):
            r_sum = g_sum = b_sum = 0
            for dy in (0, 1):
                src_row = (y + dy) * width * 3
                for dx in (0, 1):
                    src = src_row + (x + dx) * 3
                    r_sum += rgb[src]
                    g_sum += rgb[src + 1]
                    b_sum += rgb[src + 2]
            r = r_sum >> 2
            g = g_sum >> 2
            b = b_sum >> 2
            uv = uv_row + x
            uv_plane[uv] = clamp(((-43 * r - 85 * g + 128 * b) >> 8) + 128, 0, 255)
            uv_plane[uv + 1] = clamp(((128 * r - 107 * g - 21 * b) >> 8) + 128, 0, 255)

    return y_plane + uv_plane, out_width, out_height


def rgb_to_yuyv(rgb, src_width, src_height, out_width, out_height, stride=0):
    """Pack source RGB (src_width x src_height) into packed YUYV (YUY2, 4:2:2)
    at out_width x out_height, nearest-neighbour resampled, honouring the V4L2
    node's negotiated row stride (bytesperline). Macropixel = Y0 U Y1 V over a
    horizontal pixel pair, BT.601 limited-range to match rgb_to_nv12()."""
    out_width &= ~1
    if out_width < 2 or out_height < 1 or src_width < 1 or src_height < 1:
        raise RuntimeError(f"invalid YUYV geometry {src_width}x{src_height}->{out_width}x{out_height}")
    row_bytes = out_width * 2
    stride = stride or row_bytes
    out = bytearray(stride * out_height)
    for ty in range(out_height):
        sy = (ty * src_height) // out_height
        src_row = sy * src_width * 3
        dst = ty * stride
        x = 0
        while x < out_width:
            sx0 = (x * src_width) // out_width
            sx1 = ((x + 1) * src_width) // out_width
            s0 = src_row + sx0 * 3
            s1 = src_row + sx1 * 3
            r0 = rgb[s0]; g0 = rgb[s0 + 1]; b0 = rgb[s0 + 2]
            r1 = rgb[s1]; g1 = rgb[s1 + 1]; b1 = rgb[s1 + 2]
            y0 = (77 * r0 + 150 * g0 + 29 * b0) >> 8
            y1 = (77 * r1 + 150 * g1 + 29 * b1) >> 8
            ar = (r0 + r1) >> 1
            ag = (g0 + g1) >> 1
            ab = (b0 + b1) >> 1
            u = (((-43 * ar - 85 * ag + 128 * ab) >> 8) + 128)
            v = (((128 * ar - 107 * ag - 21 * ab) >> 8) + 128)
            out[dst] = y0 if y0 < 256 else 255
            out[dst + 1] = 0 if u < 0 else (255 if u > 255 else u)
            out[dst + 2] = y1 if y1 < 256 else 255
            out[dst + 3] = 0 if v < 0 else (255 if v > 255 else v)
            dst += 4
            x += 2
    return bytes(out)


def rgb_to_nv12_scaled(rgb, src_width, src_height, out_width, out_height, stride=0):
    """Pack source RGB into NV12 (4:2:0) at out_width x out_height, nearest
    resampled, honouring Y/UV row stride. Mirrors rgb_to_nv12() colour math."""
    out_width &= ~1
    out_height &= ~1
    if out_width < 2 or out_height < 2 or src_width < 1 or src_height < 1:
        raise RuntimeError(f"invalid NV12 geometry {src_width}x{src_height}->{out_width}x{out_height}")
    stride = stride or out_width
    y_plane = bytearray(stride * out_height)
    uv_plane = bytearray(stride * (out_height // 2))
    sx_for = [((x * src_width) // out_width) * 3 for x in range(out_width)]
    for ty in range(out_height):
        sy = (ty * src_height) // out_height
        src_row = sy * src_width * 3
        dst_row = ty * stride
        for x in range(out_width):
            s = src_row + sx_for[x]
            r = rgb[s]; g = rgb[s + 1]; b = rgb[s + 2]
            yv = (77 * r + 150 * g + 29 * b) >> 8
            y_plane[dst_row + x] = yv if yv < 256 else 255
    for ty in range(0, out_height, 2):
        uv_row = (ty // 2) * stride
        sy = (ty * src_height) // out_height
        src_row = sy * src_width * 3
        for x in range(0, out_width, 2):
            s0 = src_row + sx_for[x]
            s1 = src_row + sx_for[x + 1]
            r = (rgb[s0] + rgb[s1]) >> 1
            g = (rgb[s0 + 1] + rgb[s1 + 1]) >> 1
            b = (rgb[s0 + 2] + rgb[s1 + 2]) >> 1
            u = (((-43 * r - 85 * g + 128 * b) >> 8) + 128)
            v = (((128 * r - 107 * g - 21 * b) >> 8) + 128)
            uv = uv_row + x
            uv_plane[uv] = 0 if u < 0 else (255 if u > 255 else u)
            uv_plane[uv + 1] = 0 if v < 0 else (255 if v > 255 else v)
    return bytes(y_plane + uv_plane)


def gray_to_rgb_preview(gray, width, height, preview_width):
    source_width = width // 2
    source_height = height // 2
    step = max(1, math.ceil(source_width / preview_width)) if preview_width else 1
    out_width = max(1, (source_width + step - 1) // step)
    out_height = max(1, (source_height + step - 1) // step)
    rgb = bytearray(out_width * out_height * 3)
    total = 0
    count = 0
    for out_y in range(out_height):
        y = min(height - 2, out_y * 2 * step)
        row0 = y * width
        row1 = (y + 1) * width
        for out_x in range(out_width):
            x = min(width - 2, out_x * 2 * step)
            g1 = gray[row0 + x]
            r = gray[row0 + x + 1]
            b = gray[row1 + x]
            g = (g1 + gray[row1 + x + 1]) >> 1
            dst = (out_y * out_width + out_x) * 3
            rgb[dst] = r
            rgb[dst + 1] = g
            rgb[dst + 2] = b
            total += (77 * r + 150 * g + 29 * b) >> 8
            count += 1
    return rgb, out_width, out_height, total / max(1, count)


def raw10p_grbg_to_rgb_preview(data, width, height, stride, preview_width):
    stride = stride or ((width + 3) // 4) * 5
    source_width = width // 2
    source_height = height // 2
    step = max(1, math.ceil(source_width / preview_width)) if preview_width else 1
    out_width = max(1, (source_width + step - 1) // step)
    out_height = max(1, (source_height + step - 1) // step)
    rgb = bytearray(out_width * out_height * 3)
    total = 0
    count = 0
    for out_y in range(out_height):
        y = min(height - 2, out_y * 2 * step)
        row0 = y * stride
        row1 = (y + 1) * stride
        for out_x in range(out_width):
            x = min(width - 2, out_x * 2 * step)
            pos0 = row0 + (x // 4) * 5 + (x & 3)
            pos1 = row1 + (x // 4) * 5 + (x & 3)
            g1 = data[pos0]
            r = data[pos0 + 1]
            b = data[pos1]
            g = (g1 + data[pos1 + 1]) >> 1
            dst = (out_y * out_width + out_x) * 3
            rgb[dst] = r
            rgb[dst + 1] = g
            rgb[dst + 2] = b
            total += (77 * r + 150 * g + 29 * b) >> 8
            count += 1
    return rgb, out_width, out_height, total / max(1, count)


def raw_to_rgb_preview(data, width, height, raw_format, stride, shift, preview_width):
    raw_format = choose_raw_format(raw_format, len(data), width, height, stride)
    if raw_format == "bayer10p":
        rgb, out_width, out_height, luma = raw10p_grbg_to_rgb_preview(data, width, height, stride, preview_width)
        return rgb, out_width, out_height, raw_format, luma
    gray, raw_format = raw_to_gray8(data, width, height, raw_format, stride, shift)
    rgb, out_width, out_height, luma = gray_to_rgb_preview(gray, width, height, preview_width)
    return rgb, out_width, out_height, raw_format, luma


def rgb_stats(rgb, min_luma=0, max_luma=255, clip_margin=0):
    red = green = blue = 0
    selected_red = selected_green = selected_blue = 0
    all_luma = []
    selected_luma = []
    selected = 0
    pixels = len(rgb) // 3
    for index in range(0, len(rgb), 3):
        r = rgb[index]
        g = rgb[index + 1]
        b = rgb[index + 2]
        y = (77 * r + 150 * g + 29 * b) >> 8
        red += r
        green += g
        blue += b
        all_luma.append(y)
        clipped = clip_margin and (
            r <= clip_margin or g <= clip_margin or b <= clip_margin or
            r >= 255 - clip_margin or g >= 255 - clip_margin or b >= 255 - clip_margin
        )
        if min_luma <= y <= max_luma and not clipped:
            selected_red += r
            selected_green += g
            selected_blue += b
            selected_luma.append(y)
            selected += 1
    if pixels == 0:
        return {
            "red_mean": 0.0,
            "green_mean": 0.0,
            "blue_mean": 0.0,
            "luma": 0.0,
            "luma_stats": luma_summary([]),
            "selected_samples": 0,
            "selected_luma_stats": luma_summary([]),
        }
    if selected == 0:
        selected = pixels
        selected_red = red
        selected_green = green
        selected_blue = blue
        selected_luma = all_luma
    luma_stats = luma_summary(all_luma)
    selected_luma_stats = luma_summary(selected_luma)
    return {
        "red_mean": selected_red / selected,
        "green_mean": selected_green / selected,
        "blue_mean": selected_blue / selected,
        "luma": luma_stats["mean"],
        "luma_stats": luma_stats,
        "selected_samples": selected,
        "selected_luma_stats": selected_luma_stats,
    }


def apply_rgb_gains(rgb, red_gain, green_gain, blue_gain):
    for index in range(0, len(rgb), 3):
        rgb[index] = clamp(rgb[index] * red_gain, 0, 255)
        rgb[index + 1] = clamp(rgb[index + 1] * green_gain, 0, 255)
        rgb[index + 2] = clamp(rgb[index + 2] * blue_gain, 0, 255)


def apply_color_transform(rgb, ccm, gamma):
    identity_ccm = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]
    use_ccm = any(abs(a - b) > 0.0001 for a, b in zip(ccm, identity_ccm))
    use_gamma = abs(gamma - 1.0) > 0.0001
    if not use_ccm and not use_gamma:
        return False
    gamma_power = 1.0 / max(0.05, gamma)
    for index in range(0, len(rgb), 3):
        r = rgb[index]
        g = rgb[index + 1]
        b = rgb[index + 2]
        if use_ccm:
            nr = ccm[0] * r + ccm[1] * g + ccm[2] * b
            ng = ccm[3] * r + ccm[4] * g + ccm[5] * b
            nb = ccm[6] * r + ccm[7] * g + ccm[8] * b
        else:
            nr, ng, nb = r, g, b
        if use_gamma:
            nr = 255.0 * ((float_clamp(nr, 0, 255) / 255.0) ** gamma_power)
            ng = 255.0 * ((float_clamp(ng, 0, 255) / 255.0) ** gamma_power)
            nb = 255.0 * ((float_clamp(nb, 0, 255) / 255.0) ** gamma_power)
        rgb[index] = clamp(nr, 0, 255)
        rgb[index + 1] = clamp(ng, 0, 255)
        rgb[index + 2] = clamp(nb, 0, 255)
    return True


def balance_rgb_preview(rgb, args):
    awb_stats = rgb_stats(rgb, args.awb_min_luma, args.awb_max_luma, args.awb_clip_margin)
    red_gain = args.red_gain
    green_gain = args.green_gain
    blue_gain = args.blue_gain
    if args.auto_white_balance:
        green = max(awb_stats["green_mean"], 1.0)
        red_gain *= float_clamp(green / max(awb_stats["red_mean"], 1.0), args.awb_min_gain, args.awb_max_gain)
        blue_gain *= float_clamp(green / max(awb_stats["blue_mean"], 1.0), args.awb_min_gain, args.awb_max_gain)
    if any(abs(gain - 1.0) > 0.001 for gain in (red_gain, green_gain, blue_gain)):
        apply_rgb_gains(rgb, red_gain, green_gain, blue_gain)
    color_transform_applied = apply_color_transform(rgb, args.ccm, args.gamma)
    stats = rgb_stats(rgb)
    luma_stats = stats["luma_stats"]
    return rgb, metered_luma(luma_stats, args.ae_metering), {
        "red": round(red_gain, 3),
        "green": round(green_gain, 3),
        "blue": round(blue_gain, 3),
        "auto_white_balance": args.auto_white_balance,
        "awb_selected_samples": awb_stats["selected_samples"],
        "awb_luma_stats": {key: round(value, 2) if isinstance(value, float) else value for key, value in awb_stats["selected_luma_stats"].items()},
        "ae_metering": args.ae_metering,
        "ae_luma": round(metered_luma(luma_stats, args.ae_metering), 2),
        "luma_stats": {key: round(value, 2) if isinstance(value, float) else value for key, value in luma_stats.items()},
        "ccm": [round(value, 4) for value in args.ccm],
        "gamma": args.gamma,
        "color_transform_applied": color_transform_applied,
    }


def configure(args):
    if args.route == "lmi-ov13b10":
        configure_lmi_ov13b10_route(args)
    elif args.route != "none":
        raise RuntimeError(f"unsupported route: {args.route}")
    if not args.video:
        raise RuntimeError("--video is required when --route=none")


def discover_camera(args):
    configure(args)
    topology = MediaTopology(args.media)
    video_formats_all = video_formats(args.video)
    video_formats_for_mbus_code = video_formats(args.video, args.mbus_code)
    out = {
        "schema": "lmi.raw-camera.discovery.v1",
        "route": args.route,
        "media": args.media,
        "nodes": collect_nodes(),
        "enabled_links": collect_enabled_links(topology),
        "sensor": {
            "name": args.sensor,
            "subdev": args.sensor_subdev,
            "selected_mode": args.selected_mode,
            "modes": collect_sensor_modes(args),
            "metadata": collect_sensor_metadata(args, configure_route=False),
            "controls": collect_sensor_controls(args, configure_route=False),
        },
        "video": {
            "entity": args.video_entity,
            "node": args.video,
            "configured_format": args.video_configured_format,
            "querycap": video_querycap(args.video),
            "formats_all": video_formats_all,
            "formats_for_mbus_code": video_formats_for_mbus_code,
            "try_formats_from_all": video_try_formats(args.video, args.width, args.height, video_formats_all),
            "frame_sizes": video_frame_sizes(args.video, args.pixelformat),
            "frame_intervals": video_frame_intervals(args.video, args.pixelformat, args.width, args.height),
            "current_format": video_current_format(args.video),
        },
        "software_outputs": {
            "preview_image_formats": ["png-color", "bmp-gray"],
            "raw_dump": True,
            "metadata_dump": True,
            "nv12_dump": "software preview-derived NV12 for codec tests only",
            "http_stream": "multipart/x-mixed-replace using the selected preview image content type",
            "http_capture_endpoints": ["/snapshot", "/raw", "/metadata"],
            "http_capabilities_endpoint": "/capabilities",
            "software_3a": ["AE", "AWB", "preview CCM/gamma"],
        },
        "standard_stack_boundary": {
            "libcamera_pipeline_in_repo": False,
            "browser_get_user_media_ready": False,
            "expected_browser_path": "libcamera + PipeWire/portal, or a real processed V4L2/loopback node supplied by a separate userspace stack",
            "rootfs_changes_required_by_this_helper": False,
            "do_not_assume_video_node_number": True,
        },
        "kernel_output_boundary": {
            "capture_api": "V4L2 mplane + media-controller",
            "validated_output": "raw Bayer RDI",
            "validated_fourcc": args.pixelformat,
            "browser_ready_yuv_or_rgb": False,
            "kernel_isp_yuv_or_rgb": False,
            "format_list_note": "The active OV13B10 route is RAW10; any processed browser path must be produced by a real ISP/userspace stack, not by pretending this node is YUV/RGB.",
            "reason": "The validated SM8250 CAMSS path is OV13B10 -> CSIPHY1 -> CSID1 -> VFE1 RDI0 -> video node, and this mainline driver path does not expose an ISP/YUV/RGB output node.",
        },
    }
    print(json.dumps(out, ensure_ascii=False, indent=2, sort_keys=True))


def http_capabilities(args, status):
    return {
        "schema": "lmi.raw-camera.http.v1",
        "route": args.route,
        "video": status.get("video") or args.video,
        "sensor": args.sensor,
        "selected_mode": args.selected_mode,
        "preview": {
            "image_format": args.image_format,
            "content_type": status.get("content_type"),
            "width": status.get("preview_width"),
            "height": status.get("preview_height"),
        },
        "raw": {
            "available": bool(status.get("raw_available")),
            "fourcc": args.pixelformat,
            "mbus_code": f"0x{args.mbus_code:x}",
            "raw_format": status.get("raw_format"),
            "size": status.get("raw_size"),
        },
        "software_processing": {
            "auto_exposure": args.auto_exposure,
            "ae_mode": args.ae_mode,
            "ae_metering": args.ae_metering,
            "auto_white_balance": args.auto_white_balance,
            "ccm": [round(value, 4) for value in args.ccm],
            "gamma": args.gamma,
            "kernel_isp": False,
        },
        "endpoints": {
            "page": "/",
            "stream": "/stream",
            "snapshot": "/snapshot",
            "raw": "/raw",
            "metadata": "/metadata",
            "status": "/status",
            "capabilities": "/capabilities",
        },
        "standard_stack_boundary": {
            "browser_get_user_media_ready": False,
            "libcamera_pipeline_in_repo": False,
            "kernel_isp_yuv_or_rgb": False,
            "expected_browser_path": "libcamera + PipeWire/portal, or a real processed V4L2/loopback node supplied by a separate userspace stack",
        },
    }


def capture_image(camera, args):
    raw, meta = camera.read_frame()
    color_balance = None
    luma_stats = None
    if args.image_format == "png-color":
        rgb, preview_width, preview_height, raw_format, _ = raw_to_rgb_preview(
            raw,
            meta["width"],
            meta["height"],
            args.raw_format,
            meta["stride"],
            args.shift,
            args.preview_width,
        )
        rgb, luma, color_balance = balance_rgb_preview(rgb, args)
        luma_stats = color_balance["luma_stats"]
        image = rgb_to_png(rgb, preview_width, preview_height, args.png_level)
        content_type = "image/png"
    else:
        gray, raw_format = raw_to_gray8(
            raw,
            meta["width"],
            meta["height"],
            args.raw_format,
            meta["stride"],
            args.shift,
        )
        luma_stats = gray_luma_stats(gray)
        luma = metered_luma(luma_stats, args.ae_metering)
        image, preview_width, preview_height = gray_to_bmp(gray, meta["width"], meta["height"], args.preview_width)
        content_type = "image/bmp"
    controls = camera.update_auto_exposure(luma)
    meta.update({
        "raw_format": raw_format,
        "raw_size": len(raw),
        "preview_width": preview_width,
        "preview_height": preview_height,
        "ae_luma": round(luma, 2),
        "luma_stats": luma_stats,
        "controls": controls,
        "auto_exposure": args.auto_exposure,
        "ae_mode": args.ae_mode,
        "ae_metering": args.ae_metering,
        "target_luma": args.target_luma,
        "ae_max_step": args.ae_max_step,
        "image_format": args.image_format,
        "software_processing": {
            "demosaic": "2x2 GRBG preview sampling" if args.image_format == "png-color" else "none",
            "white_balance": "preview RGB gains" if color_balance else "none",
            "color": "software CCM/gamma on preview RGB" if color_balance else "none",
            "kernel_isp": False,
        },
        "color_balance": color_balance,
    })
    return image, content_type, meta, raw


def capture_loop(args, state):
    last_frame_time = None
    try:
        configure(args)
        with IoctlCamera(args) as camera:
            while not state.stop.is_set():
                started = time.monotonic()
                try:
                    image, content_type, meta, raw = capture_image(camera, args)
                    finished = time.monotonic()
                    if last_frame_time is not None and finished > last_frame_time:
                        meta["preview_fps"] = round(1.0 / (finished - last_frame_time), 2)
                    last_frame_time = finished
                    with state.condition:
                        state.frame = image
                        state.raw = raw
                        state.content_type = content_type
                        state.status.update({
                            "state": "streaming",
                            "frames": state.status.get("frames", 0) + 1,
                            "error": None,
                            "updated_at": now(),
                            "video": args.video,
                            "content_type": content_type,
                            "raw_available": raw is not None,
                            **meta,
                        })
                        state.condition.notify_all()
                except Exception as exc:
                    with state.condition:
                        state.status.update({
                            "state": "error",
                            "error": str(exc),
                            "updated_at": now(),
                            "video": args.video,
                        })
                        state.condition.notify_all()
                elapsed = time.monotonic() - started
                state.stop.wait(max(0.01, args.interval - elapsed))
    except Exception as exc:
        with state.condition:
            state.status.update({"state": "error", "error": str(exc), "updated_at": now()})
            state.condition.notify_all()


def make_handler(state, args):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *values):
            sys.stderr.write("[%s] http: %s\n" % (time.strftime("%H:%M:%S"), fmt % values))

        def send_bytes(self, status, content_type, body, headers=None):
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Cache-Control", "no-store")
            if headers:
                for name, value in headers.items():
                    self.send_header(name, value)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def current_capture(self):
            with state.lock:
                return state.frame, state.raw, state.content_type, dict(state.status)

        def send_stream(self):
            self.send_response(200)
            self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "close")
            self.end_headers()
            last_frame = 0
            while not state.stop.is_set():
                with state.condition:
                    state.condition.wait_for(
                        lambda: state.stop.is_set()
                        or state.status.get("frames", 0) != last_frame
                        or state.status.get("state") == "error",
                        timeout=5,
                    )
                    frame = state.frame
                    content_type = state.content_type
                    status = dict(state.status)
                if state.stop.is_set():
                    return
                if status.get("state") == "error" and frame is None:
                    return
                if frame is None or status.get("frames", 0) == last_frame:
                    continue
                last_frame = status.get("frames", 0)
                try:
                    self.wfile.write(
                        b"--frame\r\n"
                        + f"Content-Type: {content_type}\r\n".encode("ascii")
                        + f"Content-Length: {len(frame)}\r\n\r\n".encode("ascii")
                    )
                    self.wfile.write(frame)
                    self.wfile.write(b"\r\n")
                    self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError):
                    return

        def do_GET(self):
            path = urlparse(self.path).path
            if path == "/":
                self.send_bytes(200, "text/html; charset=utf-8", page(args).encode("utf-8"))
                return
            if path == "/stream":
                self.send_stream()
                return
            if path in ("/frame.bmp", "/frame", "/snapshot", "/snapshot.png"):
                frame, _, content_type, status = self.current_capture()
                if frame is None:
                    body = (status.get("error") or "no frame captured yet").encode("utf-8")
                    self.send_bytes(503, "text/plain; charset=utf-8", body)
                    return
                extension = "png" if content_type == "image/png" else "bmp"
                self.send_bytes(200, content_type, frame, {
                    "Content-Disposition": f"inline; filename=\"lmi-camera-frame.{extension}\"",
                })
                return
            if path in ("/raw", "/frame.raw"):
                _, raw, _, status = self.current_capture()
                if raw is None:
                    body = (status.get("error") or "no raw frame captured yet").encode("utf-8")
                    self.send_bytes(503, "text/plain; charset=utf-8", body)
                    return
                self.send_bytes(200, "application/octet-stream", raw, {
                    "Content-Disposition": "attachment; filename=\"lmi-camera-frame.raw\"",
                })
                return
            if path in ("/metadata", "/snapshot.json"):
                _, _, _, status = self.current_capture()
                body = json.dumps(status, ensure_ascii=False, indent=2, sort_keys=True).encode("utf-8")
                self.send_bytes(200, "application/json; charset=utf-8", body, {
                    "Content-Disposition": "attachment; filename=\"lmi-camera-frame.json\"",
                })
                return
            if path == "/capabilities":
                _, _, _, status = self.current_capture()
                body = json.dumps(http_capabilities(args, status), ensure_ascii=False, indent=2, sort_keys=True).encode("utf-8")
                self.send_bytes(200, "application/json; charset=utf-8", body)
                return
            if path == "/status":
                with state.lock:
                    body = json.dumps(state.status, ensure_ascii=False, indent=2).encode("utf-8")
                self.send_bytes(200, "application/json; charset=utf-8", body)
                return
            self.send_bytes(404, "text/plain; charset=utf-8", b"not found")

    return Handler


def page(args):
    title = "lmi camera preview"
    interval_ms = max(100, int(args.browser_interval * 1000))
    video = html.escape(args.video or "auto")
    return f"""<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>{title}</title>
<style>
body {{ margin: 0; background: #101214; color: #e8e8e8; font-family: sans-serif; }}
main {{ padding: 16px; }}
img {{ max-width: 100%; height: auto; image-rendering: auto; background: #000; }}
pre {{ white-space: pre-wrap; background: #181c20; padding: 12px; border-radius: 8px; }}
</style>
</head>
<body>
<main>
<h1>{title}</h1>
<p>video node: <code>{video}</code></p>
<p>
  <a href="/snapshot">preview snapshot</a> ·
  <a href="/raw">raw frame</a> ·
  <a href="/metadata">metadata JSON</a> ·
  <a href="/capabilities">capabilities JSON</a>
</p>
<img id="frame" src="/stream" alt="camera stream">
<pre id="status">starting</pre>
</main>
<script>
async function tick() {{
  try {{
    const response = await fetch('/status?t=' + Date.now(), {{cache: 'no-store'}});
    document.getElementById('status').textContent = JSON.stringify(await response.json(), null, 2);
  }} catch (error) {{
    document.getElementById('status').textContent = String(error);
  }}
}}
setInterval(tick, {interval_ms});
tick();
</script>
</body>
</html>
"""


def serve(args):
    state = PreviewState()
    worker = threading.Thread(target=capture_loop, args=(args, state), daemon=True)
    worker.start()
    server = ThreadingHTTPServer((args.host, args.port), make_handler(state, args))
    print(f"open http://{args.host}:{args.port}/ and use Ctrl-C to stop", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("stopping", flush=True)
    finally:
        state.stop.set()
        server.server_close()
        worker.join(timeout=2)


def raw_frame_to_nv12(raw, meta, args):
    rgb, width, height, raw_format, _ = raw_to_rgb_preview(
        raw,
        meta["width"],
        meta["height"],
        args.raw_format,
        meta["stride"],
        args.shift,
        args.preview_width,
    )
    rgb, _, color_balance = balance_rgb_preview(rgb, args)
    nv12, width, height = rgb_to_nv12(rgb, width, height)
    return nv12, {
        "path": "raw_bayer_rdi_to_software_preview_rgb_to_nv12",
        "width": width,
        "height": height,
        "fourcc": "NV12",
        "size": len(nv12),
        "raw_format": raw_format,
        "kernel_isp": False,
        "hardware_encoder_input_only": True,
        "color_balance": color_balance,
    }


def capture_once_to_file(args):
    if not args.output:
        raise SystemExit("--once requires --output")
    configure(args)
    with IoctlCamera(args) as camera:
        if args.auto_exposure:
            for _ in range(max(0, args.ae_warmup_frames)):
                capture_image(camera, args)
                if args.ae_warmup_interval > 0:
                    time.sleep(args.ae_warmup_interval)
        image, content_type, meta, raw = capture_image(camera, args)
    with open(args.output, "wb") as file:
        file.write(image)
    meta["content_type"] = content_type
    meta["output"] = args.output
    if args.raw_output:
        with open(args.raw_output, "wb") as file:
            file.write(raw)
        meta["raw_output"] = args.raw_output
    if args.nv12_output:
        nv12, nv12_meta = raw_frame_to_nv12(raw, meta, args)
        with open(args.nv12_output, "wb") as file:
            file.write(nv12)
        nv12_meta["output"] = args.nv12_output
        meta["nv12_output"] = nv12_meta
    if args.metadata_output:
        with open(args.metadata_output, "w", encoding="utf-8") as file:
            json.dump(meta, file, ensure_ascii=False, indent=2, sort_keys=True)
            file.write("\n")
    print("wrote %s: %s" % (args.output, json.dumps(meta, ensure_ascii=False, sort_keys=True)))


def now_status_write(path, payload):
    try:
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(payload, fh, ensure_ascii=False, indent=2, sort_keys=True)
            fh.write("\n")
    except OSError:
        pass


class DumpSink:
    """--isp-sink none: write produced frames to a file. Phase 0 validation of
    the software-ISP pipeline (demosaic + 3A + pack) with no kernel rebuild."""
    kind = "dump"

    def __init__(self, args):
        self.path = args.isp_dump
        self.max_frames = max(1, args.isp_frames)
        self.format = args.isp_format
        self.out_width = args.isp_width & ~1
        self.out_height = (args.isp_height & ~1) if args.isp_format == "nv12" else args.isp_height
        if self.format == "yuyv":
            self.stride = self.out_width * 2
            self.sizeimage = self.stride * self.out_height
        else:
            self.stride = self.out_width
            self.sizeimage = self.stride * self.out_height * 3 // 2
        self.node = self.path
        self._fh = None
        self._written = 0

    def negotiate(self):
        if self.path:
            self._fh = open(self.path, "wb")

    def write_frame(self, frame):
        if self._fh is not None and self._written < self.max_frames:
            self._fh.write(frame)
            self._written += 1

    def done(self):
        return self.path is not None and self._written >= self.max_frames

    def close(self):
        if self._fh is not None:
            self._fh.close()
            self._fh = None

    def describe(self):
        return {"sink": self.kind, "path": self.path, "frames": self.max_frames}


class LoopbackSink:
    """--isp-sink loopback: open a v4l2loopback OUTPUT node, negotiate a single
    -plane YUYV/NV12 format, then feed each frame with a plain write()."""
    kind = "loopback"

    def __init__(self, args):
        self.node = args.isp_node
        self.format = args.isp_format
        self.req_width = args.isp_width & ~1
        self.req_height = (args.isp_height & ~1) if args.isp_format == "nv12" else args.isp_height
        self.fd = None
        self.out_width = self.req_width
        self.out_height = self.req_height
        self.stride = 0
        self.sizeimage = 0

    def negotiate(self):
        self.fd = os.open(self.node, os.O_RDWR)
        fmt = V4L2Format()
        fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT
        fmt.fmt.pix.width = self.req_width
        fmt.fmt.pix.height = self.req_height
        fmt.fmt.pix.pixelformat = fourcc("YUYV" if self.format == "yuyv" else "NV12")
        fmt.fmt.pix.field = V4L2_FIELD_NONE
        if self.format == "yuyv":
            fmt.fmt.pix.bytesperline = self.req_width * 2
            fmt.fmt.pix.sizeimage = self.req_width * 2 * self.req_height
        else:
            fmt.fmt.pix.bytesperline = self.req_width
            fmt.fmt.pix.sizeimage = self.req_width * self.req_height * 3 // 2
        ioctl(self.fd, VIDIOC_S_FMT, fmt, "VIDIOC_S_FMT(output)")
        self.out_width = fmt.fmt.pix.width
        self.out_height = fmt.fmt.pix.height
        default_stride = self.out_width * 2 if self.format == "yuyv" else self.out_width
        self.stride = fmt.fmt.pix.bytesperline or default_stride
        if self.format == "yuyv":
            self.sizeimage = fmt.fmt.pix.sizeimage or (self.stride * self.out_height)
        else:
            self.sizeimage = fmt.fmt.pix.sizeimage or (self.stride * self.out_height * 3 // 2)

    def write_frame(self, frame):
        os.write(self.fd, frame)

    def done(self):
        return False

    def close(self):
        if self.fd is not None:
            try:
                os.close(self.fd)
            except OSError:
                pass
            self.fd = None

    def describe(self):
        return {"sink": self.kind, "node": self.node}


class UvcSink:
    """--isp-sink uvc: build a USB UVC gadget on the live system, run the C
    feeder (lmi-uvc-gadget) which speaks the UVC PROBE/COMMIT protocol on the
    gadget OUTPUT node, and non-blocking whole-frame-write frames into a FIFO
    the feeder drains. The phone then appears as a standard UVC webcam to a USB
    host (e.g. a Windows PC) for host UVC consumers; browser getUserMedia is a
    host-stack validation target, not something this helper itself proves."""
    kind = "uvc"

    def __init__(self, args):
        self.format = args.isp_uvc_format
        self.out_width = args.isp_width & ~1
        self.out_height = args.isp_height & ~1
        self.fps = max(1, args.isp_fps_cap or 15)
        if self.format == "yuyv":
            self.stride = self.out_width * 2
            self.sizeimage = self.stride * self.out_height
            self.format_group = "uncompressed"
            self.format_name = "yuyv"
            self.header_name = "yuyv"
        elif self.format == "mjpeg":
            self.stride = 0
            default_max = max(65536, self.out_width * self.out_height)
            self.sizeimage = args.isp_uvc_max_frame_bytes or default_max
            self.format_group = "mjpeg"
            self.format_name = "mjpg"
            self.header_name = "mjpg"
        else:
            self.stride = 0
            default_max = max(262144, self.out_width * self.out_height)
            self.sizeimage = args.isp_uvc_max_frame_bytes or default_max
            self.format_group = "framebased"
            self.format_name = "h264"
            self.header_name = "h264"
        self.gadget_name = args.isp_uvc_gadget_name
        self.feeder_bin = args.isp_uvc_feeder_bin
        self.fifo = args.isp_uvc_fifo
        self.ready_file = self.fifo + ".ready"
        self.event_fifo = args.isp_uvc_event_fifo or (self.fifo + ".events")
        self.udc = args.isp_uvc_udc
        self.keep = args.isp_uvc_keep
        self.maxpkt = args.isp_uvc_maxpkt
        self.buffers = args.isp_uvc_buffers
        self.configfs = "/sys/kernel/config/usb_gadget"
        self.gadget = "%s/%s" % (self.configfs, self.gadget_name)
        self.func = self.gadget + "/functions/uvc.0"
        self.interval = 10000000 // self.fps
        self.node = None
        self.feeder = None
        self.fifo_fd = None
        self.event_fd = None
        self.event_buf = ""
        self._built = False
        self.pipe_cap = 65536
        self.udc_name = None
        self.external_writer = False  # when True (C engine), lmi-isp writes the FIFO, not us
        self.external_proc = None  # optional external FIFO writer subprocess for orderly teardown
        self.prev_gadget = None  # gadget that held the UDC before us, to restore on close
        self.recoveries = 0
        self.recovery_retries = args.isp_uvc_recovery_retries

    def _gadget_on_udc(self, udc):
        """Return the usb_gadget configfs path currently bound to `udc`, or None."""
        try:
            names = os.listdir(self.configfs)
        except OSError:
            return None
        for nm in names:
            p = "%s/%s" % (self.configfs, nm)
            if p == self.gadget:
                continue
            try:
                with open(p + "/UDC") as fh:
                    if fh.read().strip() == udc:
                        return p
            except OSError:
                pass
        return None

    def _w(self, rel, val):
        with open(self.gadget + rel, "w") as fh:
            fh.write(str(val))

    def _write_file(self, path, val):
        with open(path, "w") as fh:
            fh.write(str(val))

    def _try_write_file(self, path, val):
        try:
            self._write_file(path, val)
        except OSError:
            pass

    def _read_gadget_udc(self, gadget):
        try:
            with open(gadget + "/UDC") as fh:
                return fh.read().strip()
        except OSError:
            return None

    def _unbind_gadget(self, gadget):
        try:
            with open(gadget + "/UDC", "w") as fh:
                fh.write("\n")
        except OSError:
            pass

    def _wait_udc_free(self, udc, timeout=3.0):
        deadline = time.monotonic() + timeout
        while True:
            busy = False
            try:
                names = os.listdir(self.configfs)
            except OSError:
                names = []
            for nm in names:
                p = "%s/%s" % (self.configfs, nm)
                try:
                    with open(p + "/UDC") as fh:
                        if fh.read().strip() == udc:
                            busy = True
                            break
                except OSError:
                    pass
            if not busy:
                return True
            if time.monotonic() >= deadline:
                return False
            time.sleep(0.05)

    def _restore_previous_gadget(self):
        if not self.prev_gadget or not self.udc_name:
            return
        try:
            with open(self.prev_gadget + "/UDC", "w") as fh:
                fh.write(self.udc_name)
            restored = ""
            try:
                with open(self.prev_gadget + "/UDC") as fh:
                    restored = fh.read().strip()
            except OSError:
                pass
            if restored != self.udc_name:
                print("[isp] uvc: warning: restore of %s on %s did not verify (UDC=%r)"
                      % (os.path.basename(self.prev_gadget), self.udc_name, restored), flush=True)
            else:
                print("[isp] uvc: restored %s on %s"
                      % (os.path.basename(self.prev_gadget), self.udc_name), flush=True)
        except OSError as exc:
            print("[isp] uvc: warning: could not restore %s on %s: %s"
                  % (self.prev_gadget, self.udc_name, exc), flush=True)

    def _frame_dir(self):
        return "%s/streaming/%s/%s/%dp" % (self.func, self.format_group, self.format_name, self.out_height)

    def _build_frame_descriptor(self):
        f = self.func
        wdir = self._frame_dir()
        os.makedirs(wdir)
        attrs = {
            "wWidth": self.out_width,
            "wHeight": self.out_height,
            "dwMaxVideoFrameBufferSize": self.sizeimage,
            "dwFrameInterval": self.interval,
            "dwDefaultFrameInterval": self.interval,
        }
        if self.format != "yuyv":
            bitrate = max(1, self.sizeimage * 8 * self.fps)
            attrs["dwMinBitRate"] = max(1, bitrate // 3)
            attrs["dwMaxBitRate"] = bitrate
        if self.format == "h264":
            attrs["dwBytesPerLine"] = 0
            attrs["bVariableSize"] = 1
        for attr, val in attrs.items():
            self._try_write_file(wdir + "/" + attr, val)

        # optional color matching (BT.601 full-ish); best-effort for YUYV/MJPEG
        if self.format in ("yuyv", "mjpeg"):
            try:
                cm = f + "/streaming/color_matching/" + self.header_name
                os.makedirs(cm)
                for attr, val in (("bColorPrimaries", 1), ("bTransferCharacteristics", 1), ("bMatrixCoefficients", 4)):
                    self._write_file(cm + "/" + attr, val)
                os.symlink(cm, "%s/streaming/%s/%s/%s" % (f, self.format_group, self.format_name, self.header_name))
            except OSError:
                pass

        os.makedirs(f + "/streaming/header/h")
        os.symlink("%s/streaming/%s/%s" % (f, self.format_group, self.format_name),
                   f + "/streaming/header/h/" + self.header_name)
        os.makedirs(f + "/streaming/class/fs", exist_ok=True)
        os.makedirs(f + "/streaming/class/hs", exist_ok=True)
        os.makedirs(f + "/streaming/class/ss", exist_ok=True)

    def _build_gadget(self):
        if os.path.isdir(self.gadget):
            self._teardown_gadget()
        g = self.gadget
        f = self.func
        os.makedirs(g)
        self._built = True
        self._w("/idVendor", "0x1d6b")
        self._w("/idProduct", "0x0102")
        self._w("/bcdUSB", "0x0200")
        self._w("/bcdDevice", "0x0010")
        self._w("/bDeviceClass", "0xEF")
        self._w("/bDeviceSubClass", "0x02")
        self._w("/bDeviceProtocol", "0x01")
        os.makedirs(g + "/strings/0x409")
        self._w("/strings/0x409/manufacturer", "lmi")
        self._w("/strings/0x409/product", "lmi OV13B10 camera")
        self._w("/strings/0x409/serialnumber", "lmi0001")
        os.makedirs(g + "/configs/c.1/strings/0x409")
        self._w("/configs/c.1/strings/0x409/configuration", "uvc")
        self._w("/configs/c.1/MaxPower", "500")
        os.makedirs(f)
        os.makedirs(f + "/control/class/fs", exist_ok=True)
        os.makedirs(f + "/control/class/ss", exist_ok=True)
        try:
            with open(f + "/streaming_maxpacket", "w") as fh:
                fh.write(str(self.maxpkt))
        except OSError:
            pass
        self._build_frame_descriptor()
        for spd in ("fs", "hs", "ss"):
            os.symlink(f + "/streaming/header/h", f + "/streaming/class/%s/h" % spd)
        os.makedirs(f + "/control/header/h")
        for spd in ("fs", "ss"):
            os.symlink(f + "/control/header/h", f + "/control/class/%s/h" % spd)
        os.symlink(f, g + "/configs/c.1/uvc.0")

    def _teardown_gadget(self):
        g = self.gadget
        f = self.func
        def rm(path):
            try:
                os.unlink(path)
            except OSError:
                pass
        def rmd(path):
            try:
                os.rmdir(path)
            except OSError:
                pass
        self._unbind_gadget(g)
        if self.udc_name:
            self._wait_udc_free(self.udc_name, timeout=3.0)
        rm(g + "/configs/c.1/uvc.0")
        for spd in ("fs", "ss"):
            rm(f + "/control/class/%s/h" % spd)
        for spd in ("fs", "hs", "ss"):
            rm(f + "/streaming/class/%s/h" % spd)
        for name in ("yuyv", "mjpg", "h264"):
            rm(f + "/streaming/header/h/" + name)
            rm(f + "/streaming/uncompressed/yuyv/" + name)
            rm(f + "/streaming/mjpeg/mjpg/" + name)
            rm(f + "/streaming/framebased/h264/" + name)
            rmd(f + "/streaming/color_matching/" + name)
        rmd(f + "/control/header/h")
        for path in sorted(glob.glob(f + "/control/*/*"), key=len, reverse=True):
            if os.path.islink(path):
                rm(path)
            elif os.path.isdir(path):
                rmd(path)
        for path in sorted(glob.glob(f + "/control/*"), key=len, reverse=True):
            if os.path.isdir(path):
                rmd(path)
        rmd(f + "/streaming/header/h")
        for group, fmt in (("uncompressed", "yuyv"), ("mjpeg", "mjpg"), ("framebased", "h264")):
            for d in glob.glob(f + "/streaming/%s/%s/*p" % (group, fmt)):
                rmd(d)
            rmd(f + "/streaming/%s/%s" % (group, fmt))
            rmd(f + "/streaming/%s" % group)
        for path in sorted(glob.glob(f + "/streaming/color_matching/*"), key=len, reverse=True):
            if os.path.isdir(path):
                rmd(path)
        rmd(f + "/streaming/color_matching")
        for path in sorted(glob.glob(f + "/streaming/*"), key=len, reverse=True):
            if os.path.isdir(path):
                rmd(path)
        rmd(f + "/streaming")
        rmd(f + "/control")
        rmd(f)
        rmd(g + "/configs/c.1/strings/0x409")
        rmd(g + "/configs/c.1")
        rmd(g + "/configs")
        rmd(g + "/strings/0x409")
        rmd(g + "/strings")
        rmd(g)
        self._built = False

    def _detect_udc(self):
        if self.udc:
            return self.udc
        udcs = sorted(os.listdir("/sys/class/udc"))
        if not udcs:
            raise RuntimeError("no UDC available in /sys/class/udc")
        return udcs[0]

    def _find_uvc_node(self, before, timeout=3.0):
        deadline = time.monotonic() + timeout
        while True:
            after = set(glob.glob("/dev/video*"))
            new_nodes = sorted(after - before)
            if new_nodes:
                return new_nodes[0]
            for v in sorted(after):
                base = os.path.basename(v)
                try:
                    with open("/sys/class/video4linux/%s/name" % base) as fh:
                        nm = fh.read().strip().lower()
                except OSError:
                    nm = ""
                try:
                    driver = os.path.basename(os.path.realpath(
                        "/sys/class/video4linux/%s/device/driver" % base)).lower()
                except OSError:
                    driver = ""
                try:
                    device = os.path.realpath("/sys/class/video4linux/%s/device" % base).lower()
                except OSError:
                    device = ""
                if "uvc" in nm or "gadget" in nm or "configfs-gadget" in driver or "gadget" in device:
                    return v
            if time.monotonic() >= deadline:
                return None
            time.sleep(0.05)

    def _wait_feeder_ready(self, timeout=3.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.feeder is not None and self.feeder.poll() is not None:
                raise RuntimeError("UVC feeder exited during startup with code %s" % self.feeder.returncode)
            if os.path.exists(self.ready_file):
                return
            time.sleep(0.05)
        raise RuntimeError("UVC feeder did not finish device setup in time")

    def open_event_reader(self):
        if self.event_fd is not None:
            return
        self.event_fd = os.open(self.event_fifo, os.O_RDWR | os.O_NONBLOCK)
        self.event_buf = ""

    def poll_events(self):
        if self.event_fd is None:
            return []
        events = []
        while True:
            try:
                chunk = os.read(self.event_fd, 4096)
            except BlockingIOError:
                break
            except OSError:
                break
            if not chunk:
                break
            self.event_buf += chunk.decode("ascii", "ignore")
            while "\n" in self.event_buf:
                line, self.event_buf = self.event_buf.split("\n", 1)
                line = line.strip()
                if line:
                    events.append(line)
        return events

    def start_feeder(self):
        try:
            os.unlink(self.ready_file)
        except FileNotFoundError:
            pass
        cmd = [
            self.feeder_bin, "--device", self.node, "--fifo", self.fifo,
            "--width", str(self.out_width), "--height", str(self.out_height),
            "--fps", str(self.fps), "--maxpkt", str(self.maxpkt), "--intf", "1",
            "--buffers", str(self.buffers), "--format", self.format,
            "--max-frame", str(self.sizeimage), "--ready-file", self.ready_file,
            "--event-fifo", self.event_fifo,
        ]
        self.feeder = subprocess.Popen(cmd)
        self._wait_feeder_ready(timeout=3.0)

    def _close_event_reader(self):
        if self.event_fd is not None:
            try:
                os.close(self.event_fd)
            except OSError:
                pass
            self.event_fd = None
        self.event_buf = ""

    def _stop_feeder(self):
        if self.feeder is None:
            return
        old_pid = self.feeder.pid
        try:
            if self.feeder.poll() is None:
                self.feeder.terminate()
                self.feeder.wait(timeout=3)
            else:
                self.feeder.wait(timeout=0)
        except Exception:
            try:
                self.feeder.kill()
                self.feeder.wait(timeout=1)
            except Exception:
                pass
        self.feeder = None
        print("[isp] uvc: feeder pid=%s stopped" % old_pid, flush=True)

    def rebind_gadget(self, reason):
        if not self.udc_name or not os.path.isdir(self.gadget):
            raise RuntimeError("cannot rebind UVC gadget before initial negotiation")
        print("[isp] uvc: %s -> rebind %s on %s"
              % (reason, self.gadget_name, self.udc_name), flush=True)
        self._close_event_reader()
        # Stop userspace first.  Unbinding the UDC while f_uvc userspace still
        # holds the video node can wedge configfs/f_uvc on lmi/dwc3.
        self._stop_feeder()
        self.node = None
        self._unbind_gadget(self.gadget)
        if not self._wait_udc_free(self.udc_name, timeout=3.0):
            print("[isp] uvc: warning: UDC %s still busy before rebind" % self.udc_name, flush=True)
        before = set(glob.glob("/dev/video*"))
        self._w("/UDC", self.udc_name)
        node = self._find_uvc_node(before, timeout=5.0)
        if not node:
            raise RuntimeError("UVC gadget video node did not appear after rebind")
        self.node = node
        self.open_event_reader()
        self.start_feeder()
        print("[isp] uvc: rebound %s on %s, node=%s, feeder pid=%s"
              % (self.gadget_name, self.udc_name, self.node, self.feeder.pid), flush=True)

    def recover_feeder(self):
        if self.feeder is None or self.feeder.poll() is None:
            return False
        if self.recoveries >= self.recovery_retries:
            return False
        old_code = self.feeder.returncode
        try:
            self.feeder.wait(timeout=0)
        except Exception:
            pass
        self.feeder = None
        self.recoveries += 1
        print("[isp] uvc feeder exited with code %s; rebinding gadget (%d/%d)"
              % (old_code, self.recoveries, self.recovery_retries), flush=True)
        self.rebind_gadget("feeder exit")
        return True

    def wait_event(self, timeout=0.5):
        events = self.poll_events()
        if events or self.event_fd is None:
            return events
        readable, _, _ = select.select([self.event_fd], [], [], timeout)
        if readable:
            events.extend(self.poll_events())
        return events

    def negotiate(self):
        if not os.path.isdir(self.configfs):
            raise RuntimeError("configfs not mounted at %s" % self.configfs)
        try:
            os.mkfifo(self.fifo, 0o600)
        except FileExistsError:
            pass
        try:
            os.mkfifo(self.event_fifo, 0o600)
        except FileExistsError:
            pass
        try:
            os.unlink(self.ready_file)
        except FileNotFoundError:
            pass
        before = set(glob.glob("/dev/video*"))
        try:
            self._build_gadget()
            udc = self._detect_udc()
            self.udc_name = udc
            # A UDC holds only one gadget; if the rootfs gadget (e.g. usb0 RNDIS/serial)
            # is bound, unbind it first and remember it so we can restore on close.
            self.prev_gadget = self._gadget_on_udc(udc)
            if self.prev_gadget:
                self._unbind_gadget(self.prev_gadget)
                if not self._wait_udc_free(udc, timeout=3.0):
                    raise RuntimeError("UDC %s did not become free after unbinding %s"
                                       % (udc, self.prev_gadget))
                print("[isp] uvc: temporarily unbound %s from %s (will restore on exit)"
                      % (os.path.basename(self.prev_gadget), udc), flush=True)
            self._w("/UDC", udc)
            self.node = self._find_uvc_node(before)
            if not self.node:
                raise RuntimeError("UVC gadget video node did not appear after UDC bind")
            self.open_event_reader()
            self.start_feeder()
            if self.external_writer:
                # the C ISP (lmi-isp --fifo) is the sole writer; we only manage the
                # gadget + feeder lifecycle here.
                print("[isp] uvc gadget %s bound to UDC %s, node=%s, feeder pid=%s fmt=%s (external writer)"
                      % (self.gadget_name, udc, self.node, self.feeder.pid, self.format), flush=True)
                return
            # open FIFO for writing; feeder holds the read end (O_RDWR) so this won't hang long
            deadline = time.monotonic() + 5.0
            while True:
                try:
                    self.fifo_fd = os.open(self.fifo, os.O_WRONLY | os.O_NONBLOCK)
                    break
                except OSError:
                    if self.feeder and self.feeder.poll() is not None:
                        raise RuntimeError("UVC feeder exited before opening FIFO")
                    if time.monotonic() > deadline:
                        raise RuntimeError("feeder did not open the FIFO read end in time")
                    time.sleep(0.1)
            import fcntl as _fcntl
            f_setpipe = getattr(_fcntl, "F_SETPIPE_SZ", 1031)
            try:
                self.pipe_cap = _fcntl.fcntl(self.fifo_fd, f_setpipe, self.sizeimage * 3)
            except OSError:
                self.pipe_cap = 65536
            print("[isp] uvc gadget %s bound to UDC %s, node=%s, feeder pid=%s fmt=%s"
                  % (self.gadget_name, udc, self.node, self.feeder.pid, self.format), flush=True)
        except Exception:
            self.close()
            raise

    def write_frame(self, frame):
        if self.fifo_fd is None:
            return
        # whole-frame-or-drop on a non-blocking pipe: only start a frame when the
        # pipe has room for all of it (keeps framing aligned), never block.
        import fcntl as _fcntl
        import array
        import termios
        try:
            cnt = array.array('i', [0])
            _fcntl.ioctl(self.fifo_fd, termios.FIONREAD, cnt, True)
            inpipe = cnt[0]
        except OSError:
            inpipe = 0
        if self.pipe_cap - inpipe < len(frame):
            return  # feeder not draining fast enough; drop this frame
        view = memoryview(frame)
        off = 0
        while off < len(view):
            try:
                off += os.write(self.fifo_fd, view[off:])
            except BlockingIOError:
                return
            except BrokenPipeError:
                return

    def done(self):
        return False

    def close(self):
        if self.fifo_fd is not None:
            try:
                os.close(self.fifo_fd)
            except OSError:
                pass
            self.fifo_fd = None
        if self.external_proc is not None and self.external_proc.poll() is None:
            try:
                self.external_proc.send_signal(signal.SIGINT)
                self.external_proc.wait(timeout=3)
            except Exception:
                try:
                    self.external_proc.terminate()
                    self.external_proc.wait(timeout=2)
                except Exception:
                    try:
                        self.external_proc.kill()
                        self.external_proc.wait(timeout=1)
                    except Exception:
                        pass
        self.external_proc = None
        if self.event_fd is not None:
            try:
                os.close(self.event_fd)
            except OSError:
                pass
            self.event_fd = None
        if self.feeder is not None:
            # Stop the f_uvc userspace endpoint before UDC unbind.  On lmi/dwc3,
            # unbinding while the feeder still has the gadget video node open can
            # block in f_uvc; after the feeder closes, the UDC disconnect completes.
            try:
                if self.feeder.poll() is None:
                    self.feeder.terminate()
                    self.feeder.wait(timeout=3)
                else:
                    self.feeder.wait(timeout=0)
            except Exception:
                try:
                    self.feeder.kill()
                    self.feeder.wait(timeout=1)
                except Exception:
                    pass
            self.feeder = None
        if self._built and not self.keep:
            self._unbind_gadget(self.gadget)
            if self.udc_name:
                self._wait_udc_free(self.udc_name, timeout=3.0)
            self._teardown_gadget()
        # restore whatever rootfs gadget (usb0 etc.) previously held the UDC
        if self.prev_gadget and self.udc_name and not self.keep:
            self._restore_previous_gadget()
        try:
            if not self.keep:
                os.unlink(self.fifo)
        except OSError:
            pass
        try:
            if not self.keep:
                os.unlink(self.ready_file)
        except OSError:
            pass
        try:
            if not self.keep:
                os.unlink(self.event_fifo)
        except OSError:
            pass

    def describe(self):
        return {"sink": self.kind, "node": self.node, "gadget": self.gadget_name, "fifo": self.fifo, "format": self.format}


def validate_isp_args(args):
    if args.isp_sink != "uvc":
        return
    uvc_format = getattr(args, "isp_uvc_format", "yuyv")
    if uvc_format == "yuyv" and args.isp_format != "yuyv":
        raise SystemExit("--isp-sink uvc --isp-uvc-format yuyv requires --isp-format yuyv")
    if uvc_format == "mjpeg" and getattr(args, "isp_engine", "c") != "c":
        raise SystemExit("--isp-uvc-format mjpeg currently requires --isp-engine c")
    if getattr(args, "isp_uvc_demand_start", False) and getattr(args, "isp_engine", "c") != "c":
        raise SystemExit("--isp-uvc-demand-start requires --isp-engine c")
    if uvc_format == "h264":
        if not getattr(args, "isp_uvc_h264_experimental", False):
            raise SystemExit("--isp-uvc-format h264 is experimental; pass --isp-uvc-h264-experimental after validating a Venus encoder")
        raise SystemExit("--isp-uvc-format h264 transport is scaffolded, but the Venus encoder bridge is not implemented yet; use --isp-uvc-format mjpeg")


def make_isp_sink(args):
    if args.isp_sink == "loopback":
        if not args.isp_node:
            raise SystemExit("--isp-sink loopback requires --isp-node /dev/videoN")
        return LoopbackSink(args)
    if args.isp_sink == "uvc":
        return UvcSink(args)
    return DumpSink(args)


_ISP_GAMMA_TABLES = {}


def isp_gamma_table(gamma):
    """Cached 256-entry per-channel tone curve (display gamma encode). Applied
    with bytes.translate() so it costs almost nothing per frame, unlike the
    per-pixel pow() in apply_color_transform()."""
    key = round(gamma, 3)
    table = _ISP_GAMMA_TABLES.get(key)
    if table is None:
        if abs(gamma - 1.0) < 1e-3:
            table = bytes(range(256))
        else:
            inv = 1.0 / gamma
            table = bytes(min(255, max(0, int(round(255.0 * ((i / 255.0) ** inv))))) for i in range(256))
        _ISP_GAMMA_TABLES[key] = table
    return table


def isp_process_frame(camera, args, sink):
    raw, meta = camera.read_frame()
    rgb, src_width, src_height, raw_format, luma = raw_to_rgb_preview(
        raw,
        meta["width"],
        meta["height"],
        args.raw_format,
        meta["stride"],
        args.shift,
        args.isp_src_width,
    )
    red_gain = args.red_gain
    green_gain = args.green_gain
    blue_gain = args.blue_gain
    if args.auto_white_balance:
        awb = rgb_stats(rgb, args.awb_min_luma, args.awb_max_luma, args.awb_clip_margin)
        green = max(awb["green_mean"], 1.0)
        red_gain *= float_clamp(green / max(awb["red_mean"], 1.0), args.awb_min_gain, args.awb_max_gain)
        blue_gain *= float_clamp(green / max(awb["blue_mean"], 1.0), args.awb_min_gain, args.awb_max_gain)
    if any(abs(gain - 1.0) > 0.001 for gain in (red_gain, green_gain, blue_gain)):
        apply_rgb_gains(rgb, red_gain, green_gain, blue_gain)
    # optional CCM (per-pixel, slow) only if non-identity; gamma is done via the
    # fast translate LUT below to give the image proper tonal latitude cheaply.
    identity_ccm = (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0)
    if any(abs(a - b) > 1e-4 for a, b in zip(args.ccm, identity_ccm)):
        apply_color_transform(rgb, args.ccm, 1.0)
    gamma = getattr(args, "isp_gamma", 2.2)
    if abs(gamma - 1.0) > 1e-3:
        rgb[:] = bytes(rgb).translate(isp_gamma_table(gamma))
    if getattr(sink, "format", args.isp_format) == "yuyv":
        frame = rgb_to_yuyv(rgb, src_width, src_height, sink.out_width, sink.out_height, sink.stride)
    else:
        frame = rgb_to_nv12_scaled(rgb, src_width, src_height, sink.out_width, sink.out_height, sink.stride)
    if sink.sizeimage and len(frame) != sink.sizeimage:
        if len(frame) < sink.sizeimage:
            frame = frame + bytes(sink.sizeimage - len(frame))
        else:
            frame = frame[:sink.sizeimage]
    controls = camera.update_auto_exposure(luma)
    info = {
        "raw_format": raw_format,
        "src_width": src_width,
        "src_height": src_height,
        "ae_luma": round(luma, 2),
        "gains": {"red": round(red_gain, 3), "green": round(green_gain, 3), "blue": round(blue_gain, 3)},
        "controls": controls,
    }
    return frame, meta, info


def build_isp_c_command(args, out, gadget=None):
    ctrl = args.control_subdev or args.sensor_subdev or ""
    cmd = [
        args.isp_engine_bin,
        "--raw", args.video,
        "--out-width", str(args.isp_width),
        "--out-height", str(args.isp_height),
        "--gamma", str(getattr(args, "isp_gamma", 2.2)),
        "--tone-highlight-knee", str(args.isp_tone_highlight_knee),
        "--tone-highlight-max", str(args.isp_tone_highlight_max),
        "--fps-cap", str(args.isp_fps_cap),
        "--max-soft-gain", str(args.isp_max_soft_gain),
    ] + out
    if ctrl:
        cmd += ["--ctrl", ctrl]
    if args.isp_sink == "uvc" and args.isp_uvc_format == "mjpeg":
        cmd += [
            "--mjpeg",
            "--mjpeg-quality", str(args.isp_uvc_mjpeg_quality),
            "--mjpeg-sharpen", str(args.isp_uvc_mjpeg_sharpen),
            "--mjpeg-smooth", str(args.isp_uvc_mjpeg_smooth),
            "--mjpeg-area-scale", str(args.isp_uvc_mjpeg_area_scale),
            "--mjpeg-subsampling", str(args.isp_uvc_mjpeg_subsampling),
            "--mjpeg-scale-mode", str(args.isp_uvc_mjpeg_scale_mode),
            "--max-frame-bytes", str(gadget.sizeimage if gadget is not None else args.isp_uvc_max_frame_bytes),
        ]
    elif args.isp_format == "nv12":
        cmd.append("--nv12")
    if args.auto_exposure:
        cmd += ["--auto-exposure", "--target", str(int(args.target_luma))]
        if args.isp_ae_clip_target is not None:
            cmd += ["--ae-clip-target", str(args.isp_ae_clip_target)]
            cmd += ["--ae-clip-weight", str(args.isp_ae_clip_weight)]
        if args.isp_max_digital_gain is not None:
            cmd += ["--max-digital-gain", str(args.isp_max_digital_gain)]
    return cmd


def stop_isp_process(proc, label="ISP"):
    if proc is None:
        return None
    if proc.poll() is not None:
        return proc.returncode
    try:
        proc.send_signal(signal.SIGINT)
        proc.wait(timeout=3)
    except Exception:
        try:
            proc.terminate()
            proc.wait(timeout=2)
        except Exception:
            try:
                proc.kill()
                proc.wait(timeout=1)
            except Exception:
                pass
    print("[isp] %s stopped" % label, flush=True)
    return proc.returncode


def run_isp_c_uvc_demand(args):
    """Keep the USB UVC gadget enumerated, but start the RAW camera + C ISP only
    while the host is actively streaming.  This is the intended long-running mode
    for boot-time camera-device presentation without keeping OV13B10/CAMSS active
    when no host application is using the webcam."""
    validate_isp_args(args)
    gadget = UvcSink(args)
    gadget.external_writer = True
    proc = None
    stream_requested = False
    pending_stop_at = None
    pending_rebind_reason = None
    next_start_at = None
    signal_handlers = {}

    def request_stop(signum, _frame):
        raise KeyboardInterrupt("signal %d" % signum)

    def start_proc():
        nonlocal proc
        if proc is not None and proc.poll() is None:
            return
        configure(args)
        cmd = build_isp_c_command(args, ["--fifo", args.isp_uvc_fifo], gadget)
        print("[isp] uvc demand: STREAMON -> starting camera/ISP: %s" % " ".join(cmd), flush=True)
        proc = subprocess.Popen(cmd)
        gadget.external_proc = proc

    def stop_proc(reason):
        nonlocal proc
        if proc is not None:
            print("[isp] uvc demand: %s -> stopping camera/ISP" % reason, flush=True)
            stop_isp_process(proc, "camera/ISP")
            proc = None
            gadget.external_proc = None

    def schedule_start_retry(reason):
        nonlocal next_start_at
        if not stream_requested:
            next_start_at = None
            return
        delay = max(0.1, args.isp_uvc_restart_delay)
        next_start_at = time.monotonic() + delay
        print("[isp] uvc demand: %s -> retry camera/ISP start in %.1fs" % (reason, delay), flush=True)

    def try_start_proc(reason):
        nonlocal next_start_at
        if not stream_requested:
            next_start_at = None
            return
        if proc is not None and proc.poll() is None:
            next_start_at = None
            return
        try:
            start_proc()
            next_start_at = None
        except Exception as exc:
            print("[isp] uvc demand: camera/ISP start failed during %s: %s" % (reason, exc), flush=True)
            stop_proc("start failure")
            schedule_start_retry("start failure")

    if threading.current_thread() is threading.main_thread():
        for signum in (signal.SIGINT, signal.SIGTERM):
            signal_handlers[signum] = signal.getsignal(signum)
            signal.signal(signum, request_stop)
    try:
        gadget.negotiate()
        print("[isp] uvc demand: gadget is enumerated; waiting for host STREAMON before starting camera/ISP", flush=True)
        while True:
            if gadget.feeder is not None and gadget.feeder.poll() is not None:
                stop_proc("feeder exit")
                try:
                    if gadget.recover_feeder():
                        stream_requested = False
                        pending_stop_at = None
                        pending_rebind_reason = None
                        next_start_at = None
                        continue
                except Exception as exc:
                    print("[isp] uvc feeder recovery failed: %s" % exc, flush=True)
                feeder_code = gadget.feeder.returncode if gadget.feeder is not None else "unknown"
                raise RuntimeError("UVC feeder exited with code %s" % feeder_code)

            for event in gadget.wait_event(timeout=0.5):
                if event == "STREAMON":
                    stream_requested = True
                    pending_stop_at = None
                    pending_rebind_reason = None
                    try_start_proc("STREAMON")
                elif event in ("STREAMOFF", "DISCONNECT"):
                    if stream_requested or proc is not None:
                        print("[isp] uvc demand: %s -> will stop camera/ISP after %.1fs grace"
                              % (event, args.isp_uvc_idle_stop_delay), flush=True)
                    stream_requested = False
                    next_start_at = None
                    pending_stop_at = time.monotonic() + max(0.0, args.isp_uvc_idle_stop_delay)
                    if event == "DISCONNECT":
                        pending_rebind_reason = "host DISCONNECT"
                        gadget.poll_events()
                elif event == "CONNECT":
                    print("[isp] uvc demand: host connected", flush=True)

            if proc is not None and proc.poll() is not None:
                print("[isp] uvc demand: camera/ISP exited with code %s" % proc.returncode, flush=True)
                proc = None
                gadget.external_proc = None
                if stream_requested:
                    pending_stop_at = None
                    schedule_start_retry("camera/ISP exit")
            if next_start_at is not None and time.monotonic() >= next_start_at:
                try_start_proc("retry")
            if pending_stop_at is not None and time.monotonic() >= pending_stop_at:
                stop_proc("idle")
                pending_stop_at = None
                if pending_rebind_reason is not None:
                    try:
                        gadget.rebind_gadget(pending_rebind_reason)
                    except Exception as exc:
                        print("[isp] uvc rebind after %s failed: %s" % (pending_rebind_reason, exc), flush=True)
                        raise
                    pending_rebind_reason = None
    except KeyboardInterrupt:
        pass
    finally:
        stop_proc("exit")
        gadget.close()
        for signum, handler in signal_handlers.items():
            signal.signal(signum, handler)


def run_isp_c(args):
    """C-engine ISP path: Python set up the route/sensor-mode (and the UVC gadget
    when needed); the fast lmi-isp binary does capture + full ISP + output. This
    is the default engine — far higher fps/resolution than the pure-Python loop."""
    validate_isp_args(args)
    gadget = None
    proc = None
    signal_handlers = {}

    def request_stop(signum, _frame):
        raise KeyboardInterrupt("signal %d" % signum)

    if threading.current_thread() is threading.main_thread():
        for signum in (signal.SIGINT, signal.SIGTERM):
            signal_handlers[signum] = signal.getsignal(signum)
            signal.signal(signum, request_stop)
    try:
        if args.isp_sink == "uvc":
            gadget = UvcSink(args)
            gadget.external_writer = True
            gadget.negotiate()
            out = ["--fifo", args.isp_uvc_fifo]
        elif args.isp_sink == "loopback":
            if not args.isp_node:
                raise SystemExit("--isp-sink loopback requires --isp-node /dev/videoN")
            out = ["--loopback", args.isp_node]
        elif args.isp_dump:
            out = ["--dump", args.isp_dump, "--frames", str(max(1, args.isp_frames))]
        else:
            raise SystemExit("--isp-engine c requires --isp-sink loopback/uvc or --isp-dump")

        cmd = build_isp_c_command(args, out, gadget)
        print("[isp] engine=c: %s" % " ".join(cmd), flush=True)
        proc = subprocess.Popen(cmd)
        if gadget is not None:
            gadget.external_proc = proc
        while True:
            try:
                proc.wait(timeout=0.5)
                break
            except subprocess.TimeoutExpired:
                if gadget is not None and gadget.feeder is not None and gadget.feeder.poll() is not None:
                    try:
                        if gadget.recover_feeder():
                            continue
                    except Exception as exc:
                        print("[isp] uvc feeder recovery failed: %s" % exc, flush=True)
                    feeder_code = gadget.feeder.returncode if gadget.feeder is not None else "unknown"
                    print("[isp] uvc feeder exited with code %s; stopping ISP" % feeder_code, flush=True)
                    stop_isp_process(proc)
                    break
    except KeyboardInterrupt:
        stop_isp_process(proc)
    finally:
        if gadget is not None:
            gadget.close()
        for signum, handler in signal_handlers.items():
            signal.signal(signum, handler)


def run_isp(args):
    validate_isp_args(args)
    if (getattr(args, "isp_engine", "c") == "c" and args.isp_sink == "uvc" and
            getattr(args, "isp_uvc_demand_start", False)):
        run_isp_c_uvc_demand(args)
        return
    configure(args)
    if getattr(args, "isp_engine", "c") == "c":
        run_isp_c(args)
        return
    sink = make_isp_sink(args)
    try:
        sink.negotiate()
    except Exception:
        sink.close()
        raise
    print(
        "[isp] sink=%s node=%s fmt=%s %dx%d stride=%d size=%d demosaic_src_w=%d"
        % (
            sink.kind,
            getattr(sink, "node", None),
            sink.format,
            sink.out_width,
            sink.out_height,
            sink.stride,
            sink.sizeimage,
            args.isp_src_width,
        ),
        flush=True,
    )
    frames = 0
    t_start = time.monotonic()
    last_report = t_start
    try:
        with IoctlCamera(args) as camera:
            if args.auto_exposure:
                for _ in range(max(0, args.ae_warmup_frames)):
                    isp_process_frame(camera, args, sink)
                    if args.ae_warmup_interval > 0:
                        time.sleep(args.ae_warmup_interval)
            while True:
                started = time.monotonic()
                frame, meta, info = isp_process_frame(camera, args, sink)
                sink.write_frame(frame)
                frames += 1
                if sink.done():
                    break
                now_t = time.monotonic()
                if now_t - last_report >= 2.0:
                    fps = frames / max(1e-6, now_t - t_start)
                    sys.stderr.write(
                        "[isp] frames=%d fps=%.2f ae_luma=%s gains=%s\n"
                        % (frames, fps, info["ae_luma"], info["gains"])
                    )
                    last_report = now_t
                    if args.isp_status:
                        now_status_write(args.isp_status, {
                            "schema": "lmi.software-isp.status.v1",
                            "sink": sink.describe(),
                            "format": sink.format,
                            "width": sink.out_width,
                            "height": sink.out_height,
                            "stride": sink.stride,
                            "sizeimage": sink.sizeimage,
                            "frames": frames,
                            "fps": round(fps, 2),
                            "kernel_isp": False,
                            "demosaic": "software 2x2 GRBG + nearest resample",
                            "raw_format": info["raw_format"],
                            "ae_luma": info["ae_luma"],
                            "gains": info["gains"],
                            "controls": info["controls"],
                            "updated_at": now(),
                        })
                if args.isp_fps_cap > 0:
                    delay = (1.0 / args.isp_fps_cap) - (time.monotonic() - started)
                    if delay > 0:
                        time.sleep(delay)
    except KeyboardInterrupt:
        print("[isp] stopping", flush=True)
    finally:
        sink.close()
    elapsed = time.monotonic() - t_start
    print(
        "[isp] done frames=%d elapsed=%.2fs avg_fps=%.2f"
        % (frames, elapsed, frames / max(1e-6, elapsed)),
        flush=True,
    )


def parse_args():
    parser = argparse.ArgumentParser(description="Serve a raw V4L2 camera node as a browser preview page.")
    parser.add_argument("--route", default="lmi-ov13b10", choices=("lmi-ov13b10", "none"))
    parser.add_argument("--media", default="/dev/media0")
    parser.add_argument("--media-print", action="store_true", help="print selected media graph before starting")
    parser.add_argument("--video", help="capture video node; auto-selected for --route=lmi-ov13b10")
    parser.add_argument("--video-entity", default="msm_vfe1_video0")
    parser.add_argument("--sensor", default="ov13b10")
    parser.add_argument("--csiphy", default="msm_csiphy1")
    parser.add_argument("--csid", default="msm_csid1")
    parser.add_argument("--vfe", default="msm_vfe1_rdi0")
    parser.add_argument("--csid-source-pad", type=int, default=1)
    parser.add_argument("--sensor-subdev")
    parser.add_argument("--csiphy-subdev")
    parser.add_argument("--csid-subdev")
    parser.add_argument("--vfe-subdev")
    parser.add_argument("--control-subdev", help="sensor subdev node used for exposure/gain controls")
    parser.add_argument("--keep-links", action="store_true", help="do not disable existing mutable CAMSS route links")
    parser.add_argument("--width", type=int, default=1364)
    parser.add_argument("--height", type=int, default=768)
    parser.add_argument("--mode-index", type=int, help="select width/height from VIDIOC_SUBDEV_ENUM_FRAME_SIZE output")
    parser.add_argument("--mbus-code", type=lambda value: int(value, 0), default=MEDIA_BUS_FMT_SGRBG10_1X10)
    parser.add_argument("--pixelformat", default="pgAA", help="V4L2 fourcc, default is packed 10-bit GRBG Bayer")
    parser.add_argument("--raw-format", choices=("auto", "gray8", "bayer8", "bayer10le", "bayer10p"), default="auto")
    parser.add_argument("--stride-bytes", type=int, default=0)
    parser.add_argument("--shift", type=int, default=2, help="right shift for unpacked 10-bit samples")
    parser.add_argument("--image-format", choices=("png-color", "bmp-gray"), default="png-color")
    parser.add_argument("--png-level", type=int, default=1, help="PNG compression level for browser preview")
    parser.add_argument("--preview-width", type=int, default=480)
    parser.add_argument("--no-auto-white-balance", dest="auto_white_balance", action="store_false", help="disable preview-only gray-world white balance")
    parser.add_argument("--red-gain", type=float, default=1.0)
    parser.add_argument("--green-gain", type=float, default=1.0)
    parser.add_argument("--blue-gain", type=float, default=1.0)
    parser.add_argument("--awb-min-gain", type=float, default=0.5)
    parser.add_argument("--awb-max-gain", type=float, default=2.0)
    parser.add_argument("--awb-min-luma", type=int, default=16, help="lowest luma sample used for preview AWB")
    parser.add_argument("--awb-max-luma", type=int, default=240, help="highest luma sample used for preview AWB")
    parser.add_argument("--awb-clip-margin", type=int, default=3, help="exclude near-clipped RGB samples from preview AWB")
    parser.add_argument("--ccm", type=parse_ccm, default=parse_ccm("1,0,0,0,1,0,0,0,1"), help="3x3 preview CCM as 9 comma-separated numbers")
    parser.add_argument("--gamma", type=float, default=1.0, help="preview gamma value applied after CCM")
    parser.set_defaults(auto_white_balance=True)
    parser.add_argument("--capture-timeout", type=float, default=5.0)
    parser.add_argument("--buffers", type=int, default=3)
    parser.add_argument("--vblank", type=int, help="initial V4L2_CID_VBLANK value")
    parser.add_argument("--exposure", type=int, help="initial V4L2_CID_EXPOSURE value")
    parser.add_argument("--analogue-gain", type=int, help="initial V4L2_CID_ANALOGUE_GAIN value")
    parser.add_argument("--digital-gain", type=int, help="initial V4L2_CID_DIGITAL_GAIN value")
    parser.add_argument("--reset-controls", action="store_true", help="reset exposure/gain controls to driver defaults before starting")
    parser.add_argument("--preserve-controls", action="store_true", help="do not reset controls before auto exposure")
    parser.add_argument("--auto-exposure", action="store_true", help="adjust exposure/gain from preview luma")
    parser.add_argument("--ae-mode", choices=("video", "balanced", "low-light"), default="video")
    parser.add_argument("--max-auto-vblank", type=int, help="highest VBLANK value auto exposure may set; overrides --ae-mode")
    parser.add_argument("--target-luma", type=float, default=110.0)
    parser.add_argument("--ae-metering", choices=("mean", "p50", "p70", "p90"), default="p50", help="preview luma statistic used by software AE")
    parser.add_argument("--ae-deadband", type=float, default=8.0)
    parser.add_argument("--ae-interval", type=int, default=3, help="captured frames between AE updates")
    parser.add_argument("--ae-max-step", type=float, default=1.12, help="largest per-update AE multiplier")
    parser.add_argument("--ae-warmup-frames", type=int, default=8, help="discarded AE frames before --once output")
    parser.add_argument("--ae-warmup-interval", type=float, default=0.03, help="seconds between --once AE warmup frames")
    parser.add_argument("--interval", type=float, default=0.03, help="seconds between captured preview frames")
    parser.add_argument("--browser-interval", type=float, default=0.2, help="seconds between browser refreshes")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--list", action="store_true", help="list media/video/subdev devices and exit")
    parser.add_argument("--list-modes", action="store_true", help="enumerate OV13B10 subdev frame sizes and intervals")
    parser.add_argument("--discover", action="store_true", help="print route, sensor, video-node, and raw-output capability discovery as JSON")
    parser.add_argument("--controls", action="store_true", help="enumerate OV13B10 V4L2 controls, ranges, defaults, and current values")
    parser.add_argument("--metadata", action="store_true", help="query OV13B10 subdev frame interval and crop selection metadata")
    parser.add_argument("--metadata-set-interval", action="store_true", help="also round-trip S_FRAME_INTERVAL using the current interval")
    parser.add_argument("--once", action="store_true", help="capture one preview image and exit")
    parser.add_argument("--output", help="preview image output path for --once")
    parser.add_argument("--raw-output", help="optional raw frame output path for --once")
    parser.add_argument("--nv12-output", help="optional software NV12 frame output path for --once, for codec tests only")
    parser.add_argument("--metadata-output", help="optional JSON metadata output path for --once")
    parser.add_argument("--isp-sink", choices=("none", "loopback", "uvc"), default="none", help="run the software-ISP daemon, feeding color frames to a standard V4L2 node (loopback/uvc) instead of serving the HTTP preview")
    parser.add_argument("--isp-node", help="target V4L2 node for --isp-sink loopback (the v4l2loopback OUTPUT node, e.g. /dev/video20)")
    parser.add_argument("--isp-format", choices=("yuyv", "nv12"), default="yuyv", help="ISP node pixel format")
    parser.add_argument("--isp-width", type=int, default=640, help="advertised ISP node width")
    parser.add_argument("--isp-height", type=int, default=480, help="advertised ISP node height")
    parser.add_argument("--isp-src-width", type=int, default=480, help="software demosaic working width (perf knob; resampled to --isp-width)")
    parser.add_argument("--isp-gamma", type=float, default=2.2, help="ISP display gamma tone curve (1.0 = linear/off); improves shadow/highlight latitude")
    parser.add_argument("--isp-tone-highlight-knee", type=int, default=0, help="optional C ISP post-gamma highlight shoulder knee (0 = off)")
    parser.add_argument("--isp-tone-highlight-max", type=int, default=255, help="optional C ISP post-gamma highlight shoulder ceiling")
    parser.add_argument("--isp-max-soft-gain", type=float, default=4.0, help="maximum software lift in the C ISP auto-tone path; lower reduces dark-scene noise")
    parser.add_argument("--isp-max-digital-gain", type=int, help="optional C ISP digital gain ceiling for software AE; lower reduces sensor noise")
    parser.add_argument("--isp-ae-clip-target", type=int, help="optional C ISP raw-highlight percentile cap for software AE; lower preserves bright lights")
    parser.add_argument("--isp-ae-clip-weight", type=int, default=50, help="C ISP AE highlight-cap penalty weight in percent")
    parser.add_argument("--isp-engine", choices=("c", "python"), default="c", help="frame engine: c = fast lmi-isp binary (default, higher fps/res), python = pure-stdlib loop")
    parser.add_argument("--isp-engine-bin", default="/tmp/lmi-isp", help="path to the cross-compiled lmi-isp binary for --isp-engine c")
    parser.add_argument("--isp-fps-cap", type=int, default=15, help="pace ISP output to at most this many fps (0 = uncapped)")
    parser.add_argument("--isp-dump", help="for --isp-sink none: write produced ISP frames to this raw file")
    parser.add_argument("--isp-frames", type=int, default=10, help="for --isp-sink none --isp-dump: stop after N frames")
    parser.add_argument("--isp-status", help="optional JSON status output path updated periodically by the ISP daemon")
    parser.add_argument("--isp-uvc-gadget-name", default="lmi_uvc", help="configfs usb_gadget name for --isp-sink uvc")
    parser.add_argument("--isp-uvc-feeder-bin", default="/tmp/lmi-uvc-gadget", help="path to the cross-compiled UVC feeder binary")
    parser.add_argument("--isp-uvc-fifo", default="/tmp/lmi-uvc.fifo", help="FIFO used to hand frames to the UVC feeder")
    parser.add_argument("--isp-uvc-event-fifo", default="", help="FIFO used by lmi-uvc-gadget to report host STREAMON/OFF events")
    parser.add_argument("--isp-uvc-demand-start", action="store_true", help="keep UVC enumerated but start OV13B10/CAMSS/lmi-isp only while the host streams")
    parser.add_argument("--isp-uvc-idle-stop-delay", type=float, default=2.0, help="seconds to keep camera/ISP alive after UVC STREAMOFF before stopping it")
    parser.add_argument("--isp-uvc-restart-delay", type=float, default=1.0, help="seconds before restarting camera/ISP if it exits while the host still streams")
    parser.add_argument("--isp-uvc-format", choices=("yuyv", "mjpeg", "h264"), default="yuyv", help="UVC advertised/transport format; MJPEG reduces USB-HS bandwidth")
    parser.add_argument("--isp-uvc-mjpeg-quality", type=int, default=60, help="software MJPEG quality for --isp-uvc-format mjpeg")
    parser.add_argument("--isp-uvc-mjpeg-sharpen", type=int, default=0, help="MJPEG RGB unsharp amount in percent (0 = off)")
    parser.add_argument("--isp-uvc-mjpeg-smooth", type=int, default=0, help="MJPEG RGB post-downscale smoothing amount in percent (0 = off)")
    parser.add_argument("--isp-uvc-mjpeg-area-scale", type=int, default=100, help="percent of each Bayer source footprint averaged before MJPEG encode (lower = sharper, noisier)")
    parser.add_argument("--isp-uvc-mjpeg-subsampling", type=int, choices=(420, 444), default=420, help="JPEG chroma subsampling for MJPEG UVC; 444 avoids chroma subsampling grid artifacts")
    parser.add_argument("--isp-uvc-mjpeg-scale-mode", choices=("bayer-area", "bayer-area-frac", "bayer-center", "demosaic-center", "demosaic-4tap", "demosaic-9tap"), default="bayer-area", help="MJPEG RGB generation path; bayer-area-frac is the current 720p UVC quality path")
    parser.add_argument("--isp-uvc-max-frame-bytes", type=int, default=0, help="maximum compressed UVC payload size (0 = format default)")
    parser.add_argument("--isp-uvc-h264-experimental", action="store_true", help="allow the scaffolded H.264 UVC path when a separate encoder helper is available")
    parser.add_argument("--isp-uvc-udc", default="", help="UDC name to bind (default: first /sys/class/udc entry)")
    parser.add_argument("--isp-uvc-maxpkt", type=int, default=1024, help="UVC streaming endpoint max packet size")
    parser.add_argument("--isp-uvc-buffers", type=int, default=2, help="UVC feeder MMAP buffer count (2-4; lower reduces latency)")
    parser.add_argument("--isp-uvc-recovery-retries", type=int, default=3, help="restart the UVC feeder this many times after transient USB/g_uvc fd loss")
    parser.add_argument("--isp-uvc-keep", action="store_true", help="do not tear down the UVC gadget/FIFO on exit (debug)")
    return parser.parse_args()


def main():
    args = parse_args()
    args.selected_mode = None
    args.video_configured_format = None
    if args.list:
        list_devices()
        return
    if args.list_modes:
        list_sensor_modes(args)
        return
    select_sensor_mode(args)
    if args.discover:
        discover_camera(args)
        return
    if args.controls:
        query_sensor_controls(args)
        return
    if args.metadata:
        query_sensor_metadata(args)
        return
    if args.once:
        capture_once_to_file(args)
        return
    if args.isp_sink != "none" or args.isp_dump:
        run_isp(args)
        return
    serve(args)


if __name__ == "__main__":
    main()
