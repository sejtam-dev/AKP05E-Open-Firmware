"""Minimal Windows HID access via ctypes (no third-party packages).

list_devices() enumerates HID interfaces, write() sends one output report.
Windows only; written against hid.dll / setupapi.dll documentation.
"""
import ctypes
import re
import time
import sys
from ctypes import wintypes

if sys.platform != "win32":
    raise ImportError("winhid works on Windows only")

hid = ctypes.WinDLL("hid.dll", use_last_error=True)
setupapi = ctypes.WinDLL("setupapi.dll", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32.dll", use_last_error=True)

HANDLE = ctypes.c_void_p
INVALID_HANDLE_VALUE = HANDLE(-1).value
GENERIC_READ, GENERIC_WRITE = 0x80000000, 0x40000000
FILE_SHARE_READ, FILE_SHARE_WRITE = 1, 2
OPEN_EXISTING = 3
DIGCF_PRESENT, DIGCF_DEVICEINTERFACE = 0x2, 0x10


class GUID(ctypes.Structure):
    _fields_ = [("Data1", wintypes.DWORD), ("Data2", wintypes.WORD),
                ("Data3", wintypes.WORD), ("Data4", ctypes.c_ubyte * 8)]


class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("InterfaceClassGuid", GUID),
                ("Flags", wintypes.DWORD), ("Reserved", ctypes.c_void_p)]


class HIDD_ATTRIBUTES(ctypes.Structure):
    _fields_ = [("Size", wintypes.ULONG), ("VendorID", wintypes.USHORT),
                ("ProductID", wintypes.USHORT), ("VersionNumber", wintypes.USHORT)]


class HIDP_CAPS(ctypes.Structure):
    _fields_ = [("Usage", wintypes.USHORT), ("UsagePage", wintypes.USHORT),
                ("InputReportByteLength", wintypes.USHORT),
                ("OutputReportByteLength", wintypes.USHORT),
                ("FeatureReportByteLength", wintypes.USHORT),
                ("Reserved", wintypes.USHORT * 17),
                ("NumberLinkCollectionNodes", wintypes.USHORT),
                ("NumberInputButtonCaps", wintypes.USHORT),
                ("NumberInputValueCaps", wintypes.USHORT),
                ("NumberInputDataIndices", wintypes.USHORT),
                ("NumberOutputButtonCaps", wintypes.USHORT),
                ("NumberOutputValueCaps", wintypes.USHORT),
                ("NumberOutputDataIndices", wintypes.USHORT),
                ("NumberFeatureButtonCaps", wintypes.USHORT),
                ("NumberFeatureValueCaps", wintypes.USHORT),
                ("NumberFeatureDataIndices", wintypes.USHORT)]


hid.HidD_GetHidGuid.argtypes = [ctypes.POINTER(GUID)]
hid.HidD_GetAttributes.argtypes = [HANDLE, ctypes.POINTER(HIDD_ATTRIBUTES)]
hid.HidD_GetAttributes.restype = wintypes.BOOLEAN
hid.HidD_GetPreparsedData.argtypes = [HANDLE, ctypes.POINTER(ctypes.c_void_p)]
hid.HidD_GetPreparsedData.restype = wintypes.BOOLEAN
hid.HidD_FreePreparsedData.argtypes = [ctypes.c_void_p]
hid.HidP_GetCaps.argtypes = [ctypes.c_void_p, ctypes.POINTER(HIDP_CAPS)]
hid.HidD_GetProductString.argtypes = [HANDLE, ctypes.c_void_p, wintypes.ULONG]
hid.HidD_GetProductString.restype = wintypes.BOOLEAN
hid.HidD_GetManufacturerString.argtypes = [HANDLE, ctypes.c_void_p, wintypes.ULONG]
hid.HidD_GetManufacturerString.restype = wintypes.BOOLEAN

setupapi.SetupDiGetClassDevsW.argtypes = [ctypes.POINTER(GUID), wintypes.LPCWSTR, wintypes.HWND, wintypes.DWORD]
setupapi.SetupDiGetClassDevsW.restype = HANDLE
setupapi.SetupDiEnumDeviceInterfaces.argtypes = [HANDLE, ctypes.c_void_p, ctypes.POINTER(GUID),
                                                 wintypes.DWORD, ctypes.POINTER(SP_DEVICE_INTERFACE_DATA)]
setupapi.SetupDiEnumDeviceInterfaces.restype = wintypes.BOOL
setupapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [HANDLE, ctypes.POINTER(SP_DEVICE_INTERFACE_DATA),
                                                      ctypes.c_void_p, wintypes.DWORD,
                                                      ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
setupapi.SetupDiGetDeviceInterfaceDetailW.restype = wintypes.BOOL
setupapi.SetupDiDestroyDeviceInfoList.argtypes = [HANDLE]

kernel32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                 wintypes.DWORD, wintypes.DWORD, HANDLE]
kernel32.CreateFileW.restype = HANDLE
kernel32.WriteFile.argtypes = [HANDLE, ctypes.c_void_p, wintypes.DWORD,
                               ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
kernel32.WriteFile.restype = wintypes.BOOL
kernel32.CloseHandle.argtypes = [HANDLE]


def _device_paths():
    guid = GUID()
    hid.HidD_GetHidGuid(ctypes.byref(guid))
    info = setupapi.SetupDiGetClassDevsW(ctypes.byref(guid), None, None, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)
    if info == INVALID_HANDLE_VALUE:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        i = 0
        while True:
            data = SP_DEVICE_INTERFACE_DATA()
            data.cbSize = ctypes.sizeof(SP_DEVICE_INTERFACE_DATA)
            if not setupapi.SetupDiEnumDeviceInterfaces(info, None, ctypes.byref(guid), i, ctypes.byref(data)):
                return
            need = wintypes.DWORD()
            setupapi.SetupDiGetDeviceInterfaceDetailW(info, ctypes.byref(data), None, 0, ctypes.byref(need), None)
            buf = ctypes.create_string_buffer(need.value)
            # cbSize of SP_DEVICE_INTERFACE_DETAIL_DATA_W: 8 on 64-bit, 6 on 32-bit
            ctypes.c_uint32.from_buffer(buf).value = 8 if ctypes.sizeof(ctypes.c_void_p) == 8 else 6
            if setupapi.SetupDiGetDeviceInterfaceDetailW(info, ctypes.byref(data), buf, need.value, None, None):
                yield ctypes.wstring_at(ctypes.addressof(buf) + 4)
            i += 1
    finally:
        setupapi.SetupDiDestroyDeviceInfoList(info)


def _open(path, access):
    h = kernel32.CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, None, OPEN_EXISTING, 0, None)
    return None if h == INVALID_HANDLE_VALUE else h


def _string(fn, h):
    buf = ctypes.create_unicode_buffer(256)
    return buf.value if fn(h, buf, ctypes.sizeof(buf)) else ""


def list_devices(vid=None, pid=None, timing=None):
    """HID interfaces of the system, optionally only those of one vid / pid.

    The vid / pid are read from the device path first, so devices of other vendors are never opened (opening every
    keyboard, mouse and receiver of the PC is slow and some of them stall). If `timing` is a list, (path, seconds,
    what) tuples are appended to it."""
    out = []
    for path in _device_paths():
        mv = re.search(r"vid_([0-9a-f]{4})", path, re.I)
        mp = re.search(r"pid_([0-9a-f]{4})", path, re.I)
        if vid is not None and mv and int(mv.group(1), 16) != vid:
            continue
        if pid is not None and mp and int(mp.group(1), 16) != pid:
            continue
        t0 = time.time()
        h = _open(path, 0)
        if timing is not None:
            timing.append((path, time.time() - t0, "open"))
        if h is None:
            continue
        try:
            attr = HIDD_ATTRIBUTES()
            attr.Size = ctypes.sizeof(attr)
            if not hid.HidD_GetAttributes(h, ctypes.byref(attr)):
                continue
            if vid is not None and attr.VendorID != vid:
                continue
            if pid is not None and attr.ProductID != pid:
                continue
            caps = HIDP_CAPS()
            pre = ctypes.c_void_p()
            if hid.HidD_GetPreparsedData(h, ctypes.byref(pre)):
                hid.HidP_GetCaps(pre, ctypes.byref(caps))
                hid.HidD_FreePreparsedData(pre)
            m = re.search(r"mi_([0-9a-f]{2})", path, re.I)
            out.append({
                "path": path, "vendor_id": attr.VendorID, "product_id": attr.ProductID,
                "interface_number": int(m.group(1), 16) if m else None,
                "usage_page": caps.UsagePage, "usage": caps.Usage,
                "input_len": caps.InputReportByteLength, "output_len": caps.OutputReportByteLength,
                "manufacturer": None, "product": None,
            })
            t1 = time.time()
            out[-1]["manufacturer"] = _string(hid.HidD_GetManufacturerString, h)
            t2 = time.time()
            out[-1]["product"] = _string(hid.HidD_GetProductString, h)
            if timing is not None:
                timing.append((path, t2 - t1, "manufacturer string"))
                timing.append((path, time.time() - t2, "product string"))
        finally:
            kernel32.CloseHandle(h)
    return out


def write(path, report):
    """Send one output report; `report` must already include the leading report-id byte."""
    h = _open(path, GENERIC_READ | GENERIC_WRITE)
    if h is None:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        buf = ctypes.create_string_buffer(bytes(report), len(report))
        written = wintypes.DWORD()
        if not kernel32.WriteFile(h, buf, len(report), ctypes.byref(written), None):
            raise ctypes.WinError(ctypes.get_last_error())
        return written.value
    finally:
        kernel32.CloseHandle(h)

# ---------------------------------------------------------------------------
# Persistent overlapped HID device (what libusb's Windows HID backend does underneath)
# ---------------------------------------------------------------------------
FILE_FLAG_OVERLAPPED = 0x40000000
WAIT_OBJECT_0, WAIT_TIMEOUT = 0, 0x102
ERROR_IO_PENDING = 997


class OVERLAPPED(ctypes.Structure):
    _fields_ = [("Internal", ctypes.c_void_p), ("InternalHigh", ctypes.c_void_p),
                ("Offset", wintypes.DWORD), ("OffsetHigh", wintypes.DWORD), ("hEvent", HANDLE)]


kernel32.CreateEventW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR]
kernel32.CreateEventW.restype = HANDLE
kernel32.ReadFile.argtypes = [HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
kernel32.ReadFile.restype = wintypes.BOOL
kernel32.WaitForSingleObject.argtypes = [HANDLE, wintypes.DWORD]
kernel32.WaitForSingleObject.restype = wintypes.DWORD
kernel32.GetOverlappedResult.argtypes = [HANDLE, ctypes.c_void_p, ctypes.POINTER(wintypes.DWORD), wintypes.BOOL]
kernel32.GetOverlappedResult.restype = wintypes.BOOL
kernel32.CancelIoEx.argtypes = [HANDLE, ctypes.c_void_p]
hid.HidD_FlushQueue.argtypes = [HANDLE]
hid.HidD_SetNumInputBuffers.argtypes = [HANDLE, wintypes.ULONG]


class HidPipe:
    """Opens one HID collection read/write with overlapped I/O. Reports include the report-id byte."""

    def __init__(self, path, timeout_ms=15000, flush=True, input_buffers=32):
        h = kernel32.CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, None,
                                 OPEN_EXISTING, FILE_FLAG_OVERLAPPED, None)
        if h == INVALID_HANDLE_VALUE:
            raise ctypes.WinError(ctypes.get_last_error())
        self.h, self.timeout = h, timeout_ms
        if input_buffers:
            hid.HidD_SetNumInputBuffers(h, input_buffers)
        if flush:
            hid.HidD_FlushQueue(h)

    def close(self):
        if self.h:
            kernel32.CloseHandle(self.h)
            self.h = None

    def _io(self, fn, buf, n):
        ev = kernel32.CreateEventW(None, True, False, None)
        ov = OVERLAPPED()
        ov.hEvent = ev
        got = wintypes.DWORD()
        try:
            ok = fn(self.h, buf, n, ctypes.byref(got), ctypes.byref(ov))
            if not ok:
                err = ctypes.get_last_error()
                if err != ERROR_IO_PENDING:
                    raise ctypes.WinError(err)
                if kernel32.WaitForSingleObject(ev, self.timeout) == WAIT_TIMEOUT:
                    kernel32.CancelIoEx(self.h, ctypes.byref(ov))
                    kernel32.GetOverlappedResult(self.h, ctypes.byref(ov), ctypes.byref(got), True)
                    raise TimeoutError("HID I/O timed out after %d ms" % self.timeout)
                if not kernel32.GetOverlappedResult(self.h, ctypes.byref(ov), ctypes.byref(got), False):
                    raise ctypes.WinError(ctypes.get_last_error())
            return got.value
        finally:
            kernel32.CloseHandle(ev)

    def write(self, report):
        buf = ctypes.create_string_buffer(bytes(report), len(report))
        return self._io(kernel32.WriteFile, buf, len(report))

    def read(self, size):
        buf = ctypes.create_string_buffer(size)
        n = self._io(kernel32.ReadFile, buf, size)
        return buf.raw[:n]
