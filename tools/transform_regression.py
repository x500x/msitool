#!/usr/bin/env python
"""Regression checks for the transform commands (`transform` / `apply` / `mst`).

Runs against two small packages that ship with the workspace's build
directory, generates a transform, applies it, and asserts the round trip is
exact.  Everything is written to ``--out`` (a temp directory by default), so
the repository stays clean.

Usage:
    python tools/transform_regression.py --exe build\\msitool_x64.exe
    python tools/transform_regression.py --base a.msi --updated b.msi
    python tools/transform_regression.py --msp C:\\Windows\\Installer\\x.msp
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

BUILD = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                     "build")

failures = []


def run(exe, args, expect=0):
    p = subprocess.run([exe] + list(args), capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    text = (p.stdout or "") + (p.stderr or "")
    ok = expect is None or p.returncode == expect
    print("%-64s rc=%d%s" % (" ".join(args)[:64], p.returncode,
                             "" if ok else "   <-- FAIL"))
    if not ok:
        failures.append((" ".join(args), p.returncode, text[-500:]))
    return text


def check(condition, label, detail=""):
    print("%-64s %s" % ("check: " + label, "ok" if condition else "FAIL"))
    if not condition:
        failures.append((label, detail))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=os.path.join(BUILD, "msitool_x64.exe"))
    ap.add_argument("--base", default=os.path.join(BUILD, "f_minimal.msi"))
    ap.add_argument("--updated", default=os.path.join(BUILD, "f_basic.msi"))
    ap.add_argument("--msp", default="",
                    help="optional .msp sample that must be rejected as input")
    ap.add_argument("--out", default="")
    args = ap.parse_args()

    out = args.out or tempfile.mkdtemp(prefix="msitool-mst-")
    os.makedirs(out, exist_ok=True)
    mst = os.path.join(out, "diff.mst")
    applied = os.path.join(out, "applied.msi")

    # 1. --dry-run: output path optional, nothing written
    text = run(args.exe, ["transform", args.base, args.updated, "--dry-run"])
    check("Changes" in text and not os.path.exists(mst), "dry run prints the diff")
    check("identical tables" not in text, "dry run reports real changes")

    # 2. generate, with error / validation flags stored in the summary
    text = run(args.exe, ["transform", args.base, args.updated, mst,
                          "--errors", "addexistingrow", "--validate",
                          "product,language", "--force"])
    check(os.path.exists(mst), "transform file written")
    check("Summary   : written" in text, "summary stream written")

    # 3. inspect: flags come back from the summary stream
    text = run(args.exe, ["mst", mst])
    check("addexistingrow" in text, "error conditions decoded")
    check("language+product" in text, "validation flags decoded")

    # 4. change log against the base package (installer's _TransformView)
    text = run(args.exe, ["mst", mst, "--base-msi", args.base])
    check("ProductCode" in text, "change log lists the product code change")
    check("_TransformView" not in text, "change log is not the raw stream list")

    # 5. validation is enforced on apply, and a failed apply leaves no file
    bad = os.path.join(out, "bad.msi")
    text = run(args.exe, ["apply", args.updated, bad, mst,
                          "--validate", "product,language", "--force"], expect=1)
    check("MsiDatabaseApplyTransformW failed" in text, "validation mismatch fails")
    check(not os.path.exists(bad), "failed apply removes the copy")

    # 6. real apply, then the round trip must be exact
    run(args.exe, ["apply", args.base, applied, mst, "--force"])
    text = run(args.exe, ["transform", applied, args.updated, "--dry-run"])
    check("none - the two packages have identical tables" in text,
          "applied package equals the updated package")

    # 7. identical packages: no empty transform
    same = os.path.join(out, "same.msi")
    shutil.copyfile(args.base, same)
    empty = os.path.join(out, "empty.mst")
    text = run(args.exe, ["transform", args.base, same, empty])
    check("identical" in text and not os.path.exists(empty),
          "identical packages produce no file")

    # 8. rejected inputs
    if args.msp and os.path.exists(args.msp):
        text = run(args.exe, ["transform", args.msp, args.updated,
                              os.path.join(out, "x.mst")], expect=1)
        check("not .msp" in text, "patch packages are rejected")
    text = run(args.exe, ["transform", args.base, args.updated,
                          os.path.join(out, "x.mst"), "--errors", "bogus"],
               expect=1)
    check("Unknown error-condition flag" in text, "unknown flag is reported")

    # 9. --no-summary drops the summary stream
    nosum = os.path.join(out, "nosum.mst")
    run(args.exe, ["transform", args.base, args.updated, nosum, "--no-summary",
                   "--force"])
    text = run(args.exe, ["mst", nosum])
    check("no summary stream" in text, "missing summary is reported")

    # 10. the read-only --transform view still sees the transformed rows
    text = run(args.exe, ["list", args.base, "properties", "--transform", mst])
    check("F basic" in text, "transform view reflects the change")

    print()
    if failures:
        print("%d FAILURE(S)" % len(failures))
        for f in failures:
            print(repr(f)[:400])
        return 1
    print("all transform checks passed (files in %s)" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
