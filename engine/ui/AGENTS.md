# engine/ui/ — Built-in RmlUI Documents

## Identity
- **Layer**: L4 (RmlUI assets shipped with the engine)
- **Format**: RmlUI `.rml` (HTML-ish) + `.rcss` (CSS-ish)
- **Loaded by**: `engine/src/middleware/ui/` (RmlUI runtime).

## File map
- `engine_debug_hud.rml`  — F3-style debug overlay (FPS, memory, GPU, draw calls, jobs).
- `engine_console_overlay.rml` — console document shell.
- `engine_settings.rml`   — in-game settings UI (graphics tier, audio bus, input rebind).

## Rules
1. **Engine-shipped, not game-owned.** Game projects opt into document HUDs with `ui/hud.rml` and `ui/hud.bindings` under their authored `source_assets` root; packaged content resolves the same virtual keys.
2. **Asset paths in RML use PhysFS-mounted paths** (e.g. `engine/ui/engine_debug_hud.rml`), NEVER absolute filesystem paths.
3. **Style hierarchy**: shared `.rcss` lives under `engine/resources/ui/` (PAK'd); per-doc overrides inline.
4. **i18n keys**: use `data-i18n="key"` attribute; runtime resolves via `jce_i18n_*`.
5. **No JavaScript** — RmlUI scripting hooks go through C event handlers registered in `engine/src/middleware/ui/`.
6. **Mobile-friendly**: every interactive element ≥ 44×44 dp; respect safe-area insets from `jce_window_*`.

## Don't
- Don't add ImGui markup here — ImGui is editor-only.
- Don't hardcode colors; use `--color-*` CSS vars defined in `engine/resources/ui/theme.rcss`.
- Don't include `<script src=...>` — disallowed.

## Common tasks
- **Add a new built-in HUD** → drop `engine_<name>.rml` here; load via `jce_ui_load_document("engine/ui/engine_<name>.rml")`.
- **Localize strings** → add keys to `engine/resources/i18n/<lang>.json`.

## Default document typography

All three stock documents select logical `JCE-UI`. The Rml bridge loads this
family from the shared host UI_LATIN policy (Print first), with the existing
licensed Aileron stock face in `fonts/` as the Latin fallback. Host CJK and
symbol faces precede the shared `JCE CJK Fallback` PAK subset. This last face
also serves ImGui and Canvas when system fallback roles are missing. The
subset is generated offline with `tools/assets/subset_stock_cjk.py` from the
upstream hash in its provenance file. Keep its OFL text with the asset.
Project CSS may select explicitly loaded project families. Do not rename CSS
to a physical system family without admitting and retaining that font, and
do not package system fonts or auto-load a user-supplied `JCE.ttf`.
