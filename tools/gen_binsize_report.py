#!/usr/bin/env python3
"""
gen_binsize_report.py ── Binary composition report from a linker map.

Parses a linker map file (MSVC /MAP, GNU ld -Map, or LLD -Map), attributes
bytes to library/module/object, and emits a self-contained HTML report
(cloning tools/gen_pak_report.py's UI), a JSON bill-of-materials, or a
two-map diff.

Usage:
    python gen_binsize_report.py <map> <out.html> [--title T] [--json out.json]
    python gen_binsize_report.py <base.map> <new.map> <out.html> --diff
"""
from __future__ import annotations

import argparse
import html
import json
import re
import subprocess
import sys
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List, Optional, Tuple


def _git_hash() -> str:
    """Best-effort short git hash of the repo this script lives in (or "")."""
    try:
        r = subprocess.run(["git", "rev-parse", "--short", "HEAD"],
                           cwd=str(Path(__file__).resolve().parent),
                           capture_output=True, text=True, timeout=5)
        if r.returncode == 0:
            return r.stdout.strip()
    except Exception:
        pass
    return ""


@dataclass
class Record:
    section: str
    symbol: str
    object: str
    lib: str
    size: int


def canon_section(name: str) -> str:
    """Map a (possibly sub-)section name to a canonical bucket."""
    n = name.lower()
    if n.startswith(".text"):
        return ".text"
    if n.startswith(".rdata") or n.startswith(".rodata"):
        return ".rdata"
    if n.startswith(".data"):
        return ".data"
    if n.startswith(".bss") or n.startswith(".tbss"):
        return ".bss"
    if n.startswith(".pdata") or n.startswith(".eh_frame") or n.startswith(".xdata"):
        return ".pdata"
    if n.startswith(".reloc"):
        return ".reloc"
    if n.startswith(".rsrc"):
        return ".rsrc"
    if n.startswith(".tls"):
        return ".tls"
    # MSVC groups many small read-only/init sub-segments that live in the
    # on-disk image; fold them into .rdata so the report is not fragmented.
    if (n.startswith(".idata") or n.startswith(".edata") or n.startswith(".crt")
            or n.startswith(".00cfg") or n.startswith(".rtc") or n.startswith(".gfids")
            or n.startswith(".giats") or n.startswith(".gehcont") or n.startswith(".voltbl")):
        return ".rdata"
    return name


def detect_format(text: str) -> str:
    head = text[:8192]
    if "Publics by Value" in head or "Preferred load address" in head:
        return "msvc"
    if "Linker script and memory map" in head:
        return "gnuld"
    if re.search(r"\bVMA\b.*\bLMA\b.*\bSize\b", head):
        return "lld"
    raise ValueError("unrecognized linker-map format (expected MSVC /MAP, GNU ld -Map, or LLD -Map)")


# input section line: " .text   0x<addr>   0x<size>   <origin>"
_GNU_INPUT = re.compile(
    r"^\s+(\.\S+)\s+0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)\s+(\S.*?)\s*$")
# wrapped: a lone section name, then the addr/size/origin on the next line
_GNU_LONE = re.compile(r"^\s+(\.\S+)\s*$")
_GNU_CONT = re.compile(r"^\s+0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)\s+(\S.*?)\s*$")


def _split_origin(origin: str) -> Tuple[str, str]:
    """origin -> (lib, object). Archive 'path/lib.a(obj.o)' -> ('lib.a','obj.o').
    Direct object 'path/to/obj.o' -> ('', 'path/to/obj.o') so the classifier can
    see directory hints like 'jce_core.dir'."""
    origin = origin.strip()
    m = re.search(r"([^/\\]+)\(([^)]+)\)\s*$", origin)
    if m:
        return m.group(1), m.group(2)
    return "", origin


def parse_gnuld(text: str) -> List[Record]:
    recs: List[Record] = []
    pending_sec: Optional[str] = None
    for line in text.splitlines():
        if pending_sec:
            mc = _GNU_CONT.match(line)
            if mc:
                size = int(mc.group(1), 16)
                lib, obj = _split_origin(mc.group(2))
                if size and "(" not in pending_sec:
                    recs.append(Record(canon_section(pending_sec), "", obj, lib, size))
                pending_sec = None
                continue
            pending_sec = None
        m = _GNU_INPUT.match(line)
        if m:
            size = int(m.group(2), 16)
            if size:
                lib, obj = _split_origin(m.group(3))
                recs.append(Record(canon_section(m.group(1)), "", obj, lib, size))
            continue
        ml = _GNU_LONE.match(line)
        if ml and not line.lstrip().startswith("*"):
            pending_sec = ml.group(1)
    return recs


# lld row: VMA LMA Size Align <rest>; object rows have "<origin>:(<section>)"
_LLD_ROW = re.compile(
    r"^\s*([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+(\d+)\s+(\S.*?)\s*$")


def parse_lld(text: str) -> List[Record]:
    recs: List[Record] = []
    for line in text.splitlines():
        m = _LLD_ROW.match(line)
        if not m:
            continue
        rest = m.group(5)
        # object rows look like "path/lib.a(obj.o):(.text)" or "path/obj.o:(.text)"
        mo = re.match(r"^(.*?):\((\.[^)]+)\)\s*$", rest)
        if not mo:
            continue  # output-section header or symbol row
        size = int(m.group(3), 16)
        if not size:
            continue
        lib, obj = _split_origin(mo.group(1))
        recs.append(Record(canon_section(mo.group(2)), "", obj, lib, size))
    return recs


# Segment-table row: "FRAME:OFFSET  LENGTHh  NAME  CLASS". The 4-hex prefix is
# a FRAME (PE section group); NAME is a $-suffixed sub-segment (COMDAT group).
_MSVC_SEGTBL = re.compile(
    r"^\s*([0-9a-fA-F]{4}):([0-9a-fA-F]{8})\s+([0-9a-fA-F]+)H\s+(\.\S+)\s+\w+\s*$")
# Publics row: "FRAME:OFFSET  symbol  RVA  [flag]  Lib:Object" (or "<absolute>").
_MSVC_PUB = re.compile(
    r"^\s*([0-9a-fA-F]{4}):([0-9a-fA-F]{8})\s+\S+\s+[0-9a-fA-F]+\s+(?:\S\s+)?(\S+)\s*$")

# Canonical sections that occupy no bytes in the on-disk image (allocated only
# at runtime). Excluded from the on-disk size total.
_NOFILE_SECTIONS = {".bss"}


def parse_msvc(text: str) -> List[Record]:
    """Parse an MSVC /MAP.

    The 4-hex prefix on each address is a FRAME (PE section group); a frame
    holds many $-suffixed sub-segments (.text$mn, .rdata, .idata$6, .bss, ...).
    Section totals are taken from the sub-segment lengths (exact). Per-object
    sizes are approximated from public-symbol VA deltas (clamped to the
    containing sub-segment), because MSVC maps omit static/anonymous symbols;
    the per-section shortfall is recorded under an '(unattributed)' object so
    the totals reconcile to the real section sizes.
    """
    subsegs: List[Tuple[str, int, int, str]] = []  # (frame, start, end, section)
    pubs: List[Tuple[str, int, str, str]] = []      # (frame, off, lib, obj)
    in_pub = False
    for line in text.splitlines():
        if not in_pub:
            if "Publics by Value" in line:
                in_pub = True
                continue
            m = _MSVC_SEGTBL.match(line)
            if m:
                frame, start = m.group(1), int(m.group(2), 16)
                length, name = int(m.group(3), 16), m.group(4)
                if length:
                    subsegs.append((frame, start, start + length, canon_section(name)))
            continue
        mp = _MSVC_PUB.match(line)
        if mp:
            frame, off, libobj = mp.group(1), int(mp.group(2), 16), mp.group(3)
            if ":" in libobj:
                lib, obj = libobj.split(":", 1)
            else:
                lib, obj = "", libobj
            pubs.append((frame, off, lib, obj))

    by_frame: Dict[str, List[Tuple[str, int, int, str]]] = {}
    for ss in subsegs:
        by_frame.setdefault(ss[0], []).append(ss)
    for entries in by_frame.values():
        entries.sort(key=lambda s: s[1])

    def find_subseg(frame: str, off: int) -> Optional[Tuple[int, int, str]]:
        for (_fr, s, e, sec) in by_frame.get(frame, ()):
            if s <= off < e:
                return (s, e, sec)
        return None

    pubs.sort(key=lambda p: (p[0], p[1]))
    recs: List[Record] = []
    attributed: Dict[str, int] = {}
    n = len(pubs)
    for i, (frame, off, lib, obj) in enumerate(pubs):
        ss = find_subseg(frame, off)
        if ss is None:
            continue
        s, e, sec = ss
        if sec in _NOFILE_SECTIONS:
            continue
        nxt = e  # clamp to the sub-segment end so a symbol can't absorb the next
        if i + 1 < n and pubs[i + 1][0] == frame:
            nxt = min(nxt, pubs[i + 1][1])
        size = max(0, nxt - off)
        if size:
            recs.append(Record(sec, "", obj, lib, size))
            attributed[sec] = attributed.get(sec, 0) + size

    sec_total: Dict[str, int] = {}
    for (_fr, s, e, sec) in subsegs:
        if sec in _NOFILE_SECTIONS:
            continue
        sec_total[sec] = sec_total.get(sec, 0) + (e - s)
    for sec, tot in sec_total.items():
        rem = tot - attributed.get(sec, 0)
        if rem > 0:
            recs.append(Record(sec, "", "(unattributed)", "", rem))
    return recs


def parse_map(text: str) -> List[Record]:
    fmt = detect_format(text)
    return {"msvc": parse_msvc, "gnuld": parse_gnuld, "lld": parse_lld}[fmt](text)


# Ordered (pattern, module) rules; first match wins. Patterns test "lib|object".
_CLASSIFY_RULES: List[Tuple[re.Pattern, str]] = [
    (re.compile(r"jce_(\w+)\.dir"), r"jce_\1"),       # engine layer dir
    (re.compile(r"JCE_Editor\.dir"), "editor"),
    (re.compile(r"(?:^|[/\\(])lib(bimg_encode)"), "bimg_encode"),
    (re.compile(r"\bbimg_encode\b"), "bimg_encode"),
    (re.compile(r"(?:lib)?(bimg)(?:_decode)?\b"), "bimg"),
    (re.compile(r"(?:lib)?(bgfx)\b"), "bgfx"),
    (re.compile(r"(?:lib)?(bullet|Bullet|LinearMath)\w*"), "bullet3"),
    (re.compile(r"(?:lib)?(assimp)\w*"), "assimp"),
    (re.compile(r"(?:lib)?(SDL|sdl)\d*"), "sdl"),
    (re.compile(r"(?:lib)?(freetype|harfbuzz|tracy|physfs|zstd|flecs|imgui|opus|ogg|dav1d|vpx|webm|recast|enet|protobuf|ozz|box2d|rmlui|enkits|mimalloc)\w*", re.I), r"\1"),
    (re.compile(r"\.conan2"), "other-conan"),
    (re.compile(r"\b(LIBCMT|libcmt|msvcrt|MSVCRT|vcruntime|ucrt|kernel32|user32|gdi32|libc|libstdc\+\+|libgcc|crt\d|crtbegin|crtend)\w*", re.I), "system"),
]


def classify(lib: str, obj: str) -> str:
    hay = f"{lib}|{obj}"
    for pat, repl in _CLASSIFY_RULES:
        m = pat.search(hay)
        if m:
            return m.expand(repl) if "\\" in repl else repl
    # fallback: archive name without extension, else basename of object
    if lib:
        return re.sub(r"\.(lib|a)$", "", lib)
    return re.split(r"[/\\]", obj)[-1]


def aggregate(recs: List[Record]) -> Dict[str, object]:
    modules: Dict[str, Dict[str, object]] = {}
    sections: Dict[str, int] = {}
    objects: Dict[Tuple[str, str], Dict[str, object]] = {}
    for r in recs:
        mod = classify(r.lib, r.object)
        m = modules.setdefault(mod, {"name": mod, "size": 0, "sections": {}})
        m["size"] += r.size
        m["sections"][r.section] = m["sections"].get(r.section, 0) + r.size
        sections[r.section] = sections.get(r.section, 0) + r.size
        ok = (mod, r.object)
        o = objects.setdefault(ok, {"module": mod, "object": r.object,
                                    "lib": r.lib, "size": 0})
        o["size"] += r.size
    mod_list = sorted(modules.values(), key=lambda m: -m["size"])
    obj_list = sorted(objects.values(), key=lambda o: -o["size"])
    return {
        "total": sum(r.size for r in recs),
        "sections": sections,
        "modules": mod_list,
        "objects": obj_list,
    }


def to_json(agg: Dict[str, object]) -> str:
    doc = {"schema": "jce.binsize.v1", **agg}
    return json.dumps(doc, indent=2)


def fmt_bytes(n: int) -> str:
    f = float(n)
    for unit in ("B", "KB", "MB", "GB"):
        if abs(f) < 1024.0 or unit == "GB":
            return (f"{f:.0f} {unit}" if unit == "B" else f"{f:.2f} {unit}")
        f /= 1024.0
    return f"{n} B"


def build_tree(agg: Dict[str, object]) -> Dict[str, object]:
    """module -> object hierarchy for the treemap."""
    children = []
    for m in agg["modules"]:
        objs = [o for o in agg["objects"] if o["module"] == m["name"]]
        children.append({
            "name": m["name"], "size": m["size"],
            "children": [{"name": o["object"], "size": o["size"]} for o in objs],
        })
    return {"name": "binary", "size": agg["total"], "children": children}


_PALETTE = ["#4c8bf5", "#f5a623", "#39c08a", "#bd6be3", "#ef6f6c", "#22b8cf",
            "#f2c14e", "#8c9eff", "#67c23a", "#e06c9f", "#5ab0d6", "#d4a05a"]
_SECTION_COLORS = {".text": "#4c8bf5", ".rdata": "#f5a623", ".data": "#39c08a",
                   ".pdata": "#bd6be3", ".rsrc": "#ef6f6c", ".tls": "#22b8cf",
                   ".bss": "#9b9b9b", ".reloc": "#67c23a"}


def _section_legend(sections: Dict[str, int], total: int) -> Tuple[str, str]:
    if total <= 0:
        return "", ""
    items = sorted(sections.items(), key=lambda kv: -kv[1])
    segs, legs = [], []
    for name, sz in items:
        pct = 100.0 * sz / total
        col = _SECTION_COLORS.get(name, "#8a8f98")
        segs.append(f'<span class="seg" style="flex:{sz};background:{col}" '
                    f'title="{html.escape(name)} {fmt_bytes(sz)} ({pct:.1f}%)"></span>')
        legs.append(f'<span class="leg"><i style="background:{col}"></i>'
                    f'{html.escape(name)} <b>{fmt_bytes(sz)}</b> '
                    f'<span class="mut">{pct:.0f}%</span></span>')
    return ('<div class="bar">' + "".join(segs) + "</div>",
            '<div class="legend">' + "".join(legs) + "</div>")


def _module_rows(agg: Dict[str, object]) -> str:
    secs = list(agg["sections"].keys())
    head = "".join(f'<th data-k="s{i}">{html.escape(s)}</th>' for i, s in enumerate(secs))
    total = agg["total"] or 1
    rows = []
    for m in agg["modules"]:
        pct = 100.0 * m["size"] / total
        cells = "".join(
            f'<td data-v="{m["sections"].get(s, 0)}">{fmt_bytes(m["sections"].get(s, 0))}</td>'
            for s in secs)
        rows.append(
            f'<tr><td>{html.escape(m["name"])}</td>'
            f'<td data-v="{m["size"]}"><span class="rb"><i style="width:{pct:.2f}%"></i></span>'
            f'{fmt_bytes(m["size"])}</td>{cells}</tr>')
    return (f'<table class="tbl" id="modtbl"><thead><tr>'
            f'<th>Module</th><th data-k="total" class="sortable sorted-desc">Total</th>'
            f'{head}</tr></thead><tbody>{"".join(rows)}</tbody></table>')


def _top_bars(agg: Dict[str, object], n: int = 20) -> str:
    objs = agg["objects"][:n]
    if not objs:
        return ""
    mx = max(o["size"] for o in objs) or 1
    rows = []
    for i, o in enumerate(objs):
        pct = 100.0 * o["size"] / mx
        label = f'{o["module"]} / {o["object"]}' if o["object"] else o["module"]
        col = _PALETTE[i % len(_PALETTE)]
        rows.append(
            f'<div class="br"><span class="brl" title="{html.escape(label)}">{html.escape(label)}</span>'
            f'<span class="brb"><i style="width:{pct:.2f}%;background:{col}"></i></span>'
            f'<span class="brv">{fmt_bytes(o["size"])}</span></div>')
    return '<div class="bars">' + "".join(rows) + "</div>"


# Token-substituted (not str.format) so CSS/JS braces stay literal.
_HTML = """<!DOCTYPE html><html lang="en" data-theme="auto"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>__TITLE__</title>__FAVICON__<style>
html[data-theme=light]{color-scheme:light}html[data-theme=dark]{color-scheme:dark}html[data-theme=auto]{color-scheme:light dark}
:root{--bg:#fbfbfc;--fg:#1a1d21;--mut:#6b7280;--card:#fff;--bd:#e6e8ec;--accent:#4c8bf5;--shadow:0 1px 2px rgba(16,24,40,.06),0 1px 3px rgba(16,24,40,.1);--track:#eef0f3;--up:#e5484d;--down:#30a46c}
html[data-theme=dark]{--bg:#0f1115;--fg:#e6e8ec;--mut:#9aa1ac;--card:#171a1f;--bd:#262a31;--accent:#5b9bff;--shadow:none;--track:#1d2127;--up:#ff6369;--down:#3dd68c}
@media(prefers-color-scheme:dark){html[data-theme=auto]{--bg:#0f1115;--fg:#e6e8ec;--mut:#9aa1ac;--card:#171a1f;--bd:#262a31;--accent:#5b9bff;--shadow:none;--track:#1d2127;--up:#ff6369;--down:#3dd68c}}
*{box-sizing:border-box}
body{margin:0;font:14px/1.55 system-ui,-apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif;background:var(--bg);color:var(--fg);-webkit-font-smoothing:antialiased}
.top{position:sticky;top:0;z-index:5;display:flex;align-items:center;gap:14px;padding:14px 26px;background:color-mix(in srgb,var(--bg) 88%,transparent);backdrop-filter:blur(8px);border-bottom:1px solid var(--bd)}
.top h1{font-size:16px;font-weight:650;margin:0;letter-spacing:.2px}
.top .sp{flex:1}
.badge{font-size:12px;color:var(--mut);background:var(--card);border:1px solid var(--bd);border-radius:999px;padding:4px 11px}
.tg{cursor:pointer;border:1px solid var(--bd);background:var(--card);color:var(--fg);border-radius:8px;width:34px;height:30px;font-size:15px;line-height:1}
.wrap{max-width:1080px;margin:0 auto;padding:26px}
.hero{margin:6px 0 22px}
.hero .big{font-size:34px;font-weight:720;letter-spacing:-.5px}
.hero .sub{color:var(--mut);font-size:13px;margin-top:2px}
.bar{display:flex;height:30px;border-radius:9px;overflow:hidden;border:1px solid var(--bd);margin-top:16px;box-shadow:var(--shadow)}
.bar .seg{height:100%;min-width:1px;transition:opacity .15s}.bar .seg:hover{opacity:.82}
.legend{display:flex;flex-wrap:wrap;gap:6px 18px;margin-top:11px;font-size:12.5px;color:var(--fg)}
.leg{display:inline-flex;align-items:center;gap:6px}.leg i{width:11px;height:11px;border-radius:3px;display:inline-block}.leg b{font-weight:620}
.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:13px;margin:22px 0}
.card{background:var(--card);border:1px solid var(--bd);border-radius:13px;padding:15px 17px;box-shadow:var(--shadow)}
.card .v{font-size:21px;font-weight:680;letter-spacing:-.3px}.card .k{color:var(--mut);font-size:12px;margin-top:2px}
h2{font-size:13px;font-weight:640;text-transform:uppercase;letter-spacing:.7px;color:var(--mut);margin:30px 0 10px}
.panel{background:var(--card);border:1px solid var(--bd);border-radius:13px;box-shadow:var(--shadow);overflow:hidden}
.tbl{width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums}
.tbl th,.tbl td{text-align:right;padding:8px 14px;border-bottom:1px solid var(--bd);white-space:nowrap}
.tbl tbody tr:last-child td{border-bottom:0}
.tbl th:first-child,.tbl td:first-child{text-align:left}
.tbl thead th{position:sticky;top:0;z-index:1;background:var(--card);font-size:11px;text-transform:uppercase;letter-spacing:.5px;color:var(--mut);cursor:pointer;user-select:none;box-shadow:inset 0 -1px 0 var(--bd)}
.tbl thead th:hover{color:var(--fg)}
.tbl thead th.sorted-desc::after{content:" \\25BC";font-size:9px}.tbl thead th.sorted-asc::after{content:" \\25B2";font-size:9px}
.tbl tbody tr:hover{background:color-mix(in srgb,var(--accent) 7%,transparent)}
.tbl td:nth-child(2){position:relative}
.rb{position:absolute;left:0;bottom:0;height:3px;width:100%;pointer-events:none}
.rb i{display:block;height:100%;background:var(--accent);opacity:.6;border-radius:2px}
.tblscroll{max-height:460px;overflow:auto}
.bars{padding:14px 16px}.br{display:flex;align-items:center;gap:10px;margin:5px 0}
.brl{width:320px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-size:12.5px}
.brb{flex:1;background:var(--track);border-radius:5px;height:15px;overflow:hidden}
.brb i{display:block;height:100%;border-radius:5px}
.brv{width:92px;text-align:right;color:var(--mut);font-size:12px;font-variant-numeric:tabular-nums}
.dhero{font-size:18px;font-weight:680;margin:2px 0 10px}.dhero.up{color:var(--up)}.dhero.down{color:var(--down)}
.dcell{position:relative}.up .dcell b{color:var(--up)}.down .dcell b{color:var(--down)}
.db{position:absolute;left:0;bottom:0;height:3px;width:100%;pointer-events:none}
.db i{display:block;height:100%;border-radius:2px;opacity:.7}
.up .db i{background:var(--up);float:right}.down .db i{background:var(--down);float:left}
.tmwrap{padding:12px 14px}
.crumb{font-size:12.5px;margin-bottom:8px;color:var(--mut);min-height:18px}
.crumb a{color:var(--accent);cursor:pointer;text-decoration:none}.crumb a:hover{text-decoration:underline}
#tm{width:100%;height:440px;position:relative}
#tm rect{cursor:pointer;transition:opacity .12s}#tm rect:hover{opacity:.85}
#tm text{pointer-events:none;font:11px system-ui;fill:#14181d}
.tip{position:fixed;pointer-events:none;background:#0b0d10;color:#fff;font-size:12px;padding:6px 9px;border-radius:7px;opacity:0;transition:opacity .1s;z-index:20;max-width:320px;box-shadow:0 4px 16px rgba(0,0,0,.3)}
.foot{color:var(--mut);font-size:11.5px;margin:26px 0 6px;text-align:center}
</style></head><body>
<div class="top"><h1>__TITLE__</h1><span class="sp"></span>
<span class="badge">total __TOTAL__</span>
<button class="tg" id="themeBtn" title="Toggle theme" aria-label="Toggle theme">◐</button></div>
<div class="wrap">
<div class="hero"><div class="big">__TOTAL__</div><div class="sub">__SUBTITLE__</div>
__SECTIONBAR__ __SECTIONLEGEND__</div>
<div class="cards">__CARDS__</div>
__DIFFSECTION__
<h2>Treemap — modules &rarr; objects</h2>
<div class="panel tmwrap"><nav class="crumb" id="crumb"></nav><div id="tm"></div></div>
<h2>Modules</h2><div class="panel tblscroll">__TABLE__</div>
<h2>Largest objects</h2><div class="panel">__TOPBARS__</div>
<div class="foot">__META__</div></div>
<div class="tip" id="tip"></div>
<script id="tmdata" type="application/json">__TREEJSON__</script>
<script>/* jce.binsize.treemap: interactive zoomable squarified treemap */
(function(){
 var root=JSON.parse(document.getElementById('tmdata').textContent);
 var host=document.getElementById('tm'),crumb=document.getElementById('crumb'),tip=document.getElementById('tip');
 var PAL=['#4c8bf5','#f5a623','#39c08a','#bd6be3','#ef6f6c','#22b8cf','#f2c14e','#8c9eff','#67c23a','#e06c9f','#5ab0d6','#d4a05a'];
 var path=[root];
 function esc(s){return String(s).replace(/[&<>]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;'}[c]});}
 function fmt(n){var u=['B','KB','MB','GB'],i=0,f=n;while(f>=1024&&i<3){f/=1024;i++;}return (i?f.toFixed(2):f)+' '+u[i];}
 function squarify(items,x,y,w,h){
  var out=[];items=items.slice().filter(function(d){return d.size>0;}).sort(function(a,b){return b.size-a.size;});
  var tot=items.reduce(function(s,d){return s+d.size;},0)||1,scale=(w*h)/tot;
  items.forEach(function(d){d._a=d.size*scale;});
  var i=0;
  function worst(row,len){var s=row.reduce(function(a,d){return a+d._a;},0),mx=Math.max.apply(0,row.map(function(d){return d._a;})),mn=Math.min.apply(0,row.map(function(d){return d._a;}));return Math.max(len*len*mx/(s*s),s*s/(len*len*mn));}
  while(i<items.length){
   var horiz=w>=h,len=horiz?h:w,row=[items[i]],j=i+1;
   while(j<items.length){var nr=row.concat([items[j]]);if(worst(row,len)<worst(nr,len))break;row=nr;j++;}
   var rs=row.reduce(function(a,d){return a+d._a;},0),side=rs/(len||1),off=0;
   row.forEach(function(d){var dl=d._a/(side||1);out.push(horiz?{x:x,y:y+off,w:side,h:dl,d:d}:{x:x+off,y:y,w:dl,h:side,d:d});off+=dl;});
   if(horiz){x+=side;w-=side;}else{y+=side;h-=side;}i=j;
  }
  return out;
 }
 function curr(){return path[path.length-1];}
 function drawCrumb(){var h='';path.forEach(function(n,i){if(i)h+=' <span style="opacity:.5">/</span> ';h+=(i<path.length-1)?'<a data-i="'+i+'">'+esc(n.name)+'</a>':'<b>'+esc(n.name)+'</b>';});crumb.innerHTML=h;
  crumb.querySelectorAll('a').forEach(function(a){a.onclick=function(){path=path.slice(0,+a.dataset.i+1);render();};});}
 function render(){
  drawCrumb();var node=curr(),W=host.clientWidth||800,H=440;
  var kids=(node.children||[]).slice();if(!kids.length){host.innerHTML='<div style="padding:40px;text-align:center;color:var(--mut)">no children</div>';return;}
  var cells=squarify(kids,0,0,W,H),tot=node.size||1;
  var svg='<svg width="'+W+'" height="'+H+'" style="display:block">';
  cells.forEach(function(c,i){
   var col=PAL[i%PAL.length],pct=(100*c.d.size/tot).toFixed(1);
   svg+='<g class="cell" data-name="'+esc(c.d.name)+'" data-size="'+c.d.size+'" data-pct="'+pct+'" data-leaf="'+(c.d.children?0:1)+'" data-idx="'+i+'">';
   svg+='<rect x="'+c.x.toFixed(1)+'" y="'+c.y.toFixed(1)+'" width="'+Math.max(0,c.w-1.5).toFixed(1)+'" height="'+Math.max(0,c.h-1.5).toFixed(1)+'" rx="3" fill="'+col+'"/>';
   if(c.w>54&&c.h>20){svg+='<text x="'+(c.x+6)+'" y="'+(c.y+16)+'" font-weight="600">'+esc(c.d.name)+'</text>';
    if(c.h>34)svg+='<text x="'+(c.x+6)+'" y="'+(c.y+30)+'" opacity=".8">'+fmt(c.d.size)+'</text>';}
   svg+='</g>';
  });
  svg+='</svg>';host.innerHTML=svg;
  host.querySelectorAll('.cell').forEach(function(g){
   var cd=cells[+g.dataset.idx].d;
   g.onmousemove=function(e){tip.style.opacity=1;tip.style.left=Math.min(e.clientX+14,innerWidth-330)+'px';tip.style.top=(e.clientY+14)+'px';tip.innerHTML='<b>'+esc(cd.name)+'</b><br>'+fmt(cd.size)+' &middot; '+g.dataset.pct+'% of '+esc(node.name)+(cd.children&&cd.children.length?'<br><span style="opacity:.7">click to zoom</span>':'');};
   g.onmouseleave=function(){tip.style.opacity=0;};
   g.onclick=function(){if(cd.children&&cd.children.length){path.push(cd);render();}};
  });
 }
 var btn=document.getElementById('themeBtn'),el=document.documentElement;
 var saved=null;try{saved=localStorage.getItem('binsize-theme');}catch(e){}
 if(saved)el.setAttribute('data-theme',saved);
 btn.onclick=function(){var cur=el.getAttribute('data-theme');var nx=cur=='dark'?'light':'dark';el.setAttribute('data-theme',nx);try{localStorage.setItem('binsize-theme',nx);}catch(e){}};
 // sortable table
 function wireSort(id){var tbl=document.getElementById(id);if(!tbl)return;var ths=tbl.tHead.rows[0].cells;for(var ci=0;ci<ths.length;ci++){(function(ci){ths[ci].classList.add('sortable');ths[ci].onclick=function(){
   var asc=!ths[ci].classList.contains('sorted-asc');for(var k=0;k<ths.length;k++)ths[k].classList.remove('sorted-asc','sorted-desc');
   ths[ci].classList.add(asc?'sorted-asc':'sorted-desc');
   var rows=[].slice.call(tbl.tBodies[0].rows);
   rows.sort(function(a,b){var ca=a.cells[ci],cb=b.cells[ci];
     if(ca.dataset.v!==undefined||cb.dataset.v!==undefined){var x=+ca.dataset.v||0,y=+cb.dataset.v||0;return asc?x-y:y-x;}
     var sx=(ca.textContent||'').trim(),sy=(cb.textContent||'').trim();return asc?sx.localeCompare(sy):sy.localeCompare(sx);});
   rows.forEach(function(r){tbl.tBodies[0].appendChild(r);});};})(ci);}}
 wireSort('modtbl');wireSort('embdiff');
 render();window.addEventListener('resize',render);
}());</script>
</body></html>"""


def _cards(agg: Dict[str, object]) -> str:
    pri = [("Total", agg["total"]), (".text", agg["sections"].get(".text", 0)),
           (".rdata", agg["sections"].get(".rdata", 0)),
           (".data", agg["sections"].get(".data", 0))]
    out = [f'<div class="card"><div class="v">{fmt_bytes(v)}</div>'
           f'<div class="k">{html.escape(k)}</div></div>' for k, v in pri]
    out.append(f'<div class="card"><div class="v">{len(agg["modules"])}</div>'
               f'<div class="k">modules</div></div>')
    out.append(f'<div class="card"><div class="v">{len(agg["objects"])}</div>'
               f'<div class="k">objects</div></div>')
    return "".join(out)


def _icon_data_uri() -> str:
    """JCE brand icon as a base64 PNG data-URI from the committed sidecar
    (tools/_jce_icon.b64), or "" if absent. Matches gen_pak_report.py."""
    sidecar = Path(__file__).resolve().parent / "_jce_icon.b64"
    try:
        if sidecar.is_file():
            uri = sidecar.read_text(encoding="ascii").strip()
            if uri.startswith("data:image"):
                return uri
    except OSError:
        pass
    return ""


def _favicon() -> str:
    uri = _icon_data_uri()
    return f'<link rel="icon" href="{uri}">' if uri else ""


def _diff_section(base_agg: Optional[Dict[str, object]],
                  new_agg: Dict[str, object], n: int = 25,
                  base_label: str = "") -> str:
    """An embedded 'vs baseline' diff view for the single report (empty if no
    baseline). Mirrors render_diff_html but inline in the composition page."""
    if not base_agg:
        return ""
    d = diff(base_agg, new_agg)
    td = d["total_delta"]
    col = "up" if td >= 0 else "down"
    blab = (html.escape(base_label) + " " if base_label else "")
    hero = (f'<h2>&Delta; vs baseline</h2>'
            f'<div class="dhero {col}">Total &Delta; {"+" if td >= 0 else "−"}{fmt_bytes(abs(td))} '
            f'<span class="mut" style="font-weight:400;font-size:13px">'
            f'(baseline {blab}{fmt_bytes(d["total_base"])} &rarr; {fmt_bytes(d["total_new"])})</span></div>')
    deltas = [r for r in d["modules"] if r["delta"] != 0]
    if not deltas:
        return hero + ('<div class="panel" style="padding:16px;color:var(--mut)">'
                       'No module-level changes.</div>')
    mx = max(abs(r["delta"]) for r in deltas) or 1
    secs = set(base_agg["sections"]) | set(new_agg["sections"])
    seg = []
    for s in sorted(secs, key=lambda s: -(new_agg["sections"].get(s, 0) - base_agg["sections"].get(s, 0))):
        dd = new_agg["sections"].get(s, 0) - base_agg["sections"].get(s, 0)
        if dd == 0:
            continue
        c = _SECTION_COLORS.get(s, "#8a8f98")
        seg.append(f'<span class="leg"><i style="background:{c}"></i>{html.escape(s)} '
                   f'<b>{"+" if dd > 0 else "−"}{fmt_bytes(abs(dd))}</b></span>')
    rows = []
    for r in deltas[:n]:
        up = r["delta"] > 0
        cls = "up" if up else "down"
        sign = "+" if up else "−"
        w = 100.0 * abs(r["delta"]) / mx
        rows.append(
            f'<tr class="{cls}"><td>{html.escape(r["name"])}</td>'
            f'<td data-v="{r["base"]}">{fmt_bytes(r["base"])}</td>'
            f'<td data-v="{r["new"]}">{fmt_bytes(r["new"])}</td>'
            f'<td data-v="{r["delta"]}" class="dcell"><span class="db"><i style="width:{w:.1f}%"></i></span>'
            f'<b>{sign}{fmt_bytes(abs(r["delta"]))}</b></td></tr>')
    table = ('<table class="tbl" id="embdiff"><thead><tr><th>Module</th><th>Base</th>'
             '<th>New</th><th class="sorted-desc">&Delta;</th></tr></thead><tbody>'
             + "".join(rows) + '</tbody></table>')
    more = ('' if len(deltas) <= n else
            f'<div class="mut" style="padding:8px 14px">+{len(deltas) - n} more changed modules</div>')
    return (hero + f'<div class="legend">{"".join(seg)}</div>'
            f'<div class="panel tblscroll">{table}{more}</div>')


def _footer(meta: str) -> str:
    base = "tools/gen_binsize_report.py &middot; linker-map sizes"
    return (html.escape(meta) + " &middot; " + base) if meta else base


def render_html(agg: Dict[str, object], title: str = "Binary",
                base_agg: Optional[Dict[str, object]] = None,
                meta: str = "", base_label: str = "") -> str:
    bar, legend = _section_legend(agg["sections"], agg["total"])
    subtitle = (f'{len(agg["modules"])} modules &middot; {len(agg["objects"])} objects '
                f'&middot; sizes attributed from the linker map')
    tree_json = json.dumps(build_tree(agg)).replace("</", "<\\/")
    repl = {
        "__TITLE__": html.escape(title),
        "__FAVICON__": _favicon(),
        "__TOTAL__": fmt_bytes(agg["total"]),
        "__SUBTITLE__": subtitle,
        "__SECTIONBAR__": bar,
        "__SECTIONLEGEND__": legend,
        "__CARDS__": _cards(agg),
        "__DIFFSECTION__": _diff_section(base_agg, agg, base_label=base_label),
        "__TABLE__": _module_rows(agg),
        "__TOPBARS__": _top_bars(agg),
        "__TREEJSON__": tree_json,
        "__META__": _footer(meta),
    }
    out = _HTML
    for k, v in repl.items():
        out = out.replace(k, v)
    return out


def diff(base: Dict[str, object], new: Dict[str, object]) -> Dict[str, object]:
    b = {m["name"]: m["size"] for m in base["modules"]}
    n = {m["name"]: m["size"] for m in new["modules"]}
    rows = []
    for name in sorted(set(b) | set(n), key=lambda k: -(n.get(k, 0) - b.get(k, 0))):
        bv, nv = b.get(name, 0), n.get(name, 0)
        rows.append({"name": name, "base": bv, "new": nv, "delta": nv - bv})
    return {"total_base": base["total"], "total_new": new["total"],
            "total_delta": new["total"] - base["total"], "modules": rows}


def render_diff_html(base: Dict[str, object], new: Dict[str, object],
                     title: str = "Binary diff", meta: str = "",
                     base_label: str = "", new_label: str = "") -> str:
    d = diff(base, new)
    deltas = [r for r in d["modules"] if r["delta"] != 0]
    mx = max((abs(r["delta"]) for r in deltas), default=1) or 1
    rows = []
    for r in deltas:
        up = r["delta"] > 0
        sign = "+" if up else "−"
        cls = "up" if up else "down"
        w = 100.0 * abs(r["delta"]) / mx
        rows.append(
            f'<tr class="{cls}"><td>{html.escape(r["name"])}</td>'
            f'<td data-v="{r["base"]}">{fmt_bytes(r["base"])}</td>'
            f'<td data-v="{r["new"]}">{fmt_bytes(r["new"])}</td>'
            f'<td data-v="{r["delta"]}" class="dcell">'
            f'<span class="db"><i style="width:{w:.1f}%"></i></span>'
            f'<b>{sign}{fmt_bytes(abs(r["delta"]))}</b></td></tr>')
    table = ('<table class="tbl" id="difftbl"><thead><tr><th>Module</th>'
             '<th>Base</th><th>New</th><th class="sorted-desc">&Delta;</th></tr>'
             '</thead><tbody>' + "".join(rows) + "</tbody></table>")
    secs = set(base["sections"]) | set(new["sections"])
    seg = []
    for s in sorted(secs, key=lambda s: -(new["sections"].get(s, 0) - base["sections"].get(s, 0))):
        dd = new["sections"].get(s, 0) - base["sections"].get(s, 0)
        if dd == 0:
            continue
        col = _SECTION_COLORS.get(s, "#8a8f98")
        seg.append(f'<span class="leg"><i style="background:{col}"></i>{html.escape(s)} '
                   f'<b>{"+" if dd > 0 else "−"}{fmt_bytes(abs(dd))}</b></span>')
    td = d["total_delta"]
    bl = (html.escape(base_label) + " " if base_label else "")
    nl = (html.escape(new_label) + " " if new_label else "")
    subline = f'{bl}{fmt_bytes(d["total_base"])} &rarr; {nl}{fmt_bytes(d["total_new"])}'
    repl = {
        "__TITLE__": html.escape(title),
        "__FAVICON__": _favicon(),
        "__DELTA__": ("+" if td >= 0 else "−") + fmt_bytes(abs(td)),
        "__DCLASS__": "up" if td >= 0 else "down",
        "__SUBLINE__": subline,
        "__SECDELTA__": '<div class="legend">' + "".join(seg) + "</div>",
        "__TABLE__": table,
        "__META__": _footer(meta),
    }
    out = _DIFF_HTML
    for k, v in repl.items():
        out = out.replace(k, v)
    return out


_DIFF_HTML = """<!DOCTYPE html><html lang="en" data-theme="auto"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>__TITLE__</title>__FAVICON__<style>
html[data-theme=light]{color-scheme:light}html[data-theme=dark]{color-scheme:dark}html[data-theme=auto]{color-scheme:light dark}
:root{--bg:#fbfbfc;--fg:#1a1d21;--mut:#6b7280;--card:#fff;--bd:#e6e8ec;--up:#e5484d;--down:#30a46c;--track:#eef0f3;--shadow:0 1px 3px rgba(16,24,40,.1)}
html[data-theme=dark]{--bg:#0f1115;--fg:#e6e8ec;--mut:#9aa1ac;--card:#171a1f;--bd:#262a31;--up:#ff6369;--down:#3dd68c;--track:#1d2127;--shadow:none}
@media(prefers-color-scheme:dark){html[data-theme=auto]{--bg:#0f1115;--fg:#e6e8ec;--mut:#9aa1ac;--card:#171a1f;--bd:#262a31;--up:#ff6369;--down:#3dd68c;--track:#1d2127;--shadow:none}}
*{box-sizing:border-box}
body{margin:0;font:14px/1.55 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--fg)}
.top{position:sticky;top:0;display:flex;align-items:center;gap:12px;padding:14px 26px;background:color-mix(in srgb,var(--bg) 88%,transparent);backdrop-filter:blur(8px);border-bottom:1px solid var(--bd)}
.top h1{font-size:16px;font-weight:650;margin:0}.top .sp{flex:1}
.tg{cursor:pointer;border:1px solid var(--bd);background:var(--card);color:var(--fg);border-radius:8px;width:34px;height:30px}
.wrap{max-width:960px;margin:0 auto;padding:26px}
.big{font-size:34px;font-weight:720;letter-spacing:-.5px}.big.up{color:var(--up)}.big.down{color:var(--down)}
.sub{color:var(--mut);margin-top:2px}
.legend{display:flex;flex-wrap:wrap;gap:6px 18px;margin:16px 0;font-size:12.5px}
.leg{display:inline-flex;align-items:center;gap:6px}.leg i{width:11px;height:11px;border-radius:3px;display:inline-block}
.panel{background:var(--card);border:1px solid var(--bd);border-radius:13px;box-shadow:var(--shadow);overflow:auto;max-height:560px;margin-top:8px}
h2{font-size:13px;font-weight:640;text-transform:uppercase;letter-spacing:.7px;color:var(--mut);margin:28px 0 4px}
.tbl{width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums}
.tbl th,.tbl td{text-align:right;padding:8px 14px;border-bottom:1px solid var(--bd);white-space:nowrap}
.tbl td:first-child,.tbl th:first-child{text-align:left}
.tbl tbody tr:last-child td{border-bottom:0}
.tbl thead th{position:sticky;top:0;background:var(--card);font-size:11px;text-transform:uppercase;letter-spacing:.5px;color:var(--mut);cursor:pointer}
.tbl thead th.sorted-desc::after{content:" \\25BC";font-size:9px}.tbl thead th.sorted-asc::after{content:" \\25B2";font-size:9px}
.dcell{position:relative}
.up .dcell b{color:var(--up)}.down .dcell b{color:var(--down)}
.db{position:absolute;left:0;bottom:0;height:3px;width:100%;pointer-events:none}
.db i{display:block;height:100%;border-radius:2px;opacity:.7}
.up .db i{background:var(--up);float:right}.down .db i{background:var(--down);float:left}
.foot{color:var(--mut);font-size:11.5px;margin:24px 0 6px;text-align:center}
</style></head><body>
<div class="top"><h1>__TITLE__</h1><span class="sp"></span>
<button class="tg" id="themeBtn" title="Toggle theme">◐</button></div>
<div class="wrap">
<div class="big __DCLASS__">Total Δ __DELTA__</div>
<div class="sub">__SUBLINE__</div>
__SECDELTA__
<h2>Per-module change</h2><div class="panel">__TABLE__</div>
<div class="foot">__META__</div>
</div>
<script>
(function(){var el=document.documentElement,b=document.getElementById('themeBtn');
 var s=null;try{s=localStorage.getItem('binsize-theme');}catch(e){}if(s)el.setAttribute('data-theme',s);
 b.onclick=function(){var n=el.getAttribute('data-theme')=='dark'?'light':'dark';el.setAttribute('data-theme',n);try{localStorage.setItem('binsize-theme',n);}catch(e){}};
 var t=document.getElementById('difftbl');if(t){var hs=t.tHead.rows[0].cells;for(var i=0;i<hs.length;i++){(function(i){hs[i].style.cursor='pointer';hs[i].onclick=function(){
  var asc=!hs[i].classList.contains('sorted-asc');for(var k=0;k<hs.length;k++)hs[k].classList.remove('sorted-asc','sorted-desc');hs[i].classList.add(asc?'sorted-asc':'sorted-desc');
  var rows=[].slice.call(t.tBodies[0].rows);rows.sort(function(a,b){var ca=a.cells[i],cb=b.cells[i];
    if(ca.dataset.v!==undefined||cb.dataset.v!==undefined){var x=+ca.dataset.v||0,y=+cb.dataset.v||0;return asc?x-y:y-x;}
    var sx=(ca.textContent||'').trim(),sy=(cb.textContent||'').trim();return asc?sx.localeCompare(sy):sy.localeCompare(sx);});
  rows.forEach(function(r){t.tBodies[0].appendChild(r);});};})(i);}}
}());
</script></body></html>"""


def _load_agg(path: Path) -> Dict[str, object]:
    text = path.read_text(encoding="utf-8", errors="replace")
    if path.suffix == ".json":
        doc = json.loads(text)
        return {k: doc[k] for k in ("total", "sections", "modules", "objects")}
    return aggregate(parse_map(text))


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Binary composition report from a linker map.")
    ap.add_argument("inputs", nargs="+", help="<map> <out.html> OR <base> <new> <out.html> --diff")
    ap.add_argument("--diff", action="store_true", help="diff two maps (base new out.html)")
    ap.add_argument("--title", default="Binary")
    ap.add_argument("--json", dest="json_out", default=None, help="also write JSON BOM")
    ap.add_argument("--baseline", default=None,
                    help="single mode: a baseline map/json to embed a 'vs baseline' diff view")
    ap.add_argument("--commit", default=None,
                    help="build label/hash for footer provenance (default: repo git short hash)")
    ap.add_argument("--base-commit", default=None,
                    help="diff/baseline: version label/hash of the BASE build (shown alongside it)")
    args = ap.parse_args(argv)

    commit = args.commit or _git_hash()
    ts = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%MZ")
    pref = (commit + " · ") if commit else ""

    if args.diff:
        if len(args.inputs) != 3:
            ap.error("--diff requires: <base.map> <new.map> <out.html>")
        base, new, out = (Path(args.inputs[0]), Path(args.inputs[1]), Path(args.inputs[2]))
        ba, na = _load_agg(base), _load_agg(new)
        meta = f"{pref}{base.name} vs {new.name} · generated {ts}"
        Path(out).write_text(
            render_diff_html(ba, na, title=args.title, meta=meta,
                             base_label=args.base_commit or "", new_label=commit),
            encoding="utf-8")
        print(f"[binsize] diff report -> {out}")
        return 0

    if len(args.inputs) != 2:
        ap.error("single mode requires: <map> <out.html>")
    src, out = Path(args.inputs[0]), Path(args.inputs[1])
    agg = _load_agg(src)
    base_agg = _load_agg(Path(args.baseline)) if args.baseline else None
    meta = f"{pref}{src.name} · generated {ts}"
    Path(out).write_text(
        render_html(agg, title=args.title, base_agg=base_agg, meta=meta,
                    base_label=args.base_commit or ""),
        encoding="utf-8")
    if args.json_out:
        Path(args.json_out).write_text(to_json(agg), encoding="utf-8")
    print(f"[binsize] report -> {out} (total {fmt_bytes(agg['total'])})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
