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

The HTML report is fully self-contained (no external deps) and provides:
  * Summary card (file count, raw size, compressed size, overall ratio)
  * Archive hash panel (JSON source only)
  * Sortable table of all assets (path / size / compressed / ratio [+ hashes])
  * Aggregation by file extension and top-level folder
  * Top 20 largest assets bar chart (pure CSS, no JS libs)
  * SVG donut by extension, log-log scatter (raw vs compressed),
    and squarified treemap (folder × extension) — pure SVG, no libs.
"""
from __future__ import annotations

import argparse
import html
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
    import json

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


# ──────────────────────────────────────────────────────────────────────
# HTML rendering
# ──────────────────────────────────────────────────────────────────────


HTML_TEMPLATE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>{title}</title>
<style>
:root {{
  --bg: #0f1216; --fg: #e6e8eb; --muted: #8b95a5; --card: #181c22;
  --accent: #4fc3f7; --good: #66bb6a; --warn: #ffa726; --bar: #4fc3f7;
}}
* {{ box-sizing: border-box; }}
body {{ margin: 0; padding: 24px; background: var(--bg); color: var(--fg);
       font-family: -apple-system, "Segoe UI", system-ui, sans-serif;
       font-size: 14px; line-height: 1.5; }}
h1 {{ margin: 0 0 4px; font-weight: 600; }}
h2 {{ margin: 32px 0 12px; font-size: 18px; color: var(--accent); }}
.subtitle {{ color: var(--muted); margin-bottom: 24px; }}
.cards {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
          gap: 12px; margin: 16px 0 24px; }}
.card {{ background: var(--card); padding: 16px; border-radius: 8px;
         border: 1px solid #232931; }}
.card .label {{ color: var(--muted); font-size: 12px; text-transform: uppercase;
                letter-spacing: 0.04em; }}
.card .value {{ font-size: 22px; font-weight: 600; margin-top: 4px; }}
.card .sub {{ color: var(--muted); font-size: 12px; margin-top: 2px; }}
table {{ width: 100%; border-collapse: collapse; background: var(--card);
         border-radius: 8px; overflow: hidden; font-size: 13px; }}
th, td {{ padding: 8px 12px; text-align: left; border-bottom: 1px solid #232931; }}
th {{ background: #1f2530; color: var(--muted); font-weight: 500;
      cursor: pointer; user-select: none; position: sticky; top: 0; }}
th:hover {{ color: var(--accent); }}
th.sort-asc::after {{ content: " ▲"; color: var(--accent); }}
th.sort-desc::after {{ content: " ▼"; color: var(--accent); }}
td.num {{ text-align: right; font-variant-numeric: tabular-nums; }}
td.hash {{ font-family: ui-monospace, Menlo, Consolas, monospace; font-size: 12px; color: #9fb3c8; white-space: nowrap; }}
.hashbar {{ display: flex; flex-wrap: wrap; gap: 18px; margin: 0 0 8px; padding: 12px 16px;
  background: #14202b; border: 1px solid #243441; border-radius: 8px;
  font-family: ui-monospace, Menlo, Consolas, monospace; font-size: 13px; }}
.hashbar b {{ color: #7fa8c9; font-weight: 600; }}
.hashbar span {{ color: #cfe2f0; }}
tr:hover td {{ background: #1d232b; }}
.ratio-good {{ color: var(--good); }}
.ratio-bad {{ color: var(--warn); }}
.ratio-stored {{ color: #ce93d8; font-weight: 600; }}
td.audit {{ text-align: center; white-space: nowrap; }}
.badge {{ display: inline-block; min-width: 22px; padding: 1px 5px; border-radius: 4px;
  font-size: 11px; font-weight: 700; font-family: ui-monospace, Menlo, Consolas, monospace; }}
.badge.off {{ color: #44525e; }}
.badge.ok {{ background: #16361f; color: #6fd58a; }}
.badge.bad {{ background: #3a1717; color: #f0726a; }}
.badge.warn {{ background: #3a2f14; color: #e6b54a; }}
.badge.info {{ background: #14283a; color: #5aa9e6; }}
.bar-row {{ display: grid; grid-template-columns: 280px 1fr 90px;
            gap: 8px; align-items: center; padding: 4px 0;
            font-variant-numeric: tabular-nums; font-size: 12px; }}
.bar-row .name {{ overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
                  color: var(--fg); }}
.bar {{ height: 14px; background: var(--bar); border-radius: 3px; }}
.bar-row .size {{ color: var(--muted); text-align: right; }}
.search {{ background: #1f2530; border: 1px solid #2a3240; color: var(--fg);
           padding: 6px 12px; border-radius: 4px; font-size: 13px; width: 280px;
           margin-bottom: 8px; }}
.viz-grid {{ display: grid; grid-template-columns: 1fr 1fr; gap: 16px;
             margin: 16px 0; }}
@media (max-width: 900px) {{ .viz-grid {{ grid-template-columns: 1fr; }} }}
.viz {{ background: var(--card); border: 1px solid #232931; border-radius: 8px;
        padding: 12px; }}
.viz h3 {{ margin: 0 0 8px; font-size: 13px; color: var(--muted);
           text-transform: uppercase; letter-spacing: 0.04em; font-weight: 500; }}
.viz svg {{ display: block; width: 100%; height: auto; }}
.legend {{ display: flex; flex-wrap: wrap; gap: 4px 12px; margin-top: 8px;
           font-size: 11px; color: var(--muted); }}
.legend .sw {{ display: inline-block; width: 10px; height: 10px;
               border-radius: 2px; margin-right: 4px; vertical-align: middle; }}
.tm-cell text {{ font-family: -apple-system, "Segoe UI", system-ui, sans-serif;
                 fill: #0f1216; pointer-events: none; }}
.tm-cell rect {{ stroke: #0f1216; stroke-width: 1; }}
.tm-cell:hover rect {{ stroke: #fff; stroke-width: 2; }}
footer {{ color: var(--muted); margin-top: 32px; font-size: 11px;
          text-align: center; }}
</style>
</head>
<body>
<h1>{title}</h1>
<p class="subtitle">Asset manifest report &middot; generated by gen_pak_report.py</p>

<div class="cards">
  <div class="card"><div class="label">Files</div>
       <div class="value">{file_count}</div></div>
  <div class="card"><div class="label">Raw size</div>
       <div class="value">{raw_total_h}</div>
       <div class="sub">{raw_total} bytes</div></div>
  <div class="card"><div class="label">Compressed</div>
       <div class="value">{comp_total_h}</div>
       <div class="sub">{comp_total} bytes</div></div>
  <div class="card"><div class="label">PAK total</div>
       <div class="value">{pak_total_h}</div>
       <div class="sub">incl. TOC + headers</div></div>
  <div class="card"><div class="label">Overall ratio</div>
       <div class="value">{overall_ratio:.2f}×</div>
       <div class="sub">raw &rarr; compressed</div></div>
  <div class="card"><div class="label">STORED (uncompressed)</div>
       <div class="value">{stored_count}</div>
       <div class="sub">{stored_raw_h} &middot; {stored_pct:.1f}% of raw</div></div>
</div>
{hash_panel}

<h2>Top 20 largest assets (raw size)</h2>
<div class="top-list">
{top_bars}
</div>

<h2>Visualisations</h2>
<div class="viz-grid">
  <div class="viz">
    <h3>Raw size by extension (top 12)</h3>
    {pie_svg}
  </div>
  <div class="viz">
    <h3>Compression scatter — raw vs compressed (log/log)</h3>
    {scatter_svg}
  </div>
</div>
<div class="viz">
  <h3>Treemap — top-level folders × extensions (raw size)</h3>
  {treemap_svg}
</div>

<h2>By extension</h2>
{ext_table}

<h2>By top-level folder</h2>
{folder_table}

<h2>All assets ({file_count})</h2>
<input class="search" id="filter" type="text" placeholder="Filter by path…">
<table id="assets">
<thead><tr>
  <th data-key="path">Path</th>
  <th data-key="size" data-num="1" class="sort-desc">Raw</th>
  <th data-key="comp" data-num="1">Compressed</th>
  <th data-key="ratio" data-num="1">Ratio</th>{extra_th}
</tr></thead>
<tbody>
{rows}
</tbody>
</table>

<footer>
JCE asset report &middot; manifest: <code>{manifest_path}</code>
</footer>

<script>
(function() {{
  const tbl = document.getElementById('assets');
  const tbody = tbl.tBodies[0];
  const headers = tbl.tHead.rows[0].cells;
  let sortKey = 'size', sortDir = -1;

  function sort() {{
    const rows = Array.from(tbody.rows);
    rows.sort((a, b) => {{
      const av = a.dataset[sortKey], bv = b.dataset[sortKey];
      const numeric = !isNaN(+av) && !isNaN(+bv);
      const cmp = numeric ? (+av - +bv) : av.localeCompare(bv);
      return cmp * sortDir;
    }});
    rows.forEach(r => tbody.appendChild(r));
    for (let h of headers) {{
      h.classList.remove('sort-asc', 'sort-desc');
      if (h.dataset.key === sortKey)
        h.classList.add(sortDir === 1 ? 'sort-asc' : 'sort-desc');
    }}
  }}

  for (let h of headers) {{
    h.addEventListener('click', () => {{
      const key = h.dataset.key;
      if (sortKey === key) sortDir = -sortDir;
      else {{ sortKey = key; sortDir = h.dataset.num ? -1 : 1; }}
      sort();
    }});
  }}

  document.getElementById('filter').addEventListener('input', e => {{
    const q = e.target.value.toLowerCase();
    for (let r of tbody.rows)
      r.style.display = r.dataset.path.toLowerCase().includes(q) ? '' : 'none';
  }});
}})();
</script>
</body>
</html>
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
            f'<div><div class="bar" style="width:{pct:.1f}%"></div></div>'
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
        rows.append(
            f'<tr data-path="{html.escape(path)}" data-size="{size}" '
            f'data-comp="{c}" data-ratio="{ratio:.4f}" data-stored="{int(stored)}" '
            f'data-dup="{int(bool(dup))}">'
            f'<td>{html.escape(path)}</td>'
            f'<td class="num">{fmt_bytes(size)}</td>'
            f'<td class="num">{fmt_bytes(c)}</td>'
            f'<td class="num">{ratio_html}</td>{hash_cells}{audit_cells}</tr>'
        )
    return "\n".join(rows)


# ──────────────────────────────────────────────────────────────────────
# SVG visualisations (pure SVG, no external libs)
# ──────────────────────────────────────────────────────────────────────


_PALETTE = [
    "#4fc3f7", "#66bb6a", "#ffa726", "#ce93d8", "#f06292", "#9575cd",
    "#4dd0e1", "#aed581", "#ff8a65", "#ba68c8", "#7986cb", "#dce775",
    "#a1887f", "#90a4ae", "#fff176", "#4db6ac",
]


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
        d = (f"M {x1:.2f} {y1:.2f} "
             f"A {r_outer:.2f} {r_outer:.2f} 0 {large} 1 {x2:.2f} {y2:.2f} "
             f"L {x3:.2f} {y3:.2f} "
             f"A {r_inner:.2f} {r_inner:.2f} 0 {large} 0 {x4:.2f} {y4:.2f} Z")
        title = f"{key}: {fmt_bytes(raw)} ({frac * 100:.1f}%)"
        paths.append(
            f'<path d="{d}" fill="{_color(i)}" stroke="#0f1216" stroke-width="1">'
            f'<title>{html.escape(title)}</title></path>'
        )
        legend.append(
            f'<span><span class="sw" style="background:{_color(i)}"></span>'
            f'{html.escape(key)} &middot; {fmt_bytes(raw)} ({frac * 100:.1f}%)</span>'
        )
        angle = a2

    centre_label = fmt_bytes(total)
    svg = (
        f'<svg viewBox="0 0 {size} {size}" preserveAspectRatio="xMidYMid meet">'
        f'{"".join(paths)}'
        f'<text x="{cx}" y="{cy - 4}" text-anchor="middle" '
        f'fill="#e6e8eb" font-size="13" font-weight="600">{html.escape(centre_label)}</text>'
        f'<text x="{cx}" y="{cy + 12}" text-anchor="middle" '
        f'fill="#8b95a5" font-size="10">total raw</text>'
        f'</svg>'
        f'<div class="legend">{"".join(legend)}</div>'
    )
    return svg


def render_scatter_svg(sizes: List[int], comp: List[int], flags: List[int],
                       width: int = 480, height: int = 320) -> str:
    """Log-log scatter of raw vs compressed size."""
    points = [(s, c, f) for s, c, f in zip(sizes, comp, flags) if s > 0 and c > 0]
    if not points:
        return "<p>(no data)</p>"

    import math
    pad_l, pad_b, pad_t, pad_r = 44, 36, 12, 12
    plot_w = width - pad_l - pad_r
    plot_h = height - pad_t - pad_b

    log_xs = [math.log10(s) for s, _, _ in points]
    log_ys = [math.log10(c) for _, c, _ in points]
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
            f'stroke="#232931" stroke-width="1"/>'
        )
        grid.append(
            f'<line x1="{pad_l}" y1="{y:.1f}" x2="{pad_l + plot_w}" y2="{y:.1f}" '
            f'stroke="#232931" stroke-width="1"/>'
        )
        lbl = decade_labels.get(d, f"1e{d}")
        grid.append(
            f'<text x="{x:.1f}" y="{pad_t + plot_h + 14}" text-anchor="middle" '
            f'fill="#8b95a5" font-size="10">{lbl}</text>'
        )
        grid.append(
            f'<text x="{pad_l - 6}" y="{y + 3:.1f}" text-anchor="end" '
            f'fill="#8b95a5" font-size="10">{lbl}</text>'
        )

    # y = x reference line (no compression baseline)
    diag = (f'<line x1="{sx(lo):.1f}" y1="{sy(lo):.1f}" '
            f'x2="{sx(hi):.1f}" y2="{sy(hi):.1f}" '
            f'stroke="#8b95a5" stroke-width="1" stroke-dasharray="3,3"/>')

    dots = []
    for s, c, fl in points:
        x = sx(math.log10(s))
        y = sy(math.log10(c))
        stored = bool(fl & 1)
        color = "#ce93d8" if stored else ("#66bb6a" if c < s * 0.5 else "#4fc3f7")
        dots.append(
            f'<circle cx="{x:.1f}" cy="{y:.1f}" r="2" fill="{color}" '
            f'fill-opacity="0.6"/>'
        )

    axis_labels = (
        f'<text x="{pad_l + plot_w / 2:.1f}" y="{height - 4}" text-anchor="middle" '
        f'fill="#8b95a5" font-size="11">raw size</text>'
        f'<text x="14" y="{pad_t + plot_h / 2:.1f}" text-anchor="middle" '
        f'fill="#8b95a5" font-size="11" '
        f'transform="rotate(-90 14 {pad_t + plot_h / 2:.1f})">compressed size</text>'
    )

    legend = (
        '<div class="legend">'
        '<span><span class="sw" style="background:#66bb6a"></span>good (≥2×)</span>'
        '<span><span class="sw" style="background:#4fc3f7"></span>compressed</span>'
        '<span><span class="sw" style="background:#ce93d8"></span>STORED</span>'
        '<span style="color:#8b95a5">— dashed: y = x (no compression)</span>'
        '</div>'
    )

    svg = (
        f'<svg viewBox="0 0 {width} {height}" preserveAspectRatio="xMidYMid meet">'
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
                       width: int = 960, height: int = 420) -> str:
    """Two-level squarified treemap: top-level folder → extension."""
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
        # Inner squarify by extension
        ext_items: List[Tuple[str, float]] = sorted(
            ((ek, float(ev)) for ek, ev in folders[k].items()),
            key=lambda e: e[1], reverse=True,
        )
        sub = _squarify(ext_items, x, y, w, h)
        base = folder_color[k]
        # Outer fill (slightly darker to underlay sub-cells)
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
                f'fill="{base}" fill-opacity="{shade:.2f}">'
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
        # Folder label overlay
        if w > 80 and h > 22:
            out.append(
                f'<text x="{x + 6:.2f}" y="{y + 16:.2f}" '
                f'font-size="13" font-weight="700" fill="#0f1216" '
                f'stroke="#fff" stroke-width="0.5" stroke-opacity="0.4">'
                f'{html.escape(k)}</text>'
            )

    out.append('</svg>')

    legend = '<div class="legend">' + "".join(
        f'<span><span class="sw" style="background:{folder_color[k]}"></span>'
        f'{html.escape(k)} &middot; {fmt_bytes(int(v))}</span>'
        for k, v in folder_totals
    ) + '</div>'

    return "".join(out) + legend


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

    return HTML_TEMPLATE.format(
        title=html.escape(title),
        file_count=data["file_count"] or len(paths),
        raw_total=raw_total, raw_total_h=fmt_bytes(raw_total),
        comp_total=comp_total, comp_total_h=fmt_bytes(comp_total),
        pak_total_h=fmt_bytes(pak_total),
        overall_ratio=overall,
        stored_count=stored_count,
        stored_raw_h=fmt_bytes(stored_raw),
        stored_pct=stored_pct,
        hash_panel=hash_panel,
        extra_th=extra_th,
        top_bars=render_top_bars(paths, sizes),
        pie_svg=render_pie_svg(by_ext),
        scatter_svg=render_scatter_svg(sizes, comp, flags),
        treemap_svg=render_treemap_svg(paths, sizes),
        ext_table=render_agg_table(by_ext),
        folder_table=render_agg_table(by_folder),
        rows=render_rows(paths, sizes, comp, flags, hashes, crcs,
                         page_aligned, encrypted, duplicate, verified),
        manifest_path=html.escape(str(manifest_path)),
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
