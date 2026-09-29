# editor/resources/ — Editor-Shipped Assets

## Identity
- **Role**: assets bundled INTO the editor binary (icon, fonts, i18n strings).
- **Mount**: PhysFS mount point `/editor` (editor process only).

## File map
- `JCE_icon.png`            — editor window icon.
- `assets/fonts/`           — ImGui-loaded TTFs (CJK + Latin merged for i18n).
- `assets/i18n/`            — `<lang>.json` translation tables (loaded by `editor/src/core/jce_editor_i18n.cpp`).

## Rules
1. **Editor-only** — never mounted by the game runtime.
2. **Fonts must include CJK ranges** (zh/ja/ko) — baseline ImGui Latin is not enough.
3. **i18n JSON**: flat `"key": "translated"` pairs; key canonical order enforced by `scripts/tools sort_i18n.py`.
4. **Add a new language** → drop `<lang>.json` here → register lang code in `jce_editor_config`.
5. **License**: every font's license note goes into `THIRD_PARTY_LICENSES.md`.

## Common tasks
- **Edit translations** → modify `assets/i18n/<lang>.json` → run `python tools/sort_i18n.py` → commit.
- **Add UI string** → add key in EN base; missing-key audit fires via `tools/lint/i18n_audit.py`.
