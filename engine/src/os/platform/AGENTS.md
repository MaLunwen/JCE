# engine/src/os/platform — Platform / OS Wrappers (L2)

> Platform abstraction. Everything OS-specific belongs HERE (and nowhere else).

## Identity

- **Layer**: L2 (Platform). Sits next to `os/core`; depends on it. C99 (with one `.c` per-platform subdir when needed; see `macos/`).
- **Public umbrella**: `<jce/api_platform.h>` → `<jce/os/platform/jce_*.h>`
- **Deps (PUBLIC link)**: `jce_core`.
- **Deps (PRIVATE)**: `SDL3_image::SDL3_image` (for icons/cursor image decode).
- **Backend**: **SDL3** for window/input/clipboard/dialog. SDL must NOT leak into public `<jce/...>` headers.

## File map

| Header | TU | Role |
|--------|----|------|
| `jce_window.h` | `jce_window.c` (+ `jce_window_internal.h`, `jce_window_modal_loop.c`) | Window create/destroy, native handle, viewport letterbox, `jce_window_set_title()` (thin SDL_SetWindowTitle wrapper) |
| `jce_window_event.h` | (in `jce_window.c`) | Window event dispatch. **P3-B.3**: `jce_engine_event` (L6) also inspects window events here to emit `jce_lifecycle_emit(FOCUS_GAINED/LOST/PAUSE/RESUME)` — see SDL event-to-lifecycle mapping table below. |
| `jce_input.h` / `jce_input_actions.h` / `jce_input_record.h` | `jce_input.c` / `jce_input_actions.c` / `jce_input_record.c` | Keyboard/mouse, action mapping, record-and-replay |
| `jce_keys.h` | (header) | Key constants — values match SDL_SCANCODE_* (no conversion overhead) |
| `jce_gamepad.h` | (in `jce_input.c`) | Gamepad enumeration + state |
| `jce_cursor.h` | `jce_cursor.c` | System cursor / custom cursor |
| `jce_clipboard.h` | `jce_clipboard.c` | Text clipboard |
| `jce_host_dialog.h` | `jce_host_dialog.c` | Native file/folder/message dialog |
| `jce_host_shell.h` | `jce_host_shell.c` | Open URL / reveal-in-file-manager |
| `jce_file_watcher.h` | `jce_file_watcher.c` | Filesystem change notification (hot-reload) |
| `jce_single_instance.h` | `jce_single_instance.c` | Single-instance lock (per-OS impl) |
| `jce_mmap.h` | `jce_mmap.c` | Read-only whole-file memory mapping (Win `MapViewOfFile` / POSIX `mmap`) with read-into-buffer fallback; backs `jce_archive_open_file()` zero-copy (spec §8) |
| (no public header — internal) | `jce_jni_bridge.c` | JNI ↔ engine bridge for Java consumers |
| (no public header — internal) | `macos/*.m` | Objective-C bits for macOS-only paths |

`codec_ports/` contains JCE-owned compiler/OS adapters for pristine codec
sources (libhevc Win32 threading and MSVC/generic macros, FDK NDK logging).
These are compiled by `engine/cmake/codec_ports/`, not the `jce_core` glob.
Upstream sources and their build directories are never edited.

## Rules

1. **All OS-specific code lives here.** No `_WIN32` / `__APPLE__` / `__linux__` etc. anywhere else in first-party code.
2. **SDL is hidden.** Public headers use `JceWindow*`, `JceInput*` opaque types or POD. Flag values may share bit layout with SDL where helpful (documented in header) but the API surface is JCE.
3. **One TU per concern.** Don't merge "window + input + dialog" into a god file.
4. **Per-platform subdir** (`macos/`, etc.) when an implementation diverges; CMake glob picks it up. Add new subdirs sparingly — prefer SDL.
5. **Image decode for icons/cursors** uses `SDL3_image` here only; runtime asset images go through `engine/src/resource/jce_image_decode.c` (stb_image).
6. **JNI bridge** is the only consumer-facing FFI bridge in the engine. Keep it minimal — Java side lives in `engine/java/com/jce/`.

## Common tasks

| Task | Steps |
|------|-------|
| Add a new input source (e.g., touch axis) | Extend `JceInput*` in `jce_input.h`, implement via SDL in `jce_input.c`; never expose SDL types |
| Add a host-side dialog | New API in `jce_host_dialog.h` → SDL impl in `jce_host_dialog.c` |
| Support a new desktop OS | Prefer relying on SDL; if a divergence is necessary add `engine/src/os/platform/<os>/` subdir |

## Don't

- Don't expose SDL types/macros in any `<jce/...>` header.
- Don't use raw Win32/POSIX in higher layers — call into this layer.
- Don't add a competing input library.

## P3-B.3 — SDL → JCE lifecycle event mapping

The actual SDL→`JceLifecycleEvent` dispatch lives in `jce_engine_event`
(L6 application), kept there because the SDL_Event struct is already
unpacked once at that point.  This table is the source of truth so
future ports (Android JNI bridge, custom event sources) keep parity:

| SDL3 event                          | JCE lifecycle event           | Notes                                    |
|-------------------------------------|-------------------------------|------------------------------------------|
| `SDL_EVENT_WINDOW_FOCUS_GAINED`     | `JCE_LIFECYCLE_FOCUS_GAINED`  | all platforms                            |
| `SDL_EVENT_WINDOW_FOCUS_LOST`       | `JCE_LIFECYCLE_FOCUS_LOST`    | all platforms                            |
| `SDL_EVENT_WINDOW_MINIMIZED`        | `JCE_LIFECYCLE_PAUSE`         | desktop pause                            |
| `SDL_EVENT_DID_ENTER_BACKGROUND`    | `JCE_LIFECYCLE_PAUSE`         | mobile pause (Android/iOS)               |
| `SDL_EVENT_WINDOW_RESTORED`         | `JCE_LIFECYCLE_RESUME`        | desktop resume                           |
| `SDL_EVENT_WILL_ENTER_FOREGROUND`   | `JCE_LIFECYCLE_RESUME`        | mobile resume                            |
| `SDL_EVENT_LOW_MEMORY`              | `JCE_LIFECYCLE_LOW_MEMORY`    | mobile mostly; bridged to streaming      |
| `SDL_EVENT_QUIT` / `_CLOSE_REQUESTED` | `JCE_LIFECYCLE_WILL_QUIT`   | fired just before returning JCE_APP_SUCCESS |
| `SDL_EVENT_TERMINATING`             | `JCE_LIFECYCLE_WILL_QUIT`     | mobile force-kill warning                |
| `SDL_EVENT_RENDER_DEVICE_RESET`     | `JCE_LIFECYCLE_DEVICE_RESET`  | **SDL_Render only — dead on the bgfx path**, see below |
| bgfx `BGFX_FATAL_DEVICE_LOST`       | `JCE_LIFECYCLE_DEVICE_LOST`   | via `jce_renderer_take_device_lost()`, polled in `jce_engine_iterate` |

## Three channels were on the wire with no accessor (2026-08-29)

`JceInputWheelEvent` already carried `.x` and `JceInputKeyEvent` already carried
`.repeat`. Neither had an accumulator or an accessor, so every polling consumer
hard-wired the horizontal wheel to `0.0f` and read key presses as strict rising
edges. Composed text had no event kind at all.

The shipped runtime's UIInputField was the visible cost: it started OS text input
on focus (so the IME opened over the game) with nothing listening, deleted exactly
one byte per HELD Backspace, and ignored sideways scroll — while the editor Game
View, which sources those from ImGui, did all three. That is a parity break the
editor could never reveal, because the two sides never shared a code path.

* `jce_input_text()` — this frame's composed UTF-8, accumulated (one frame can
  carry several IME commits), cleared by `jce_input_update`. Fed by
  `JCE_INPUT_EVENT_TEXT`, which was APPENDED before `_KIND_COUNT` — the enum is a
  wire format, so nothing may be inserted or reordered.
* `jce_input_key_repeated()` — down OR OS auto-repeat. `key_pressed` cannot
  express repeat: on a repeat frame `keys_prev` is already true.
* `jce_input_mouse_wheel_h()` — the axis that was already arriving.

Truncation on both sides of the text path stops on a UTF-8 continuation byte, so
a partial sequence never lands in a buffer.

## 2026-09-21 — `DEVICE_LOST` 零发射点,而「显而易见的修法」是错的

`JCE_LIFECYCLE_DEVICE_LOST` 在全树只出现四次:枚举声明、
`jce_lifecycle_event_to_string()` 里的 case、`contracts/abi-snapshot.txt` 里的一行,
以及**上面那张表里的一句「(no SDL counterpart yet);renderer may emit manually」**。

**出货游戏 `jce_lifecycle_register(JCE_LIFECYCLE_DEVICE_LOST, ...)` 会成功返回,
处理器是死代码,引擎/编辑器/日志都不说一声。** 授权是真的,事件永远不来。

**那句注释本身是这条缺陷活下来的原因**——它给出了一个理由,于是没人再去看。
而它已经过期了:`SDL_EVENT_RENDER_DEVICE_LOST` 在 SDL3 里**是存在的**
(`SDL_events.h:265`)。

**但照它接上去仍然是错的,这才是重点。** 那是一个 **SDL_Render 事件**
(`SDL_events.h` 里 `/* Render events */` 一节,由 `SDL_RenderEvent` 携带),
只为 `SDL_Renderer` 发出。而本引擎**只在安全模式软件回落里**创建 `SDL_Renderer`
(`jce_renderer.c:1483`「Force the pure CPU software renderer」)。
在 `jce_engine.c` 的 `DEVICE_RESET` 旁边加一个 `case` 会让它**只在诊断回落里触发,
在任何真实后端上都不触发**——同一个缺陷,披着一个变绿的计数。

⇒ **并且这说明上面那张表里 `DEVICE_RESET` 那一行同样是死的**:同样的理由。
这里**只记录、不顺手修**——让它在 bgfx 上有意义是另一个决定
(「reset」对一条 bgfx 交换链意味着什么),不是这一单的范围。

**真信号是 `BGFX_FATAL_DEVICE_LOST`**,送到 bgfx 的 fatal 回调。两个约束决定了形状:

1. **层**:renderer 不得 include `<jce/application/...>`,
   `check_layer_dependencies.py` 当场拒绝(实测:我先写错了,它红了)。
2. **线程**:除 macOS(强制单线程以绕开 CAMetalLayer 死锁)外,
   bgfx 在**渲染线程**上调这个回调,而每一个 lifecycle 监听器都是按主线程写的。

⇒ 回调置一个标志,`jce_engine_iterate` 在主线程 `jce_renderer_take_device_lost()`
取走并 `jce_lifecycle_emit`。与同一个文件里的 `jce_rcb_capture_wants_shot`
是同一个形状,存在的理由也一样。

**而进程确实活得到被轮询**:bgfx 的 `fatal()` **只在没有安装回调时** `abort()`;
装了回调就完全委派并返回(`bgfx.cpp:543`)。本引擎的回调打个日志就返回——
这是既有决定,也正是延迟投递能送到任何人手里的前提。
