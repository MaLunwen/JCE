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
| (internal) `jce_rml_font_bytes.h` | `jce_rml_font_bytes.c` | Register owned font bytes before Rml borrows them; retire rejection once; clear accepted spans only after Rml shutdown |
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

RmlUI document pointer processing uses the same jce_input_primary_pointer owner as the default ECS Canvas. Keep primary press/release state per backend context, retain release coordinates, and preserve other physical mouse buttons and wheel handling. SDL mirror rejection cannot make either UI family mouse-only.

## Document default fonts and retention

The Rml bridge consumes the shared `jce_host_default_font` UI_LATIN and UI_CJK
roles. The host owner selects Print before the system Sans fallback. Latin is
registered under logical family `JCE-UI`; if the host face cannot be read or
admitted, the existing licensed PAK `fonts/Aileron-Regular.ttf` is its fallback.
CJK uses `JCE-UI-CJK` with Rml missing-glyph fallback enabled, including for
explicit project families. It does not replace a project's primary face. No
automatic `JCE.ttf` load or system-font copying into delivery assets is allowed.
A missing CJK role is logged as Latin-only; the stock face has no CJK coverage.
The path-only host role currently selects collection face index zero; loading
a CJK role is not evidence that every CJK glyph or collection face is covered.

Host reads are capped before allocation and PAK bytes must pass decompression
and content verification. Font sources have a private 64 MiB cap. Retention
registry allocation precedes the actual borrowed-span admission. Rejected
faces and registry OOM release their source once; accepted sources live until
`Rml::Shutdown` completes. The bridge owns one global fixed-name context on
the UI thread and rejects a second backend before replacing its interfaces.
Failed initialization clears borrowed global interface pointers. A context
without default fonts remains available for explicitly loaded project faces
and reports the missing default instead of declaring it loaded.

Fixed-name trace scopes distinguish source read/load, actual face admission
and final release. Geometry compile and generated texture counters record
actual accepted work, not GPU completion. Failure logs identify source policy
and stage without printing system font paths or contents; they are emitted
on attempts, never every frame.

`tests/middleware/ui` has an exact C retention-owner unit fixture, a real Rml
document/layout/atlas fixture with bgfx Noop, and a compiled old-preload-wire
contract control. Resolver ports use licensed stock bytes and real host/PAK
owners. Noop texture creation, layout and geometry do not certify visible
pixels, physical GPU performance, all CJK glyphs, or platform font selection.

## ImGui dynamic atlas ownership and trace

`jce_imgui_renderer.cpp` instantiates the single private
`jce_imgui_texture_upload_internal.h` transaction with real ImTextureData and
bgfx ports. Only RGBA32/4-byte requests within actual backend dimensions and
signed ImGui pixel-offset limits are accepted; unknown formats, absent pixels,
invalid IDs and dirty rectangles fail closed. Texture IDs encode handle+1, with
zero reserved for no resource; never truncate an oversized ID into a live handle.
Create/recreate retains the old owner until new creation and staging copy are
accepted. A failed copy destroys only its new handle once and keeps WantCreate
for retry. Failed partial staging preserves the old texture and WantUpdates;
packed rows use bgfx's automatic pitch, never a narrowed uint16 byte pitch.
Only accepted submission publishes OK. Destroy clears the ID through ImGui's
setters and never destroys an invalid or already-cleared handle. Honor ImGui's
SetStatus re-create semantics and delayed WantDestroy/UnusedFrames admission.

BackendUserData holds only this backend's tagged last failure phase/reason,
never a borrowed pointer or an allocation. Each texture reports a warning on
failure transition, not on every repeated frame, and clears its diagnostic on
accepted completion/cleanup. Common service scopes trace create/update/destroy
and rollback with owner, handle, dimensions, current CPU logical request bytes
and accepted staging bytes. Logical request bytes are not GPU residency or
physical memory, and a destroy's current dimensions need not describe an older
replaced allocation. Submission success is not GPU completion. Draw admission
rejects pending creation/destruction and invalid IDs before the ImDrawCmd
assertion; failed Updates may draw their valid prior contents. Trace counts
rejected draw commands without warning spam. Idle textures and disabled trace
emit no service events and preserve the caller's current task identity.

`test_jce_imgui_texture_upload` consumes this exact private transaction with
counted GPU boundary ports and the common real trace. Its old-wire contract
control changes only create admission, must compile successfully, and must fail
from the same runtime ownership assertions. Neither ports nor Noop certify
physical GPU upload, font pixels, backend format support or performance.

Standalone Rml layout controls explicitly author the document body viewport;
Rml does not supply browser default body sizing. Absolute-only children in an
auto-sized document can legally yield a zero-width box. Require the actual
intrinsic text box to agree with the same font owner measurement. Observe
context clearing from the real OnShutdown callback while core state is alive;
do not query GetNumContexts after Rml Shutdown returns.

Rml default preload consumes shared Latin, CJK and Symbols roles, with CJK and
Symbols registered through the existing fallback-font admission/retained-byte
owner. Missing optional faces preserve Latin and explicit project faces. There
is no per-frame system lookup, duplicate font storage owner, or redistribution of
system fonts. Licensed-byte/Noop controls prove role routing, retention, layout
and release; they do not certify that a substituted fixture contains a symbol or
that the physical GPU drew it. Check actual face coverage and pixels separately.

ImGui GPU admission names its actual bgfx view UI/ImGui; the admitted recording mirror uses UI/Recording. Keep view IDs, submission order, targets and draw behavior unchanged. A backend view statistic is the latest reported snapshot, not proof of current-frame physical GPU completion.

Window capture and recording retain one explicit RGBA16F texture reference
beside the framebuffer's attachment reference. The framebuffer create call
passes false for ownership transfer. A rejected allocation returns the caller
reference exactly once, including framebuffer handle exhaustion. A live target
unbinds its view, destroys the framebuffer, then releases the caller texture.
Resize, recording Stop at the next setup boundary, and UI shutdown use the
same owner. Valid handle zero is retained. A failed recording target admits no
mirror draws. UI shutdown clears only capture intent that has not been submitted;
readback staging, pending CPU pixels and file completion remain with the renderer
readback owner. The focused CPU controls run the actual setup/shutdown owner
with counted boundary ports and an ordinary missing-release negative.

## ImGui framebuffer clip admission

The first-party backend caches the exact uint16 drawable extent admitted by
setup_view; shutdown clears it with the existing state owner. Current Editor
vertices and clips use drawable pixels with FramebufferScale=1. The single
private jce_imgui_clip_internal.h helper removes DisplayPos, retains old finite
float subtraction and fractional-origin rounding, intersects the real extent,
and rejects nonfinite or integer-empty results before scissor/texture/state/submit.
All uint16 conversions are checked. It has no allocation, OS query or GPU work.
Callbacks, dynamic texture maintenance before zero-vertex return, combined
transient allocation, blend/MSAA state and recording submissions keep their
existing owner/order. This does not introduce general DPI/multi-window handling
or certify GPU timing, visible pixels or presentation.

The existing optional trace counter `ui.clip.draw.rejected` aggregates commands
rejected by clip admission within one draw call. It includes ordinary offscreen
or integer-empty clips as well as invalid coordinates; it is not an error count.
Valid-only draws emit no clip counter, and the existing disabled trace producer
stays authoritative. No per-command log or additional tracing service is added.


## Optional ImGui CPU submission observation

`jce_imgui_renderer_draw_observed` shares the complete ordinary draw owner and
observes one live texture handle index only on request. The fixed-width sized
POD copies no caller input and retains no pointers. NULL/short storage, an
invalid watched handle or uint32 count overflow preserves all caller bytes,
returns false and still draws once. A larger output buffer retains its tail.
The ordinary entry passes no observer, adding no queue scan, allocation, query,
writer or additional texture getter. Both entries stay on the existing UI thread.

The final queue is inspected before zero vertices, transient refusal and clip
admission. The shared safe decoder protects pending dynamic/font texture IDs.
Only noncallback nonempty matching commands count. Separate availability bits
distinguish unavailable initialization/draw data from an observed zero. Main
and recording counts advance at their respective actual bgfx_submit calls;
zero-element calls retain prior drawing behavior but certify no texture use.
These observations are CPU calls, never GPU completion, visibility or Present.

The existing complete-draw-owner `test_jce_imgui_clip_draw` CPU fixture also
covers sized rejection/tail preservation, valid handle zero, absent data, empty
and clipped draws, multi-list transient refusal, callbacks, pending/update font
textures, oversized IDs and recording distinction. Compiled complete-owner
`missing_queue_observation` and `missing_submission_counts` controls remove only
the real observation wires and must fail ordinary count/availability assertions.
Backend ports do not establish physical GPU execution or pixels.
