#!/usr/bin/env python
r"""Regression checks for `msitool effects`.

    python tools/effects_regression.py
    python tools/effects_regression.py --exe build\msitool_x86.exe --msi setup.msi

What it verifies (all outputs go to a temporary directory):

* --format all / scripts / manifest write the right set of files;
* registry.reg is UTF-16 LE with BOM and contains the expected value types,
  the .cmd scripts contain sc / setx commands, environment.ps1 and
  shortcuts.ps1 are real PowerShell scripts and the static formatter resolved
  [INSTALLFOLDER] / [#file] placeholders, pack.xml is well-formed XML in the
  pack namespace with <properties> / <servicecontrols> sections;
* the generated pack.xml really is a lossless round-trip manifest: packing a
  tiny payload with it reproduces Registry / ServiceInstall / ServiceControl /
  Environment / Shortcut / Property / CustomAction rows (including a
  non-default ServiceControl Event/Arguments/Wait) and passes MsiVerifyPackage;
* custom actions survive the round trip (t_ca.msi / t_cabin.msi when present);
* a second export refuses to overwrite, --force allows it;
* --strict rejects unresolved MSI formatted text without leaving a directory,
  while the default mode comments the affected commands out;
* an invalid --format is rejected with a non-zero exit code.

The tool itself never installs anything; neither does this script.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NS = "{https://msitool.dev/schema/pack/v1}"

failures = []
checks = 0


def check(condition, message):
    global checks
    checks += 1
    if not condition:
        failures.append(message)
        print("FAIL  " + message)
    return condition


def run(exe, *args):
    return subprocess.run([os.path.abspath(exe)] + list(args),
                          cwd=ROOT, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT)


def table(exe, msi, name):
    r = run(exe, "list", msi, name, "--tsv")
    if r.returncode != 0:
        return []
    lines = r.stdout.decode("utf-8", "replace").splitlines()
    # `list` also prints a "N row(s) in Table." footer; TSV rows all have tabs.
    return [line.split("\t") for line in lines if "\t" in line]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=os.path.join(ROOT, "build", "msitool_x64.exe"))
    ap.add_argument("--msi", default=os.path.join(ROOT, "build", "f_manifest.msi"))
    ap.add_argument("--work", default=None, help="keep outputs in this directory")
    ap.add_argument("--keep", action="store_true", help="do not delete the work dir")
    args = ap.parse_args()

    exe = args.exe if os.path.isabs(args.exe) else os.path.join(ROOT, args.exe)
    msi = args.msi if os.path.isabs(args.msi) else os.path.join(ROOT, args.msi)
    if not os.path.exists(exe) or not os.path.exists(msi):
        print("missing exe or msi: %s / %s" % (exe, msi))
        return 2
    work = args.work or tempfile.mkdtemp(prefix="msitool_effects_")
    os.makedirs(work, exist_ok=True)
    print("exe : %s\nmsi : %s\nwork: %s\n" % (exe, msi, work))

    scripts = ["registry.reg", "registry-uninstall.reg", "services-install.cmd",
               "services-uninstall.cmd", "environment.cmd", "environment.ps1",
               "shortcuts.ps1", "custom-actions.txt", "properties.txt",
               "tables.txt", "effects.md"]
    manifest = "pack.xml"

    # ---- 1. three formats ------------------------------------------------
    for fmt in ("all", "scripts", "manifest"):
        out = os.path.join(work, "fmt_" + fmt)
        r = run(exe, "effects", msi, out, "--format", fmt)
        check(r.returncode == 0, "%s: effects exits 0 (%s)" %
              (fmt, r.stdout.decode("utf-8", "replace")[-200:]))
        files = set(os.listdir(out)) if os.path.isdir(out) else set()
        check("effects.md" in files, "%s: effects.md is always written" % fmt)
        if fmt in ("all", "scripts"):
            for f in scripts:
                check(f in files, "%s: %s is written" % (fmt, f))
        else:
            for f in scripts[:-1]:
                check(f not in files, "manifest: %s is not written" % f)
        check((manifest in files) == (fmt in ("all", "manifest")),
              "%s: pack.xml presence matches --format" % fmt)

    allout = os.path.join(work, "fmt_all")
    reg = open(os.path.join(allout, "registry.reg"), "rb").read()
    check(reg[:2] == b"\xff\xfe", "registry.reg is UTF-16 LE with BOM")
    text16 = reg[2:].decode("utf-16-le", "replace")
    check("Windows Registry Editor Version 5.00" in text16,
          "registry.reg has the regedit header")
    check("dword:00000001" in text16, "registry.reg maps #1 to dword")
    check("[HKEY_CURRENT_USER\\Software\\Acme\\MyApp]" in text16,
          "registry.reg maps Root=1 to HKCU")
    svc = open(os.path.join(allout, "services-install.cmd"), "rb").read().decode("utf-8")
    check("sc create" in svc and "sc description" in svc,
          "services-install.cmd contains sc create / description")
    check("chcp 65001" in svc, "services-install.cmd switches to UTF-8")
    env = open(os.path.join(allout, "environment.cmd"), "rb").read().decode("utf-8")
    check("setx" in env and "%PATH%" in env,
          "environment.cmd contains an append via setx")
    envps = open(os.path.join(allout, "environment.ps1"), "rb").read()
    check(envps[:3] == b"\xef\xbb\xbf", "environment.ps1 is UTF-8 with BOM")
    ps_text = envps.decode("utf-8-sig")
    check("SetEnvironmentVariable" in ps_text and "GetEnvironmentVariable" in ps_text,
          "environment.ps1 reads/writes the persisted variable")
    check("$env:ProgramFiles" in ps_text,
          "static formatter resolves [INSTALLFOLDER] in PowerShell")
    check("WM_SETTINGCHANGE" in ps_text,
          "environment.ps1 broadcasts WM_SETTINGCHANGE")
    check("%ProgramFiles%" in svc,
          "static formatter resolves the service image path in .cmd")
    ps = open(os.path.join(allout, "shortcuts.ps1"), "rb").read()
    check(ps[:3] == b"\xef\xbb\xbf", "shortcuts.ps1 is UTF-8 with BOM")
    check(b"CreateShortcut" in ps, "shortcuts.ps1 creates .lnk files")
    check(os.path.exists(os.path.join(allout, "icons", "StartMenuIcon.ico")),
          "Icon streams are exported")

    # ---- 2. pack.xml is valid and round-trips ----------------------------
    xml_path = os.path.join(allout, manifest)
    raw = open(xml_path, "rb").read()
    check(raw[:3] == b"\xef\xbb\xbf", "pack.xml is UTF-8 with BOM")
    try:
        root = ET.fromstring(raw.decode("utf-8-sig"))
        check(root.tag == NS + "msitool", "pack.xml root is <msitool> in the pack namespace")
        check(root.find(NS + "registry") is not None, "pack.xml has <registry>")
        check(root.find(NS + "services") is not None, "pack.xml has <services>")
        check(root.find(NS + "environment") is not None, "pack.xml has <environment>")
        check(root.find(NS + "shortcuts") is not None, "pack.xml has <shortcuts>")
        check(root.find(NS + "properties") is not None, "pack.xml has <properties>")
        scs = root.find(NS + "servicecontrols")
        check(scs is not None, "pack.xml has <servicecontrols>")
        if scs is not None:
            sc = scs.find(NS + "servicecontrol")
            check(sc is not None and sc.get("event") == "163" and
                  sc.get("wait") == "1",
                  "pack.xml preserves ServiceControl event/wait")
    except ET.ParseError as e:
        check(False, "pack.xml is well-formed XML (%s)" % e)

    payload = os.path.join(work, "payload")
    os.makedirs(os.path.join(payload, "bin"), exist_ok=True)
    with open(os.path.join(payload, "bin", "app.exe"), "wb") as f:
        f.write(b"stub executable\n")
    rebuilt = os.path.join(work, "roundtrip.msi")
    r = run(exe, "pack", payload, rebuilt, "--config", xml_path, "--force")
    log = r.stdout.decode("utf-8", "replace")
    check(r.returncode == 0 and "MsiVerifyPackage: OK" in log,
          "pack --config <generated pack.xml> succeeds (%s)" % log[-300:])

    expected = {"Registry": 4, "ServiceInstall": 1, "ServiceControl": 1,
                "Environment": 1, "Shortcut": 2}
    for name, count in expected.items():
        rows = table(exe, rebuilt, name)
        check(len(rows) == count + 1,
              "round-trip %s has %d row(s) (got %d)" %
              (name, count, max(0, len(rows) - 1)))
    rows = table(exe, rebuilt, "Environment")
    check(any("[~];[INSTALLFOLDER]bin" in " ".join(row) for row in rows),
          "round-trip Environment keeps the MSI formatted value")
    rows = table(exe, rebuilt, "ServiceControl")
    check(any("163" in row for row in rows),
          "round-trip ServiceControl has the standard event bits")
    props = table(exe, rebuilt, "Property")
    check(any(len(row) > 1 and row[0] == "WIXUI_INSTALLDIR" for row in props[1:]),
          "round-trip preserves custom Property rows")

    # ---- 3. custom action round trips ------------------------------------
    for name in ("t_ca.msi", "t_cabin.msi", "merge_ref.msi"):
        src = os.path.join(ROOT, "build", name)
        if not os.path.exists(src):
            continue
        out = os.path.join(work, "ca_" + name.replace(".", "_"))
        r = run(exe, "effects", src, out, "--format", "manifest")
        check(r.returncode == 0, "%s: effects --format manifest exits 0" % name)
        cfg = os.path.join(out, manifest)
        dest = os.path.join(work, "ca_" + name.replace(".", "_") + ".msi")
        r = run(exe, "pack", payload, dest, "--config", cfg, "--force")
        check(r.returncode == 0, "%s: pack --config round trip succeeds (%s)" %
              (name, r.stdout.decode("utf-8", "replace")[-200:]))
        src_rows = table(exe, src, "CustomAction")
        dst_rows = table(exe, dest, "CustomAction")
        check(len(src_rows) == len(dst_rows) and len(src_rows) > 1,
              "%s: CustomAction row count round-trips (%d -> %d)" %
              (name, len(src_rows) - 1, len(dst_rows) - 1))
    cabin = os.path.join(work, "ca_t_cabin_msi")
    check(os.path.isdir(os.path.join(cabin, "binaries")),
          "Binary-backed custom action payload is exported")

    # ---- 4. lossless <servicecontrols> / <properties> round trip ---------
    lossless_xml = os.path.join(work, "lossless.xml")
    with open(lossless_xml, "w", encoding="utf-8") as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n'
                '<msitool xmlns="https://msitool.dev/schema/pack/v1" minVersion="1">\n'
                '  <pack name="Lossless" version="1.0"/>\n'
                '  <properties><property name="CUSTOM_FLAG" value="on"/></properties>\n'
                '  <services><service id="Svc" name="SvcX" control="none"/></services>\n'
                '  <servicecontrols><servicecontrol id="SvcCtl" name="SvcX" '
                'event="170" arguments="--x" wait="0"/></servicecontrols>\n'
                '</msitool>\n')
    src_loss = os.path.join(work, "lossless_src.msi")
    r = run(exe, "pack", payload, src_loss, "--config", lossless_xml, "--force")
    check(r.returncode == 0, "lossless: source package builds")
    out_loss = os.path.join(work, "lossless_effects")
    r = run(exe, "effects", src_loss, out_loss, "--format", "manifest")
    check(r.returncode == 0, "lossless: effects --format manifest exits 0")
    cfg_loss = os.path.join(out_loss, manifest)
    dest_loss = os.path.join(work, "lossless_dest.msi")
    r = run(exe, "pack", payload, dest_loss, "--config", cfg_loss, "--force")
    check(r.returncode == 0 and "MsiVerifyPackage: OK" in
          r.stdout.decode("utf-8", "replace"), "lossless: round-trip pack succeeds")
    ctl = table(exe, dest_loss, "ServiceControl")
    check(any(len(row) > 4 and "170" in row and "--x" in row and "0" in row
              for row in ctl[1:]),
          "lossless: non-default Event/Arguments/Wait survive the round trip")
    props = table(exe, dest_loss, "Property")
    check(any(len(row) > 1 and row[0] == "CUSTOM_FLAG" and row[1] == "on"
              for row in props[1:]),
          "lossless: custom Property rows survive the round trip")

    # ---- 5. overwrite protection ------------------------------------------
    ov = os.path.join(work, "overwrite")
    check(run(exe, "effects", msi, ov).returncode == 0,
          "overwrite: first export succeeds")
    check(run(exe, "effects", msi, ov).returncode != 0,
          "overwrite: second export is refused")
    check(run(exe, "effects", msi, ov, "--force").returncode == 0,
          "overwrite: --force allows the second export")

    # ---- 6. --strict / unresolved formatted text --------------------------
    unres_xml = os.path.join(work, "unresolved.xml")
    with open(unres_xml, "w", encoding="utf-8") as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n'
                '<msitool xmlns="https://msitool.dev/schema/pack/v1" minVersion="1">\n'
                '  <pack name="Unresolved" version="1.0"/>\n'
                '  <registry><registryValue id="R1" root="HKCU" key="Software\\Demo" '
                'name="Who" value="[TOTALLY_UNKNOWN_PROP]" type="string"/></registry>\n'
                '  <environment><variable id="E1" name="DEMO_PATH" '
                'value="[ANOTHER_UNKNOWN]bin" action="set" system="no"/></environment>\n'
                '</msitool>\n')
    src_un = os.path.join(work, "unresolved.msi")
    check(run(exe, "pack", payload, src_un, "--config", unres_xml, "--force")
          .returncode == 0, "strict: source package builds")
    out_un = os.path.join(work, "unresolved_effects")
    check(run(exe, "effects", src_un, out_un, "--strict").returncode != 0,
          "strict: unresolved formatted text fails the export")
    check(not os.path.exists(out_un),
          "strict: failure leaves no output directory behind")
    check(run(exe, "effects", src_un, out_un).returncode == 0,
          "strict: default mode succeeds with unresolved text")
    reg_un = open(os.path.join(out_un, "registry.reg"), "rb").read()[2:].decode("utf-16-le")
    check("TODO(unresolved" in reg_un,
          "strict: unresolved registry value is commented out")
    env_un = open(os.path.join(out_un, "environment.ps1"), "rb").read().decode("utf-8-sig")
    check("TODO(unresolved" in env_un,
          "strict: unresolved environment command is commented out")

    # ---- 7. invalid format ------------------------------------------------
    r = run(exe, "effects", msi, os.path.join(work, "bad"), "--format", "bogus")
    check(r.returncode != 0, "invalid --format is rejected")

    print("\n%d check(s), %d failure(s)" % (checks, len(failures)))
    if failures:
        for f in failures:
            print("  - " + f)
    if not args.keep and not args.work:
        shutil.rmtree(work, ignore_errors=True)
    else:
        print("work kept: " + work)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
