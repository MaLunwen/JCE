# engine/src/middleware/ui — UI / HUD (L4)

> Game-side UI: RmlUI (HTML/CSS HUD), touch HUD, debug HUD, settings. Plus the ImGui backend used by the editor.

## Identity

- **Layer**: L4. C99 + C++ bridges for RmlUI / ImGui.
- **Public umbrella**: `<jce/api_ui.h>` → `<jce/middleware/ui/jce_*.h>`
- **Deps**: RmlUI (C++ bridge), `jce_tools_imgui` (editor-only path).

## File map

| Header | TU | Role |
|--------|----|------|
| `jce_ui.h` | `jce_ui.c` | Public UI façade (document load, event dispatch) |
| `jce_game_hud.h` | `jce_game_hud.c` | Pre-built game HUD layout helpers. **NOT in `<jce/api.h>`** (2026-08-31): rule 1 below gives SCENE UI including the HUD to the ECS Canvas, so aggregating a second HUD in `api_ui.h` would read as the engine offering two. It still draws and still ships — a user includes the header directly. Zero callers in this repo, caged_kingdom included; exempted with that reason in `tools/lint/api_closure_exempt.txt`. Two features its top comment advertised — a weapon-slot indicator and a callback-driven compass/mini-map — were removed from that comment the same day because neither exists in the code. |
| `jce_touch_hud.h` | `jce_touch_hud.c` | Virtual joystick / buttons for mobile |
| `jce_ui_debug_hud.h` | `jce_ui_debug_hud.c` | FPS / frame-time / draw-call overlay |
| `jce_ui_settings.h` | `jce_ui_settings.c` | Settings menu primitives |
| (internal) `jce_ui_backend.h` | — | Backend ABI (renderer + input glue) |
| (internal) — | `jce_ui_rmlui.cpp` | RmlUI bridge: document/event/render backend |
| (internal) — | `jce_imgui_renderer.cpp` | bgfx renderer for ImGui — **only used by editor + debug HUD**. Game code never imports `<imgui.h>`. |

## Rules

1. **Two shipping UI systems, split by capability — see ADR-0002.**
   - **ECS Canvas** (`middleware/scene/jce_ui_canvas.h`) owns SCENE UI: HUD,
     world-space UI, anything an artist places in a scene or a gameplay script
     addresses per entity. It is the only one with entities, prefabs,
     serialization and the per-entity script bridge — the editor authors nine
     widget types on it.
   - **RmlUI** (`jce_ui.h`) owns DOCUMENT UI: front-end shell, settings, menus,
     anything needing flow/flex layout or CSS cascade. It is the only one with a
     real layout engine.
   This rule previously read "RmlUI is the GAME UI framework", which contradicted
   both the editor wiring and the script API. Neither system can take the
   other's job without becoming it; that is why both ship.
   **ImGui stays editor-only** (plus the debug-HUD opt-in). Don't ship ImGui
   dialogs in games.
2. **`<RmlUi/...>` / `<imgui.h>` stay out of public `<jce/...>` headers.**
3. **Input integration**: pull events from `os/platform/jce_input` — don't poll SDL.
4. **Renderer integration**: draw via bgfx through the existing backend in `jce_imgui_renderer.cpp` / `jce_ui_rmlui.cpp`. No direct GL/Vulkan calls.
5. **Touch HUD** auto-enables on `JCE_PLATFORM_ANDROID` / `JCE_PLATFORM_IOS`; can be toggled by consumers.
6. **i18n**: route through `jce_i18n` for any user-visible string.

## Don't

- Don't expose RmlUI / ImGui types in public headers.
- Don't pull SDL or bgfx headers into panel/HUD code.
- Don't add a third UI library.
