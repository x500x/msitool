#!/usr/bin/env python3
"""Regenerate resources/msitool.ico from resources/msitool.svg.

The icon is pure vector (no fonts, no raster sources). Each ICO entry is
rasterised independently at 4x the target size by headless Chromium/Edge,
downsampled with Lanczos, then packed:

  * <= 128 px : 32-bit BGRA DIB (maximum compatibility with rc.exe / Explorer)
  *    256 px : PNG-in-ICO (Vista+, and what the Windows shell prefers)

For 16/24/32 px a slightly heavier variant of the artwork is used (thicker
strokes, no hairline inner ring / sheen) so the M stays legible on a taskbar.

Usage:
    python tools/make_icon.py
    python tools/make_icon.py --chrome "C:\\...\\msedge.exe"

Intermediate HTML/PNG files live in the OS temp directory; only the .ico is
written into the workspace.
"""
from __future__ import annotations

import argparse
import io
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile

from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
SVG_PATH = ROOT / "resources" / "msitool.svg"
ICO_PATH = ROOT / "resources" / "msitool.ico"
SIZES = (16, 24, 32, 48, 64, 128, 256)
SUPERSAMPLE = 4
SMALL_LIMIT = 32

HTML_TEMPLATE = """<!doctype html><html><head><meta charset="utf-8"><style>
html,body{margin:0;padding:0;background:transparent;overflow:hidden}
svg{position:absolute;top:0;left:0;display:block;width:__PX__px;height:__PX__px}
</style></head><body>__SVG__</body></html>
"""

_LANCZOS = getattr(Image, "Resampling", Image).LANCZOS


def find_chrome(explicit: str | None) -> str:
    candidates = [
        explicit,
        os.environ.get("MSITOOL_CHROME"),
        shutil.which("chrome"),
        shutil.which("msedge"),
        r"C:\Program Files\Google\Chrome\Application\chrome.exe",
        r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
        os.path.expandvars(r"%LOCALAPPDATA%\Google\Chrome\Application\chrome.exe"),
        r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
        r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
    ]
    for c in candidates:
        if c and pathlib.Path(c).is_file():
            return str(c)
    raise SystemExit("Chromium/Edge not found; pass --chrome <path> "
                     "or set MSITOOL_CHROME")


def small_variant(svg: str) -> str:
    """Heavyweight artwork for tiny sizes (keeps the same silhouette)."""
    s = re.sub(r'<circle cx="128" cy="128" r="104"[^>]*/>', "", svg)
    s = re.sub(r'<path d="M4 96[^>]*/>', "", s)
    replacements = {
        'stroke-width="22"': 'stroke-width="28"',
        'stroke-width="14"': 'stroke-width="20"',
        'x="90" y="196" width="76" height="8"':
            'x="86" y="192" width="84" height="14"',
        'stroke-opacity="0.22" stroke-width="2"':
            'stroke-opacity="0.30" stroke-width="4"',
    }
    for old, new in replacements.items():
        if old not in s:
            raise SystemExit(f"small-variant pattern missing from SVG: {old}")
        s = s.replace(old, new)
    return s


def render(svg: str, px: int, chrome: str, tmp: pathlib.Path) -> Image.Image:
    """Render `svg` at exactly px x px with a transparent background."""
    html = tmp / f"render_{px}.html"
    shot = tmp / f"shot_{px}.png"
    html.write_text(
        HTML_TEMPLATE.replace("__PX__", str(px)).replace("__SVG__", svg),
        encoding="utf-8",
    )
    side = max(800, px + 64)
    cmd = [
        chrome,
        "--headless=new",
        "--disable-gpu",
        "--no-first-run",
        "--no-default-browser-check",
        "--hide-scrollbars",
        "--force-device-scale-factor=1",
        "--default-background-color=00000000",
        f"--user-data-dir={tmp / 'profile'}",
        f"--window-size={side},{side}",
        f"--screenshot={shot}",
        html.as_uri(),
    ]
    subprocess.run(cmd, check=True, capture_output=True, timeout=120)
    with Image.open(shot) as im:
        return im.convert("RGBA").crop((0, 0, px, px))


def to_dib(img: Image.Image) -> bytes:
    """32bpp BGRA bottom-up XOR bitmap + all-zero AND mask (alpha governs)."""
    w, h = img.size
    rgba = img.tobytes()
    rows = []
    for y in range(h - 1, -1, -1):
        row = rgba[y * w * 4:(y + 1) * w * 4]
        bgra = bytearray(row)
        bgra[0::4] = row[2::4]   # B <- B
        bgra[2::4] = row[0::4]   # R <- R
        rows.append(bytes(bgra))
    xor = b"".join(rows)
    and_mask = bytes(((w + 31) // 32) * 4 * h)
    header = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0,
                         len(xor) + len(and_mask), 0, 0, 0, 0)
    return header + xor + and_mask


def pack_ico(images: dict[int, Image.Image]) -> bytes:
    blobs: list[bytes] = []
    for size in SIZES:
        img = images[size]
        if size >= 256:
            buf = io.BytesIO()
            img.save(buf, format="PNG", optimize=True)
            blobs.append(buf.getvalue())
        else:
            blobs.append(to_dib(img))

    out = bytearray(struct.pack("<HHH", 0, 1, len(SIZES)))
    offset = 6 + 16 * len(SIZES)
    for size, blob in zip(SIZES, blobs):
        dim = 0 if size >= 256 else size
        out += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32,
                           len(blob), offset)
        offset += len(blob)
    for blob in blobs:
        out += blob
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--svg", type=pathlib.Path, default=SVG_PATH)
    ap.add_argument("--out", type=pathlib.Path, default=ICO_PATH)
    ap.add_argument("--chrome")
    args = ap.parse_args()

    svg = args.svg.read_text(encoding="utf-8")
    small = small_variant(svg)
    chrome = find_chrome(args.chrome)

    tmp = pathlib.Path(tempfile.mkdtemp(prefix="msitool_icon_"))
    try:
        images: dict[int, Image.Image] = {}
        for size in SIZES:
            src = small if size <= SMALL_LIMIT else svg
            big = render(src, size * SUPERSAMPLE, chrome, tmp)
            images[size] = big.resize((size, size), _LANCZOS)
            print(f"  {size:3d}x{size:<3d} rendered "
                  f"({'small' if src is small else 'normal'} variant)")
        ico = pack_ico(images)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(ico)
    print(f"wrote {args.out}  ({len(ico):,} bytes, {len(SIZES)} sizes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
