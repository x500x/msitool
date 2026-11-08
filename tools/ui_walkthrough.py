#!/usr/bin/env python
"""Automated walkthrough of the built-in msitool installer wizard.

Drives the real Windows Installer dialogs (MsiDialog windows) with BM_CLICK.
Per-user packages are used so no elevation is required.

Modes:
  install  welcome -> license (Next disabled until accepted) -> destination
           (open + cancel BrowseDlg) -> ready -> install -> finish
  repair   silent install, then the maintenance wizard -> Repair -> finish
  remove   silent install, then the maintenance wizard -> Remove -> finish

    python tools/ui_walkthrough.py --msi build\\regression\\peruser.msi
    python tools/ui_walkthrough.py --msi build\\regression\\peruser.msi --mode repair
    python tools/ui_walkthrough.py --msi build\\zhuser.msi --language zh-CN
"""

import argparse
import ctypes
import os
import subprocess
import sys
import time
from ctypes import wintypes as wt

user32 = ctypes.WinDLL("user32", use_last_error=True)
user32.EnumWindows.argtypes = [ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM), wt.LPARAM]
user32.EnumChildWindows.argtypes = [wt.HWND, ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM), wt.LPARAM]
user32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
user32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
user32.GetWindowTextLengthW.argtypes = [wt.HWND]
user32.IsWindowEnabled.argtypes = [wt.HWND]
user32.IsWindowVisible.argtypes = [wt.HWND]
user32.SendMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]

BM_CLICK = 0x00F5

WORDS = {
    "en": {"next": "next", "install": "install", "finish": "finish",
           "cancel": "cancel", "change": "change", "ok": "ok", "new": "new",
           "accept": "accept", "repair": "pair", "remove": "remove"},
    "zh-CN": {"next": "下一步", "install": "安装", "finish": "完成",
              "cancel": "取消", "change": "更改", "ok": "确定", "new": "新建",
              "accept": "接受", "repair": "修复", "remove": "删除"},
}


def text_of(hwnd):
    n = user32.GetWindowTextLengthW(hwnd)
    buf = ctypes.create_unicode_buffer(n + 2)
    user32.GetWindowTextW(hwnd, buf, n + 2)
    return buf.value


def class_of(hwnd):
    buf = ctypes.create_unicode_buffer(128)
    user32.GetClassNameW(hwnd, buf, 128)
    return buf.value


def children(hwnd):
    out = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, _):
        out.append(h)
        return True

    user32.EnumChildWindows(hwnd, cb, 0)
    return out


def msi_dialogs():
    out = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, _):
        if not user32.IsWindowVisible(h):
            return True
        cls = class_of(h)
        if cls.startswith("MsiDialog") or cls == "#32770":
            out.append(h)
        return True

    user32.EnumWindows(cb, 0)
    return out


def controls(dialog):
    return [(h, class_of(h), text_of(h)) for h in children(dialog)]


def find_button(dialog, needle):
    needle = needle.lower().replace("&", "")
    for h, cls, txt in controls(dialog):
        if cls.lower() != "button":
            continue
        if needle in txt.lower().replace("&", ""):
            return h, txt
    return None, None


def wait_dialog(pred=bool, timeout=40.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        for h in msi_dialogs():
            ctrls = controls(h)
            if pred(ctrls):
                return h, ctrls
        time.sleep(0.2)
    return None, None


def click(dialog, needle, what):
    h, txt = find_button(dialog, needle)
    if not h:
        raise RuntimeError("button not found: %s (dialog=%r)" % (what, text_of(dialog)))
    user32.SendMessageW(h, BM_CLICK, 0, 0)
    print("   clicked %-8s -> %r" % (what, txt))
    return h


def has_button(ctrls, needle):
    needle = needle.lower().replace("&", "")
    return any(c.lower() == "button" and needle in t.lower().replace("&", "")
               for _, c, t in ctrls)


def button_enabled(dialog, needle):
    h, _ = find_button(dialog, needle)
    return bool(h) and bool(user32.IsWindowEnabled(h))


def check_license(dialog, accept_word):
    for h, cls, txt in controls(dialog):
        if cls.lower() == "button" and accept_word in txt.lower():
            user32.SendMessageW(h, BM_CLICK, 0, 0)
            print("   checked license box -> %r" % txt)
            return
    raise RuntimeError("license checkbox not found")


def install_dir_of(msi, override):
    if override:
        return override
    local = os.environ.get("LOCALAPPDATA", "")
    if os.path.isdir(local):
        for candidate in os.listdir(local):
            full = os.path.join(local, candidate)
            if os.path.isdir(full) and os.path.exists(os.path.join(full, "bin", "app.exe")):
                return full
    return os.path.join(local, os.path.splitext(os.path.basename(msi))[0])


def silent(args):
    subprocess.run(["msiexec"] + args + ["/qn"], check=True)


def run_wizard(msi, mode, W, seen):
    """Drives one full-UI `msiexec /i` run. Returns after the exit dialog."""
    proc = subprocess.Popen(["msiexec", "/i", msi])
    try:
        # First page: welcome (install) or maintenance welcome (repair/remove).
        dlg, ctrls = wait_dialog(lambda c: has_button(c, W["next"]) and
                                 not has_button(c, W["change"]))
        if not dlg:
            raise RuntimeError("welcome dialog did not appear")
        label = "install" if mode == "install" else "maintenance"
        print("1. %-12s:" % label, text_of(dlg)); seen.append(text_of(dlg))
        click(dlg, W["next"], "Next")

        nxt, nctrls = wait_dialog(
            lambda c: any(W["accept"] in t.lower() for _, _, t in c) or
            has_button(c, W["repair"]) or has_button(c, W["remove"]))
        if not nxt:
            raise RuntimeError("second dialog did not appear")
        if has_button(nctrls, W["repair"]) or has_button(nctrls, W["remove"]):
            # Maintenance type: repair or remove.
            print("2. maintenance      :", text_of(nxt)); seen.append(text_of(nxt))
            word = W["remove"] if mode == "remove" else W["repair"]
            click(nxt, word, mode.title())
            ready, rctrls = wait_dialog(lambda c: has_button(c, W["install"]))
            if not ready:
                raise RuntimeError("ready dialog did not appear after maintenance")
            print("3. ready            :", text_of(ready)); seen.append(text_of(ready))
            click(ready, W["install"], "Install")
        else:
            # License -> accept -> destination folder -> BrowseDlg cancel -> Next
            if not any(W["accept"] in t.lower() for _, _, t in nctrls):
                raise RuntimeError("license dialog did not appear")
            print("2. license          :", text_of(nxt)); seen.append(text_of(nxt))
            if button_enabled(nxt, W["next"]):
                raise RuntimeError("Next should be disabled until the license is accepted")
            check_license(nxt, W["accept"])
            time.sleep(0.3)
            if not button_enabled(nxt, W["next"]):
                raise RuntimeError("Next did not become enabled after accepting the license")
            click(nxt, W["next"], "Next")

            dest, dctrls = wait_dialog(lambda c: has_button(c, W["change"]))
            if not dest:
                raise RuntimeError("destination folder dialog did not appear")
            print("3. destination      :", text_of(dest)); seen.append(text_of(dest))
            click(dest, W["change"], "Change...")
            browse, bctrls = wait_dialog(lambda c: has_button(c, W["ok"]) and
                                         has_button(c, W["new"]))
            if not browse:
                raise RuntimeError("BrowseDlg did not appear")
            print("   browse dialog    :", text_of(browse)); seen.append(text_of(browse))
            click(browse, W["cancel"], "Cancel")
            dest, _ = wait_dialog(lambda c: has_button(c, W["change"]))
            if not dest:
                raise RuntimeError("did not return to the destination folder dialog")
            click(dest, W["next"], "Next")

            ready, rctrls = wait_dialog(lambda c: has_button(c, W["install"]))
            if not ready:
                raise RuntimeError("ready dialog did not appear")
            print("4. ready            :", text_of(ready)); seen.append(text_of(ready))
            click(ready, W["install"], "Install")

        finish, _ = wait_dialog(lambda c: has_button(c, W["finish"]), timeout=120)
        if not finish:
            raise RuntimeError("finish dialog did not appear")
        print("   finish           :", text_of(finish)); seen.append(text_of(finish))
        click(finish, W["finish"], "Finish")

        rc = proc.wait(timeout=60)
        if rc != 0:
            raise RuntimeError("msiexec /i exit code %d" % rc)
    finally:
        if proc.poll() is None:
            proc.terminate()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--msi", required=True)
    ap.add_argument("--install-dir", default=None)
    ap.add_argument("--language", default="en", choices=sorted(WORDS))
    ap.add_argument("--mode", default="install", choices=["install", "repair", "remove"])
    args = ap.parse_args()
    W = WORDS[args.language]

    msi = os.path.abspath(args.msi)
    if not os.path.exists(msi):
        raise SystemExit("MSI not found: " + msi)

    seen = []
    # Clean up any leftover install from an earlier run (1605 = not installed).
    subprocess.run(["msiexec", "/x", msi, "/qn"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    if args.mode != "install":
        print("installing silently first (%s) ..." % args.mode)
        silent(["/i", msi])

    run_wizard(msi, args.mode, W, seen)

    installed = install_dir_of(msi, args.install_dir)
    payload = os.path.join(installed, "bin", "app.exe")
    if args.mode == "remove":
        if os.path.exists(payload):
            raise RuntimeError("payload still present after Remove: " + payload)
        print("payload removed     : OK")
    else:
        if not os.path.exists(payload):
            raise RuntimeError("installed payload not found under " + installed)
        print("payload installed   :", installed)

    print("uninstalling ...")
    # Already gone after a Remove walkthrough (1605) - that is fine.
    subprocess.run(["msiexec", "/x", msi, "/qn"], check=False)
    print("walkthrough OK: mode=%s, %d dialog(s)" % (args.mode, len(seen)))


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print("WALKTHROUGH FAILED:", e, file=sys.stderr)
        sys.exit(1)
