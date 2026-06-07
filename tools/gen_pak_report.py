#!/usr/bin/env python3
"""
gen_pak_report.py  ── Generate a self-contained HTML report from either a
jce_pak manifest (.cmake) or a `jce.pakbom.v1` JSON bill-of-materials
(emitted by `jce_pak --inspect <pak> --json out.json`).

Usage:
    python gen_pak_report.py <manifest.cmake | bom.json> <output.html> [--title "..."]

The input type is auto-detected (by extension and leading byte). The JSON
source additionally surfaces archive content hashes (XXH3-64 data/index),
per-entry path hash + CRC32, dictionary tags + per-dict usage counts,
per-entry audit badges (page-aligned / encrypted / duplicate / verify state),
and duplicate/verify summary cards (verify state requires `--verify`).

The HTML report is fully self-contained (no external deps). The UI is modelled
on the GraalVM native-image build report: a fixed left sidebar with icon
navigation, a banner hero with a compression-ratio headline plus a multi-segment
artifact-breakdown bar (compression + PAK composition), themed summary cards, a
light/dark toggle (persisted), scroll-spy nav highlighting, and pure-SVG
visualisations that follow the active theme — including an interactive,
click-to-zoom squarified treemap with a breadcrumb. It provides:
  * Hero compression ratio + raw→compressed→saved + PAK composition bars
  * Summary cards (file count, raw size, compressed size, PAK total, STORED)
  * Archive hash panel (JSON source only)
  * Sortable / filterable table of all assets (path / size / compressed / ratio [+ hashes])
  * Aggregation by file extension and top-level folder
  * Top 20 largest assets bar chart (pure CSS, no JS libs)
  * SVG donut by extension, log-log scatter (raw vs compressed),
    and an interactive zoomable treemap (folder × extension) — pure SVG, no libs.
"""
from __future__ import annotations

import argparse
import html
import json
import os
import re
import sys
from collections import defaultdict
from pathlib import Path
from typing import Dict, List, Tuple


# ──────────────────────────────────────────────────────────────────────
# Manifest parsing
# ──────────────────────────────────────────────────────────────────────

_SET_QUOTED_RE = re.compile(r'set\(\s*(\w+)\s+"([^"]*)"\s*\)', re.MULTILINE | re.DOTALL)
_SET_PLAIN_RE = re.compile(r'set\(\s*(\w+)\s+([^"\s)][^)]*?)\s*\)', re.MULTILINE)


def parse_manifest(path: Path) -> Dict[str, object]:
    text = path.read_text(encoding="utf-8")
    raw: Dict[str, str] = {}
    for m in _SET_QUOTED_RE.finditer(text):
        raw[m.group(1)] = m.group(2)
    for m in _SET_PLAIN_RE.finditer(text):
        raw.setdefault(m.group(1), m.group(2).strip())

    paths = raw.get("ASSET_PATHS", "").split(";") if raw.get("ASSET_PATHS") else []
    sizes = [int(x) for x in raw.get("ASSET_SIZES", "").split(";") if x]
    comp = [int(x) for x in raw.get("ASSET_COMPRESSED", "").split(";") if x]
    flags_raw = raw.get("ASSET_FLAGS", "")
    flags = [int(x) for x in flags_raw.split(";") if x] if flags_raw else [0] * len(paths)

    if not (len(paths) == len(sizes) == len(comp)):
        raise SystemExit(
            f"manifest field length mismatch: paths={len(paths)} sizes={len(sizes)} comp={len(comp)}"
        )
    if len(flags) != len(paths):
        flags = (flags + [0] * len(paths))[: len(paths)]

    return {
        "paths": paths,
        "sizes": sizes,
        "compressed": comp,
        "flags": flags,
        "raw_total": int(raw.get("ASSET_RAW_TOTAL", "0") or 0),
        "comp_total": int(raw.get("ASSET_COMP_TOTAL", "0") or 0),
        "pak_total": int(raw.get("ASSET_PAK_TOTAL", "0") or 0),
        "file_count": int(raw.get("ASSET_FILE_COUNT", "0") or 0),
        "stored_count": int(raw.get("ASSET_STORED_COUNT", "0") or 0),
        "stored_raw_total": int(raw.get("ASSET_STORED_RAW_TOTAL", "0") or 0),
        "hashes": None,
        "crcs": None,
        "header": None,
    }


def parse_bom_json(path: Path) -> Dict[str, object]:
    """Parse a `jce.pakbom.v1` JSON document emitted by `jce_pak --inspect`."""
    doc = json.loads(path.read_text(encoding="utf-8"))
    schema = doc.get("schema", "")
    if schema != "jce.pakbom.v1":
        raise SystemExit(f"unsupported BOM schema: {schema!r} (expected jce.pakbom.v1)")

    hdr = doc.get("header", {}) or {}
    totals = doc.get("totals", {}) or {}
    entries = doc.get("entries", []) or []

    paths, sizes, comp, flags, hashes, crcs = [], [], [], [], [], []
    comp_names, dict_ids, page_aligned, encrypted, dup_e, verified_e = [], [], [], [], [], []
    dict_usage: Dict[int, int] = {}
    stored_raw_total = 0
    for i, e in enumerate(entries):
        p = e.get("path") or f"<hash:{e.get('path_hash', i)}>"
        orig = int(e.get("original_size", 0))
        stored = int(e.get("stored_size", 0))
        is_stored = int(e.get("compression", 0)) == 0
        paths.append(p)
        sizes.append(orig)
        comp.append(stored)
        # Flag bit 0 == STORED/uncompressed, matching the .cmake manifest convention.
        flags.append(1 if is_stored else 0)
        hashes.append(str(e.get("path_hash", "")))
        crcs.append(str(e.get("content_crc", "")))
        comp_names.append(str(e.get("compression_name", "")))
        did = e.get("dict_id")
        dict_ids.append(did)
        if did is not None:
            dict_usage[int(did)] = dict_usage.get(int(did), 0) + 1
        page_aligned.append(bool(e.get("page_aligned", False)))
        encrypted.append(bool(e.get("encrypted", False)))
        dup_e.append(bool(e.get("duplicate", False)))
        verified_e.append(e.get("verified"))
        if is_stored:
            stored_raw_total += orig

    return {
        "paths": paths,
        "sizes": sizes,
        "compressed": comp,
        "flags": flags,
        "raw_total": int(totals.get("original_size", 0)),
        "comp_total": int(totals.get("stored_size", 0)),
        "pak_total": int(doc.get("file_size", 0)),
        "file_count": int(totals.get("entries", len(entries))),
        "stored_count": int(totals.get("stored_count", 0)),
        "stored_raw_total": stored_raw_total,
        "hashes": hashes,
        "crcs": crcs,
        "comp_names": comp_names,
        "dict_ids": dict_ids,
        "page_aligned": page_aligned,
        "encrypted": encrypted,
        "duplicate": dup_e,
        "verified": verified_e,
        "dict_usage": dict_usage,
        "totals": totals,
        "header": hdr,
        "file": doc.get("file", ""),
        "dictionaries": doc.get("dictionaries", []) or [],
    }


def _looks_like_json(path: Path) -> bool:
    if path.suffix.lower() == ".json":
        return True
    try:
        with path.open("r", encoding="utf-8") as fh:
            for ch in iter(lambda: fh.read(1), ""):
                if ch.isspace():
                    continue
                return ch in "{["
    except OSError:
        return False
    return False


# ──────────────────────────────────────────────────────────────────────
# Aggregation helpers
# ──────────────────────────────────────────────────────────────────────


def fmt_bytes(n: int) -> str:
    units = ["B", "KB", "MB", "GB"]
    f = float(n)
    for u in units:
        if f < 1024.0 or u == units[-1]:
            return f"{f:.2f} {u}" if u != "B" else f"{int(f)} B"
        f /= 1024.0
    return f"{n} B"


def aggregate(paths: List[str], sizes: List[int], comp: List[int],
              key_fn) -> List[Tuple[str, int, int, int]]:
    bucket: Dict[str, List[int]] = defaultdict(lambda: [0, 0, 0])
    for p, s, c in zip(paths, sizes, comp):
        b = bucket[key_fn(p)]
        b[0] += 1
        b[1] += s
        b[2] += c
    out = [(k, v[0], v[1], v[2]) for k, v in bucket.items()]
    out.sort(key=lambda r: r[2], reverse=True)
    return out


def ext_key(p: str) -> str:
    base = os.path.basename(p)
    if "." not in base:
        return "(none)"
    return "." + base.rsplit(".", 1)[1].lower()


def folder_key(p: str) -> str:
    parts = p.replace("\\", "/").split("/", 1)
    return parts[0] if len(parts) > 1 else "(root)"


def build_treemap_data(paths: List[str], sizes: List[int]) -> Dict[str, object]:
    """Folder → extension size tree for the client-side interactive treemap."""
    folders: Dict[str, Dict[str, int]] = defaultdict(lambda: defaultdict(int))
    for p, s in zip(paths, sizes):
        folders[folder_key(p)][ext_key(p)] += s
    ftot = sorted(((k, sum(v.values())) for k, v in folders.items()),
                  key=lambda x: x[1], reverse=True)
    out_folders = []
    for k, tot in ftot:
        exts = sorted(folders[k].items(), key=lambda e: e[1], reverse=True)
        out_folders.append({
            "name": k, "total": tot,
            "exts": [{"ext": ek, "size": ev} for ek, ev in exts],
        })
    return {"totalRaw": sum(t for _, t in ftot), "folders": out_folders}


# ──────────────────────────────────────────────────────────────────────
# HTML rendering
# ──────────────────────────────────────────────────────────────────────


# Shared colour palette for SVG charts AND the JS treemap (kept in lockstep).
_PALETTE = [
    "#4fc3f7", "#66bb6a", "#ffa726", "#ce93d8", "#f06292", "#9575cd",
    "#4dd0e1", "#aed581", "#ff8a65", "#ba68c8", "#7986cb", "#dce775",
    "#a1887f", "#90a4ae", "#fff176", "#4db6ac",
]


def _icon_data_uri() -> str:
    """Return the JCE brand icon as a small base64 PNG data-URI, or "".

    Primary source is the committed sidecar `tools/_jce_icon.b64` (so no image
    library is needed at build time). If absent, regenerate from the repo icon
    via Pillow (keeps it current). Returns "" if neither is available — the
    report then falls back to a text "JCE" brand mark.
    """
    here = Path(__file__).resolve().parent
    sidecar = here / "_jce_icon.b64"
    try:
        if sidecar.is_file():
            uri = sidecar.read_text(encoding="ascii").strip()
            if uri.startswith("data:image"):
                return uri
    except OSError:
        pass
    try:
        from PIL import Image
        import base64
        import io
        src = here.parent / "engine" / "resources" / "JCE_icon.png"
        if src.is_file():
            im = Image.open(src).convert("RGBA")
            im.thumbnail((64, 64), Image.LANCZOS)
            buf = io.BytesIO()
            im.save(buf, format="PNG", optimize=True)
            return "data:image/png;base64," + base64.b64encode(buf.getvalue()).decode("ascii")
    except Exception:
        pass
    return ""


HTML_TEMPLATE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="color-scheme" content="light dark">
{favicon}
<title>{title}</title>
<style>
/* ── design tokens: auto (system) with manual override ───────────── */
:root {{
  color-scheme: light dark;
  --bg: #fbfbfc; --surface: #ffffff; --surface-2: #f4f5f7; --surface-3: #eceef2;
  --ink: #16181d; --muted: #6b7280; --border: #e9eaee;
  --accent: #4f46e5; --accent-2: #0ea5e9;
  --good: #12a150; --warn: #c2790f; --purple: #7c5cfc;
  --seg-comp: #4f46e5; --seg-saved: #10b981; --seg-payload: #4f46e5;
  --seg-dict: #8b5cf6; --seg-overhead: #cdd3dd;
  --shadow: 0 1px 2px rgba(16,18,29,.04), 0 1px 3px rgba(16,18,29,.05);
  --shadow-lg: 0 14px 44px rgba(16,18,29,.16);
  --ring: color-mix(in srgb, var(--accent) 22%, transparent);
  --grid: var(--border); --svg-fg: var(--ink); --svg-muted: var(--muted); --svg-stroke: var(--surface);
  --mono: ui-monospace, "SF Mono", "JetBrains Mono", "Cascadia Code", Consolas, monospace;
  --sans: "Inter", -apple-system, "Segoe UI", system-ui, Roboto, Helvetica, Arial, sans-serif;
  --radius: 14px;
}}
:root[data-theme="dark"] {{
  color-scheme: dark;
  --bg: #09090b; --surface: #111114; --surface-2: #17181c; --surface-3: #202127;
  --ink: #f3f4f6; --muted: #9197a3; --border: #25262d;
  --accent: #818cf8; --accent-2: #38bdf8;
  --good: #34d399; --warn: #fbbf24; --purple: #a78bfa;
  --seg-comp: #818cf8; --seg-saved: #34d399; --seg-payload: #818cf8;
  --seg-dict: #a78bfa; --seg-overhead: #3a3d47;
  --shadow: 0 1px 2px rgba(0,0,0,.4); --shadow-lg: 0 14px 44px rgba(0,0,0,.55);
  --svg-stroke: var(--surface);
}}
@media (prefers-color-scheme: dark) {{
  :root:not([data-theme]) {{
    color-scheme: dark;
    --bg: #09090b; --surface: #111114; --surface-2: #17181c; --surface-3: #202127;
    --ink: #f3f4f6; --muted: #9197a3; --border: #25262d;
    --accent: #818cf8; --accent-2: #38bdf8;
    --good: #34d399; --warn: #fbbf24; --purple: #a78bfa;
    --seg-comp: #818cf8; --seg-saved: #34d399; --seg-payload: #818cf8;
    --seg-dict: #a78bfa; --seg-overhead: #3a3d47;
    --shadow: 0 1px 2px rgba(0,0,0,.4); --shadow-lg: 0 14px 44px rgba(0,0,0,.55);
    --svg-stroke: var(--surface);
  }}
}}
* {{ box-sizing: border-box; }}
html {{ scroll-behavior: smooth; }}
body {{ margin: 0; background: var(--bg); color: var(--ink); font-family: var(--sans);
        font-size: 14px; line-height: 1.6; -webkit-font-smoothing: antialiased;
        text-rendering: optimizeLegibility; }}
a {{ color: inherit; text-decoration: none; }}
::selection {{ background: color-mix(in srgb, var(--accent) 26%, transparent); }}

/* ── sticky top bar (opaque so scrolled content never bleeds through) ─ */
.topbar {{ position: sticky; top: 0; z-index: 30;
           background: var(--surface);
           border-bottom: 1px solid var(--border); }}
.tb-inner {{ max-width: 1080px; margin: 0 auto; padding: 0 28px; height: 60px;
             display: flex; align-items: center; gap: 18px; }}
.brand {{ display: flex; align-items: center; gap: 10px; font-weight: 700; letter-spacing: -.01em; }}
.brand-mark {{ width: 30px; height: 30px; border-radius: 9px; flex: none;
               background: linear-gradient(135deg, var(--accent), var(--accent-2));
               color: #fff; font-weight: 800; font-size: 11px; letter-spacing: .02em;
               display: flex; align-items: center; justify-content: center;
               box-shadow: 0 3px 10px color-mix(in srgb, var(--accent) 45%, transparent); }}
.brand-img {{ background: none; padding: 0; overflow: hidden;
              box-shadow: 0 1px 5px rgba(16,18,29,.22); }}
.brand-img img {{ width: 100%; height: 100%; object-fit: cover; display: block; }}
.brand small {{ color: var(--muted); font-weight: 500; font-size: 12px; margin-left: 2px; }}
.nav {{ display: flex; gap: 2px; margin-left: 14px; flex: 1; overflow-x: auto;
        scrollbar-width: none; }}
.nav::-webkit-scrollbar {{ display: none; }}
.nav-link {{ position: relative; padding: 8px 12px; border-radius: 8px; white-space: nowrap;
             color: var(--muted); font-weight: 500; font-size: 13.5px;
             display: flex; align-items: center; gap: 7px; transition: color .12s, background .12s; }}
.nav-link .ni {{ width: 15px; height: 15px; flex: none; stroke: currentColor;
                 fill: none; stroke-width: 1.7; stroke-linecap: round; stroke-linejoin: round; }}
.nav-link:hover {{ color: var(--ink); background: var(--surface-2); }}
.nav-link.active {{ color: var(--accent); }}
.nav-link.active::after {{ content: ""; position: absolute; left: 12px; right: 12px; bottom: -1px;
                           height: 2px; border-radius: 2px; background: var(--accent); }}
.tb-actions {{ display: flex; align-items: center; gap: 8px; }}
.icon-btn {{ display: inline-flex; align-items: center; gap: 7px; cursor: pointer;
             background: var(--surface-2); border: 1px solid var(--border); color: var(--ink);
             padding: 7px 11px; border-radius: 9px; font-size: 13px; font-family: var(--sans);
             transition: border-color .12s, color .12s; }}
.icon-btn:hover {{ border-color: var(--accent); color: var(--accent); }}
#theme-txt {{ font-size: 12.5px; }}

/* ── content column ──────────────────────────────────────────────── */
.wrap {{ max-width: 1080px; margin: 0 auto; padding: 40px 28px 80px; }}
.eyebrow {{ font-size: 11px; font-weight: 600; letter-spacing: .12em; text-transform: uppercase;
            color: var(--muted); }}
.page-head {{ margin-bottom: 6px; }}
.page-head h1 {{ font-size: 30px; font-weight: 780; letter-spacing: -.025em; margin: 7px 0 5px; }}
.page-head .subtitle {{ color: var(--muted); margin: 0; }}
section {{ scroll-margin-top: 80px; }}
.sec-head {{ margin: 50px 0 18px; }}
.sec-head .eyebrow {{ color: var(--accent); }}
.sec-head h2 {{ font-size: 20px; font-weight: 760; letter-spacing: -.015em; margin: 5px 0 0; }}
.subhead {{ font-size: 12px; font-weight: 650; text-transform: uppercase; letter-spacing: .06em;
            color: var(--muted); margin: 26px 0 11px; }}

/* ── hero ────────────────────────────────────────────────────────── */
.hero {{ background: var(--surface); border: 1px solid var(--border); border-radius: 18px;
         padding: 30px 32px; box-shadow: var(--shadow); margin: 20px 0 16px; }}
.hero-row {{ display: flex; justify-content: space-between; align-items: flex-start;
             gap: 28px; flex-wrap: wrap; }}
.hero-name {{ font-family: var(--mono); font-size: 23px; font-weight: 700; letter-spacing: -.01em;
              margin-top: 7px; word-break: break-all; }}
.hero-meta {{ color: var(--muted); font-size: 13px; margin-top: 9px; font-variant-numeric: tabular-nums; }}
.hero-meta b {{ color: var(--ink); font-weight: 650; }}
.hero-ratio-wrap {{ text-align: right; flex: none; }}
.hero-ratio {{ font-size: 58px; line-height: 1; margin-bottom: 4px; font-weight: 820;
               letter-spacing: -.04em; color: var(--accent); font-variant-numeric: tabular-nums; }}
.hero-ratio span {{ font-size: 30px; font-weight: 600; opacity: .55; margin-left: 1px; }}
.bars {{ margin-top: 28px; display: grid; gap: 20px; }}
.bar-head {{ display: flex; justify-content: space-between; align-items: baseline; margin-bottom: 9px; }}
.bar-name {{ font-size: 12px; font-weight: 650; text-transform: uppercase; letter-spacing: .06em; }}
.bar-total {{ color: var(--muted); font-size: 12px; font-family: var(--mono); }}
.hero-bar {{ display: flex; height: 12px; border-radius: 999px; overflow: hidden; background: var(--surface-2); }}
.seg {{ height: 100%; transition: filter .12s; }}
.seg:hover {{ filter: brightness(1.1); }}
.seg + .seg {{ box-shadow: inset 1.5px 0 0 var(--surface); }}
.seg-comp {{ background: var(--seg-comp); }}
.seg-saved {{ background: var(--seg-saved); }}
.seg-payload {{ background: var(--seg-payload); }}
.seg-dict {{ background: var(--seg-dict); }}
.seg-overhead {{ background: var(--seg-overhead); }}
.bar-legend {{ display: flex; flex-wrap: wrap; gap: 6px 18px; margin-top: 10px;
               color: var(--muted); font-size: 12px; font-variant-numeric: tabular-nums; }}
.bar-legend .dot {{ display: inline-block; width: 9px; height: 9px; border-radius: 3px;
                    margin-right: 6px; vertical-align: middle; }}
.dot-comp {{ background: var(--seg-comp); }}
.dot-saved {{ background: var(--seg-saved); }}
.dot-payload {{ background: var(--seg-payload); }}
.dot-dict {{ background: var(--seg-dict); }}
.dot-overhead {{ background: var(--seg-overhead); }}

/* ── KPI strip ───────────────────────────────────────────────────── */
.kpis {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(140px, 1fr));
         border: 1px solid var(--border); border-radius: var(--radius); overflow: hidden;
         background: var(--surface); box-shadow: var(--shadow); margin-bottom: 16px; }}
.kpi {{ padding: 17px 19px; border-right: 1px solid var(--border); }}
.kpi:last-child {{ border-right: none; }}
.kpi-label {{ font-size: 11px; text-transform: uppercase; letter-spacing: .05em; color: var(--muted); }}
.kpi-value {{ font-size: 24px; font-weight: 760; margin-top: 6px; letter-spacing: -.02em;
              font-variant-numeric: tabular-nums; }}
.kpi-sub {{ font-size: 12px; color: var(--muted); margin-top: 3px; }}

/* ── hash panel ──────────────────────────────────────────────────── */
.hashbar {{ display: flex; flex-wrap: wrap; gap: 10px 20px; margin: 0; padding: 14px 17px;
            background: var(--surface); border: 1px solid var(--border); border-radius: var(--radius);
            font-family: var(--mono); font-size: 12.5px; box-shadow: var(--shadow); }}
.hashbar b {{ color: var(--accent); font-weight: 600; }}
.hashbar span {{ color: var(--ink); }}

/* ── tables ──────────────────────────────────────────────────────── */
table {{ width: 100%; border-collapse: collapse; background: var(--surface);
         border: 1px solid var(--border); border-radius: var(--radius); overflow: hidden;
         font-size: 13px; box-shadow: var(--shadow); }}
th, td {{ padding: 10px 13px; text-align: left; border-bottom: 1px solid var(--border); }}
tbody tr:last-child td {{ border-bottom: none; }}
th {{ background: var(--surface-2); color: var(--muted); font-weight: 600;
      cursor: pointer; user-select: none; white-space: nowrap; }}
th.num {{ text-align: right; }}
th:hover {{ color: var(--accent); }}
th.sort-asc::after {{ content: " ↑"; color: var(--accent); }}
th.sort-desc::after {{ content: " ↓"; color: var(--accent); }}
td.num {{ text-align: right; font-variant-numeric: tabular-nums; }}
td.hash {{ font-family: var(--mono); font-size: 12px; color: var(--muted); white-space: nowrap; }}
tbody tr:nth-child(even) td {{ background: color-mix(in srgb, var(--surface-2) 42%, transparent); }}
tr:hover td {{ background: var(--surface-2); }}
.ratio-good {{ color: var(--good); font-weight: 600; }}
.ratio-bad {{ color: var(--warn); font-weight: 600; }}
.ratio-stored {{ color: var(--purple); font-weight: 700; }}
td.audit {{ text-align: center; white-space: nowrap; }}
.badge {{ display: inline-block; min-width: 22px; padding: 1px 6px; border-radius: 6px;
          font-size: 11px; font-weight: 700; font-family: var(--mono); }}
.badge.off {{ color: var(--muted); opacity: .5; }}
.badge.ok {{ color: var(--good); background: color-mix(in srgb, var(--good) 16%, transparent); }}
.badge.bad {{ color: #e5484d; background: color-mix(in srgb, #e5484d 16%, transparent); }}
.badge.warn {{ color: var(--warn); background: color-mix(in srgb, var(--warn) 18%, transparent); }}
.badge.info {{ color: var(--accent); background: color-mix(in srgb, var(--accent) 16%, transparent); }}

/* ── top-N bars ──────────────────────────────────────────────────── */
.top-list {{ background: var(--surface); border: 1px solid var(--border); border-radius: var(--radius);
             padding: 15px 19px; box-shadow: var(--shadow); }}
.bar-row {{ display: grid; grid-template-columns: 300px 1fr 96px; gap: 14px; align-items: center;
            padding: 5px 0; font-variant-numeric: tabular-nums; font-size: 12px; }}
.bar-row .name {{ overflow: hidden; text-overflow: ellipsis; white-space: nowrap; color: var(--ink);
                  font-family: var(--mono); font-size: 11.5px; }}
.bar-track {{ background: var(--surface-2); border-radius: 999px; overflow: hidden; height: 9px; }}
.bar {{ height: 100%; border-radius: 999px;
        background: linear-gradient(90deg, var(--accent), var(--accent-2)); }}
.bar-row .size {{ color: var(--muted); text-align: right; }}

/* ── search + viz ────────────────────────────────────────────────── */
.search {{ background: var(--surface); border: 1px solid var(--border); color: var(--ink);
           padding: 9px 13px; border-radius: 9px; font-size: 13px; width: 320px; margin-bottom: 11px;
           font-family: var(--sans); }}
.search:focus {{ outline: none; border-color: var(--accent); box-shadow: 0 0 0 3px var(--ring); }}
.search::placeholder {{ color: var(--muted); }}
.viz-grid {{ display: grid; grid-template-columns: 1fr 1fr; gap: 16px; margin: 0 0 16px; }}
@media (max-width: 900px) {{ .viz-grid {{ grid-template-columns: 1fr; }} }}
.viz {{ background: var(--surface); border: 1px solid var(--border); border-radius: var(--radius);
        padding: 16px 18px; box-shadow: var(--shadow); }}
.viz h3 {{ margin: 0 0 12px; font-size: 12px; color: var(--muted);
           text-transform: uppercase; letter-spacing: .05em; font-weight: 600; }}
.viz svg {{ display: block; width: 100%; height: auto; }}
.legend {{ display: flex; flex-wrap: wrap; gap: 5px 14px; margin-top: 12px; font-size: 11px; color: var(--muted); }}
.legend .sw {{ display: inline-block; width: 10px; height: 10px; border-radius: 3px;
               margin-right: 5px; vertical-align: middle; }}
.legend .lg-item {{ cursor: default; border-radius: 5px; padding: 1px 4px; margin: -1px -4px;
                    transition: background .12s, color .12s; }}
.legend .lg-item.hot {{ background: var(--surface-2); color: var(--ink); }}

/* donut interactivity */
.donut .slice {{ transition: transform .14s ease, opacity .14s; transform-origin: center;
                 cursor: default; }}
.donut.has-hover .slice {{ opacity: .32; }}
.donut .slice.hot {{ opacity: 1; }}

/* scatter interactivity */
.scatter .dot {{ transition: r .1s ease, fill-opacity .1s; }}
.scatter .dot:hover {{ r: 5; fill-opacity: 1; }}

/* ── interactive treemap ─────────────────────────────────────────── */
.tm-toolbar {{ display: flex; align-items: center; justify-content: space-between;
               gap: 12px; margin-bottom: 12px; }}
.tm-crumb {{ display: flex; align-items: center; gap: 8px; font-size: 13px; margin-bottom: 12px;
             min-height: 20px; }}
.tm-crumb .crumb-link {{ color: var(--accent); cursor: pointer; font-weight: 500; }}
.tm-crumb .crumb-link:hover {{ text-decoration: underline; }}
.tm-crumb .crumb-sep {{ color: var(--muted); }}
.tm-crumb .crumb-cur {{ font-weight: 650; }}
.tm-hint {{ color: var(--muted); font-size: 11px; }}
.tm-host {{ position: relative; }}
.tm-host svg {{ display: block; width: 100%; height: auto; }}
.tm-cell text {{ font-family: var(--sans); fill: #0c0e12; pointer-events: none; }}
.tm-cell rect {{ stroke: var(--svg-stroke); stroke-width: 2; transition: fill-opacity .12s; }}
.tm-cell.zoomable {{ cursor: pointer; }}
.tm-cell:hover rect {{ fill-opacity: 1; stroke: var(--ink); }}
.chart-tip {{ position: fixed; z-index: 50; pointer-events: none; max-width: 290px;
           background: var(--surface); color: var(--ink); border: 1px solid var(--border);
           border-radius: 9px; padding: 7px 10px; font-size: 12px; box-shadow: var(--shadow-lg);
           font-variant-numeric: tabular-nums; }}
.chart-tip b {{ color: var(--accent); }}
footer {{ color: var(--muted); margin-top: 46px; padding-top: 18px; border-top: 1px solid var(--border);
          font-size: 11px; font-family: var(--mono); }}

@media (max-width: 720px) {{
  .tb-inner, .wrap {{ padding-left: 16px; padding-right: 16px; }}
  .hero {{ padding: 22px 20px; }}
  .hero-ratio {{ font-size: 46px; }}
  .brand small {{ display: none; }}
}}
</style>
</head>
<body>
<header class="topbar">
  <div class="tb-inner">
    <a href="#overview" class="brand">{brand_mark}PAK Report <small>· asset manifest</small></a>
    <nav class="nav">
      <a href="#overview" class="nav-link"><svg class="ni" viewBox="0 0 24 24"><path d="M3 11l9-8 9 8"/><path d="M5 10v10h14V10"/></svg>Overview</a>
      <a href="#largest" class="nav-link"><svg class="ni" viewBox="0 0 24 24"><path d="M4 20V10M10 20V4M16 20v-7M22 20H2"/></svg>Largest</a>
      <a href="#charts" class="nav-link"><svg class="ni" viewBox="0 0 24 24"><path d="M21 12a9 9 0 1 1-9-9v9z"/><path d="M12 3a9 9 0 0 1 9 9h-9z"/></svg>Charts</a>
      <a href="#breakdown" class="nav-link"><svg class="ni" viewBox="0 0 24 24"><path d="M12 3l9 5-9 5-9-5 9-5z"/><path d="M3 13l9 5 9-5"/></svg>Breakdown</a>
      <a href="#assets" class="nav-link"><svg class="ni" viewBox="0 0 24 24"><path d="M8 6h13M8 12h13M8 18h13M3 6h.01M3 12h.01M3 18h.01"/></svg>Assets</a>
    </nav>
    <div class="tb-actions">
      <button id="theme-toggle" class="icon-btn" type="button" title="Toggle theme"><span id="theme-ico">☾</span><span id="theme-txt">Dark</span></button>
    </div>
  </div>
</header>

<main class="wrap">
<header class="page-head">
  <div class="eyebrow">JCE asset report</div>
  <h1>{title}</h1>
  <p class="subtitle">Generated by gen_pak_report.py</p>
</header>

<section id="overview">
<div class="hero">
  <div class="hero-row">
    <div>
      <div class="eyebrow">PAK artifact</div>
      <div class="hero-name">{archive_name}</div>
      <div class="hero-meta"><b>{file_count}</b> files &nbsp;·&nbsp; raw <b>{raw_total_h}</b> &nbsp;·&nbsp; on disk <b>{pak_total_h}</b></div>
    </div>
    <div class="hero-ratio-wrap">
      <div class="hero-ratio">{overall_ratio:.2f}<span>&times;</span></div>
      <div class="eyebrow">compression ratio</div>
    </div>
  </div>
  <div class="bars">
    <div class="bar-block">
      <div class="bar-head"><span class="bar-name">Compression</span><span class="bar-total">raw {raw_total_h}</span></div>
      <div class="hero-bar">{comp_segs}</div>
      <div class="bar-legend">{comp_legend}</div>
    </div>
    <div class="bar-block">
      <div class="bar-head"><span class="bar-name">PAK composition</span><span class="bar-total">total {pak_total_h}</span></div>
      <div class="hero-bar">{pak_segs}</div>
      <div class="bar-legend">{pak_legend}</div>
    </div>
  </div>
</div>

<div class="kpis">
  <div class="kpi"><div class="kpi-label">Files</div><div class="kpi-value">{file_count}</div></div>
  <div class="kpi"><div class="kpi-label">Raw size</div><div class="kpi-value">{raw_total_h}</div><div class="kpi-sub">{raw_total} bytes</div></div>
  <div class="kpi"><div class="kpi-label">Compressed</div><div class="kpi-value">{comp_total_h}</div><div class="kpi-sub">{comp_total} bytes</div></div>
  <div class="kpi"><div class="kpi-label">PAK total</div><div class="kpi-value">{pak_total_h}</div><div class="kpi-sub">{overhead_h} overhead</div></div>
  <div class="kpi"><div class="kpi-label">STORED</div><div class="kpi-value">{stored_count}</div><div class="kpi-sub">{stored_pct:.1f}% of raw</div></div>
</div>
{hash_panel}
</section>

<section id="largest">
<div class="sec-head"><div class="eyebrow">Distribution</div><h2>Top 20 largest assets</h2></div>
<div class="top-list">
{top_bars}
</div>
</section>

<section id="charts">
<div class="sec-head"><div class="eyebrow">Visualisations</div><h2>Charts</h2></div>
<div class="viz-grid">
  <div class="viz" id="viz-pie">
    <h3>Raw size by extension (top 12)</h3>
    {pie_svg}
  </div>
  <div class="viz" id="viz-scatter">
    <h3>Compression scatter — raw vs compressed (log/log)</h3>
    {scatter_svg}
  </div>
</div>
<div class="viz">
  <div class="tm-toolbar">
    <h3 style="margin:0">Treemap — folders × extensions</h3>
    <span class="tm-hint">click a folder to zoom</span>
  </div>
  <nav id="tm-crumb" class="tm-crumb" aria-label="treemap breadcrumb"></nav>
  <div id="tm-host" class="tm-host">{treemap_svg}</div>
</div>
</section>

<section id="breakdown">
<div class="sec-head"><div class="eyebrow">Aggregation</div><h2>Breakdown</h2></div>
<div class="subhead">By extension</div>
{ext_table}
<div class="subhead">By top-level folder</div>
{folder_table}
</section>

<section id="assets">
<div class="sec-head"><div class="eyebrow">Inventory</div><h2>All assets (<span id="asset-count">{file_count}</span>)</h2></div>
<input class="search" id="filter" type="text" placeholder="Filter by path…">
<table id="asset-table">
<thead><tr>
  <th data-key="path">Path</th>
  <th data-key="size" data-num="1" class="num sort-desc">Raw</th>
  <th data-key="comp" data-num="1" class="num">Compressed</th>
  <th data-key="ratio" data-num="1" class="num">Ratio</th>{extra_th}
</tr></thead>
<tbody>
{rows}
</tbody>
</table>
</section>

<footer>
JCE asset report &middot; source: <code>{manifest_path}</code>
</footer>
</main>

<div id="chart-tip" class="chart-tip" hidden></div>
<script>{scripts}</script>
</body>
</html>
"""


# JS is kept OUT of the .format() template so it can use normal single braces
# and embed the treemap JSON via the __TREEMAP_DATA__ token (no brace-doubling).
JS_CODE = r"""
(function () {
  // ── theme: auto (follow system) with persisted manual override ──────
  var root = document.documentElement;
  var KEY = 'jce-report-theme';
  var mq = window.matchMedia ? window.matchMedia('(prefers-color-scheme: dark)') : null;
  var storedTheme = localStorage.getItem(KEY);
  if (storedTheme === 'light' || storedTheme === 'dark') root.setAttribute('data-theme', storedTheme);
  function effectiveTheme() {
    var d = root.getAttribute('data-theme');
    if (d) return d;
    return (mq && mq.matches) ? 'dark' : 'light';
  }
  function themeLabel() {
    var dark = effectiveTheme() === 'dark';
    var ico = document.getElementById('theme-ico');
    var txt = document.getElementById('theme-txt');
    if (ico) ico.textContent = dark ? '☀' : '☾';
    if (txt) txt.textContent = dark ? 'Light' : 'Dark';
  }
  themeLabel();
  var tbtn = document.getElementById('theme-toggle');
  if (tbtn) tbtn.addEventListener('click', function () {
    root.setAttribute('data-theme', effectiveTheme() === 'dark' ? 'light' : 'dark');
    localStorage.setItem(KEY, root.getAttribute('data-theme'));
    themeLabel();
    if (window.__tmRender) window.__tmRender();   // recolour treemap chrome
  });
  if (mq && mq.addEventListener) mq.addEventListener('change', function () {
    if (!localStorage.getItem(KEY)) { themeLabel(); if (window.__tmRender) window.__tmRender(); }
  });

  // ── scroll-spy nav highlighting ─────────────────────────────────────
  var links = Array.prototype.slice.call(document.querySelectorAll('.nav-link'));
  var map = {};
  links.forEach(function (l) { map[l.getAttribute('href').slice(1)] = l; });
  if ('IntersectionObserver' in window) {
    var obs = new IntersectionObserver(function (entries) {
      entries.forEach(function (e) {
        if (e.isIntersecting) {
          links.forEach(function (l) { l.classList.remove('active'); });
          if (map[e.target.id]) map[e.target.id].classList.add('active');
        }
      });
    }, { rootMargin: '-12% 0px -75% 0px', threshold: 0 });
    document.querySelectorAll('main section[id]').forEach(function (s) { obs.observe(s); });
  }

  // ── sortable + filterable asset table ───────────────────────────────
  var tbl = document.getElementById('asset-table');
  if (tbl && tbl.tBodies && tbl.tBodies[0] && tbl.tHead) {
    var tbody = tbl.tBodies[0];
    var headers = tbl.tHead.rows[0].cells;
    var sortKey = 'size', sortDir = -1;
    function sortRows() {
      var rows = Array.prototype.slice.call(tbody.rows);
      rows.sort(function (a, b) {
        var av = a.dataset[sortKey]; if (av === undefined || av === null) av = '';
        var bv = b.dataset[sortKey]; if (bv === undefined || bv === null) bv = '';
        var numeric = av !== '' && bv !== '' && !isNaN(+av) && !isNaN(+bv);
        var cmp = numeric ? (+av - +bv) : String(av).localeCompare(String(bv));
        return cmp * sortDir;
      });
      rows.forEach(function (r) { tbody.appendChild(r); });
      for (var i = 0; i < headers.length; i++) {
        headers[i].classList.remove('sort-asc', 'sort-desc');
        if (headers[i].dataset.key === sortKey)
          headers[i].classList.add(sortDir === 1 ? 'sort-asc' : 'sort-desc');
      }
    }
    for (var i = 0; i < headers.length; i++) {
      (function (h) {
        h.addEventListener('click', function () {
          var key = h.dataset.key;
          if (sortKey === key) sortDir = -sortDir;
          else { sortKey = key; sortDir = h.dataset.num ? -1 : 1; }
          sortRows();
        });
      })(headers[i]);
    }
    var filter = document.getElementById('filter');
    var cnt = document.getElementById('asset-count');
    if (filter) filter.addEventListener('input', function (e) {
      var q = e.target.value.toLowerCase();
      var shown = 0;
      for (var r = 0; r < tbody.rows.length; r++) {
        var row = tbody.rows[r];
        var m = row.dataset.path.toLowerCase().indexOf(q) !== -1;
        row.style.display = m ? '' : 'none';
        if (m) shown++;
      }
      if (cnt) cnt.textContent = shown;
    });
  }

  // ── shared chart tooltip ────────────────────────────────────────────
  var tip = document.getElementById('chart-tip');
  function escHtml(s) { return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;'); }
  function showTip(ev, head, rest) {
    if (!tip) return;
    tip.hidden = false;
    tip.innerHTML = '<b>' + escHtml(head) + '</b>' + (rest ? ' ' + escHtml(rest) : '');
    var tx = Math.min(ev.clientX + 14, window.innerWidth - tip.offsetWidth - 8);
    var ty = Math.min(ev.clientY + 14, window.innerHeight - tip.offsetHeight - 8);
    tip.style.left = Math.max(8, tx) + 'px'; tip.style.top = Math.max(8, ty) + 'px';
  }
  function hideTip() { if (tip) tip.hidden = true; }

  // ── interactive zoomable treemap ────────────────────────────────────
  var TM = __TREEMAP_DATA__;
  var PAL = __PALETTE__;
  var host = document.getElementById('tm-host');
  var crumbEl = document.getElementById('tm-crumb');
  if (host && crumbEl && TM && TM.folders && TM.folders.length) {
    try {
      var zoom = null;          // null = root view, else folder name
      var H = 440;
      var folderColor = {};
      TM.folders.forEach(function (f, i) { folderColor[f.name] = PAL[i % PAL.length]; });

      function fmtBytes(n) {
        var u = ['B', 'KB', 'MB', 'GB'], f = n;
        for (var i = 0; i < u.length; i++) {
          if (f < 1024 || i === u.length - 1)
            return (u[i] === 'B') ? (Math.round(f) + ' B') : (f.toFixed(2) + ' ' + u[i]);
          f /= 1024;
        }
        return n + ' B';
      }
      function escA(s) {
        return String(s).replace(/&/g, '&amp;').replace(/"/g, '&quot;')
          .replace(/</g, '&lt;').replace(/>/g, '&gt;');
      }
      function escT(s) {
        return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
      }

      // squarified treemap (port of the Python _squarify)
      function squarify(items, x, y, w, h) {
        var out = [];
        if (!items.length || w <= 0 || h <= 0) return out;
        function worst(row, length) {
          if (!row.length || length <= 0) return Infinity;
          var s = 0, rmax = -Infinity, rmin = Infinity;
          for (var i = 0; i < row.length; i++) {
            s += row[i]; if (row[i] > rmax) rmax = row[i]; if (row[i] < rmin) rmin = row[i];
          }
          return Math.max((length * length * rmax) / (s * s), (s * s) / (length * length * rmin));
        }
        var remaining = items.slice();
        var cx = x, cy = y, cw = w, ch = h;
        var curTotal = items.reduce(function (a, b) { return a + b.v; }, 0) || 1;
        while (remaining.length) {
          var length = Math.min(cw, ch);
          var scale = curTotal ? (cw * ch) / curTotal : 0;
          var scaled = remaining.map(function (it) { return it.v * scale; });
          var row = [], rowItems = [], i = 0;
          while (i < scaled.length) {
            var cand = row.concat([scaled[i]]);
            if (worst(cand, length) <= worst(row, length) || row.length === 0) {
              row = cand; rowItems.push(remaining[i]); i++;
            } else break;
          }
          var rowSum = row.reduce(function (a, b) { return a + b; }, 0) || 1;
          var j;
          if (cw <= ch) {
            var rowH = rowSum / cw, ox = cx;
            for (j = 0; j < rowItems.length; j++) {
              var ww = rowH ? row[j] / rowH : 0;
              out.push({ k: rowItems[j].k, v: rowItems[j].v, x: ox, y: cy, w: ww, h: rowH });
              ox += ww;
            }
            cy += rowH; ch -= rowH;
          } else {
            var rowW = rowSum / ch, oy = cy;
            for (j = 0; j < rowItems.length; j++) {
              var hh = rowW ? row[j] / rowW : 0;
              out.push({ k: rowItems[j].k, v: rowItems[j].v, x: cx, y: oy, w: rowW, h: hh });
              oy += hh;
            }
            cx += rowW; cw -= rowW;
          }
          var consumed = 0;
          for (j = 0; j < i; j++) consumed += remaining[j].v;
          curTotal -= consumed;
          remaining = remaining.slice(i);
        }
        return out;
      }

      function cellSvg(c, fill, opacity, label, sublabel, tipTxt, zoomable) {
        var s = '<g class="tm-cell' + (zoomable ? ' zoomable' : '') + '"'
          + (zoomable ? ' data-folder="' + escA(c.k) + '"' : '')
          + ' data-tip="' + escA(tipTxt) + '">';
        s += '<rect x="' + c.x.toFixed(2) + '" y="' + c.y.toFixed(2)
          + '" width="' + c.w.toFixed(2) + '" height="' + c.h.toFixed(2)
          + '" rx="3" fill="' + fill + '" fill-opacity="' + opacity + '"></rect>';
        if (c.w > 54 && c.h > 17) {
          s += '<text x="' + (c.x + 6).toFixed(2) + '" y="' + (c.y + 15).toFixed(2)
            + '" font-size="12" font-weight="650">' + escT(label) + '</text>';
          if (c.h > 33 && sublabel)
            s += '<text x="' + (c.x + 6).toFixed(2) + '" y="' + (c.y + 29).toFixed(2)
              + '" font-size="10" fill-opacity="0.78">' + escT(sublabel) + '</text>';
        }
        s += '</g>';
        return s;
      }

      function render() {
        var w = Math.max(320, host.clientWidth || 900);
        var svg = '<svg viewBox="0 0 ' + w + ' ' + H + '" preserveAspectRatio="xMidYMid meet">';
        if (zoom === null) {
          var items = TM.folders.map(function (f) { return { k: f.name, v: f.total }; });
          squarify(items, 0, 0, w, H).forEach(function (c) {
            if (c.w < 2 || c.h < 2) return;
            var pct = TM.totalRaw ? (c.v * 100 / TM.totalRaw) : 0;
            svg += cellSvg(c, folderColor[c.k] || '#888', 0.85, c.k, fmtBytes(c.v),
              c.k + ' — ' + fmtBytes(c.v) + ' (' + pct.toFixed(1) + '%) · click to zoom', true);
          });
        } else {
          var f = null;
          for (var i = 0; i < TM.folders.length; i++) if (TM.folders[i].name === zoom) f = TM.folders[i];
          if (!f) { zoom = null; render(); return; }
          var base = folderColor[zoom] || '#888';
          var eitems = f.exts.map(function (e) { return { k: e.ext, v: e.size }; });
          squarify(eitems, 0, 0, w, H).forEach(function (c, j) {
            if (c.w < 2 || c.h < 2) return;
            var pct = f.total ? (c.v * 100 / f.total) : 0;
            var op = 0.55 + 0.32 * ((j % 3) / 2);
            svg += cellSvg(c, base, op, c.k, fmtBytes(c.v),
              zoom + '/' + c.k + ' — ' + fmtBytes(c.v) + ' (' + pct.toFixed(1) + '%)', false);
          });
        }
        svg += '</svg>';
        host.innerHTML = svg;

        Array.prototype.forEach.call(host.querySelectorAll('.tm-cell'), function (el) {
          var t = el.getAttribute('data-tip');
          el.addEventListener('mousemove', function (ev) {
            showTip(ev, t.split(' — ')[0], t.split(' — ').slice(1).join(' — '));
          });
          el.addEventListener('mouseleave', hideTip);
          if (el.classList.contains('zoomable'))
            el.addEventListener('click', function () {
              zoom = el.getAttribute('data-folder'); hideTip(); renderCrumb(); render();
            });
        });
      }

      function renderCrumb() {
        if (zoom === null) {
          crumbEl.innerHTML = '<span class="crumb-cur">All folders</span>';
        } else {
          crumbEl.innerHTML = '<span class="crumb-link" role="button" tabindex="0">All folders</span>'
            + '<span class="crumb-sep">▸</span><span class="crumb-cur">' + escT(zoom) + '</span>';
          var a = crumbEl.querySelector('.crumb-link');
          a.addEventListener('click', function () { zoom = null; hideTip(); renderCrumb(); render(); });
          a.addEventListener('keydown', function (ev) {
            if (ev.key === 'Enter' || ev.key === ' ') { zoom = null; hideTip(); renderCrumb(); render(); }
          });
        }
      }

      window.__tmRender = render;
      var rt;
      window.addEventListener('resize', function () { clearTimeout(rt); rt = setTimeout(render, 120); });
      renderCrumb();
      render();
    } catch (err) {
      // On any failure, leave the server-rendered fallback SVG in place.
      if (window.console) console.warn('treemap init failed:', err);
    }
  }

  // ── donut hover: pull the slice out, dim others, update centre label ─
  (function () {
    var wrap = document.getElementById('viz-pie');
    if (!wrap) return;
    var donut = wrap.querySelector('svg.donut');
    if (!donut) return;
    var main = wrap.querySelector('#pie-main');
    var sub = wrap.querySelector('#pie-sub');
    var slices = Array.prototype.slice.call(donut.querySelectorAll('.slice'));
    var legendItems = Array.prototype.slice.call(wrap.querySelectorAll('.lg-item'));
    function setHot(idx, on) {
      donut.classList.toggle('has-hover', on);
      slices.forEach(function (s) {
        var hot = on && s.getAttribute('data-idx') === idx;
        s.classList.toggle('hot', hot);
        if (hot) s.setAttribute('transform', 'translate(' + s.getAttribute('data-dx') + ',' + s.getAttribute('data-dy') + ')');
        else s.removeAttribute('transform');
      });
      legendItems.forEach(function (l) {
        l.classList.toggle('hot', on && l.getAttribute('data-idx') === idx);
      });
      if (on && main && sub) {
        for (var i = 0; i < slices.length; i++) if (slices[i].getAttribute('data-idx') === idx) {
          main.textContent = slices[i].getAttribute('data-pct') + '%';
          sub.textContent = slices[i].getAttribute('data-key');
        }
      } else if (main && sub) {
        main.textContent = main.getAttribute('data-default');
        sub.textContent = sub.getAttribute('data-default');
      }
    }
    slices.forEach(function (s) {
      var idx = s.getAttribute('data-idx');
      var info = s.getAttribute('data-val') + ' · ' + s.getAttribute('data-pct') + '%';
      s.addEventListener('mouseenter', function (ev) { setHot(idx, true); showTip(ev, s.getAttribute('data-key'), info); });
      s.addEventListener('mousemove', function (ev) { showTip(ev, s.getAttribute('data-key'), info); });
      s.addEventListener('mouseleave', function () { setHot(idx, false); hideTip(); });
    });
    legendItems.forEach(function (l) {
      var idx = l.getAttribute('data-idx');
      l.addEventListener('mouseenter', function () { setHot(idx, true); });
      l.addEventListener('mouseleave', function () { setHot(idx, false); });
    });
  })();

  // ── scatter hover: per-point tooltip ────────────────────────────────
  (function () {
    var wrap = document.getElementById('viz-scatter');
    if (!wrap) return;
    Array.prototype.forEach.call(wrap.querySelectorAll('.dot'), function (d) {
      var parts = (d.getAttribute('data-tip') || '').split(' — ');
      d.addEventListener('mousemove', function (ev) { showTip(ev, parts[0], parts.slice(1).join(' — ')); });
      d.addEventListener('mouseleave', hideTip);
    });
  })();
})();
"""


def render_top_bars(paths: List[str], sizes: List[int], n: int = 20) -> str:
    indexed = sorted(zip(sizes, paths), reverse=True)[:n]
    if not indexed:
        return "<p>(no assets)</p>"
    max_size = indexed[0][0] or 1
    out = []
    for size, path in indexed:
        pct = (size / max_size) * 100.0
        out.append(
            f'<div class="bar-row">'
            f'<div class="name" title="{html.escape(path)}">{html.escape(path)}</div>'
            f'<div class="bar-track"><div class="bar" style="width:{pct:.1f}%"></div></div>'
            f'<div class="size">{fmt_bytes(size)}</div>'
            f'</div>'
        )
    return "\n".join(out)


def render_agg_table(rows: List[Tuple[str, int, int, int]]) -> str:
    if not rows:
        return "<p>(empty)</p>"
    out = ['<table>',
           '<thead><tr><th>Key</th><th class="num">Files</th>'
           '<th class="num">Raw</th><th class="num">Compressed</th>'
           '<th class="num">Ratio</th></tr></thead><tbody>']
    for key, n, raw, comp in rows:
        ratio = (raw / comp) if comp else 0.0
        cls = "ratio-good" if ratio >= 2.0 else ("ratio-bad" if ratio < 1.05 else "")
        out.append(
            f'<tr><td>{html.escape(key)}</td>'
            f'<td class="num">{n}</td>'
            f'<td class="num">{fmt_bytes(raw)}</td>'
            f'<td class="num">{fmt_bytes(comp)}</td>'
            f'<td class="num {cls}">{ratio:.2f}×</td></tr>'
        )
    out.append('</tbody></table>')
    return "\n".join(out)


def render_rows(paths: List[str], sizes: List[int], comp: List[int],
                flags: List[int],
                hashes: List[str] | None = None,
                crcs: List[str] | None = None,
                page_aligned: List[bool] | None = None,
                encrypted: List[bool] | None = None,
                duplicate: List[bool] | None = None,
                verified: list | None = None) -> str:
    rows: List[str] = []
    n = len(paths)
    hashes = hashes or [""] * n
    crcs = crcs or [""] * n
    page_aligned = page_aligned or [False] * n
    encrypted = encrypted or [False] * n
    duplicate = duplicate or [False] * n
    verified = verified if verified is not None else [None] * n
    have_hash = any(hashes) or any(crcs)
    have_audit = any(page_aligned) or any(encrypted) or any(duplicate) \
        or any(v is not None for v in verified)
    indexed = sorted(
        zip(sizes, comp, paths, flags, hashes, crcs,
            page_aligned, encrypted, duplicate, verified),
        reverse=True, key=lambda t: t[0])
    for size, c, path, fl, ph, crc, pa, enc, dup, ver in indexed:
        ratio = (size / c) if c else 0.0
        stored = bool(fl & 1)
        if stored:
            cls = "ratio-stored"
            ratio_html = '<span class="ratio-stored">STORED</span>'
        else:
            cls = "ratio-good" if ratio >= 2.0 else ("ratio-bad" if ratio < 1.05 else "")
            ratio_html = f'<span class="{cls}">{ratio:.2f}×</span>'
        hash_cells = ""
        if have_hash:
            hash_cells = (
                f'<td class="hash">{html.escape(str(ph))}</td>'
                f'<td class="hash">{html.escape(str(crc))}</td>'
            )
        audit_cells = ""
        if have_audit:
            def _badge(on: bool, txt: str, kind: str) -> str:
                return f'<span class="badge {kind}">{txt}</span>' if on else \
                    '<span class="badge off">·</span>'
            if ver is None:
                ver_html = '<span class="badge off">·</span>'
            elif ver:
                ver_html = '<span class="badge ok">OK</span>'
            else:
                ver_html = '<span class="badge bad">FAIL</span>'
            audit_cells = (
                f'<td class="audit">{_badge(bool(pa), "PG", "info")}</td>'
                f'<td class="audit">{_badge(bool(enc), "EN", "warn")}</td>'
                f'<td class="audit">{_badge(bool(dup), "DUP", "warn")}</td>'
                f'<td class="audit">{ver_html}</td>'
            )
        # Backing sort keys for the optional hash/audit columns, so clicking
        # those headers actually sorts (and never hits undefined.localeCompare).
        sort_attrs = ""
        if have_hash:
            sort_attrs += (f' data-phash="{html.escape(str(ph))}"'
                           f' data-crc="{html.escape(str(crc))}"')
        if have_audit:
            ver_v = "" if ver is None else ("1" if ver else "0")
            sort_attrs += (f' data-pg="{int(bool(pa))}" data-enc="{int(bool(enc))}"'
                           f' data-ver="{ver_v}"')
        rows.append(
            f'<tr data-path="{html.escape(path)}" data-size="{size}" '
            f'data-comp="{c}" data-ratio="{ratio:.4f}" data-stored="{int(stored)}" '
            f'data-dup="{int(bool(dup))}"{sort_attrs}>'
            f'<td>{html.escape(path)}</td>'
            f'<td class="num">{fmt_bytes(size)}</td>'
            f'<td class="num">{fmt_bytes(c)}</td>'
            f'<td class="num">{ratio_html}</td>{hash_cells}{audit_cells}</tr>'
        )
    return "\n".join(rows)


# ──────────────────────────────────────────────────────────────────────
# SVG visualisations (pure SVG, no external libs)
# ──────────────────────────────────────────────────────────────────────


def _color(i: int) -> str:
    return _PALETTE[i % len(_PALETTE)]


def render_pie_svg(rows: List[Tuple[str, int, int, int]],
                   max_slices: int = 12,
                   size: int = 320) -> str:
    """Donut chart by extension. rows = [(key, n, raw, comp), ...]."""
    if not rows:
        return "<p>(no data)</p>"
    sorted_rows = sorted(rows, key=lambda r: r[2], reverse=True)
    head = sorted_rows[:max_slices]
    tail = sorted_rows[max_slices:]
    if tail:
        head.append(("other", sum(r[1] for r in tail),
                     sum(r[2] for r in tail), sum(r[3] for r in tail)))

    total = sum(r[2] for r in head) or 1
    cx = cy = size / 2
    r_outer = size * 0.45
    r_inner = size * 0.27

    import math
    paths = []
    legend = []
    angle = -math.pi / 2  # start at 12 o'clock
    for i, (key, _, raw, _) in enumerate(head):
        frac = raw / total
        if frac <= 0:
            continue
        a2 = angle + frac * 2 * math.pi
        large = 1 if frac > 0.5 else 0
        x1 = cx + r_outer * math.cos(angle)
        y1 = cy + r_outer * math.sin(angle)
        x2 = cx + r_outer * math.cos(a2)
        y2 = cy + r_outer * math.sin(a2)
        x3 = cx + r_inner * math.cos(a2)
        y3 = cy + r_inner * math.sin(a2)
        x4 = cx + r_inner * math.cos(angle)
        y4 = cy + r_inner * math.sin(angle)
        title = f"{key}: {fmt_bytes(raw)} ({frac * 100:.1f}%)"
        slice_attrs = (
            f'data-idx="{i}" data-key="{html.escape(key)}" '
            f'data-val="{html.escape(fmt_bytes(raw))}" data-pct="{frac * 100:.1f}"'
        )
        if frac >= 0.999:
            # Full ring: a single elliptical arc whose start == end renders
            # nothing, so draw the slice as concentric circles (outer fill +
            # a background-coloured inner hole) to guarantee a visible ring.
            paths.append(
                f'<circle class="slice" {slice_attrs} data-dx="0" data-dy="0" '
                f'cx="{cx:.2f}" cy="{cy:.2f}" r="{r_outer:.2f}" '
                f'fill="{_color(i)}" stroke="var(--svg-stroke)" stroke-width="1.5">'
                f'<title>{html.escape(title)}</title></circle>'
                f'<circle cx="{cx:.2f}" cy="{cy:.2f}" r="{r_inner:.2f}" fill="var(--surface)"/>'
            )
        else:
            d = (f"M {x1:.2f} {y1:.2f} "
                 f"A {r_outer:.2f} {r_outer:.2f} 0 {large} 1 {x2:.2f} {y2:.2f} "
                 f"L {x3:.2f} {y3:.2f} "
                 f"A {r_inner:.2f} {r_inner:.2f} 0 {large} 0 {x4:.2f} {y4:.2f} Z")
            mid = (angle + a2) / 2.0  # bisector, for hover pull-out
            dx = math.cos(mid) * (size * 0.022)
            dy = math.sin(mid) * (size * 0.022)
            paths.append(
                f'<path class="slice" {slice_attrs} '
                f'data-dx="{dx:.2f}" data-dy="{dy:.2f}" '
                f'd="{d}" fill="{_color(i)}" stroke="var(--svg-stroke)" stroke-width="1.5">'
                f'<title>{html.escape(title)}</title></path>'
            )
        legend.append(
            f'<span class="lg-item" data-idx="{i}">'
            f'<span class="sw" style="background:{_color(i)}"></span>'
            f'{html.escape(key)} &middot; {fmt_bytes(raw)} ({frac * 100:.1f}%)</span>'
        )
        angle = a2

    centre_label = fmt_bytes(total)
    svg = (
        f'<svg viewBox="0 0 {size} {size}" preserveAspectRatio="xMidYMid meet" class="donut">'
        f'<g class="slices">{"".join(paths)}</g>'
        f'<text id="pie-main" x="{cx}" y="{cy - 3}" text-anchor="middle" '
        f'data-default="{html.escape(centre_label)}" '
        f'fill="var(--svg-fg)" font-size="15" font-weight="700">{html.escape(centre_label)}</text>'
        f'<text id="pie-sub" x="{cx}" y="{cy + 14}" text-anchor="middle" '
        f'data-default="total raw" fill="var(--svg-muted)" font-size="10">total raw</text>'
        f'</svg>'
        f'<div class="legend">{"".join(legend)}</div>'
    )
    return svg


def render_scatter_svg(sizes: List[int], comp: List[int], flags: List[int],
                       paths: List[str] | None = None,
                       width: int = 480, height: int = 320) -> str:
    """Log-log scatter of raw vs compressed size."""
    paths = paths or [""] * len(sizes)
    points = [(s, c, f, p) for s, c, f, p in zip(sizes, comp, flags, paths) if s > 0 and c > 0]
    if not points:
        return "<p>(no data)</p>"

    import math
    pad_l, pad_b, pad_t, pad_r = 44, 36, 12, 12
    plot_w = width - pad_l - pad_r
    plot_h = height - pad_t - pad_b

    log_xs = [math.log10(s) for s, _, _, _ in points]
    log_ys = [math.log10(c) for _, c, _, _ in points]
    lo = min(min(log_xs), min(log_ys))
    hi = max(max(log_xs), max(log_ys))
    if hi - lo < 0.5:
        hi = lo + 0.5

    def sx(v: float) -> float:
        return pad_l + (v - lo) / (hi - lo) * plot_w

    def sy(v: float) -> float:
        return pad_t + plot_h - (v - lo) / (hi - lo) * plot_h

    # Grid + axis labels at decade boundaries
    grid = []
    lo_dec = int(math.floor(lo))
    hi_dec = int(math.ceil(hi))
    decade_labels = {0: "1B", 1: "10B", 2: "100B", 3: "1KB", 4: "10KB",
                     5: "100KB", 6: "1MB", 7: "10MB", 8: "100MB", 9: "1GB"}
    for d in range(lo_dec, hi_dec + 1):
        x = sx(d)
        y = sy(d)
        grid.append(
            f'<line x1="{x:.1f}" y1="{pad_t}" x2="{x:.1f}" y2="{pad_t + plot_h}" '
            f'stroke="var(--grid)" stroke-width="1"/>'
        )
        grid.append(
            f'<line x1="{pad_l}" y1="{y:.1f}" x2="{pad_l + plot_w}" y2="{y:.1f}" '
            f'stroke="var(--grid)" stroke-width="1"/>'
        )
        lbl = decade_labels.get(d, f"1e{d}")
        grid.append(
            f'<text x="{x:.1f}" y="{pad_t + plot_h + 14}" text-anchor="middle" '
            f'fill="var(--svg-muted)" font-size="10">{lbl}</text>'
        )
        grid.append(
            f'<text x="{pad_l - 6}" y="{y + 3:.1f}" text-anchor="end" '
            f'fill="var(--svg-muted)" font-size="10">{lbl}</text>'
        )

    # y = x reference line (no compression baseline)
    diag = (f'<line x1="{sx(lo):.1f}" y1="{sy(lo):.1f}" '
            f'x2="{sx(hi):.1f}" y2="{sy(hi):.1f}" '
            f'stroke="var(--svg-muted)" stroke-width="1" stroke-dasharray="3,3"/>')

    dots = []
    for s, c, fl, p in points:
        x = sx(math.log10(s))
        y = sy(math.log10(c))
        stored = bool(fl & 1)
        color = "#ce93d8" if stored else ("#66bb6a" if c < s * 0.5 else "#4fc3f7")
        ratio = (s / c) if c else 0.0
        kind = "STORED" if stored else f"{ratio:.2f}×"
        name = p.rsplit("/", 1)[-1] if p else "asset"
        tip = f"{name} — raw {fmt_bytes(s)} · comp {fmt_bytes(c)} · {kind}"
        dots.append(
            f'<circle class="dot" data-tip="{html.escape(tip)}" '
            f'cx="{x:.1f}" cy="{y:.1f}" r="2.4" fill="{color}" fill-opacity="0.62"/>'
        )

    axis_labels = (
        f'<text x="{pad_l + plot_w / 2:.1f}" y="{height - 4}" text-anchor="middle" '
        f'fill="var(--svg-muted)" font-size="11">raw size</text>'
        f'<text x="14" y="{pad_t + plot_h / 2:.1f}" text-anchor="middle" '
        f'fill="var(--svg-muted)" font-size="11" '
        f'transform="rotate(-90 14 {pad_t + plot_h / 2:.1f})">compressed size</text>'
    )

    legend = (
        '<div class="legend">'
        '<span><span class="sw" style="background:#66bb6a"></span>good (≥2×)</span>'
        '<span><span class="sw" style="background:#4fc3f7"></span>compressed</span>'
        '<span><span class="sw" style="background:#ce93d8"></span>STORED</span>'
        '<span style="color:var(--muted)">— dashed: y = x (no compression)</span>'
        '</div>'
    )

    svg = (
        f'<svg viewBox="0 0 {width} {height}" preserveAspectRatio="xMidYMid meet" class="scatter">'
        f'{"".join(grid)}{diag}{"".join(dots)}{axis_labels}'
        f'</svg>{legend}'
    )
    return svg


def _squarify(items: List[Tuple[str, float]],
              x: float, y: float, w: float, h: float
              ) -> List[Tuple[str, float, float, float, float, float]]:
    """Return [(label, value, x, y, w, h), ...] using squarified treemap.

    items must be sorted descending by value.
    """
    if not items or w <= 0 or h <= 0:
        return []
    total = sum(v for _, v in items) or 1.0
    out: List[Tuple[str, float, float, float, float, float]] = []

    def worst(row, length):
        if not row or length <= 0:
            return float("inf")
        s = sum(v for _, v in row)
        rmax = max(v for _, v in row)
        rmin = min(v for _, v in row)
        return max((length * length * rmax) / (s * s),
                   (s * s) / (length * length * rmin))

    remaining = list(items)
    cx, cy, cw, ch = x, y, w, h
    cur_total = total

    while remaining:
        length = min(cw, ch)
        row: List[Tuple[str, float]] = []
        scale = (cw * ch) / cur_total if cur_total else 0.0
        scaled = [(k, v * scale) for k, v in remaining]
        i = 0
        while i < len(scaled):
            cand = row + [scaled[i]]
            if worst(cand, length) <= worst(row, length) or not row:
                row = cand
                i += 1
            else:
                break

        # lay out row along the shorter side
        row_sum = sum(v for _, v in row) or 1.0
        if cw <= ch:
            row_h = row_sum / cw
            ox = cx
            for k, v in row:
                ww = v / row_h if row_h else 0
                out.append((k, v, ox, cy, ww, row_h))
                ox += ww
            cy += row_h
            ch -= row_h
        else:
            row_w = row_sum / ch
            oy = cy
            for k, v in row:
                hh = v / row_w if row_w else 0
                out.append((k, v, cx, oy, row_w, hh))
                oy += hh
            cx += row_w
            cw -= row_w

        cur_total -= sum(v for _, v in remaining[:i])
        remaining = remaining[i:]

    return out


def render_treemap_svg(paths: List[str], sizes: List[int],
                       width: int = 960, height: int = 440) -> str:
    """Static two-level squarified treemap (server-side fallback if JS is off)."""
    if not paths or not sizes:
        return "<p>(no data)</p>"

    folders: Dict[str, Dict[str, int]] = defaultdict(lambda: defaultdict(int))
    for p, s in zip(paths, sizes):
        folders[folder_key(p)][ext_key(p)] += s

    folder_totals = sorted(((k, sum(v.values())) for k, v in folders.items()),
                           key=lambda x: x[1], reverse=True)
    folder_items: List[Tuple[str, float]] = [(k, float(v)) for k, v in folder_totals]
    cells = _squarify(folder_items, 0.0, 0.0, float(width), float(height))

    folder_color = {k: _color(i) for i, (k, _) in enumerate(folder_totals)}

    out = [f'<svg viewBox="0 0 {width} {height}" preserveAspectRatio="xMidYMid meet">']

    for k, v, x, y, w, h in cells:
        if w < 2 or h < 2:
            continue
        ext_items: List[Tuple[str, float]] = sorted(
            ((ek, float(ev)) for ek, ev in folders[k].items()),
            key=lambda e: e[1], reverse=True,
        )
        sub = _squarify(ext_items, x, y, w, h)
        base = folder_color[k]
        out.append(
            f'<rect x="{x:.2f}" y="{y:.2f}" width="{w:.2f}" height="{h:.2f}" '
            f'fill="{base}" fill-opacity="0.15"/>'
        )
        for j, (ek, ev, ex, ey, ew, eh) in enumerate(sub):
            if ew < 1 or eh < 1:
                continue
            tip = f"{k}/{ek}: {fmt_bytes(int(ev))}"
            shade = 0.55 + 0.35 * (j % 3) / 2.0
            out.append(
                f'<g class="tm-cell">'
                f'<rect x="{ex:.2f}" y="{ey:.2f}" width="{ew:.2f}" height="{eh:.2f}" '
                f'rx="3" fill="{base}" fill-opacity="{shade:.2f}">'
                f'<title>{html.escape(tip)}</title></rect>'
            )
            if ew > 60 and eh > 18:
                out.append(
                    f'<text x="{ex + 4:.2f}" y="{ey + 13:.2f}" '
                    f'font-size="11" font-weight="600">{html.escape(ek)}</text>'
                )
                if eh > 32:
                    out.append(
                        f'<text x="{ex + 4:.2f}" y="{ey + 26:.2f}" '
                        f'font-size="10" fill-opacity="0.75">'
                        f'{html.escape(fmt_bytes(int(ev)))}</text>'
                    )
            out.append('</g>')

    out.append('</svg>')
    return "".join(out)


# ──────────────────────────────────────────────────────────────────────
# Hero breakdown bars (GraalVM-style multi-segment artifact composition)
# ──────────────────────────────────────────────────────────────────────


def _seg(cls: str, pct: float, title: str) -> str:
    return f'<div class="seg {cls}" style="width:{max(pct, 0.0):.3f}%" title="{html.escape(title)}"></div>'


def _leg(dotcls: str, label: str) -> str:
    return f'<span><i class="dot {dotcls}"></i>{html.escape(label)}</span>'


def render_html(data: Dict[str, object], manifest_path: Path, title: str) -> str:
    paths = data["paths"]
    sizes = data["sizes"]
    comp = data["compressed"]
    flags = data.get("flags") or [0] * len(paths)
    hashes = data.get("hashes")
    crcs = data.get("crcs")
    header = data.get("header") or {}
    raw_total = data["raw_total"] or sum(sizes)
    comp_total = data["comp_total"] or sum(comp)
    pak_total = data["pak_total"] or comp_total
    overall = (raw_total / comp_total) if comp_total else 0.0
    stored_count = data.get("stored_count", 0)
    stored_raw = data.get("stored_raw_total", 0)
    stored_pct = (stored_raw * 100.0 / raw_total) if raw_total else 0.0

    # Compression breakdown (of raw): compressed payload + bytes saved.
    saved_total = max(raw_total - comp_total, 0)
    saved_pct = (saved_total * 100.0 / raw_total) if raw_total else 0.0
    comp_pct = (comp_total * 100.0 / raw_total) if raw_total else 0.0
    comp_total_h = fmt_bytes(comp_total)
    comp_segs = (_seg("seg-comp", comp_pct, f"Compressed payload {comp_total_h}")
                 + _seg("seg-saved", saved_pct, f"Saved {fmt_bytes(saved_total)}"))
    comp_legend = (_leg("dot-comp", f"Compressed {comp_total_h} ({comp_pct:.1f}%)")
                   + _leg("dot-saved", f"Saved {fmt_bytes(saved_total)} ({saved_pct:.1f}%)"))

    # PAK composition (of the on-disk artifact): payload + dictionaries + TOC/headers.
    dicts = data.get("dictionaries") or []
    dict_total = sum(int(d.get("size", 0)) for d in dicts)
    pak = pak_total or comp_total or 1
    payload_pct = comp_total * 100.0 / pak
    dict_pct = dict_total * 100.0 / pak
    overhead = max(pak_total - comp_total - dict_total, 0)
    overhead_pct = overhead * 100.0 / pak
    pak_segs = _seg("seg-payload", payload_pct, f"Compressed payload {comp_total_h}")
    pak_legend = _leg("dot-payload", f"Payload {comp_total_h} ({payload_pct:.1f}%)")
    if dict_total > 0:
        pak_segs += _seg("seg-dict", dict_pct, f"Dictionaries {fmt_bytes(dict_total)}")
        pak_legend += _leg("dot-dict", f"Dictionary {fmt_bytes(dict_total)} ({dict_pct:.1f}%)")
    pak_segs += _seg("seg-overhead", overhead_pct, f"TOC + headers {fmt_bytes(overhead)}")
    pak_legend += _leg("dot-overhead", f"TOC + headers {fmt_bytes(overhead)} ({overhead_pct:.1f}%)")
    # Card overhead = everything in the artifact that is not compressed payload.
    overhead_card = max(pak_total - comp_total, 0)

    by_ext = aggregate(paths, sizes, comp, ext_key)
    by_folder = aggregate(paths, sizes, comp, folder_key)

    have_hash = bool((hashes and any(hashes)) or (crcs and any(crcs)))
    page_aligned = data.get("page_aligned")
    encrypted = data.get("encrypted")
    duplicate = data.get("duplicate")
    verified = data.get("verified")
    have_audit = bool(
        (page_aligned and any(page_aligned)) or (encrypted and any(encrypted))
        or (duplicate and any(duplicate))
        or (verified and any(v is not None for v in verified)))
    extra_th = ""
    if have_hash:
        extra_th += ('  <th data-key="phash">Path hash</th>\n'
                     '  <th data-key="crc">CRC32</th>')
    if have_audit:
        extra_th += ('\n  <th data-key="pg">Page</th>\n'
                     '  <th data-key="enc">Enc</th>\n'
                     '  <th data-key="dup">Dup</th>\n'
                     '  <th data-key="ver">Verify</th>')
    hash_panel = _render_hash_panel(header, data) if header else ""

    archive_name = os.path.basename(str(data.get("file") or "")) or Path(manifest_path).stem

    # Brand icon (real JCE icon, embedded as a small data-URI) → favicon + mark.
    icon_uri = _icon_data_uri()
    if icon_uri:
        favicon = f'<link rel="icon" type="image/png" href="{icon_uri}">'
        brand_mark = f'<span class="brand-mark brand-img"><img src="{icon_uri}" alt="JCE" width="30" height="30"></span>'
    else:
        favicon = ""
        brand_mark = '<span class="brand-mark">JCE</span>'

    # Treemap data for the interactive client-side renderer (JSON-safe for <script>).
    tm_json = json.dumps(build_treemap_data(paths, sizes), separators=(",", ":"))
    tm_json = tm_json.replace("</", "<\\/")
    pal_json = json.dumps(_PALETTE)
    scripts = (JS_CODE
               .replace("__TREEMAP_DATA__", tm_json)
               .replace("__PALETTE__", pal_json))

    return HTML_TEMPLATE.format(
        title=html.escape(title),
        favicon=favicon,
        brand_mark=brand_mark,
        archive_name=html.escape(archive_name),
        file_count=data["file_count"] or len(paths),
        raw_total=raw_total, raw_total_h=fmt_bytes(raw_total),
        comp_total=comp_total, comp_total_h=comp_total_h,
        pak_total_h=fmt_bytes(pak_total),
        overhead_h=fmt_bytes(overhead_card),
        overall_ratio=overall,
        comp_segs=comp_segs, comp_legend=comp_legend,
        pak_segs=pak_segs, pak_legend=pak_legend,
        stored_count=stored_count,
        stored_raw_h=fmt_bytes(stored_raw),
        stored_pct=stored_pct,
        hash_panel=hash_panel,
        extra_th=extra_th,
        top_bars=render_top_bars(paths, sizes),
        pie_svg=render_pie_svg(by_ext),
        scatter_svg=render_scatter_svg(sizes, comp, flags, paths),
        treemap_svg=render_treemap_svg(paths, sizes),
        ext_table=render_agg_table(by_ext),
        folder_table=render_agg_table(by_folder),
        rows=render_rows(paths, sizes, comp, flags, hashes, crcs,
                         page_aligned, encrypted, duplicate, verified),
        manifest_path=html.escape(str(manifest_path)),
        scripts=scripts,
    )


def _render_hash_panel(header: Dict[str, object], data: Dict[str, object]) -> str:
    def cell(label: str, value) -> str:
        return f'<div><b>{html.escape(label)}</b> <span>{html.escape(str(value))}</span></div>'

    parts = []
    if data.get("file"):
        parts.append(cell("archive", data["file"]))
    if "data_content_hash" in header:
        parts.append(cell("data XXH3-64", header["data_content_hash"]))
    if "index_content_hash" in header:
        parts.append(cell("index XXH3-64", header["index_content_hash"]))
    verified = header.get("header_verified")
    if verified is not None:
        parts.append(cell("header integrity", "OK" if verified else "FAILED"))
    dicts = data.get("dictionaries") or []
    if dicts:
        usage = data.get("dict_usage") or {}
        tags = ", ".join(
            f'{d.get("tag")}({fmt_bytes(int(d.get("size", 0)))}'
            f'{", " + str(usage.get(int(d.get("id", -1)), 0)) + " files" if usage else ""})'
            for d in dicts)
        parts.append(cell(f"dictionaries ({len(dicts)})", tags))
    totals = data.get("totals") or {}
    dg = int(totals.get("duplicate_groups", 0))
    if dg:
        reclaimed = int(totals.get("duplicate_reclaimed_bytes", 0))
        wasted = int(totals.get("duplicate_wasted_bytes", 0))
        parts.append(cell(
            "duplicates",
            f'{dg} groups / {int(totals.get("duplicate_entries", 0))} files / '
            f'{fmt_bytes(reclaimed)} reclaimed / {fmt_bytes(wasted)} wasted'))
    if totals.get("verify_ran"):
        corrupt = int(totals.get("corrupt_count", 0))
        parts.append(cell(
            "verify",
            f'{int(totals.get("verified_count", 0))} OK / {corrupt} corrupt / '
            f'{int(totals.get("skipped_count", 0))} skipped'))
    if not parts:
        return ""
    return '<div class="hashbar">' + "".join(parts) + "</div>"


# ──────────────────────────────────────────────────────────────────────
# CLI
# ──────────────────────────────────────────────────────────────────────


def main() -> int:
    ap = argparse.ArgumentParser(description="Generate HTML asset report.")
    ap.add_argument("input", type=Path,
                    help="Path to *_assets_manifest.cmake OR a jce.pakbom.v1 JSON "
                         "(from `jce_pak --inspect ... --json out.json`)")
    ap.add_argument("output", type=Path, help="Output HTML file")
    ap.add_argument("--title", default=None, help="Report title")
    args = ap.parse_args()

    if not args.input.is_file():
        print(f"input not found: {args.input}", file=sys.stderr)
        return 1

    is_json = _looks_like_json(args.input)
    title = args.title or f"JCE Asset Report — {args.input.stem}"
    data = parse_bom_json(args.input) if is_json else parse_manifest(args.input)
    html_str = render_html(data, args.input, title)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(html_str, encoding="utf-8")
    src = "BOM JSON" if is_json else "manifest"
    print(f"wrote {args.output} ({len(html_str)} bytes, "
          f"{data['file_count']} assets, source={src})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
