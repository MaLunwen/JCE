#!/usr/bin/env python3
"""
gen_pak_report.py  ── Generate a self-contained HTML report from a
jce_pak manifest (.cmake) emitted by tools/jce_pak.c.

Usage:
    python gen_pak_report.py <manifest.cmake> <output.html> [--title "..."]

The HTML report is fully self-contained (no external deps) and provides:
  * Summary card (file count, raw size, compressed size, overall ratio)
  * Sortable table of all assets (path / size / compressed / ratio)
  * Aggregation by file extension and top-level folder
  * Top 20 largest assets bar chart (pure CSS, no JS libs)
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
    }


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
tr:hover td {{ background: #1d232b; }}
.ratio-good {{ color: var(--good); }}
.ratio-bad {{ color: var(--warn); }}
.ratio-stored {{ color: #ce93d8; font-weight: 600; }}
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

<h2>Top 20 largest assets (raw size)</h2>
<div class="top-list">
{top_bars}
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
  <th data-key="ratio" data-num="1">Ratio</th>
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
                flags: List[int]) -> str:
    rows: List[str] = []
    indexed = sorted(zip(sizes, comp, paths, flags), reverse=True)
    for size, c, path, fl in indexed:
        ratio = (size / c) if c else 0.0
        stored = bool(fl & 1)
        if stored:
            cls = "ratio-stored"
            ratio_html = '<span class="ratio-stored">STORED</span>'
        else:
            cls = "ratio-good" if ratio >= 2.0 else ("ratio-bad" if ratio < 1.05 else "")
            ratio_html = f'<span class="{cls}">{ratio:.2f}×</span>'
        rows.append(
            f'<tr data-path="{html.escape(path)}" data-size="{size}" '
            f'data-comp="{c}" data-ratio="{ratio:.4f}" data-stored="{int(stored)}">'
            f'<td>{html.escape(path)}</td>'
            f'<td class="num">{fmt_bytes(size)}</td>'
            f'<td class="num">{fmt_bytes(c)}</td>'
            f'<td class="num">{ratio_html}</td></tr>'
        )
    return "\n".join(rows)


def render_html(data: Dict[str, object], manifest_path: Path, title: str) -> str:
    paths = data["paths"]
    sizes = data["sizes"]
    comp = data["compressed"]
    flags = data.get("flags") or [0] * len(paths)
    raw_total = data["raw_total"] or sum(sizes)
    comp_total = data["comp_total"] or sum(comp)
    pak_total = data["pak_total"] or comp_total
    overall = (raw_total / comp_total) if comp_total else 0.0
    stored_count = data.get("stored_count", 0)
    stored_raw = data.get("stored_raw_total", 0)
    stored_pct = (stored_raw * 100.0 / raw_total) if raw_total else 0.0

    by_ext = aggregate(paths, sizes, comp, ext_key)
    by_folder = aggregate(paths, sizes, comp, folder_key)

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
        top_bars=render_top_bars(paths, sizes),
        ext_table=render_agg_table(by_ext),
        folder_table=render_agg_table(by_folder),
        rows=render_rows(paths, sizes, comp, flags),
        manifest_path=html.escape(str(manifest_path)),
    )


# ──────────────────────────────────────────────────────────────────────
# CLI
# ──────────────────────────────────────────────────────────────────────


def main() -> int:
    ap = argparse.ArgumentParser(description="Generate HTML asset report.")
    ap.add_argument("manifest", type=Path, help="Path to *_assets_manifest.cmake")
    ap.add_argument("output", type=Path, help="Output HTML file")
    ap.add_argument("--title", default=None, help="Report title")
    args = ap.parse_args()

    if not args.manifest.is_file():
        print(f"manifest not found: {args.manifest}", file=sys.stderr)
        return 1

    title = args.title or f"JCE Asset Report — {args.manifest.stem}"
    data = parse_manifest(args.manifest)
    html_str = render_html(data, args.manifest, title)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(html_str, encoding="utf-8")
    print(f"wrote {args.output} ({len(html_str)} bytes, {data['file_count']} assets)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
