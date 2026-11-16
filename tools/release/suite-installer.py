#!/usr/bin/env python3
"""Gridline Suite and Suite Mini: the pick-your-plugins installers, one file per system.

    suite-installer.py --suite gridline|mini --family linux|bsd|macos|windows --version V
                       --catalogue members.json --input [LABEL=]DIR [--input ...] --out DIR
                       [--license LICENSE.txt]
    suite-installer.py --self-test
    suite-installer.py --print-names V        every installer file name of a release, one per line
    suite-installer.py --real-test SUITE PLUGIN_DIR GRIDLINE_CLI   (macOS: this build's bundles)

Every plugin of a suite is one engine binary; a member list in the bundle (gridline-members.txt)
says which plugins hosts see. So an installer carries the engine once per format and writes, for
each format picked, one bundle named like the suite's ("Gridline Suite.vst3") listing the picked
plugins. Suite and Mini go through the same code; only the catalogue and the suite id differ.

  linux, bsd  Gridline-Suite[-Mini]-<V>-<label>-setup.run, one per processor or system (LABEL is
              "linux-x86_64", "freebsd-x86_64", ...): a shell script with the payload appended. It
              unpacks to a temporary folder and runs suite-setup/setup.sh (text picker, scripted
              options, receipt, uninstall; see that file).
  macos       Gridline-Suite[-Mini]-<V>-macos-universal.pkg (productbuild): a choice per format
              and per plugin (Instruments and Effects, by category), for everyone or for this user
              only. The format packages install the engine bundles; each plugin choice is a tiny
              package that records its pick; a last package writes the member lists, keeps only
              the picked Audio Unit components, signs the changed bundles ad hoc and leaves an
              uninstall script in Application Support/Gridline/<suite name>.
  windows     Gridline-Suite[-Mini]-<V>-windows.iss, compiled in CI by Inno Setup into
              Gridline-Suite[-Mini]-<V>-windows-setup.exe: one setup for x64 and ARM64 (the
              processor's CLAP is installed), components per format and per plugin, per-user or
              all-users install, its own entry in installed apps. --input labels: x64, arm64.

Public text only: no task numbers, no other products.
"""
import argparse
import hashlib
import io
import json
import os
import plistlib
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
SUITES = {
    "gridline": {"name": "Gridline Suite", "asset": "Gridline-Suite"},
    "mini": {"name": "Gridline Suite Mini", "asset": "Gridline-Suite-Mini"},
}
OWNER = "suite-installer"


def installer_name(suite, version, family, label=""):
    a = SUITES[suite]["asset"]
    if family == "macos":
        return "%s-%s-macos-universal.pkg" % (a, version)
    if family == "windows":
        return "%s-%s-windows.iss" % (a, version)
    return "%s-%s-%s-setup.run" % (a, version, label)


def _fixed_mtime():
    try:
        return int(os.environ.get("SOURCE_DATE_EPOCH", ""))
    except ValueError:
        return 946684800


def catalogue_tsv(catalogue):
    clean = lambda s: str(s).replace("\t", " ").replace("\n", " ")
    return "".join("%s\t%s\t%s\t%s\n" % (clean(m["type"]), clean(m["name"]), clean(m.get("kind", "effect")), clean(m.get("category", "")))
                   for m in catalogue)


def _formats(folder, sname):
    out = {}
    for ext in ("vst3", "clap", "component"):
        if os.path.exists(os.path.join(folder, sname + "." + ext)):
            out[ext] = os.path.join(folder, sname + "." + ext)
    return out


# ------------------------------------------------------------ linux, bsd ----

RUN_HEADER = """#!/bin/sh
# %(title)s installer. Run it with: sh %(file)s   (options: sh %(file)s --help)
# It unpacks into a temporary folder, lets you pick plugins and formats, and installs them.
T=$(mktemp -d "${TMPDIR:-/tmp}/gridline-suite.XXXXXX") || exit 1
trap 'rm -rf "$T"' EXIT INT TERM
L=$(awk '/^__GRIDLINE_PAYLOAD__$/ { print NR + 1; exit }' "$0")
tail -n +"$L" "$0" | gzip -dc | (cd "$T" && tar -xf -) || { echo "Error: this installer file is damaged; download it again." >&2; exit 1; }
sh "$T/setup.sh" "$@"
exit $?
__GRIDLINE_PAYLOAD__
"""


def _tar_add(tar, path, arcname, mtime):
    st = os.lstat(path)
    ti = tarfile.TarInfo(arcname)
    ti.mtime = mtime
    ti.uid = ti.gid = 0
    ti.uname = ti.gname = ""
    if stat.S_ISDIR(st.st_mode):
        ti.type = tarfile.DIRTYPE
        ti.mode = 0o755
        tar.addfile(ti)
        for name in sorted(os.listdir(path)):
            _tar_add(tar, os.path.join(path, name), arcname + "/" + name, mtime)
    elif stat.S_ISLNK(st.st_mode):
        ti.type = tarfile.SYMTYPE
        ti.linkname = os.readlink(path)
        ti.mode = 0o777
        tar.addfile(ti)
    else:
        ti.size = st.st_size
        ti.mode = 0o755 if st.st_mode & 0o111 else 0o644
        with open(path, "rb") as f:
            tar.addfile(ti, f)


def build_run(suite, version, label, catalogue, folder, out_dir, license_text=None):
    """One .run for one processor (linux) or system (bsd)."""
    sname = SUITES[suite]["name"]
    fmts = _formats(folder, sname)
    if "vst3" not in fmts and "clap" not in fmts:
        raise SystemExit("no %s bundles in %s" % (sname, folder))
    name = installer_name(suite, version, "linux", label)
    mtime = _fixed_mtime()
    with tempfile.TemporaryDirectory() as tmp:
        root = os.path.join(tmp, "p")
        os.makedirs(os.path.join(root, "engine"))
        shutil.copy(os.path.join(HERE, "suite-setup", "setup.sh"), os.path.join(root, "setup.sh"))
        os.chmod(os.path.join(root, "setup.sh"), 0o755)
        with open(os.path.join(root, "suite.txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write("%s\t%s\t%s\n" % (suite, sname, version))
        with open(os.path.join(root, "catalogue.tsv"), "w", encoding="utf-8", newline="\n") as f:
            f.write(catalogue_tsv(catalogue))
        if license_text is not None:
            with open(os.path.join(root, "LICENSE.txt"), "w", encoding="utf-8", newline="\n") as f:
                f.write(license_text)
        if "vst3" in fmts:
            shutil.copytree(fmts["vst3"], os.path.join(root, "engine", sname + ".vst3"), symlinks=True)
        if "clap" in fmts:
            shutil.copy2(fmts["clap"], os.path.join(root, "engine", sname + ".clap"))
            assets = os.path.join(folder, sname + "-assets")
            if os.path.isdir(assets):
                shutil.copytree(assets, os.path.join(root, "engine", sname + "-assets"), symlinks=True)
        # The plugins read only part of the app's assets (package-plugins.py decides what).
        pp = _load_package_plugins()
        types = [m["type"] for m in catalogue]
        pp.trim_assets(os.path.join(root, "engine", sname + ".vst3", "Contents", "Resources", "assets"), types)
        pp.trim_assets(os.path.join(root, "engine", sname + "-assets"), types)
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as tar:
            for entry in sorted(os.listdir(root)):
                _tar_add(tar, os.path.join(root, entry), entry, mtime)
        import gzip
        gz = io.BytesIO()
        with gzip.GzipFile(fileobj=gz, mode="wb", compresslevel=9, mtime=mtime) as g:
            g.write(buf.getvalue())
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, name)
    with open(out, "wb") as f:
        f.write((RUN_HEADER % {"title": "%s %s" % (sname, version), "file": name}).encode("utf-8"))
        f.write(gz.getvalue())
    os.chmod(out, 0o755)
    return name


def _load_package_plugins():
    import importlib.util
    spec = importlib.util.spec_from_file_location("package_plugins", os.path.join(HERE, "package-plugins.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# ----------------------------------------------------------------- macos ----

# Run by the last package, after the format packages and the plugin choices. $2 is the install
# location (/Library/Audio/Plug-Ins, or ~/Library/Audio/Plug-Ins for "this user only").
MAC_FINISH = r"""#!/bin/sh
# %(sname)s: writes the picked plugins into the installed bundles (see suite-installer.py).
DEST="${2:-/Library/Audio/Plug-Ins}"
PICKS="${GRIDLINE_SUITE_PICKS:-/tmp/%(pkgid)s.picks}"
CODES="$(dirname "$0")/au-codes.tsv"
[ -f "$PICKS" ] || { echo "no plugins picked"; exit 0; }
TYPES=$(sort -u "$PICKS")
list() { printf '# Gridline plugin bundle: the plugins it shows to hosts\nsuite %(suite)s\n%%s\n' "$TYPES"; }
for b in "$DEST/VST3/%(sname)s.vst3" "$DEST/CLAP/%(sname)s.clap" "$DEST/Components/%(sname)s.component"; do
  [ -d "$b" ] || continue
  mkdir -p "$b/Contents/Resources"
  list > "$b/Contents/Resources/gridline-members.txt"
  printf '%%s %%s\n' "%(owner)s" "%(version)s" > "$b/Contents/Resources/installed-by.txt"
  case "$b" in
    *.component)  # only the picked Audio Unit components stay in the bundle's list
      P="$b/Contents/Info.plist"
      n=0
      while /usr/libexec/PlistBuddy -c "Print :AudioComponents:$n" "$P" >/dev/null 2>&1; do n=$((n + 1)); done
      i=$((n - 1))
      while [ "$i" -ge 0 ]; do
        code="$(/usr/libexec/PlistBuddy -c "Print :AudioComponents:$i:type" "$P")/$(/usr/libexec/PlistBuddy -c "Print :AudioComponents:$i:subtype" "$P")"
        keep=0
        for t in $TYPES; do
          if awk -F'\t' -v t="$t" -v c="$code" '$1 == t && $2 == c { f = 1 } END { exit !f }' "$CODES"; then keep=1; break; fi
        done
        [ "$keep" = 1 ] || /usr/libexec/PlistBuddy -c "Delete :AudioComponents:$i" "$P"
        i=$((i - 1))
      done ;;
  esac
  if [ -z "${GRIDLINE_SUITE_NO_SIGN:-}" ]; then
    /usr/bin/codesign --force --sign - "$b" >/dev/null 2>&1 || echo "could not sign $b"
  fi
done
SUP="$(dirname "$(dirname "$(dirname "$DEST")")")/Library/Application Support/Gridline/%(sname)s"
case "$DEST" in /Library/*) SUP="/Library/Application Support/Gridline/%(sname)s" ;; esac
mkdir -p "$SUP"
U="$SUP/Uninstall %(sname)s.command"
cat > "$U" <<EOF
#!/bin/sh
# Removes %(sname)s as its installer put it in $DEST.
echo "Removing %(sname)s from $DEST"
SUDO=""; [ -w "$DEST" ] || SUDO=sudo
\$SUDO rm -rf "$DEST/VST3/%(sname)s.vst3" "$DEST/CLAP/%(sname)s.clap" "$DEST/Components/%(sname)s.component"
for p in \$(pkgutil --pkgs | grep '^%(pkgid)s\.'); do \$SUDO pkgutil --forget "\$p" >/dev/null 2>&1; done
\$SUDO rm -rf "$SUP"
echo "Done. Rescan plugins in your music program."
EOF
chmod 755 "$U"
rm -f "$PICKS"
exit 0
"""

MAC_PICK = """#!/bin/sh
echo "%(type)s" >> "${GRIDLINE_SUITE_PICKS:-/tmp/%(pkgid)s.picks}"
exit 0
"""

MAC_BEGIN = """#!/bin/sh
rm -f "${GRIDLINE_SUITE_PICKS:-/tmp/%(pkgid)s.picks}"
exit 0
"""


def _xml(s):
    return str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")


def _choice_id(t):
    return "p." + re.sub(r"[^A-Za-z0-9]", "_", t)


def mac_distribution(suite, version, catalogue, fmts, pkgid, refs):
    """The Distribution file: refs maps a package id to its file name."""
    sname = SUITES[suite]["name"]
    fmt_titles = {"vst3": "VST3", "clap": "CLAP", "component": "Audio Unit"}
    lines = ['<?xml version="1.0" encoding="utf-8"?>', '<installer-gui-script minSpecVersion="2">',
             "  <title>%s %s</title>" % (_xml(sname), _xml(version)),
             '  <welcome file="welcome.txt" mime-type="text/plain"/>',
             '  <license file="LICENSE.txt" mime-type="text/plain"/>' if "license" in refs.get("_res", ()) else "",
             '  <domains enable_localSystem="true" enable_currentUserHome="true" enable_anywhere="false"/>',
             '  <options customize="always" require-scripts="false" hostArchitectures="x86_64,arm64"/>',
             "  <choices-outline>", '    <line choice="begin"/>', '    <line choice="formats">']
    lines = [l for l in lines if l]
    for f in fmts:
        lines.append('      <line choice="fmt.%s"/>' % f)
    lines.append("    </line>")
    groups = []
    for kind, title in (("instrument", "Instruments"), ("effect", "Effects")):
        ms = [m for m in catalogue if m.get("kind") == kind]
        if not ms:
            continue
        cats = []
        for m in ms:
            c = m.get("category") or "Other"
            if c not in cats:
                cats.append(c)
        lines.append('    <line choice="%s">' % kind)
        for c in cats:
            cid = "%s.%s" % (kind, re.sub(r"[^A-Za-z0-9]", "_", c))
            lines.append('      <line choice="%s">' % cid)
            for m in ms:
                if (m.get("category") or "Other") == c:
                    lines.append('        <line choice="%s"/>' % _choice_id(m["type"]))
            lines.append("      </line>")
            groups.append((cid, c, ""))
        lines.append("    </line>")
        groups.append((kind, title, "Pick the %s to install." % title.lower()))
    lines += ['    <line choice="finish"/>', "  </choices-outline>",
              '  <choice id="begin" visible="false"><pkg-ref id="%s.begin"/></choice>' % pkgid,
              '  <choice id="formats" title="Formats" description="One file per format holds every plugin you pick."/>']
    for f in fmts:
        lines.append('  <choice id="fmt.%s" title="%s" description="%s"><pkg-ref id="%s.%s"/></choice>' % (
            f, fmt_titles[f], "Installed in Library/Audio/Plug-Ins/%s." % {"vst3": "VST3", "clap": "CLAP", "component": "Components"}[f], pkgid, f))
    for cid, title, desc in groups:
        lines.append('  <choice id="%s" title="%s" description="%s"/>' % (_xml(cid), _xml(title), _xml(desc)))
    for m in catalogue:
        lines.append('  <choice id="%s" title="%s" description="%s"><pkg-ref id="%s.%s"/></choice>' % (
            _choice_id(m["type"]), _xml(m["name"]), _xml("%s, %s" % (m.get("kind", ""), m.get("category", ""))), pkgid, _choice_id(m["type"])))
    lines.append('  <choice id="finish" visible="false"><pkg-ref id="%s.finish"/></choice>' % pkgid)
    for ident, fname in sorted(refs.items()):
        if ident == "_res":
            continue
        lines.append('  <pkg-ref id="%s" version="%s" onConclusion="none">%s</pkg-ref>' % (ident, _xml(version), _xml(fname)))
    lines.append("</installer-gui-script>")
    return "\n".join(lines) + "\n"


def _pkgbuild(args):
    subprocess.run(["pkgbuild", "--quiet"] + args, check=True, stdout=subprocess.DEVNULL)


def _script_dir(tmp, name, text, extra=None):
    d = os.path.join(tmp, "scripts", name)
    os.makedirs(d)
    p = os.path.join(d, "postinstall")
    with open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    os.chmod(p, 0o755)
    for fname, content in (extra or {}).items():
        with open(os.path.join(d, fname), "w", encoding="utf-8", newline="\n") as f:
            f.write(content)
    return d


def mac_scripts(suite, version, catalogue):
    """The finish script and its AU code list (also used by the self-test without pkgbuild)."""
    sname = SUITES[suite]["name"]
    pkgid = "org.gridline.suite.%s" % suite
    finish = MAC_FINISH % {"sname": sname, "suite": suite, "version": version, "owner": OWNER, "pkgid": pkgid}
    codes = "".join("%s\t%s\n" % (m["type"], "/".join(m.get("au", "").split("/")[:2])) for m in catalogue)
    return pkgid, finish, codes


def build_pkg(suite, version, catalogue, folder, out_dir, license_text=None):
    sname = SUITES[suite]["name"]
    fmts = _formats(folder, sname)
    if not fmts:
        raise SystemExit("no %s bundles in %s" % (sname, folder))
    pkgid, finish, codes = mac_scripts(suite, version, catalogue)
    pp = _load_package_plugins()
    types = [m["type"] for m in catalogue]
    name = installer_name(suite, version, "macos")
    with tempfile.TemporaryDirectory() as tmp:
        comp = os.path.join(tmp, "pkgs")
        os.makedirs(comp)
        refs = {}
        for f in sorted(fmts):
            root = os.path.join(tmp, "root", f)
            os.makedirs(root)
            dst = os.path.join(root, os.path.basename(fmts[f]))
            shutil.copytree(fmts[f], dst, symlinks=True)
            pp.trim_assets(os.path.join(dst, "Contents", "Resources", "assets"), types)
            loc = "/Library/Audio/Plug-Ins/" + {"vst3": "VST3", "clap": "CLAP", "component": "Components"}[f]
            fname = "fmt-%s.pkg" % f
            _pkgbuild(["--root", root, "--identifier", "%s.%s" % (pkgid, f), "--version", version, "--install-location", loc,
                       os.path.join(comp, fname)])
            refs["%s.%s" % (pkgid, f)] = fname
        _pkgbuild(["--nopayload", "--scripts", _script_dir(tmp, "begin", MAC_BEGIN % {"pkgid": pkgid}), "--identifier", pkgid + ".begin",
                   "--version", version, os.path.join(comp, "begin.pkg")])
        refs[pkgid + ".begin"] = "begin.pkg"
        for m in catalogue:
            cid = _choice_id(m["type"])
            fname = "%s.pkg" % cid
            _pkgbuild(["--nopayload", "--scripts", _script_dir(tmp, cid, MAC_PICK % {"type": m["type"], "pkgid": pkgid}),
                       "--identifier", "%s.%s" % (pkgid, cid), "--version", version, os.path.join(comp, fname)])
            refs["%s.%s" % (pkgid, cid)] = fname
        _pkgbuild(["--nopayload", "--scripts", _script_dir(tmp, "finish", finish, {"au-codes.tsv": codes}), "--identifier", pkgid + ".finish",
                   "--version", version, "--install-location", "/Library/Audio/Plug-Ins", os.path.join(comp, "finish.pkg")])
        refs[pkgid + ".finish"] = "finish.pkg"
        res = os.path.join(tmp, "res")
        os.makedirs(res)
        with open(os.path.join(res, "welcome.txt"), "w", encoding="utf-8") as f:
            f.write(mac_welcome(suite, version))
        if license_text is not None:
            with open(os.path.join(res, "LICENSE.txt"), "w", encoding="utf-8") as f:
                f.write(license_text)
            refs["_res"] = ("license",)
        dist = os.path.join(tmp, "Distribution.xml")
        with open(dist, "w", encoding="utf-8") as f:
            f.write(mac_distribution(suite, version, catalogue, sorted(fmts), pkgid, refs))
        os.makedirs(out_dir, exist_ok=True)
        subprocess.run(["productbuild", "--quiet", "--distribution", dist, "--package-path", comp, "--resources", res,
                        os.path.join(out_dir, name)], check=True, stdout=subprocess.DEVNULL)
    return name


def mac_welcome(suite, version):
    sname = SUITES[suite]["name"]
    return ("%s %s\n\nThis installs the %s plugins for other music programs. On the next pages, pick the\n"
            "formats (VST3, CLAP, Audio Unit) and the plugins you want; each format is one file holding\n"
            "every plugin you pick. Run the installer again to change your picks.\n\n"
            "The installer is not signed by Apple. If macOS says it cannot verify it, open System Settings >\n"
            "Privacy & Security, scroll down and click Open Anyway, then open the installer again.\n\n"
            "The plugins look for a new version once a day (no personal data; you can turn it off in any\n"
            "plugin's menu) and never install anything by themselves.\n\n"
            "To remove: Library/Application Support/Gridline/%s/Uninstall %s.command\n" % (sname, version, sname, sname, sname))


# --------------------------------------------------------------- windows ----

def _iss_name(t):
    return re.sub(r"[^A-Za-z0-9_]", "_", t)


def _iss_str(s):
    return str(s).replace('"', "'")


def windows_iss(suite, version, catalogue, inputs):
    """The Inno Setup script. inputs: {"x64": folder, "arm64": folder} with the suite's bundles
    (the VST3 bundle in either holds both processors after package-plugins' merge, or each its own)."""
    sname = SUITES[suite]["name"]
    app_id = str(uuid.UUID(hashlib.md5(("gridline-suite-installer:" + suite).encode()).hexdigest()))
    asset = SUITES[suite]["asset"]
    L = []
    L += ["; %s %s: the pick-your-plugins installer (written by tools/release/suite-installer.py)." % (sname, version),
          "; Compile with Inno Setup 6: iscc this-file.iss", "",
          "[Setup]",
          "AppId={{%s}" % app_id.upper(),
          "AppName=%s" % sname, "AppVersion=%s" % version, "AppVerName=%s %s" % (sname, version),
          "AppPublisher=Gridline", "AppPublisherURL=https://github.com/lolitemaultes/Gridline",
          "DefaultDirName={autopf}\\Gridline\\%s" % sname, "DisableDirPage=yes", "DisableProgramGroupPage=yes",
          "PrivilegesRequired=admin", "PrivilegesRequiredOverridesAllowed=commandline dialog",
          "ArchitecturesAllowed=x64compatible or arm64", "ArchitecturesInstallIn64BitMode=x64compatible or arm64",
          "OutputBaseFilename=%s-%s-windows-setup" % (asset, version), "OutputDir=.",
          "Compression=lzma2/ultra64", "SolidCompression=yes", "WizardStyle=modern",
          "UninstallDisplayName=%s" % sname, "Uninstallable=yes", "",
          "[Types]",
          'Name: "full"; Description: "Every plugin, every format"',
          'Name: "custom"; Description: "Pick plugins and formats"; Flags: iscustom', "",
          "[Components]",
          'Name: "fmt"; Description: "Formats"; Types: full custom; Flags: fixed',
          'Name: "fmt\\vst3"; Description: "VST3"; Types: full custom',
          'Name: "fmt\\clap"; Description: "CLAP"; Types: full custom']
    for kind, title in (("instrument", "Instruments"), ("effect", "Effects")):
        ms = [m for m in catalogue if m.get("kind") == kind]
        if not ms:
            continue
        L.append('Name: "%s"; Description: "%s"; Types: full custom' % (kind, title))
        cats = []
        for m in ms:
            c = m.get("category") or "Other"
            if c not in cats:
                cats.append(c)
        for c in cats:
            cn = "%s\\c_%s" % (kind, _iss_name(c))
            L.append('Name: "%s"; Description: "%s"; Types: full custom' % (cn, _iss_str(c)))
            for m in ms:
                if (m.get("category") or "Other") == c:
                    L.append('Name: "%s\\p_%s"; Description: "%s"; Types: full custom' % (cn, _iss_name(m["type"]), _iss_str(m["name"])))
    L += ["", "[Tasks]",
          'Name: "updatecheck"; Description: "Let the plugins check for updates (once a day, no personal data)"', "",
          "[Files]"]
    flags = "Flags: ignoreversion recursesubdirs createallsubdirs"
    vst3_src = None
    for label in ("x64", "arm64"):
        if label in inputs and os.path.isdir(os.path.join(inputs[label], sname + ".vst3")):
            vst3_src = label
            break
    if vst3_src:
        L.append('Source: "%s\\%s.vst3\\*"; DestDir: "{autocf}\\VST3\\%s.vst3"; Components: fmt\\vst3; %s' % (vst3_src, sname, sname, flags))
    for label, check in (("x64", "not IsArm64"), ("arm64", "IsArm64")):
        if label in inputs and os.path.exists(os.path.join(inputs[label], sname + ".clap")):
            L.append('Source: "%s\\%s.clap"; DestDir: "{autocf}\\CLAP"; Components: fmt\\clap; Check: %s; Flags: ignoreversion' % (label, sname, check))
            L.append('Source: "%s\\%s-assets\\*"; DestDir: "{autocf}\\CLAP\\%s-assets"; Components: fmt\\clap; Check: %s; %s' % (
                label, sname, sname, check, flags))
    L += ["", "[UninstallDelete]",
          'Type: files; Name: "{autocf}\\VST3\\%s.vst3\\Contents\\Resources\\gridline-members.txt"' % sname,
          'Type: files; Name: "{autocf}\\VST3\\%s.vst3\\Contents\\Resources\\installed-by.txt"' % sname,
          'Type: files; Name: "{autocf}\\CLAP\\%s-assets\\gridline-members.txt"' % sname,
          'Type: files; Name: "{autocf}\\CLAP\\%s-assets\\installed-by.txt"' % sname, "",
          "[Code]",
          "function MemberList(): String;",
          "var S: String;",
          "begin",
          "  S := '# Gridline plugin bundle: the plugins it shows to hosts' + #13#10 + 'suite %s' + #13#10;" % suite]
    for m in catalogue:
        kind = m.get("kind", "effect")
        comp = "%s\\c_%s\\p_%s" % (kind, _iss_name(m.get("category") or "Other"), _iss_name(m["type"]))
        L.append("  if WizardIsComponentSelected('%s') then S := S + '%s' + #13#10;" % (comp, m["type"]))
    L += ["  Result := S;", "end;", "",
          "procedure WriteLists(Folder: String);",
          "begin",
          "  if DirExists(Folder) then begin",
          "    SaveStringToFile(Folder + '\\gridline-members.txt', MemberList(), False);",
          "    SaveStringToFile(Folder + '\\installed-by.txt', '%s %s' + #13#10, False);" % (OWNER, version),
          "  end;",
          "end;", "",
          "procedure TurnOffUpdateCheck();",
          "var Dir, F, S: String;",
          "begin",
          "  Dir := ExpandConstant('{userappdata}\\gridline');",
          "  F := Dir + '\\update-check.json';",
          "  ForceDirectories(Dir);",
          "  if LoadStringFromFile(F, S) and (Pos('\"pluginsAuto\"', S) > 0) then begin",
          "    StringChangeEx(S, '\"pluginsAuto\":true', '\"pluginsAuto\":false', True);",
          "    SaveStringToFile(F, S, False);",
          "  end else if LoadStringFromFile(F, S) and (Pos('{', S) = 1) then begin",
          "    Insert('\"pluginsAuto\":false,', S, 2);",
          "    SaveStringToFile(F, S, False);",
          "  end else",
          "    SaveStringToFile(F, '{\"pluginsAuto\":false}' + #13#10, False);",
          "end;", "",
          "function AppCopy(Path: String): Boolean;",
          "var S: String;",
          "begin",
          "  Result := DirExists(Path) and not (LoadStringFromFile(Path + '\\Contents\\Resources\\installed-by.txt', S) and (Pos('%s', S) = 1));" % OWNER,
          "end;", "",
          "function InitializeSetup(): Boolean;",
          "begin",
          "  Result := True;",
          "  if AppCopy(ExpandConstant('{commoncf64}\\VST3\\%s.vst3')) then" % sname,
          "    Result := MsgBox('Gridline (the app) already installed every plugin of %s on this computer. ' +" % sname,
          "      'Continuing replaces them with the plugins you pick here. Continue?', mbConfirmation, MB_YESNO) = IDYES;",
          "end;", "",
          "procedure CurStepChanged(CurStep: TSetupStep);",
          "begin",
          "  if CurStep = ssPostInstall then begin",
          "    if WizardIsComponentSelected('fmt\\vst3') then WriteLists(ExpandConstant('{autocf}\\VST3\\%s.vst3\\Contents\\Resources'));" % sname,
          "    if WizardIsComponentSelected('fmt\\clap') then WriteLists(ExpandConstant('{autocf}\\CLAP\\%s-assets'));" % sname,
          "    if not WizardIsTaskSelected('updatecheck') then TurnOffUpdateCheck();",
          "  end;",
          "end;"]
    return "\n".join(L) + "\n"


def build_iss(suite, version, catalogue, inputs, out_dir):
    name = installer_name(suite, version, "windows")
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, name), "w", encoding="utf-8", newline="\r\n") as f:
        f.write(windows_iss(suite, version, catalogue, dict(inputs)))
    return name


# ------------------------------------------------------------- self test ----

def self_test():
    fails = []

    def check(ok, what):
        if not ok:
            fails.append(what)

    pp = _load_package_plugins()
    cat = [{"type": "gl.plasma", "name": "Plasma", "kind": "instrument", "category": "Synth", "au": "aumu/plas/GrLn"},
           {"type": "gl.reverb", "name": "Reverb", "kind": "effect", "category": "Reverb", "au": "aufx/revb/GrLn"},
           {"type": "gl.hihat", "name": "Hi-hat", "kind": "instrument", "category": "Drums", "au": "aumu/hiht/GrLn"},
           {"type": "gl.edison", "name": "Loupe", "kind": "effect", "category": "Misc", "au": "aufx/edsn/GrLn"}]
    sys.path.insert(0, HERE)
    import downloads
    with tempfile.TemporaryDirectory() as tmp:
        # ---- linux .run: install, change picks, uninstall, all in a scratch folder
        src = os.path.join(tmp, "in", "linux")
        for fmt in ("vst3", "clap"):
            pp._fake_bundle(src, "Gridline Suite", fmt, "linux", "x86_64-linux")
        out = os.path.join(tmp, "out")
        name = build_run("gridline", "0.8.0", "linux-x86_64", cat, src, out, "licence\n")
        check(name == "Gridline-Suite-0.8.0-linux-x86_64-setup.run", "linux installer name")
        e = downloads.classify(name, "0.8.0")
        check(e is not None and e.get("kind") == "installer" and e.get("product") == "suite", "the release page places the .run")
        run = os.path.join(out, name)
        dest = os.path.join(tmp, "dest")
        home = os.path.join(tmp, "home")
        os.makedirs(home)
        env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=os.path.join(home, "cfg"), XDG_DATA_HOME=os.path.join(home, "data"), TMPDIR=tmp)
        env.pop("GRIDLINE_CONFIG_HOME", None)

        def sh(*args):
            r = subprocess.run(["sh", run] + list(args), env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            return r.returncode, r.stdout

        rc, text = sh("--list")
        check(rc == 0 and "Plasma" in text and "4 plugins" in text, "the .run lists the plugins: " + text[:200])
        rc, text = sh("--dest", dest, "--yes", "--plugins", "Plasma,gl.reverb", "--formats", "vst3,clap", "--no-update-check")
        check(rc == 0, "the .run installs: " + text)
        vst3 = os.path.join(dest, "vst3", "Gridline Suite.vst3")
        members = os.path.join(vst3, "Contents", "Resources", "gridline-members.txt")
        clap_list = os.path.join(dest, "clap", "Gridline Suite-assets", "gridline-members.txt")

        def lines(p):
            try:
                with open(p, encoding="utf-8") as f:
                    return [l for l in f.read().splitlines() if l and not l.startswith("#")]
            except OSError:
                return []
        check(lines(members) == ["suite gridline", "gl.plasma", "gl.reverb"], "the VST3 lists the picks: %r" % lines(members))
        check(lines(clap_list) == ["suite gridline", "gl.plasma", "gl.reverb"], "the CLAP lists the picks")
        check(os.access(os.path.join(dest, "clap", "Gridline Suite.clap"), os.X_OK), "the CLAP keeps its execute bit")
        check(not os.path.exists(os.path.join(vst3, "Contents", "Resources", "assets", "sounds")) and
              os.path.exists(os.path.join(vst3, "Contents", "Resources", "assets", "fonts", "Inter-Regular.ttf")), "the installer carries trimmed assets")
        check(lines(os.path.join(home, "cfg", "gridline", "update-check.json")) == ['{"pluginsAuto":false}'], "the update check opt-out is written")
        rc, text = sh("--dest", dest, "--yes", "--plugins", "instruments", "--formats", "vst3")
        check(rc == 0 and lines(members) == ["suite gridline", "gl.plasma", "gl.hihat"], "running it again changes the picks: %r" % lines(members))
        rc, text = sh("--dest", dest, "--yes", "--plugins", "nothing-like-it")
        check(rc != 0 and "no plugin called" in text, "an unknown plugin is refused")
        # a copy the app put there is kept unless --replace
        other = os.path.join(tmp, "dest2")
        os.makedirs(os.path.join(other, "vst3", "Gridline Suite.vst3"))
        rc, text = sh("--dest", other, "--yes", "--formats", "vst3")
        check(rc == 0 and "Kept the copy" in text and not os.path.exists(os.path.join(other, "vst3", "Gridline Suite.vst3", "Contents")),
              "the app's copy is kept")
        rc, text = sh("--dest", other, "--yes", "--formats", "vst3", "--replace")
        check(rc == 0 and os.path.exists(os.path.join(other, "vst3", "Gridline Suite.vst3", "Contents", "Resources", "gridline-members.txt")),
              "--replace replaces it")
        uninstall = os.path.join(dest, "share", "gridline", "suite", "gridline", "uninstall")
        r = subprocess.run(["sh", uninstall], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        check(r.returncode == 0 and not os.path.exists(vst3) and not os.path.exists(os.path.join(dest, "clap", "Gridline Suite.clap"))
              and not os.path.exists(os.path.join(dest, "clap", "Gridline Suite-assets")), "uninstall removes everything: " + r.stdout)
        check(not os.path.exists(os.path.join(dest, "share", "gridline", "suite", "gridline")), "uninstall removes its receipt")
        # Mini through the same code
        msrc = os.path.join(tmp, "in", "mini")
        for fmt in ("vst3", "clap"):
            pp._fake_bundle(msrc, "Gridline Suite Mini", fmt, "bsd", "x86_64-freebsd")
        mcat = [{"type": "gl.minitrim", "name": "Trim", "kind": "effect", "category": "Utility", "au": "aufx/mtrm/GrLn"}]
        mname = build_run("mini", "0.8.0", "freebsd-x86_64", mcat, msrc, out)
        check(mname == "Gridline-Suite-Mini-0.8.0-freebsd-x86_64-setup.run", "Mini BSD installer name")
        e = downloads.classify(mname, "0.8.0")
        check(e is not None and e.get("product") == "mini" and e.get("family") == "bsd", "the release page places the Mini .run")
        run = os.path.join(out, mname)
        rc, text = sh("--dest", os.path.join(tmp, "mdest"), "--yes")
        check(rc == 0 and lines(os.path.join(tmp, "mdest", "vst3", "Gridline Suite Mini.vst3", "Contents", "Resources", "gridline-members.txt"))
              == ["suite mini", "gl.minitrim"], "the Mini installer installs: " + text)

        # ---- macos: the Distribution and the finish script (run against a scratch folder)
        pkgid, finish, codes = mac_scripts("gridline", "0.8.0", cat)
        refs = {pkgid + ".vst3": "fmt-vst3.pkg", pkgid + ".begin": "begin.pkg", pkgid + ".finish": "finish.pkg"}
        refs.update({"%s.%s" % (pkgid, _choice_id(m["type"])): _choice_id(m["type"]) + ".pkg" for m in cat})
        dist = mac_distribution("gridline", "0.8.0", cat, ["clap", "component", "vst3"], pkgid, refs)
        import xml.dom.minidom
        try:
            xml.dom.minidom.parseString(dist)
            parsed = True
        except Exception:
            parsed = False
        check(parsed, "the Distribution file is well formed")
        check('enable_currentUserHome="true"' in dist and 'choice id="p.gl_plasma" title="Plasma"' in dist and "Audio Unit" in dist,
              "the Distribution offers this-user installs, plugins and formats")
        check(dist.index('line choice="begin"') < dist.index('line choice="formats"') < dist.index('line choice="finish"'),
              "begin first, finish last")
        plug = os.path.join(tmp, "Library", "Audio", "Plug-Ins")
        for fmt, d in (("vst3", "VST3"), ("clap", "CLAP"), ("component", "Components")):
            pp._fake_bundle(os.path.join(plug, d), "Gridline Suite", fmt, "macos", "")
        sdir = os.path.join(tmp, "finish")
        os.makedirs(sdir)
        with open(os.path.join(sdir, "postinstall"), "w") as f:
            f.write(finish)
        with open(os.path.join(sdir, "au-codes.tsv"), "w") as f:
            f.write(codes)
        picks = os.path.join(tmp, "picks")
        with open(picks, "w") as f:
            f.write("gl.reverb\ngl.plasma\n")
        if os.path.exists("/usr/libexec/PlistBuddy"):
            fenv = dict(env, GRIDLINE_SUITE_PICKS=picks, GRIDLINE_SUITE_NO_SIGN="1")
            r = subprocess.run(["sh", os.path.join(sdir, "postinstall"), "x", plug, "/"], env=fenv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            check(r.returncode == 0, "the finish script runs: " + r.stdout)
            for d, ext in (("VST3", "vst3"), ("CLAP", "clap"), ("Components", "component")):
                got = lines(os.path.join(plug, d, "Gridline Suite." + ext, "Contents", "Resources", "gridline-members.txt"))
                check(got == ["suite gridline", "gl.plasma", "gl.reverb"], "macos %s lists the picks: %r" % (ext, got))
            with open(os.path.join(plug, "Components", "Gridline Suite.component", "Contents", "Info.plist"), "rb") as f:
                comps = plistlib.load(f).get("AudioComponents", [])
            check(sorted(c["subtype"] for c in comps) == ["plas", "revb"], "macos: the AU keeps only the picked components: %r" % comps)
            check(not os.path.exists(picks), "the picks file is cleared")
            unin = os.path.join(tmp, "Library", "Application Support", "Gridline", "Gridline Suite", "Uninstall Gridline Suite.command")
            check(os.access(unin, os.X_OK), "the uninstall command is left in Application Support")
        if shutil.which("pkgbuild") and shutil.which("productbuild") and shutil.which("pkgutil"):
            mac_src = os.path.join(tmp, "in", "macos")
            for fmt in ("vst3", "clap", "component"):
                pp._fake_bundle(mac_src, "Gridline Suite", fmt, "macos", "")
            pname = build_pkg("gridline", "0.8.0", cat, mac_src, out, "licence\n")
            check(pname == "Gridline-Suite-0.8.0-macos-universal.pkg", "macOS installer name")
            e = downloads.classify(pname, "0.8.0")
            check(e is not None and e.get("kind") == "installer", "the release page places the .pkg")
            ex = os.path.join(tmp, "expanded")
            subprocess.run(["pkgutil", "--expand", os.path.join(out, pname), ex], check=True)
            parts = sorted(os.listdir(ex))
            check("Distribution" in parts and "fmt-vst3.pkg" in parts and "finish.pkg" in parts and "p.gl_plasma.pkg" in parts,
                  "the .pkg holds the format, plugin and finish packages: %r" % parts)
            with open(os.path.join(ex, "Distribution"), encoding="utf-8") as f:
                check("Gridline Suite 0.8.0" in f.read(), "the .pkg's Distribution is ours")
        # ---- windows: the Inno script
        iss = windows_iss("gridline", "0.8.0", cat, {"x64": src, "arm64": src})
        check("AppName=Gridline Suite" in iss and 'Name: "instrument\\c_Synth\\p_gl_plasma"; Description: "Plasma"' in iss, "the Inno script names components")
        check("WizardIsComponentSelected('instrument\\c_Synth\\p_gl_plasma') then S := S + 'gl.plasma'" in iss, "the Inno script writes the picks")
        check("Check: IsArm64" in iss and "Check: not IsArm64" in iss, "one setup for both processors")
        check("PrivilegesRequiredOverridesAllowed=commandline dialog" in iss and "{autocf}" in iss, "per-user or all-users install")
        ids = re.findall(r"^AppId=\{\{([0-9A-F-]+)\}$", iss, re.M)
        mini_iss = windows_iss("mini", "0.8.0", mcat, {"x64": msrc})
        check(ids and ids != re.findall(r"^AppId=\{\{([0-9A-F-]+)\}$", mini_iss, re.M), "Suite and Mini have their own installed-apps entries")
        app_iss = os.path.join(HERE, "..", "windows", "gridline.iss")
        if os.path.exists(app_iss):
            with open(app_iss, encoding="utf-8", errors="replace") as f:
                check(ids[0] not in f.read().upper(), "the suite setup's id is not the app's")
        for text in (iss, dist, finish, mac_welcome("gridline", "0.8.0")):
            check(not re.search("[–—]", text), "no dashes in public text")
    for f in fails:
        print("FAIL", f)
    print("suite-installer self-test: %s" % ("ok" if not fails else "%d failed" % len(fails)))
    return 1 if fails else 0


# The systems the release builds installers for (labels of the .run files).
RUN_LABELS = ("linux-x86_64", "linux-aarch64", "freebsd-x86_64", "openbsd-x86_64", "netbsd-x86_64")


def release_installer_names(version):
    out = []
    for suite in SUITES:
        out.append(installer_name(suite, version, "macos"))
        out.append("%s-%s-windows-setup.exe" % (SUITES[suite]["asset"], version))
        out += [installer_name(suite, version, "linux", l) for l in RUN_LABELS]
    return out


def real_test(suite, plugin_dir, cli):
    """macOS: the real installers made from this build's bundles. The .pkg is built and taken apart,
    its finish step runs on a scratch copy of the real bundles (signed ad hoc, then verified), and a
    .run made from the same bundles installs into a scratch folder."""
    fails = []

    def check(ok, what):
        print("%s - %s" % ("ok" if ok else "not ok", what))
        if not ok:
            fails.append(what)

    sname = SUITES[suite]["name"]
    catalogue = json.loads(subprocess.run([cli, "suite-info", suite, "--json"], check=True, capture_output=True, text=True).stdout)
    check(len(catalogue) > 0, "%s: %d plugins in the catalogue" % (sname, len(catalogue)))
    picks = [catalogue[0]["type"], catalogue[-1]["type"]]
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "out")
        name = build_pkg(suite, "0.0.0", catalogue, plugin_dir, out)
        size = os.path.getsize(os.path.join(out, name))
        check(size > 1000000, "%s built (%.1f MB)" % (name, size / 1e6))
        ex = os.path.join(tmp, "expanded")
        subprocess.run(["pkgutil", "--expand", os.path.join(out, name), ex], check=True)
        parts = os.listdir(ex)
        check(sum(1 for p in parts if p.startswith("p.")) == len(catalogue), "one choice package per plugin")
        check(all(("fmt-%s.pkg" % f) in parts for f in ("vst3", "clap", "component")), "one package per format")
        # The finish step on a scratch copy of the real bundles.
        lib = os.path.join(tmp, "Library", "Audio", "Plug-Ins")
        for d, ext in (("VST3", "vst3"), ("CLAP", "clap"), ("Components", "component")):
            shutil.copytree(os.path.join(plugin_dir, "%s.%s" % (sname, ext)), os.path.join(lib, d, "%s.%s" % (sname, ext)), symlinks=True)
        pkgid, finish, codes = mac_scripts(suite, "0.0.0", catalogue)
        sdir = os.path.join(tmp, "finish")
        os.makedirs(sdir)
        with open(os.path.join(sdir, "postinstall"), "w") as f:
            f.write(finish)
        with open(os.path.join(sdir, "au-codes.tsv"), "w") as f:
            f.write(codes)
        pf = os.path.join(tmp, "picks")
        with open(pf, "w") as f:
            f.write("\n".join(picks) + "\n")
        env = dict(os.environ, GRIDLINE_SUITE_PICKS=pf)
        r = subprocess.run(["sh", os.path.join(sdir, "postinstall"), "x", lib, "/"], env=env, capture_output=True, text=True)
        check(r.returncode == 0 and "could not sign" not in r.stdout, "the finish step runs and signs: " + r.stdout.strip())
        for d, ext in (("VST3", "vst3"), ("CLAP", "clap"), ("Components", "component")):
            b = os.path.join(lib, d, "%s.%s" % (sname, ext))
            with open(os.path.join(b, "Contents", "Resources", "gridline-members.txt")) as f:
                got = [l for l in f.read().splitlines() if l and not l.startswith("#")]
            check(got == ["suite " + suite] + sorted(picks), "%s lists the two picks" % ext)
            v = subprocess.run(["codesign", "--verify", "--deep", "--strict", b], capture_output=True, text=True)
            check(v.returncode == 0, "%s: the changed bundle's ad-hoc signature verifies %s" % (ext, v.stderr.strip()))
        with open(os.path.join(lib, "Components", sname + ".component", "Contents", "Info.plist"), "rb") as f:
            comps = plistlib.load(f).get("AudioComponents", [])
        want = sorted("/".join(m["au"].split("/")[:2]) for m in catalogue if m["type"] in picks)
        check(sorted("%s/%s" % (c.get("type"), c.get("subtype")) for c in comps) == want, "the Audio Unit lists the two picked components")
        # The .run's installer from the same bundles (the script is the same on every system).
        vonly = os.path.join(tmp, "vst3only")  # (a macOS CLAP is a bundle; the .run carries the Linux file form)
        shutil.copytree(os.path.join(plugin_dir, sname + ".vst3"), os.path.join(vonly, sname + ".vst3"), symlinks=True)
        rname = build_run(suite, "0.0.0", "test", catalogue, vonly, out)
        dest = os.path.join(tmp, "dest")
        home = os.path.join(tmp, "home")
        os.makedirs(home)
        renv = dict(os.environ, HOME=home, XDG_CONFIG_HOME=os.path.join(home, "cfg"), XDG_DATA_HOME=os.path.join(home, "data"), TMPDIR=tmp)
        r = subprocess.run(["sh", os.path.join(out, rname), "--dest", dest, "--yes", "--plugins", ",".join(picks)], env=renv,
                           stdin=subprocess.DEVNULL, capture_output=True, text=True)
        check(r.returncode == 0, "the .run installs the real bundles: " + (r.stdout + r.stderr).strip()[-300:])
        exe = os.path.join("Contents", "MacOS", sname)
        with open(os.path.join(plugin_dir, sname + ".vst3", exe), "rb") as a, open(os.path.join(dest, "vst3", sname + ".vst3", exe), "rb") as b:
            check(a.read() == b.read(), "the installed engine is the built one")
    print("%s real installers: %s" % (sname, "ok" if not fails else "%d failed" % len(fails)))
    return 1 if fails else 0


def _parse_inputs(values):
    out = []
    for v in values:
        label, sep, folder = v.partition("=")
        out.append((label, folder) if sep else ("", v))
    return out


def main(argv):
    if argv[1:] == ["--self-test"]:
        return self_test()
    if len(argv) == 3 and argv[1] == "--print-names":
        print("\n".join(release_installer_names(argv[2])))
        return 0
    if len(argv) == 5 and argv[1] == "--real-test":
        return real_test(argv[2], argv[3], argv[4])
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--suite", choices=sorted(SUITES), required=True)
    ap.add_argument("--family", choices=("linux", "bsd", "macos", "windows"), required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--catalogue", required=True)
    ap.add_argument("--input", action="append", required=True, help="[LABEL=]folder with the suite's bundles")
    ap.add_argument("--out", required=True)
    ap.add_argument("--license")
    a = ap.parse_args(argv[1:])
    with open(a.catalogue, encoding="utf-8") as f:
        catalogue = json.load(f)
    lic = None
    if a.license:
        with open(a.license, encoding="utf-8") as f:
            lic = f.read()
    inputs = _parse_inputs(a.input)
    names = []
    if a.family in ("linux", "bsd"):
        for label, folder in inputs:
            if not label:
                raise SystemExit("--input needs a label for %s (linux-x86_64, freebsd-x86_64, ...)" % a.family)
            names.append(build_run(a.suite, a.version, label, catalogue, folder, a.out, lic))
    elif a.family == "macos":
        names.append(build_pkg(a.suite, a.version, catalogue, inputs[0][1], a.out, lic))
    else:
        names.append(build_iss(a.suite, a.version, catalogue, inputs, a.out))
    for n in names:
        print(n)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
