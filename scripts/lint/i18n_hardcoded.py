#!/usr/bin/env python3
"""
i18n_hardcoded.py — Scan editor C++ sources for ImGui UI calls that pass
hardcoded English literals instead of routing through jce_editor_i18n().

Scope: editor/src/{panels,ui,dialogs}/**/*.cpp.

Heuristic:
  * Match a curated set of ImGui calls that render user-visible text.
  * Inside the call, look for string literals.
  * Skip the literal if any jce_editor_i18n[_or|_id]() — or the project's
    `BL()` helper, which wraps jce_editor_i18n_or() — appears in the same
    call expression (handles wrapped or raw fallback patterns).
  * Skip ID-only literals ("##foo", "###foo"), printf format strings
    starting with '%', and pure-punctuation/number tokens.
  * Skip very short tokens (<3 letters) since those are usually labels
    like "X" / "Y" / "RGB".

Exit status:
  0 — no offenders.
  1 — one or more hardcoded UI strings found.

Pass --quiet to suppress per-line listing and only print totals.
"""
from __future__ import annotations
import argparse, re, sys
from pathlib import Path

CALL_RE = re.compile(
    r'ImGui::(?P<call>'
    r'Text|TextUnformatted|TextDisabled|TextColored|TextWrapped|TextLink|'
    r'TextLinkOpenURL|BulletText|LabelText|SetTooltip|SetItemTooltip|'
    r'Button|SmallButton|InvisibleButton|ArrowButton|RadioButton|'
    r'MenuItem|BeginMenu|BeginTabItem|BeginTabBar|BeginPopupModal|'
    r'BeginListBox|BeginCombo|BeginChild|Begin|'
    r'CollapsingHeader|TreeNode|TreeNodeEx|Selectable|Checkbox|'
    r'InputText|InputTextMultiline|InputTextWithHint|'
    r'InputFloat|InputFloat2|InputFloat3|InputFloat4|InputInt|'
    r'InputInt2|InputInt3|InputInt4|InputDouble|'
    r'SliderFloat|SliderFloat2|SliderFloat3|SliderFloat4|SliderInt|'
    r'SliderInt2|SliderInt3|SliderInt4|SliderAngle|VSliderFloat|VSliderInt|'
    r'DragFloat|DragFloat2|DragFloat3|DragFloat4|DragInt|DragInt2|'
    r'DragInt3|DragInt4|DragFloatRange2|DragIntRange2|'
    r'Combo|ColorEdit3|ColorEdit4|ColorPicker3|ColorPicker4|ColorButton|'
    r'PlotLines|PlotHistogram|ProgressBar|'
    r'OpenPopup|BeginPopup|BeginPopupContextWindow|BeginPopupContextItem|'
    r'BeginPopupContextVoid|BeginDragDropSource|BeginDragDropTarget|'
    r'PushID'
    r')\s*\(')
I18N_RE = re.compile(r'(?:jce_editor_i18n(?:_or|_id)?|\bBL)\s*\(')
STR_RE  = re.compile(r'"((?:[^"\\]|\\.)*?)"')

# Calls whose first string argument is an internal ID, not user-visible text.
ID_ONLY_CALLS = {
    'BeginChild', 'BeginPopupContextWindow', 'BeginPopupContextItem',
    'BeginPopupContextVoid', 'BeginDragDropSource', 'BeginDragDropTarget',
    'PushID', 'OpenPopup', 'BeginPopup', 'BeginTabBar',
}

# Substrings that indicate the literal is technical/non-UI.
SKIP_LITERAL_SUBSTR = ('://',)   # URLs and similar.

def is_punct_only(s: str) -> bool:
    return all(c in '%sdfxlu .,:;#-+_/()[]{}|*<>0123456789' for c in s)

def looks_like_format_id(s: str) -> bool:
    if not s: return True
    if s.startswith('##') or s.startswith('###'): return True
    if s.startswith('%'): return True
    if is_punct_only(s): return True
    if not re.search(r'[A-Za-z]{3}', s): return True
    return False

def is_user_text(s: str) -> bool:
    """Trigger only on plausibly user-facing English text:
       has 3+ ASCII letters AND (contains a space OR starts with uppercase).
       Strips any ##id / ###id suffix first so 'Q##tool_hand' is judged
       on its visible portion ('Q'), not the internal ID."""
    if any(t in s for t in SKIP_LITERAL_SUBSTR): return False
    visible = s.split('##', 1)[0]
    if len(visible) < 3:
        return False
    if visible in SKIP_VISIBLE: return False
    return (' ' in visible) or visible[0].isupper()

# ImGui IDs that look like real text but are window/dock identifiers,
# never rendered. Add as needed.
SKIP_VISIBLE = {
    'DockSpace', 'MainDockSpace', 'AssetGridChild',
}

def _call_chunk(src: str, start: int) -> str:
    """Return the call substring including only its own argument list,
    properly balanced for nested parens. `start` points at the '(' just
    after the function name."""
    depth = 0
    i = start
    n = len(src)
    while i < n:
        c = src[i]
        if c == '"':
            i += 1
            while i < n and src[i] != '"':
                if src[i] == '\\': i += 1
                i += 1
        elif c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                return src[start:i+1]
        i += 1
    return src[start:]

def scan_file(path: Path) -> list[tuple[int,str]]:
    src = path.read_text(encoding='utf-8', errors='replace')
    hits = []
    for m in CALL_RE.finditer(src):
        call = m.group('call')
        if call in ID_ONLY_CALLS:
            continue
        # m.end() points just after the '(' of the matched call.
        chunk = _call_chunk(src, m.end() - 1)
        if I18N_RE.search(chunk): continue
        for sm in STR_RE.finditer(chunk):
            s = sm.group(1)
            if looks_like_format_id(s): continue
            if not is_user_text(s): continue
            line = src.count('\n', 0, m.start()) + 1
            hits.append((line, s[:100]))
            break  # one diag per call site
    return hits

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--quiet', action='store_true')
    ap.add_argument('--root', default='editor/src',
                    help='root directory to scan (default: editor/src)')
    args = ap.parse_args()

    root = Path(args.root)
    scan_dirs = [root / 'panels', root / 'ui', root / 'dialogs']
    files = []
    for d in scan_dirs:
        if d.exists():
            files.extend(sorted(d.rglob('*.cpp')))

    total = 0
    per_file = []
    for p in files:
        hits = scan_file(p)
        if hits:
            per_file.append((p, hits))
            total += len(hits)

    if not args.quiet:
        for p, hits in per_file:
            rel = p.as_posix()
            print(f'== {rel} ({len(hits)}) ==')
            for ln, s in hits:
                print(f'  L{ln}: {s}')

    print(f'TOTAL hardcoded UI strings: {total}  (files: {len(per_file)})')
    return 1 if total else 0

if __name__ == '__main__':
    sys.exit(main())
