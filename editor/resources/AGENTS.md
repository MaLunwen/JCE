# editor/resources/ — Editor-Shipped Assets

## Identity
- **Role**: assets bundled INTO the editor binary (icon, fonts, i18n strings).
- **Mount**: PhysFS mount point `/editor` (editor process only).

## File map
- `JCE_icon.png`            — editor window icon.
- Shared defaults in `engine/ui/fonts/` supply stock Latin and CJK fallbacks.
  Installed fonts and explicit overrides retain priority.
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

Cadence help distinguishes automatic authoring background (5 FPS) from unfocused
Play (15 FPS). An explicit background preference overrides both through the
existing cadence policy; displayed authoring defaults are not a Play throughput
claim. Keep all locale defaults synchronized with that owner.

Build Profiles help in every locale distinguishes SDK-native CMake/Ninja builds
from engine-workspace scripts/Conan, build-cache executable from Pack destination,
and default Dist single-executable publication from explicit Release development
packages that may contain companion files.


Profiler interval/cadence labels in every locale distinguish the current UI
wall interval from the status bar's rolling UI update rate. Neither represents
presentation FPS. Keep the current interval, history summary, clipboard rate
and explanatory tooltip keys synchronized; do not relabel backend/GPU waits as
current UI subphases.

Trace toolbar labels distinguish automatic cursor refresh from producer
recording. All locales provide explicit recording-on/off text and explain
retained history when recording is off; refresh never enables recording.
