# engine/include/jce/ — Public C API (THE CONTRACT)

> **This directory is the only thing consumers see.** Every header here is part of the published ABI/API. Treat changes as breaking unless additive.

## Identity
- **Layer**: L1–L6 (public surface for every layer)
- **Language**: **C99** with `extern "C"` guards for C++ consumers
- **Audience**: game code (`caged_kingdom`), editor, third-party native FFI (JNI, P/Invoke, Python ctypes, Lua ffi…)

## File map
- `api.h` — master umbrella; includes all `api_*.h` in dependency order. Documents L1→L7 layering.
- `jce.h` — minimal convenience header (version + compat only).
- `jce_compat.h` — compile-time platform / arch / SIMD / C99 assertions (L0 baseline).
- `jce_version.h.in` — CMake-configured `JCE_VERSION_*` macros.
- `api_core.h` (L1)        — alloc / log / math / jobs / fs / time / string / hash
- `api_platform.h` (L2)    — window / input / dialog / clipboard / power (SDL hidden)
- `api_graphics.h` (L3)    — low-level GPU primitives (bgfx hidden)
- `api_animation.h` (L3)   — skeleton / clip / blend / SM / IK
- `api_audio.h` (L3)       — mixer / sources / reverb (miniaudio hidden)
- `api_resource.h` (L3)    — PAK / asset / bundle (PhysFS hidden)
- `api_streaming.h` (L3)   — async bundle streaming
- `api_render.h` (L4)      — high-level RenderGraph + material/mesh/camera/light
- `api_scene.h` (L5)       — ECS world / entity / components / prefab (flecs hidden)
- `api_app.h` (L6)         — `JceEngine` lifecycle, subsystem registration
- `api_input.h`            — input mapping (paired with `api_platform.h`)
- `api_middleware.h`       — middleware umbrella
- `api_ai.h` / `api_net.h` / `api_physics.h` / `api_ui.h` — cross-cutting
- Sub-folders (`os/`, `renderer/`, `resource/`, `runtime/`, `application/`, `middleware/`, `ui/`) — fine-grained public headers grouped per layer; included by the `api_*.h` aggregators above.

## Rules (NON-NEGOTIABLE)
1. **C99 only.** No `//` only after `/* */`, no VLAs, no designated-init inside macros. Wrap every header with `#ifdef __cplusplus extern "C" { ... }`.
2. **Zero third-party leakage.** Never `#include <SDL.h>`, `<bgfx/...>`, `<flecs.h>`, `<imgui.h>`, `<bullet/...>`, `<miniaudio.h>`, `<physfs.h>`, `<enkiTS/...>`, `<mimalloc.h>`, `<Tracy.hpp>`. All such types appear as **opaque handles** (`typedef struct JceXxx JceXxx;` + `JceXxxHandle`).
3. **No platform headers.** No `<windows.h>`, `<unistd.h>`, `<pthread.h>`, `<sys/*>`. Use `jce_compat.h` features only.
4. **No platform macros** (`_WIN32`, `__APPLE__`, `__linux__`, `__ANDROID__`, `__EMSCRIPTEN__`). If a public symbol differs per OS, hide it behind a function pointer table.
5. **Layer order in `api.h` is law.** Higher-layer headers may include lower-layer ones, NEVER reverse.
6. **Naming**: functions `jce_<module>_<verb>()`, types `JceXxx`, enums `JCE_<MODULE>_<VALUE>`, macros `JCE_UPPER`.
7. **No inline implementation** beyond trivial getters; keep TU coupling minimal. Definitions live in `engine/src/**`.
8. **ABI-stable structs**: add new fields only at the END, gated by `size`/`version` fields; never reorder.
9. **No C++ features**: no `bool` from `<stdbool.h>` exposed in struct layouts (use `int` or `uint8_t` for ABI), no `_Generic`, no anonymous union in public structs unless `JCE_C11` is asserted. **Recorded exemption — tagged unions:** `JceEvent` (`os/platform/jce_window_event.h`) and `JceInputEvent` (`os/platform/jce_input_event.h`) use an anonymous union. `JCE_C11` does not exist in this tree; every supported compiler accepts the construct in C99 mode, and naming the member would put two spellings of the same access into every consumer. The exemption is for the union ONLY, and it does not certify either struct's full layout: `JceInputEvent` uses `uint8_t`/`int32_t` throughout the rest of its fields, so the union is the only thing it needs excused; `JceEvent` predates this rule and does not itself comply — `JceKeyboardEvent.repeat` is `bool` and `JceEvent.type` is the enum `JceEventType`, not an ABI-stable integer — so it is cited here only as the anonymous-union precedent, not as a template for the rest of rule 9. No new public struct may reintroduce `bool` or `_Generic` under cover of this note.
10. **Doxygen-style comments** on every public symbol; describe ownership, threading, and error model.
11. **Frame-pointer & symbol export**: every public function must be `JCE_API` decorated (defined in `jce_compat.h`) so DLL exports + FFI work everywhere.
12. **Script asset text is VFS-only**: `jce.asset_read_text()` accepts normalized relative asset paths, rejects absolute/traversal/backslash forms, and caps a single read at `JCE_SCRIPT_TEXT_ASSET_MAX_BYTES` (1 MiB).
13. **Runtime touches are transient**: `JceRuntimeInput.touches` is append-only ABI state at the end of the struct, bounded by `JCE_RUNTIME_MAX_TOUCHES` (5), replaced in stable platform order by `jce_runtime_set_touch_input()`, and cleared at the next step unless the host resubmits it.

## Don't
- Don't add a header that pulls in `<stdio.h>` or `<stdlib.h>` for `malloc/fopen`. Use `jce_alloc_*` / `jce_fs_*`.
- Don't introduce STL or templates here, ever.
- Don't expose `flecs_world_t*`, `bgfx::ViewId`, `SDL_Window*`, etc.
- Don't add new public headers without also extending `api.h`.
- Don't move a header between layers without updating `api.h` AND `engine/CMakeLists.txt` include propagation.

## Common tasks
- **Add a new public function** → declare in the layer's `api_<layer>.h`, implement in `engine/src/<layer>/`, add Doxygen, version-bump `JCE_VERSION_MINOR` (additive) or `_MAJOR` (breaking).
- **Add a new opaque handle** → `typedef struct JceFoo JceFoo;` here; full definition stays in `engine/src/**` private header.
- **Deprecate a symbol** → mark `JCE_DEPRECATED("use jce_bar instead")`, keep for one minor cycle, then remove on major bump.


## 2026-09-20 — `jce_curve.h`：**两侧的 out 参数契约可以不同，而且都对**

`JceScriptHost.curve_eval` 失败时**不碰** `*out_value`（调用方的默认值得以存活）；
生成的 `jce_script_api_curve_eval` 转发器在 absent 分支上**把每个 out 槽 memset 成 0**，
对每一个 `fallible_out` 都如此。跨 C ABI 交回一块**没被写过**的缓冲区
比交回一个确定值更糟，所以两条规则各自在各自的位置上是对的——
**它们只是不是同一个承诺**。两边的注释现在都写了这句。

这是被 `test_jce_script_api_abi.c` 抓到的：我把 host 表那句话写进了
转发器的测试里。**那个测试本身值得记住**——它断言
`JCE_SCRIPT_API_TAIL_ENTRY` 等于一个字面量，作用是：
`JceScriptHost` 一追加，尾部成员就变，而「短 host 不被读过界」这个用例
**必须重新按新尾部成员的原型改写**。保留旧调用会让它**继续绿着并且什么都不测**——
上一个尾部成员已经不再被截断掉了。


## 2026-09-21 — 往 enum 里追加，动的是**哨兵**

给 `JceSeqPropId` 追加十一个 UI 属性，ABI 快照判的是
**incompatible change 而不是 append**，而它是对的：真正变的是
`JCE_SEQ_PROP_COUNT`（17 → 28）。任何**按它定过尺寸**的东西——引擎自己的
`k_prop_names`、编辑器的选择器循环、SDK 消费者的表——在重建之前都与库不一致，
**而越界索引不会大声失败**。

这是 `JceScriptHost` 那条规则的 enum 形态：在中间插入成员 ⟹ 签名不同的指针被
静默调用；在这里 ⟹ 尺寸不对的数组被索引。**同一个机制，两种外衣，都不报错。**

⇒ 判据不是「我追加在最后所以安全」。**只要 `_COUNT` 会动，追加就是一次 ABI 声明**，
要么写下「消费者必须重建」，要么这个 enum 根本不该有 `_COUNT` 哨兵。
本引擎头与库一起发，所以答案是重建——**写下来，而不是默认**。
线格式不受影响：`.seq` 存的是**点号名字**（`bindProp` 与旧的 `"<id>/<prop>"`），
从来不是整数，所以已授权的序列在任何重新编号下都存活。
