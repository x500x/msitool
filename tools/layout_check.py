#!/usr/bin/env python
"""Geometry check for the owner-drawn UI (no screenshots needed).

The app can dump the computed layout of the custom action editor:

    set MSITOOL_LAYOUT_DUMP=%TEMP%\\ca
    build\\msitool_x64.exe

which writes ``ca_<dpi>_<w>x<h>.{props,seq}.txt`` for a matrix of logical
window sizes and DPI scales. Each file lists the rects of the visible child
widgets (``W``), the drawn cards (``C``) and the text labels (``L``).

This script re-reads those dumps and fails when

* two controls overlap,
* a control overlaps a text label,
* a label overlaps another label,
* a control or label sticks out of the page card it belongs to.

Those are precisely the symptoms of a cramped dialog: on a short or high-DPI
window the old single-column editor pushed the property rows and the action
buttons over the sequence timeline label.

Usage:
    python tools/layout_check.py --dir %TEMP%\\ca
    python tools/layout_check.py --dir %TEMP%\\ca -v
"""

import argparse
import glob
import os
import sys


def decode(raw):
    if raw[:2] in (b"\xff\xfe", b"\xfe\xff"):
        return raw.decode("utf-16")
    if raw[1:2] == b"\x00":
        return raw.decode("utf-16-le", "replace")
    return raw.decode("utf-8", "replace")


def parse(path):
    """Return (client_rect, dpi, widgets, cards, labels) from one dump."""
    with open(path, "rb") as f:
        text = decode(f.read())
    dpi = 96
    client = (0, 0, 0, 0)
    widgets, cards, labels = [], [], []
    for line in text.splitlines():
        line = line.rstrip("\r")
        if not line:
            continue
        if line.startswith("#"):
            parts = line[1:].split()
            for i, v in enumerate(parts):
                if v == "client" and i + 2 < len(parts):
                    client = (0, 0, int(parts[i + 1]), int(parts[i + 2]))
                elif v == "dpi" and i + 1 < len(parts):
                    dpi = int(parts[i + 1])
            continue
        kind, rest = line[0], line[2:]
        if kind not in ("W", "C", "L"):
            continue
        nums = rest.split(None, 4)
        if len(nums) < 4:
            continue
        try:
            rect = tuple(int(v) for v in nums[:4])
        except ValueError:
            continue
        label = nums[4] if len(nums) > 4 else ""
        {"W": widgets, "C": cards, "L": labels}[kind].append((rect, label))
    return client, dpi, widgets, cards, labels


def area(a, b):
    w = min(a[2], b[2]) - max(a[0], b[0])
    h = min(a[3], b[3]) - max(a[1], b[1])
    return w * h if w > 0 and h > 0 else 0


def inside(inner, outer, tol=0):
    return (inner[0] >= outer[0] - tol and inner[1] >= outer[1] - tol and
            inner[2] <= outer[2] + tol and inner[3] <= outer[3] + tol)


def describe(kind, rect, text=""):
    tag = {"W": "control", "L": "label", "C": "card"}[kind]
    return "%s [%d,%d %dx%d]%s" % (tag, rect[0], rect[1], rect[2] - rect[0],
                                   rect[3] - rect[1],
                                   (' "%s"' % text) if text else "")


def check(path, verbose):
    client, dpi, widgets, cards, labels = parse(path)
    problems = []

    # The form card is the wide *and* tall card; the tab strip is wide but
    # short. Content that starts below the form card top has to stay inside
    # it, which is what catches a property sheet overflowing its pane.
    page = client
    for rect, _ in cards:
        if ((rect[2] - rect[0]) > client[2] * 0.5 and
                (rect[3] - rect[1]) > client[3] * 0.4):
            page = rect
            break

    for kind, items in (("W", widgets), ("L", labels)):
        for rect, text in items:
            if not inside(rect, client):
                problems.append("%s outside the window" % describe(kind, rect, text))
            # Chrome outside the form card (caption, tab strip, navigation
            # rail, status-bar progress) is laid out against the window.
            elif (rect[1] >= page[1] - 2 and rect[1] < page[3] and
                  rect[0] >= page[0] - 2 and rect[2] <= page[2] + 2 and
                  not inside(rect, page)):
                problems.append("%s outside the page card" % describe(kind, rect, text))

    pairs = [(r, "W", "") for r, _ in widgets] + [(r, "L", t) for r, t in labels]
    for i in range(len(pairs)):
        for j in range(i + 1, len(pairs)):
            ra, ka, ta = pairs[i]
            rb, kb, tb = pairs[j]
            ov = area(ra, rb)
            if ov > 6:      # a few px of label/control overlap is rounding noise
                problems.append("%s / %s overlap by %d px" %
                                (describe(ka, ra, ta), describe(kb, rb, tb), ov))

    title = os.path.basename(path)
    if problems:
        print("%-30s dpi %d FAIL (%d)" % (title, dpi, len(problems)))
        for p in problems:
            print("    " + p)
    elif verbose:
        print("%-30s dpi %d ok  (%d controls, %d labels)" %
              (title, dpi, len(widgets), len(labels)))
    return len(problems)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True,
                    help="directory with the MSITOOL_LAYOUT_DUMP files")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    # Any dump written by MSITOOL_LAYOUT_DUMP: ca_* (custom action editor)
    # and <page>_* for the form pages (MSITOOL_LAYOUT_PAGE=<page>).
    files = sorted(glob.glob(os.path.join(args.dir, "*.txt")))
    if not files:
        print("no dump files in " + args.dir)
        return 2
    total = 0
    for f in files:
        total += check(f, args.verbose)
    print("\n%d file(s), %d problem(s)" % (len(files), total))
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
