#!/usr/bin/env python3
"""Gridline - extracts every translatable UI string into the template catalogue.

    python3 tools/i18n_extract.py            # writes assets/i18n/gridline.pot
    python3 tools/i18n_extract.py --check    # exit 1 when the template is out of date

Collected (src/app, src/packs/*/*_ui.cpp, src/collab, src/core/Keymap.cpp, src/core/ProjectFiles.cpp, src/engine/Chop.cpp, src/plugin .cpp and .mm):
  * i18n::tr("...") and i18n::N_("...") literals (N_ marks text translated later);
  * the English text handed to the widgets that translate themselves:
    ui::Hint, ui::Tooltip, ui::Label, ui::BeginSection, ui::TextButton (label, hint),
    ui::DangerButton (label, hint), ui::IconButton / ui::ToggleIcon (hint), ui::EmptyState, formLabel / label,
    dialogTitle, openPrompt, roleColour, ui::BeginDockWindow / ui::windowTitle (window titles),
    ui::confirmTheme (what changed; confirmation titles are marked N_),
    browser sectionHeader / folderSection / emptyLine;
  * App::notify messages: a literal, or the leading literal of "text: " + ... (the
    runtime translates the leading part, i18n::trMessage).
A literal starting with "##" or with fewer than two letters is an id, not text.
Translators copy gridline.pot to <code>.po (e.g. de.po), fill in msgstr and add
"Language-Name: Deutsch\\n" to the header; Settings > General lists it.
"""
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "assets", "i18n", "gridline.pot")

# function name -> argument indexes holding English text (None = every literal argument)
HELPERS = {
    "Hint": [0], "Tooltip": [0], "Label": [0], "BeginSection": [0], "TextButton": [0, 3], "DangerButton": [0, 3], "IconButton": [4],
    "ToggleIcon": [4], "EmptyState": [3, 4], "formLabel": [0], "label": [0], "dialogTitle": [1, 2],
    "openPrompt": [0], "roleColour": [1, 2], "BeginDockWindow": [0], "windowTitle": [0],
    "sectionHeader": [0], "folderSection": [1, 4], "emptyLine": [0], "notify": [0],
    # J-77: Keep / Revert confirmations (title, row caption; what changed).
    "confirmTheme": [2],
}
# Helpers called on an object (a.notify(...)).
MEMBER_HELPERS = {"notify"}
CONV = re.compile(r"%[-+ #0]*(\d+|\*)?(\.(\d+|\*))?[hlLzjt]*[diouxXeEfgGcspn%]")
STR = r'"(?:[^"\\]|\\.)*"'
LITS = re.compile(r"^\s*((?:" + STR + r"\s*)+)$")
TERN = re.compile(r"^\s*[^\"?]+\?\s*(" + STR + r")\s*:\s*(" + STR + r")\s*$")
# "text: " + value / std::string("text ") + value: the leading literal.
LEAD = re.compile(r"^\s*(?:std::string\(\s*)?(" + STR + r")\s*\)?\s*\+")


def source_files():
    files = glob.glob(os.path.join(ROOT, "src", "app", "*.cpp")) + glob.glob(os.path.join(ROOT, "src", "app", "*.h"))
    files += glob.glob(os.path.join(ROOT, "src", "packs", "*", "*_ui.cpp"))
    files += [os.path.join(ROOT, "src", "core", "Keymap.cpp")]
    files += [os.path.join(ROOT, "src", "core", "ExternalDrop.cpp")]  # J-75: drop hints ("Drop to add 3 audio clips ...")
    files += [os.path.join(ROOT, "src", "core", "ProjectFiles.cpp")]  # J-78: open / save dialog file types (N_)
    files += [os.path.join(ROOT, "src", "engine", "Chop.cpp")]  # PL-13: chop preset names and hints (N_)
    files += glob.glob(os.path.join(ROOT, "src", "plugin", "*.cpp"))  # PH-09: the plugin build's own panel and messages
    files += glob.glob(os.path.join(ROOT, "src", "plugin", "*.mm"))  # J-64: Cocoa views, hosted plugins' windows, Audio Unit
    files += glob.glob(os.path.join(ROOT, "src", "collab", "*.cpp"))  # J-111: collaboration messages (codes, refusals, connection test)
    return sorted(files)


def decode(lit_seq):
    """C string literal(s) -> text (UTF-8)."""
    out = bytearray()
    for lit in re.findall(STR, lit_seq):
        body, i = lit[1:-1], 0
        while i < len(body):
            c = body[i]
            if c != "\\":
                out += c.encode("utf-8")
                i += 1
                continue
            n = body[i + 1]
            if n == "x":
                m = re.match(r"[0-9a-fA-F]+", body[i + 2:])
                out.append(int(m.group(0), 16) & 0xFF)
                i += 2 + len(m.group(0))
            elif n in "01234567":
                m = re.match(r"[0-7]{1,3}", body[i + 1:])
                out.append(int(m.group(0), 8) & 0xFF)
                i += 1 + len(m.group(0))
            else:
                out += {"n": b"\n", "t": b"\t", "r": b"\r", "\\": b"\\", '"': b'"', "'": b"'", "?": b"?"}.get(n, n.encode())
                i += 2
    return out.decode("utf-8", errors="replace")


def is_text(t):
    if t.startswith("##"):
        return False
    vis = CONV.sub("", t.split("##")[0])
    return len(re.findall(r"[A-Za-z]", vis)) >= 2


def split_args(s, i):
    args, depth, start, j = [], 0, i, i
    while j < len(s):
        c = s[j]
        if c == '"' or c == "'":
            q = c
            j += 1
            while j < len(s) and s[j] != q:
                j += 2 if s[j] == "\\" else 1
        elif c in "([{":
            depth += 1
        elif c in ")]}":
            if depth == 0:
                args.append(s[start:j])
                return args
            depth -= 1
        elif c == "," and depth == 0:
            args.append(s[start:j])
            start = j + 1
        j += 1
    return []


def strip_comments(s):
    # Keeps line numbers: comments become spaces (string literals are left alone).
    out, i, n = [], 0, len(s)
    while i < n:
        c = s[i]
        if c == '"' or c == "'":
            q, j = c, i + 1
            while j < n and s[j] != q:
                j += 2 if s[j] == "\\" else 1
            out.append(s[i:j + 1])
            i = j + 1
        elif s.startswith("//", i):
            j = s.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif s.startswith("/*", i):
            j = s.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", s[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def texts_in_arg(arg, lead=False):
    m = LITS.match(arg)
    if m:
        return [decode(m.group(1))]
    m = TERN.match(arg)
    if m:
        return [decode(m.group(1)), decode(m.group(2))]
    if lead:
        m = LEAD.match(arg)
        if m:
            return [decode(m.group(1)).rstrip(" :")]
    return []


def extract():
    found = {}  # msgid -> set of files
    order = []
    call = re.compile(r"\b(i18n::tr|tr|i18n::N_|N_|" + "|".join(HELPERS) + r")\s*\(")
    for path in source_files():
        rel = os.path.relpath(path, ROOT)
        s = strip_comments(open(path, encoding="utf-8", errors="replace").read())
        for m in call.finditer(s):
            name = m.group(1).split("::")[-1]
            pre = s[max(0, m.start() - 12):m.start()]
            if re.search(r"(void|bool|float|int|auto)\s+$", pre):
                continue  # a definition, not a call
            q = re.search(r"(\w+)::$", pre)
            if (q and q.group(1) not in ("ui", "i18n")) or (re.search(r"(\.|->)$", pre) and name not in MEMBER_HELPERS):
                continue  # another namespace's / object's function (keys::label, x.label)
            args = split_args(s, m.end())
            if not args:
                continue
            idxs = [0] if name in ("tr", "N_") else HELPERS[name]
            for k in idxs:
                if k >= len(args):
                    continue
                for t in texts_in_arg(args[k], lead=name in MEMBER_HELPERS):
                    if not is_text(t):
                        continue
                    t = t.split("##")[0] if "##" in t and not t.startswith("##") else t
                    if t not in found:
                        found[t] = set()
                        order.append(t)
                    found[t].add(rel)
    return found, order


def po_escape(t):
    return t.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\t", "\\t")


def render(found, order):
    lines = [
        "# Gridline interface strings (template). Generated by tools/i18n_extract.py: do not edit by hand.",
        "# Copy to <language code>.po in this folder, fill in msgstr and set Language-Name.",
        'msgid ""',
        'msgstr ""',
        '"Content-Type: text/plain; charset=UTF-8\\n"',
        '"Language-Name: \\n"',
        "",
    ]
    for t in sorted(order, key=lambda x: (sorted(found[x])[0], x)):
        lines.append("#: " + " ".join(sorted(found[t])))
        lines.append('msgid "' + po_escape(t) + '"')
        lines.append('msgstr ""')
        lines.append("")
    return "\n".join(lines)


def main():
    found, order = extract()
    text = render(found, order)
    if "--check" in sys.argv:
        old = open(OUT, encoding="utf-8").read() if os.path.exists(OUT) else ""
        if old != text:
            print("assets/i18n/gridline.pot is out of date: run python3 tools/i18n_extract.py")
            return 1
        print("gridline.pot is up to date (%d strings)" % len(order))
        return 0
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(text)
    print("%d strings -> %s" % (len(order), os.path.relpath(OUT, ROOT)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
