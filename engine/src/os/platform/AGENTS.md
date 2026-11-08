# engine/src/os/platform — Platform / OS Wrappers (L2)

`jce_gl_entry_native_internal.h` owns the private GL function-pointer ABI used
by Renderer capability queries. It derives the type from official SDL OpenGL
scalar/calling-convention declarations; Win32 stdcall must not be replaced by
a default-C pointer cast. It introduces no GL drawing, direct symbol linkage,
loader cache or public ABI. Typed-header controls and real provider calls qualify
the ABI only; actual backend fallback/crash behavior requires target execution.


Desktop JNI lifecycle orchestration belongs to
`engine/src/application/jce_jni_bridge.c`, beside the SDL host. Platform owns OS
services only; it receives no JNI headers/definition and has no Application
include exemption. Android's separate platform integration is unchanged.

`jce_executable_path_native_internal.h` is the sole OS adapter for the Core
current-executable query, compiled by the dedicated `jce_executable_path.c` TU.
Windows grows a temporary UTF-16 buffer from 256 to 32768 code units before
strict UTF-8 conversion. The explicit XP lane validates surrogate pairing before
using conversion flags 0; modern Windows retains WC_ERR_INVALID_CHARS. No XP
flag redefinition weakens the strict UTF-8 contract. The isolated xp_os fixture
consumes the complete adapter with typed native-query/conversion ports, retains
an ordinary omitted-validation control, and checks a real current-image query;
source controls alone do not certify native or XP execution. Linux grows a bounded readlink buffer for
`/proc/self/exe` and rejects invalid UTF-8. Neither accepts possible truncation
as success or searches paths/argv. The first include selects POSIX.1-2001 only
when the caller has not selected a feature level. All temporary allocations
use the existing JCE allocator and are freed on every exit. macOS, mobile,
Web and other unsupported targets return false; actual target tests remain
required. No Platform target linkage is introduced into Core.

`jce_native_atomic_word_internal.h` is the private allocation-free counter
adapter for foundational owners compiled directly by host tools too. Static
zero initialization, native-word MSVC intrinsics and GNU/Clang relaxed atomic
builtins need no SDL, heap, lock, registration or runtime lifetime. Unsupported
compiler/architecture or non-lock-free native-word targets fail explicitly.
It is not an object-publication barrier or a multi-counter snapshot service.
Native-word wrap remains; 32-bit words are not advertised as 64-bit cumulative
precision. Web and other architectures require actual compiler validation.

`jce_thread_description_native_internal.h` owns the optional Windows native
thread-description lookup used by Core sampling and crash labels. Callers supply
native Windows declarations or an explicit typed test port. Lookup only borrows
already-loaded KernelBase/Kernel32 modules and introduces no cached registry,
loader search, allocation or new target edge. Missing exports preserve the
caller's thread-ID fallback. Returned string ownership stays with the caller's
existing LocalFree path, regardless of the native HRESULT. Runtime lookup avoids
a static dependency on the newer API; it is not complete Windows 7 validation.

> Platform abstraction. Everything OS-specific belongs HERE (and nowhere else).

The TCP adapter selects POSIX.1-2001 declarations before including any header
on Unix hosts; this keeps getaddrinfo available with strict C99 compilation.
Existing caller-selected feature levels are preserved.

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
| `SDL_EVENT_WINDOW_MINIMIZED`        | `JCE_LIFECYCLE_FOCUS_LOST`    | desktop render suspension; effective background permission decides CPU updates |
| `SDL_EVENT_DID_ENTER_BACKGROUND`    | `JCE_LIFECYCLE_PAUSE`         | mobile pause (Android/iOS)               |
| `SDL_EVENT_WINDOW_RESTORED` / `_SHOWN` | observed `FOCUS_GAINED` / `FOCUS_LOST` | query actual window focus; failed query emits neither; does not release OS/manual pause |
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

## Shared-library loading

jce_library.c owns all load/probe/symbol/unload paths. Windows explicit UTF-8
paths use LoadLibraryExW with a per-load dependency search in the module's
directory; bare system names retain normal search. Never change process-wide
DLL directories to load a project plugin. Other hosts use SDL's loader.
The first-party test_jce_library fixture keeps a real dependency beside its
module, outside the executable directory, and checks independent references.

SDL button events publish a zero-displacement position sample before the button state, so a click without prior motion cannot use stale coordinates. A one-record translation buffer prioritizes button delivery. No new input wire fields are needed.

SDL gameplay input translation ingests each physical pointer stream once: discard SDL_TOUCH_MOUSEID mouse mirrors and SDL_MOUSE_TOUCHID finger mirrors before emission. Raw window/application event dispatch remains independent, retaining synthetic pointer events for mouse-only UI. Preserve ordinary mouse button coordinates and real touch phases; test mirror rejection alongside both ordinary channels and verify actual browser gestures plus touch UI.

The public jce_input_primary_pointer query supplies logical-surface mouse or primary-touch coordinates and down state without merging their gameplay channels. Touch release preserves its final coordinate for pointer-only UI; live and replay paths share this state. Default Canvas UI uses this query. Invalid surface/output arguments do not modify outputs.

Web SDL touch events use inclusive pixel endpoints. The live SDL adapter converts them to neutral logical-surface normalization using the event window size; the pure translator and replay remain backend-independent. Do not compensate per project or alter physical mouse coordinates. Verify subpixel gesture trajectories against actual browser client coordinates.

Mouse event sampling retains same-frame DOWN/UP as a private one-frame tap,
shared by button queries, primary pointer, actions and capture/replay. Roll the
prior pulse before clearing it so the next frame exposes release;
keep physical held state independent and preserve the existing frame wire ABI.
Event-seam CPU regression: test_jce_input_mouse_tap (no window or hardware).

Keep physical previous-held state separate from previous-tap release history,
so adjacent live taps each produce a public pressed edge. The unchanged sampled
wire/action down masks preserve a single tap and its following release, but do
not encode event counts or distinguish adjacent down samples from a held button.

## Nonactivating windows and observed flags

JCE_WINDOW_NOT_FOCUSABLE is an unsigned C99 macro using the existing uint32
creation flags, without a configuration or ABI-layout change. The SDL adapter
pins its value at compile time and delegates creation to the approved SDL
implementation. jce_window_get_flags queries that adapter's current state,
returns only supported JCE bits and distinguishes valid zero from invalid
arguments. JCE_WINDOW_INPUT_FOCUS is read-only observation, not creation intent.
NULL window/backend/output returns false and preserves the caller's output.
The getter is main-thread only and borrows a live engine/window lifetime.

The exact adapter CPU fixture uses controlled SDL boundary providers without
creating an OS window. Flag transfer and bit/state observation controls do not
certify actual desktop activation, visibility, input focus or presentation.
Observe those separately on the declared host; never use topmost/raise hacks.

jce_host_font is the sole installed-font default/family lookup owner. Use on main-thread startup or resource-change, with empty output on failure; Linux fc-match has a 1000 ms wait and 4096-byte output bound and validates CJK language coverage. Do not stat/query on each frame or redistribute machine system-font bytes.

Latin defaults prefer genuinely installed Ink Free Regular, then the original
platform system Sans candidates; explicit readable user paths remain a caller's
higher-priority override. CJK keeps its independent language admission and never
uses Ink Free to claim Chinese coverage. Windows uses the known segoepr.ttf and
segoeprb.ttf faces; Linux checks fc-match actual family/style and rejects a
substitution. macOS checks registered PostScript names, family, bold trait and a
local readable CoreText URL, with automatic activation/download disabled. All
copied CoreText objects are released locally. A normal missing preferred face
falls back quietly; explicit Print mismatch/failure emits one contextual warning
per startup/resource-change lookup, never a per-frame scan. No machine fonts are
redistributed, and CPU selection controls do not certify glyph/GPU presentation.

UI_SYMBOLS appends role value 2 without changing Latin/CJK values or layout.
Windows selects the known installed seguisym.ttf face; explicit Segoe UI Symbol,
SegoeUISymbol and seguisym aliases use the same owner. Linux admits only an
exact installed symbol family/Regular style from bounded Fontconfig output;
macOS reuses the registered PostScript/family/local URL checks with activation
and download disabled. Missing Symbols leaves the existing primary/CJK chain
usable. Candidate selection is not universal glyph coverage; verify authored
codepoints against the actual selected face. No system font bytes enter packages.

With explicit JCE_PERF_LOG enabled, the successful window create owner reports
its actual logical/pixel sizes through public jce_log_write(INFO), retaining
source and runtime logger filtering even in DIST. Ordinary window bootstrap
keeps its existing compile-time log policy. This witness does not claim current
DPI/focus or later size stability; acceptance must observe those on the actual
window and must not infer them from requested dimensions.


Windows single-instance activation snapshots both the published HWND and PID,
releases the mapping handles, and checks GetWindowThreadProcessId before any
restore, foreground or taskbar attention call. Missing/stale/recycled identities
are refused; a successful return means a validated best-effort attention attempt,
not native foreground success or protection from external replacement after the
identity snapshot. Mutex/lock acquired or not-acquired observations are direct
INFO only with the shared JCE_EDITOR_CADENCE_DIAG or JCE_PERF_LOG flag, including
DIST. A lock error remains a failure, never assumed to be a duplicate success.
The actual-owner counted Windows ports and old PID equality runtime mutant do
not activate real windows or certify native focus. POSIX lock behavior remains;
its display activation is still unavailable.

The window owner caches actual SDL point size separately from configured logical
layout and drawable pixels, at creation and existing resize handling. Cached
point-size/read conversion makes no platform query and never clamps outside
points into valid input. Native ScriptHost and Canvas both convert physical mouse
points through jce_window_point_to_pixels; normalized touch is first resolved in
point space and converted once. Preserve raw input/replay coordinates and ImGui
Editor input space. test_jce_window_coordinates uses the exact SDL adapter with
counted providers; its old_scale negative control must fail by an ordinary
coordinate assertion. CPU provider checks do not certify actual desktop DPI,
window resize ordering or Native/Editor click/selection accuracy.


Private `jce_window_adapter.c` resolves the SDL HMONITOR through GetMonitorInfoW
to a GDI display name, then active Windows CCD source-name packets to the source
adapter LUID. A bounded classic DXGI enumeration resolves that LUID to the
vendor/device selection key with the pinned bgfx protocol: CreateDXGIFactory1
using IID_IDXGIFactory, EnumAdapters and GetDesc. GetDesc1 supplies only a
verified software flag; its key never replaces the classic descriptor key.
DXGI Output.HMONITOR is not a physical display
owner on hybrid drivers and is never queried by production. Source and target
LUIDs remain distinct; only the matching source LUID controls selection.
Two complete path/mode snapshots around the adapter enumeration and a repeated
window/monitor/GDI observation reject observed topology changes. Exact repeated
clone sources are allowed; differing source IDs/LUIDs sharing a name refuse.
Remote, indirect and unknown output technologies return ccd_unsupported_path.
CCD API/symbol, counts, packet-header, string, capacity and allocation failures
keep explicit reasons. Four retries per snapshot, 1024 paths, 4096 modes and a
250 ms observed-time deadline bound first-party work; this deadline cannot
interrupt an OS call. Acquired buffers, libraries and COM objects release once
on every exit. Distinct LUIDs sharing one vendor/device key remain distinct.
Such a key is representable only when bgfx's first matching record is the unique
CCD source candidate; otherwise backend_key_mismatch refuses it. Multiple source
LUID matches still refuse as dxgi_luid_ambiguous, and conflicting CCD names use
ccd_source_ambiguous. The renderer separately refuses a candidate outside its
actual Caps::gpu enumeration range. Refusal preserves validated CCD source and
counts plus the backend-expected LUID/index prediction; that is never actual
selected GPU ownership. Explicit selected caps and process-bound native GPU
LUID observations remain required for runtime acceptance.
The private provider exposes no SDL/CCD/COM type and changes no public SDK ABI.
`test_jce_window_adapter` consumes the complete owner with typed counted ports;
wrong-LUID, old-DXGI-output, old-strict-key and blind-first-key same-source controls
must fail ordinary unchanged
assertions. These CPU controls do not certify native selection, Copy traffic,
DWM ownership or physical low-end performance. Query once per renderer creation,
reuse across backend attempts, and never hot-migrate the running device.

Windows high-performance observation uses the same classic factory and
selection-key matcher. An optional runtime IDXGIFactory6 query ranks adapters
with DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE. The first hardware result owns the
request. Software entries are released and skipped. An unrepresentable first
hardware result refuses the preference. Two bounded rank observations around
the classic enumeration must agree on rank, LUID and descriptor key, and the
factory must remain current. Unsupported headers/interfaces, no hardware and
other observation failures retain explicit reasons for the existing fallback.
Ranked source LUID and preference index are distinct from the classic adapter
index and its predicted first-match identity. This port does not inspect CCD
or change the display routing, link imports, or public SDK ABI.

Private Web sample-query interop stays in jce_webgl_query_internal.c/.h and
jce_webgl_interop.c/.js. A linker wrap reaches the actual Emscripten proc-address
entry; the CPU pointer probe proves that seam separately from physical WebGL.
The private C owner holds the sole renderbuffer SAMPLES format gate. Legal
queries delegate to pristine Emscripten; only the first owned output is zeroed
so empty lists cannot leave bgfx's sample uninitialized. Invalid target/format/
pname retain INVALID_ENUM through GL.recordError without issuing a browser
invalid query; invalid values and unavailable contexts remain distinct errors.
RGB/RGBA query aliases, integer empty results and observed float/half/norm16
extensions retain their real behavior; storage-only DEPTH_STENCIL is not mapped.
The JS bridge observes only the actual current WebGL2 context and enabled
extension objects, with one bounded current-context cache. Query-observed loss clears that
cache; the next valid observation and replacement re-observe. Actual VERSION/RENDERER strings
are logged on context changes, without certifying tier admission, GPU ownership
or completion. No browser prototype mutation, console filter, upstream patch,
second backend or public ABI is added. Native builds exclude the Web boundary.

Resizable Web windows add SDL3's private SDL_WINDOW_FILL_DOCUMENT creation flag
in the existing window owner. SDL3.4.0 owns canvas/document size changes and
resize-event publication; RESIZABLE alone does not bind document size. Fixed
Web windows and native creation retain their requested policy and original
flags. No second JS resize service or new public ABI flag is introduced; the
observed public flag query still filters private backend bits. The full-owner
window-fill fixture models the Web selection after native public/SDL headers
with typed SDL ports and checks fixed/native behavior, existing size-cache/
resize consumption and create refusal. Its missing-fill variant must compile
and fail ordinary unchanged assertions. These CPU controls do not certify
actual browser resize delivery, DPR, rendering or presentation.

Web context observations share `JceWebGlQueryContext` in the existing interop
bridge. Actual VERSION, GLSL and renderer strings are cached with one current
Emscripten handle and a nonzero epoch. WebGL1 can be observed for version refusal;
only WebGL2 admits sample queries. Observed loss, native canvas loss/restored
events, handle replacement and explicit cache reset invalidate the epoch;
listeners detach on replacement/reset. No preventDefault, GL context creation,
prototype mutation, cap fabrication or second registry is introduced. Private
`jce_webgl_context_internal.h` transports the bounded snapshot to L3, including
equal begin/end epochs; no public ABI changes. Node controls compile the entire
actual JS owner into counted CPU ports and separately omit error, observed-loss
invalidation or epoch publication. They do not certify real context loss/rebuild.

The private jce_filesystem_presence_native_internal.h is compiled by the
existing Core filesystem host owner, like the private process-handle bridge.
SDL3.4.0 GetPathInfo clears its output before returning false, including missing
and access failures; false/NONE is not an absence signal. This adapter never
parses SDL error text: Win UTF-8 conversion/metadata errors and POSIX stat errno
remain here. Only FILE/PATH_NOT_FOUND or ENOENT prove absence; nonregular,
reparse, access, allocation/conversion and unsupported cases stay UNKNOWN.
CPU port controls and native temporary-file probes do not certify every SDK
architecture or physical Web execution. No upstream implementation is copied.

## Current input snapshot replay origin

`jce_input_snapshot_was_replayed` is a read-only CPU-snapshot observation on the
existing opaque JceInput, not an environment request or a second input service.
Only successful canonical JIRC replay apply sets it. Frame-end input_update,
ordinary apply (including refusal), valid live ingestion and replay attempt
failure/EOF clear it. The private replay-apply/clear entry points stay in
jce_input_internal.h; no public setter exists. JceInputFrame and JIRC bytes,
versions and strides are unchanged. Main-thread callers borrow the live input.
An input snapshot may be replayed even if the file later fails or is closed;
this flag does not certify file durability, physical input or application use.

## A1 XP OS entropy adapter (experimental)

`jce_entropy.c` selects XP CryptoAPI only when `JCE_WINXP_LEGACY=1` in
the isolated LLVM-MinGW-XP C99 probe.  Modern Windows retains the CNG
implementation; the native C ABI, `true`=OS CSPRNG success and `false`=
unsafe developer fallback semantics remain unchanged.  Do not use a
false-return buffer to generate release encryption or signing keys.
The XP probe statically audits `advapi32!CryptGenRandom` and rejects
Bcrypt.dll imports; actual VM execution is a separate required check.

## Private fixed-width atomics and publication

`jce_native_atomic_integer_internal.h` adds allocation-free signed 32/64-bit
post-add operations with defined modulo wrap and sequentially consistent RMW.
Callers provide live writable objects aligned to 4/8 bytes; packed or concurrent
ordinary accesses are unsupported. The new acquire-load, release-store and
strong acquire/release CAS wrappers reuse `JceNativeAtomicWord`; CAS failure
has acquire ordering. The original `jce_native_atomic_word_internal.h` remains
unchanged and promises at least relaxed counter operations, not publication.

Supported compiler/architecture adapters require lock-free 4/8-byte operations;
the x86 64-bit path requires CMPXCHG8B. There is no heap, OS lock or plain-RMW
fallback, and lock-free does not mean wait-free. The real-header
`tests/compatibility/xp_flecs/atomic_control.c` and typed bridge controls keep
integer and callback evidence separate from full Flecs-world/Editor validation.

`compatibility/jce_windows_xp_sync_internal.h` owns only the explicit private
LLVM-MinGW-XP exclusive SRW subset, using pointer-typed acquire/release atomics
and exact-current-process exit 91 for impossible state. The mimalloc consumer
header adds aliases after native declarations and conservatively refuses its
five optional NUMA probes when the real current-CPU export is missing; it
borrows Kernel32 without loading, caching or inventing a processor index.
This is private capability refusal, not native API equivalence. Codec-only
extensions live in separate `jce_windows_xp_codec_sync_internal.h` and
`jce_windows_xp_codecs_internal.h`; they do not broaden the mimalloc port.
Reviewed codec consumers admit only infinite exclusive condition waits,
synchronous flags-zero once with NULL completion context, and truthful
unavailable processor-group affinity. Each active wait owns one kernel event;
valid wait-resource failure stops the exact current process instead of letting
callers spin after ignored errors. Failed/duplicate once completion never
publishes success or resets completed state. A later generated codec header can
raise its declaration floor; the entry-floor guard is not uniform preprocessing
or full XP qualification. Actual decode, worker retirement and guest execution
remain separate checks.

`jce_windows_xp_stdio_internal.h` is an explicit consumer-only port for reviewed
Flecs/miniz/STB byte-file calls. It forwards standard modes to system MSVCRT
`fopen`, `_wfopen` and `_wfreopen`, retaining wide paths and real reopen
consumption on failure. It rejects secure/Unicode-stream mode extensions and
invalid arguments, clears a valid output slot, and does not emulate secureCRT
sharing or invalid-parameter handlers. No global CRT/linker symbols or public
API are introduced; modern builds do not include these aliases.

`jce_windows_xp_bx_memory_internal.h` owns the reviewed BX large-page-size
query only. It borrows loaded Kernel32, calls the real export when available
and reports zero when unavailable, preserving BX's documented unsupported
page-size result. It does not allocate large pages or introduce a global OS
symbol shim. Native SDK declarations precede the alias; scoped `near`/`far`
macro preservation avoids changing BX math declarations. This separate port
does not broaden the codec or mimalloc synchronization subsets.

The isolated C99 xp_mimalloc fixture consumes the complete synchronization
and mimalloc headers with unchanged mimalloc atomics; its serial omitted-try
assertion runs before concurrency. Source controls do not certify native/guest
execution, full Editor operation or graphical support.
## Private optional DbgHelp refresh

`jce_dbghelp_native_internal.h` borrows already-loaded DbgHelp and resolves only
`SymRefreshModuleList`. The typed WINAPI call preserves the real process handle,
BOOL result and last-error value. An absent module/export returns FALSE with
ERROR_CALL_NOT_IMPLEMENTED; it never loads another DLL or invents success.
Symbol initialization, ordinary symbol fallback and sampler lifetime stay with
the Core owner. There is no public ABI, global lookup cache or linker shim.
Typed-port controls and real x86/x64 calling-convention controls are separate
from exact-guest import admission and complete Editor/GPU qualification.
