#!/usr/bin/env python
"""DPI smoke test for the msitool GUI (owner-drawn Win32 UI).

What it does
------------
1. Launches build\\msitool_x64.exe with MSITOOL_DPI=<dpi> (a QA hook in
   systemDpi() that forces the scale factor of the whole UI) and, optionally, a
   .msi so the data pages are populated.
2. Verifies the window's DPI awareness context is PER_MONITOR_AWARE_V2.
3. Screenshots the window with PrintWindow into a .bmp.
4. With --transition <newDpi> it then fakes a monitor switch by sending
   WM_DPICHANGED (wParam = new dpi, lParam = suggested rect), waits, and takes a
   second screenshot -- this exercises the real WM_DPICHANGED code path
   (theme rebuild + widget refresh + re-layout).

Usage
-----
    python tools/dpi_probe.py --dpi 96  --out shots/home_100.bmp
    python tools/dpi_probe.py --dpi 144 --msi build\\demo.msi --out shots/info_150.bmp
    python tools/dpi_probe.py --dpi 96 --transition 144 --msi build\\demo.msi \
        --out shots/switch_before.bmp --out2 shots/switch_after.bmp
"""
import argparse
import ctypes
import os
import struct
import subprocess
import sys
import time
from ctypes import wintypes as wt

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

WM_CLOSE = 0x0010
WM_DPICHANGED = 0x02E0
CLASS = "msitool_main_v1"


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG),
        ("biPlanes", wt.WORD), ("biBitCount", wt.WORD),
        ("biCompression", wt.DWORD), ("biSizeImage", wt.DWORD),
        ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG),
        ("biClrUsed", wt.DWORD), ("biClrImportant", wt.DWORD),
    ]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", wt.DWORD * 3)]


user32.FindWindowW.restype = wt.HWND
user32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
user32.GetDC.restype = wt.HDC
user32.GetDC.argtypes = [wt.HWND]
user32.ReleaseDC.argtypes = [wt.HWND, wt.HDC]
user32.GetClientRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.PrintWindow.argtypes = [wt.HWND, wt.HDC, wt.UINT]
user32.WindowFromPoint.argtypes = [ctypes.c_int, ctypes.c_int]
user32.ScreenToClient.argtypes = [wt.HWND, ctypes.POINTER(wt.POINT)]
user32.ClientToScreen.argtypes = [wt.HWND, ctypes.POINTER(wt.POINT)]
user32.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int,
                                ctypes.c_int, ctypes.c_int, wt.UINT]
user32.SendMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.GetWindowDpiAwarenessContext.restype = ctypes.c_void_p
user32.GetWindowDpiAwarenessContext.argtypes = [wt.HWND]
user32.AreDpiAwarenessContextsEqual.restype = wt.BOOL
user32.AreDpiAwarenessContextsEqual.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
gdi32.CreateCompatibleDC.restype = wt.HDC
gdi32.CreateCompatibleDC.argtypes = [wt.HDC]
gdi32.CreateCompatibleBitmap.restype = wt.HBITMAP
gdi32.CreateCompatibleBitmap.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int]
gdi32.SelectObject.restype = wt.HGDIOBJ
gdi32.SelectObject.argtypes = [wt.HDC, wt.HGDIOBJ]
gdi32.GetDIBits.argtypes = [wt.HDC, wt.HBITMAP, wt.UINT, wt.UINT,
                            ctypes.c_void_p, ctypes.POINTER(BITMAPINFO), wt.UINT]
gdi32.DeleteObject.argtypes = [wt.HGDIOBJ]
gdi32.DeleteDC.argtypes = [wt.HDC]

DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = ctypes.c_void_p(-4)

# The probe itself must be per-monitor aware, otherwise every coordinate it
# reads (GetWindowRect / GetClientRect / PrintWindow) is DPI-virtualised: on a
# 125% display a 1550x1075 window would be reported as 1240x860 and the
# screenshots would be cropped.
user32.SetProcessDpiAwarenessContext.argtypes = [ctypes.c_void_p]
user32.SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)


def wait_window(timeout=20.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        hwnd = user32.FindWindowW(CLASS, None)
        if hwnd:
            return hwnd
        time.sleep(0.05)
    raise SystemExit("main window not found")


def client_size(hwnd):
    rc = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(rc))
    return rc.right, rc.bottom


def size_window(hwnd, w, h):
    rc = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rc))
    cw, ch = client_size(hwnd)
    dw = (rc.right - rc.left) - cw
    dh = (rc.bottom - rc.top) - ch
    # SWP_NOZORDER | SWP_NOACTIVATE; ptMaxTrackSize still caps the size, so
    # huge (off-screen) windows are reported when that happens.
    user32.SetWindowPos(hwnd, None, 0, 0, w + dw, h + dh, 0x0002 | 0x0004)


def awareness(hwnd):
    ctx = user32.GetWindowDpiAwarenessContext(hwnd)
    if user32.AreDpiAwarenessContextsEqual(
            ctx, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2):
        return "PER_MONITOR_AWARE_V2"
    return "other (%#x)" % (ctx or 0)


def capture(hwnd, path):
    rc = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rc))
    w, h = rc.right - rc.left, rc.bottom - rc.top
    screen = user32.GetDC(None)
    mem = gdi32.CreateCompatibleDC(screen)
    bmp = gdi32.CreateCompatibleBitmap(screen, w, h)
    old = gdi32.SelectObject(mem, bmp)
    ok = user32.PrintWindow(hwnd, mem, 2)          # PW_RENDERFULLCONTENT
    bi = BITMAPINFO()
    bi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.bmiHeader.biWidth = w
    bi.bmiHeader.biHeight = h                      # bottom-up
    bi.bmiHeader.biPlanes = 1
    bi.bmiHeader.biBitCount = 32
    bi.bmiHeader.biCompression = 0                 # BI_RGB
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bi), 0)
    data = buf.raw
    # PrintWindow may return a blank DC for windows that paint only on
    # WM_PAINT; report it instead of silently writing an empty screenshot.
    uniq = len({data[i:i + 4] for i in range(0, len(data), 4 * 997)})
    gdi32.SelectObject(mem, old)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mem)
    user32.ReleaseDC(None, screen)
    hdr = struct.pack("<2sIHHI", b"BM", 14 + 40 + len(data), 0, 0, 14 + 40)
    info = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 32, 0, len(data), 0, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(hdr + info + data)
    return ok, uniq


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wt.LONG), ("dy", wt.LONG), ("mouseData", wt.DWORD),
                ("dwFlags", wt.DWORD), ("time", wt.DWORD),
                ("dwExtraInfo", ctypes.POINTER(wt.ULONG))]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wt.DWORD), ("mi", MOUSEINPUT)]


def click_client(hwnd, x, y):
    """Click at a client coordinate of the main window.

    Uses real input (SetCursorPos + SendInput) rather than posted messages:
    the owner-drawn widgets only fire onClick() while they see the pointer
    hovering them, and a posted WM_MOUSEMOVE does not move the actual cursor.
    The window is brought to the foreground first, so the click lands in the
    app rather than wherever the cursor happened to be.
    """
    pt = wt.POINT(x, y)
    user32.ClientToScreen(hwnd, ctypes.byref(pt))
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.3)
    user32.SetCursorPos(pt.x, pt.y)
    time.sleep(0.2)
    down = INPUT(0, MOUSEINPUT(0, 0, 0, 0x0002, 0, None))   # LEFTDOWN
    up = INPUT(0, MOUSEINPUT(0, 0, 0, 0x0004, 0, None))     # LEFTUP
    user32.SendInput(1, ctypes.byref(down), ctypes.sizeof(INPUT))
    time.sleep(0.05)
    user32.SendInput(1, ctypes.byref(up), ctypes.sizeof(INPUT))
    print("click           = screen %d,%d (real input)" % (pt.x, pt.y))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dpi", type=int, default=96)
    ap.add_argument("--transition", default="",
                    help="comma separated DPI list to fake monitor switches with "
                         "(each one sends WM_DPICHANGED)")
    ap.add_argument("--msi", default="")
    ap.add_argument("--logical", default="1160x860",
                    help="client size in 96-DPI logical pixels")
    ap.add_argument("--click", default="",
                    help="click at a 96-DPI logical client position, e.g. 100,178")
    ap.add_argument("--out", required=True)
    ap.add_argument("--out2", default="")
    ap.add_argument("--exe", default=os.path.join("build", "msitool_x64.exe"))
    args = ap.parse_args()

    lw, lh = (int(v) for v in args.logical.lower().split("x"))
    env = dict(os.environ, MSITOOL_DPI=str(args.dpi))
    cmd = [args.exe] + ([args.msi] if args.msi else [])
    proc = subprocess.Popen(cmd, env=env)
    try:
        hwnd = wait_window()
        print("hwnd            = 0x%X" % hwnd)
        print("dpi awareness   = %s" % awareness(hwnd))
        # Let onCreate() finish (it may load the remembered package) before
        # resizing: a WM_SIZE sent into a half-built UI only gets dropped by
        # the layout() guard.
        time.sleep(1.5)
        size_window(hwnd, lw * args.dpi // 96, lh * args.dpi // 96)
        time.sleep(1.2)
        if args.click:
            cx, cy = (int(v) for v in args.click.split(","))
            click_client(hwnd, cx * args.dpi // 96, cy * args.dpi // 96)
            time.sleep(0.8)
        cw, ch = client_size(hwnd)
        wr = wt.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(wr))
        print("client size     = %dx%d (logical %.0fx%.0f @ %d%%)" % (
            cw, ch, cw * 96 / args.dpi, ch * 96 / args.dpi, args.dpi * 100 // 96))
        print("window rect     = %d,%d %dx%d" % (
            wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top))
        ok, uniq = capture(hwnd, args.out)
        print("shot 1          -> %s (PrintWindow=%d, distinct samples=%d)" %
              (args.out, ok, uniq))

        dpi = args.dpi
        for step, d2 in enumerate(
                [int(v) for v in args.transition.split(",") if v.strip()], 2):
            cw, ch = client_size(hwnd)
            sug = wt.RECT(0, 0, cw * d2 // dpi, ch * d2 // dpi)
            user32.SendMessageW(hwnd, WM_DPICHANGED,
                                (d2 & 0xFFFF) | (d2 << 16),
                                ctypes.addressof(sug))
            dpi = d2
            time.sleep(0.8)
            out = args.out2 if (step == 2 and args.out2) else                 args.out.replace(".bmp", "_step%d.bmp" % step)
            ok2, uniq2 = capture(hwnd, out)
            print("shot %-11d-> %s (PrintWindow=%d, distinct samples=%d)" %
                  (step, out, ok2, uniq2))
            print("client after    = %dx%d (dpi %d)" % (client_size(hwnd) + (d2,)))
    finally:
        hwnd = user32.FindWindowW(CLASS, None)
        if hwnd:
            user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    main()
