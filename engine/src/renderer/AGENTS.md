# engine/src/renderer — Renderer Layer (L3)

> Sub-memory for the renderer. See repo-root `AGENTS.md` for global rules.

## Identity

- **Layer**: L3 (Renderer) — sits above OS, below middleware.
- **Language**: **C99** (.c). Implementation TUs ending in `.cpp` only when bridging C++ third-party (none here today).
- **Public headers**: `engine/include/jce/renderer/jce_*.h`
- **Public umbrella**: `<jce/api_graphics.h>` (low-level) + `<jce/api_render.h>` (graph/scene/lighting/postfx).
- **Backend**: **bgfx** is the ONLY graphics backend. Do not introduce raw GL/Vulkan/D3D/Metal calls — go through bgfx.
- **ECS**: rendering data lives in flecs components (`jce_renderer_ecs.*`).
- **Internal headers**: under `engine/src/renderer/internal/` and `*_internal.h` — never include from outside this directory.

## File map (current)

| File | Role |
|------|------|
| `jce_renderer.c` / `jce_renderer_caps.c` | bgfx init, frame loop, capability query |
| `jce_render_graph.c` / `jce_render_queue.c` | Render graph & sorted queue |
| `jce_scene_renderer.c` | Scene-level render path (ties ECS → passes) |
| `jce_gpu_scene.c` | Adaptive GPU Scene / MDI backend: per-model visibility groups, compute compaction, shared-survivor indirect draws, bounded growth, frame counters, and CPU fallback |
| `jce_renderer_ecs.c` | flecs components for renderable, light, camera, etc. |
| `jce_camera.c` / `jce_views.c` | Camera + view matrices |
| `jce_mesh.c` / `jce_skinned_mesh.c` / `jce_model.c` / `jce_primitives.c` | Geometry |
| `jce_material.c` / `jce_pbr_material.c` / `jce_shaders.c` / `jce_shader_manager` | Materials + shader management |
| `jce_texture.c` / `jce_image.c` / `jce_stb_image_impl.c` | Texture upload + image decode + **runtime mip streaming** (P3-A.2: per-texture / global bias, residency requests, caps-floor matrix). Cooked format dispatch is fail-closed and includes `R/RG/RGBA 16F/32F`; unsupported backend formats never fall back to RGBA8. |
| `jce_lighting*.c` / `jce_light_cluster.c` / `jce_ibl.c` / `jce_lightmapper.c` | Lighting (forward+ cluster, IBL, baked).  `jce_ibl.c`'s two convolutions are pure functions of a DIRECTION and take their sampler as a parameter, which is how the reflection probe reuses them (`jce_ibl_convolve.h`) instead of carrying a second copy of the same integrals.  Two conventions live here and the difference is PI: the sky path passes PI, the probe path passes 1.0, and `fs_pbr_body.sh` wants the latter -- see the note on `ibl_convolve_irradiance_f32`. |
| `jce_csm.c` / `jce_shadow_filter.c` | Cascaded shadow maps |
| `jce_postfx.c` / `jce_taa.c` / `jce_ssao.c` / `jce_ssr.c` / `jce_volumetric_fog.c` | Post processing |
| `jce_render_pipeline.c` | Render Pipeline Asset (`.rp.json`) — JSON I/O, 4 tier presets, boot autopick, cached descriptor surfaced via `jce_render_pipeline_is_feature_enabled()` (P3-E.4) |
| `jce_reflection_probe_bake.c` | Reflection probe bake worker (P3-E.3) — single-slot dedicated `jce_thread`, atomic status/progress/cancel.  The faces come from `jce_scene_probe_capture.c` (six real 90-degree renders); the CPU procedural sky gradient is now only the FALLBACK for a host with no renderer, and it has an f16 twin so an HDR bake that falls back still produces a container at the stride its header declares. Encode stage now writes a **real KTX1 cubemap** via the `jce_ktx2_writer` bridge + an `.irr.ktx` irradiance sidecar (placeholder source data shared with the specular write for v2). Rewrites caller-supplied `.cube`/`.ktx2` suffixes to `.ktx` in place. Stable public API in `<jce/renderer/jce_reflection_probe_bake.h>`.  **Steps 2 and 3 used to do no work at all** -- the irradiance sidecar was a copy of the specular faces and the container declared one mip, so every roughness read the mirror image and the diffuse term read a mirror of the scene.  They now call the real integrals.  Mip 0 is copied from the source, not integrated: roughness 0 is a mirror, and integrating it cost ~800M samples to compute a copy.  **`desc.hdr` carries RGBA16F through the whole worker** -- both convolutions, the chain allocation (`mipchain_bytes` counts ELEMENTS, so the byte count doubles and the element count does not) and both writer calls.  The one place that must agree and is easy to miss is the SUBMIT-time copy of the caller's faces: it sized itself at 4 bytes per texel while the worker read 8, so an HDR bake read faces 3-5 past the allocation and faces 0-2 looked right. |
| `jce_ktx2_writer.h` / `jce_ktx2_writer.cpp` | Private C↔C++ bridge to bgfx's bundled `bimg::imageWriteKtx`. In-memory `bx::WriterI` backed by `jce_alloc`, blob flushed via `jce_fs_host_write_all` (no raw `fopen`). Ships **KTX1** because bimg has no KTX2 supercompression writer in the vendored bgfx — file is still parseable by bgfx + RenderDoc + KTXSoftware. Also exposes `jce__ktx_load_cubemap(path)` (P1-baked-gi-consume): a **hand-rolled KTX1 reader** that loads the cube via `bgfx_create_texture_cube`.  It takes the pixel format FROM THE CONTAINER (`glInternalFormat`, with `glType` cross-checked only when non-zero -- bimg writes 0 for RGBA8): 0x8058 -> 4 bytes, 0x881A -> 8 (RGBA16F), anything else refused rather than guessed.  It used to assume 4 in three separate places, which was true while the writer could only emit RGBA8 and stopped being true the moment an HDR probe could be baked. It does NOT use `bimg::imageParse` on purpose — that pulls in `bimg_decode`'s vendored `miniz`, which collides at link with the engine's `zip` dep (LNK2005). NOT a public header.  **TWO LAYOUTS, transposes of each other:** KTX1 (and `imageWriteKtx`'s source pointer) is MIP-MAJOR -- each mip, then its 6 faces; `bgfx_create_texture_cube` wants SIDE-MAJOR -- each face, then its whole chain.  `jce__ktx_parse_cubemap` does that transpose and is split out so a test can round-trip the real container with no GPU.  This file's own comment claimed face-major and was believed once. |
| `jce_particles.c` / `jce_gpu_particles.c` | CPU/GPU particles.  Both submit the SAME `fs_particle` program, so anything per-draw (texture flag, blend, the soft-particle depth fade) must be wired in BOTH or it is a setting that works for some emitters.  Soft particles: `jce_particles_set_soft_fade_distance()` is the process-global both hosts write; `JceGpuParticleSoft` carries the frame's depth into the GPU draw via `_render_soft` (`_render_ex` keeps its ABI and delegates with NULL). |
| `jce_sprite.c` / `jce_sprite_batch.c` / `jce_text.c` / `jce_skybox.c` / `jce_decals.c` | 2D/quad systems. **`jce_text.c` batches a glyph run into ONE draw** via `jce_draw_textured_quads_view` (`jce_renderer_internal.h`); it submitted one draw call PER GLYPH until 2026-09-01. The batch breaks only on an atlas change (`g->dynamic`), a full staging array, or the end of the run — all three preserve submit order, which IS draw order on the SEQUENTIAL UI views. `jce_sprite_batch.c` is unrelated: world-space, 4x4 transform per sprite. The quad geometry is checked headless by `jce_primitives_self_test()` (`jce_primitives_selftest.h`). |
| `jce_debug_draw.c` | Debug line/box/text drawing |
| `jce_occlusion_culler.c` | Occlusion culling |
| `jce_offscreen_target.c` | Offscreen rendertargets (editor scene view) |
| `jce_gltf_loader.c` | glTF mesh/skeleton loader (consumed by resource layer) |
| `jce_gpu_caps.c` | GPU caps query / feature tier |
| `jce_lowlevel.c` | Thin bgfx wrappers for graphics API |
| `jce_material_registry.c` | Dev-mode `.mat.json` mtime watcher (`jce_material_registry.h`); inert outside `--dev` mode. Callback patches MeshRenderer in-place on disk change. |

## Rules (renderer-specific)

1. **No SDL** — windowing belongs to `os/platform`. Renderer receives a native window handle via bgfx init.
2. **Do not include bgfx headers in public `jce/renderer/*.h`** — wrap handles as opaque `JceXxxHandle` typedef'd ints. bgfx headers only appear in `*.c` / `*_internal.h`.
3. **No game logic** — renderer reads ECS, writes nothing about gameplay. Components live in `middleware/scene`; renderer's own components are pure rendering data.
4. **Baseline budget** — must run on integrated/no-GPU hardware. Prefer forward-clustered over deferred by default; toggle expensive passes (SSR/SSAO/volumetrics/TAA) via render graph flags, default OFF on low-tier devices (check `jce_gpu_caps`).
5. **Shaders** live in `engine/shaders/`, cross-compiled via bgfx's `shaderc`. Per-backend variants generated at build time. Don't hand-write per-API shaders.
    - Sub-folders: `standard/` (normal builds compile for `glsl_120`/`essl_300`/`dx11_sm5_0`; the manually requested `essl1` compatibility subset is not in any shipped auto profile), `pbr/`, `decals/`, `particles/`, `postfx/`, `ssao/`, `ssr/`, `volfog/`, `weather/`, `imgui/` (editor-only).
    - Files: `vs_<name>.sc` + `fs_<name>.sc` (+ optional `cs_<name>.sc`), shared IO via `varying.def.sc`. Output is build-local under `<build>/generated/engine_assets/shaders/`; generated binaries never write into the source tree.
    - Add via `tools/compile_shaders.cmake` shader list; bump `JCE_SHADER_BUNDLE_VERSION` on breaking changes.
    - Uniforms: stable slot order via `uniform vec4 u_<name>;` — reorder breaks runtime binding. Size budget: `standard/` < 5 KB compiled / backend, postfx < 20 KB.
6. **Memory**: textures/buffers via `jce_alloc` + bgfx's own allocator interface routed to mimalloc.
7. **Math**: only `jce_math` (`jce_vec3`, `jce_mat4`, etc.). Never include cglm or roll your own.
8. **Profiling**: each pass `JCE_PROFILE_ZONE("pass_name")` so Tracy GPU/CPU traces line up.
9. **Adding a pass**: register it in `jce_render_graph.c`, allocate resources via the graph, integrate into `jce_scene_renderer.c` per-view path.
10. **File caps**: hot files like `jce_scene_renderer.c` approach the limit — split before adding large features (see `jce_render_queue.c` / `internal/` pattern).
11. **Mip streaming** (`jce_texture_*_mip_*` API): `effective_top_mip = max(global_bias, per_texture_bias, caps_floor)`, clamped so the 4×4 mip-tail is always resident. Caps-floor matrix: **Low → 1**, **Mid → 0**, **High → 0**. The global bias is driven by the streaming pressure hook installed in `engine/src/resource/jce_world_streamer.c` (OK→0 / SOFT→1 / HARD→2); textures must opt-in (call any of the streaming setters) to be affected. **Honest-demotion rule (audit F29):** a demotion is physically applied **only** for source-backed textures (those holding a retained CPU mip-0). Default textures keep no redundant CPU mirror (low-RAM baseline), so their requests are recorded as intent and `get_size`/`get_resident_top_mip` keep reporting the **true** GPU level — never fake a shrink that did not happen. Real default-texture streaming awaits an on-disk mip-chain asset format (stream from PAK, not a RAM mirror). Do **not** "fix" this by stashing a CPU copy of every texture.

## Common tasks

| Task | Steps |
|------|-------|
| Add a post-fx | New `jce_<fx>.c/h` under renderer; add public header to `<jce/renderer/>`; hook into `jce_postfx.c` chain; expose enable flag in render graph |
| Add a material feature | Extend `JcePbrMaterial` → add uniform in shader → update `jce_shader_manager` permutations → write inspector drawer (editor side) |
| Add a renderable component | Define struct in `jce_renderer_ecs.h`, register in `jce_renderer_ecs.c`, consume in `jce_scene_renderer.c` |
| Lower-tier fallback | Query `jce_gpu_caps`, branch in render graph build, never `#ifdef _WIN32` |

## Don't

- Don't expose bgfx types in `<jce/...>` public headers.
- Don't allocate per-frame on the hot path — use ring/pool allocators from `os/core`.
- Don't add a 5th shadow algorithm without removing/refactoring an existing one.
- Don't `#include <SDL3/...>` or `<windows.h>` here.

## P3-E.5 — Light Cookies + IES Profiles

Spot lights ship end-to-end (cookie texture + IES LM-63 photometric LUT) via `JceSpotLight` fields (`cookie_texture` / `ies_lut_texture` / `cookie_strength` / `cookie_path` / `ies_path`) and matching `JceSpotLightDesc` fields. Sampler slots **13 = `s_cookie`** (a 2D **ARRAY**) and **14 = `s_iesLut`**. New public header: `<jce/renderer/jce_ies_profile.h>` (`jce_ies_parse`, `jce_ies_bake_lut_from_file`, `jce_ies_bake_lut_from_memory`).

**EVERY light projects its OWN cookie** (2026-09-18). The v1 note that used to be here — "binds the FIRST spot light that has a cookie / IES" — is gone, and so is `u_cookieParams` (now `u_iesParams`: `.x` has_ies, `.y` ies_spot_index) and `u_cookieSpotVP`. What a light carries now:

- its **atlas layer** in `u_spotLights[i*4+3].y` / `u_dirLights[i*2+1].w`, registered by `jce_light_env_register_cookie`;
- its **strength** in `u_spotLights[i*4+3].z`;
- its **frustum**, DERIVED in `fs_pbr_main.sh` from its own position, direction and outer cone — reproducing `jce_m4_perspective(2*acos(outerCos), 1, ..) * jce_m4_look_at()` term for term, so per-light projection costs no uniform and the shared mat4 upload is gone.

The layers are filled by one `bgfx_blit` per (slot, texture) on `JCE_VIEW_MORPH_DEFORM` — a view id BELOW every scene base, because bgfx orders blits by view id exactly as it orders draws, and a blit on a higher id lands after the colour pass that samples it.

**There is no 2D fallback, deliberately.** `s_cookie` is `SAMPLER2DARRAY` in every program, so a plain 2D texture bound there is a sampler TYPE mismatch: desktop D3D/GL tolerate it silently, WebGL2 rejects the whole draw. When `BGFX_CAPS_TEXTURE_2D_ARRAY` is missing (no backend at this engine's GL 3.1 / GLES 3.0 tier floor is), the bind is a 1×1 white **one-layer array** and every slot is forced to 0 — cookies switch off cleanly. `JCE_COOKIE_NO_ARRAY=1` forces that path, which is the cookies-on/off ablation without editing a scene.

**No shader permutation guards any of this.** bgfx rewrites each shader's `#version` to 140 at the GL floor and emits `#define texture2DArray texture`, so profile-120 source compiles to valid GLSL 1.40 — read in bgfx's `renderer_gl.cpp`, not assumed. An earlier note claiming the desktop GL profile 120 made `texture2DArray` impossible was wrong; the lever for a newer GL is the graphics TIER, never that profile.

**Zero is a valid bgfx handle.** A `memset` desc therefore claims a cookie it never asked for, and the atlas used to treat handle 0 as its "empty slot" sentinel — both measured, both fixed. Build light descs with `jce_dir_light_desc_default()` / `jce_spot_light_desc_default()`; the atlas sentinels are `JCE_COOKIE_SLOT_EMPTY` / `_RESERVED`.

**Rebuild shaders** after touching `fs_pbr.sc`: run shaderc against `engine/shaders/pbr/fs_pbr.sc` to refresh `_cooked/*/shaders/*/fs_pbr.bin`.

## P3-E.5b — Directional cookie + multi-cookie atlas

Directional light cookies now project. CPU builds `u_cookieDirVP` as a **world-aligned ortho box** (±50 m half-extent, 0.1–200 m near/far) centred on the camera position and aimed along the chosen directional light's forward — v1 chooses stability over CSM-tight fit (CSM cascade-0 reuse is the v2 path). `fs_pbr.sc` adds a `u_cookieDirParams` vec4 + `u_cookieDirVP` mat4 and a directional-loop branch that samples `s_cookie` with clamp-to-zero outside `[0,1]`. Sampler 13 is no longer shared — see the section above. The directional VP is still ONE matrix, and legitimately so: there is at most one cookie-bearing directional light (the sun), and `u_cookieDirParams.x` gates the whole block. Its atlas layer comes from the light's own pack (`u_dirLights[i*2+1].w`); the dead array code read `u_cookieDirParams.w`, which is packed as literal 0, so it would have sampled the white layer forever.

The multi-cookie **atlas** is plumbed: `JceLightEnv` owns a 16-slot LRU table (`JCE_COOKIE_ATLAS_CAPACITY` = 16, slot 0 reserved white default), and `jce_light_env_register_cookie()` returns a stable per-handle slice index used in `u_dirLights[i*2+1].w` and `u_spotLights[i*4+3].y`. Eviction policy: smallest `last_used` counter wins. Sampler 13 still single-binds (one cookie image per draw) on the current ship — the indices unlock per-light array sampling the moment `s_cookie` is promoted to `SAMPLER2DARRAY` and `fs_pbr.sc` swaps `texture2D` → `texture2DArray` (no CPU changes required). Promotion gated on `BGFX_CAPS_TEXTURE_2D_ARRAY` (already queried in `jce_gpu_caps.c`) + a `bgfx_blit` slice-population pass into a fixed-size array texture (deferred — requires authoring cookies at a canonical resolution, e.g. 256×256, to satisfy bgfx blit equal-rect rule). **Rebuild shaders**: `fs_pbr.sc` adds `u_cookieDirParams` + `u_cookieDirVP` — run shaderc to refresh `_cooked/*/shaders/*/fs_pbr.bin`.

## Shader cross-backend portability (`engine/shaders/*.sc`)

Every normally shipped `.sc` compiles to HLSL s_5_0, SPIR-V, GLSL 330, ESSL 300,
Metal). Danger class: code that compiles everywhere but *means something different
per profile* — ships green, renders wrong on the backends nobody checked.
`tools/shader_lint.py` enforces the mechanical rules at build time (a finding fails
the build). (Relocated here from the former `engine/shaders/AGENTS.md`, which the
AGENTS.md placement lint excludes — rules for excluded dirs live in the parent.)

**Iron rules (lint-enforced):**
1. **Never use a raw multi-argument matrix constructor.** `mat3(a,b,c)` packs
   args as rows on HLSL, columns on GLSL — same source, transposed matrix per
   backend. Use `mtxFromCols(a,b,c)` / `mtxFromRows(a,b,c)` from `bgfx_shader.sh`.
   Single-arg forms (`mat3(someMat4)` truncation, `mat3(1.0)`) are portable/allowed.
2. **Never combine a matrix with `*`.** GLSL `*` is the matrix product; HLSL `*`
   is component-wise. Always `mul(a,b)`. Convention: `mtxFromCols` matrices
   multiply column vectors from the right — `mul(M, v)`. Escape hatch for a
   verified exception: `// shader-lint: allow` on the same line (justify it).

**Iron rules (review-enforced):**
3. **GLSL 330 / ESSL 300 are the shipped floors.** GL 2.1 and GLES 2.0 are not
   members of the runtime auto chain. Keep loops and uniform-array indexing
   conservative because the stable tier still targets old integrated drivers;
   compute shaders use GLSL 430 / ESSL 310 and are capability-gated.
4. **Y-flip / depth-range differences go through helpers + documented blocks.**
   NDC depth: `toShadowDepth()`. GL render-target sampling is bottom-left origin —
   every `#if BGFX_SHADER_LANGUAGE_GLSL` flip must comment which convention
   mismatch it fixes (see the atlas tile-row flip in `sampleLocalShadow`). Never
   copy a flip without re-deriving it for the new texture's writer.
5. **A lighting/shadow/normal change is not done until it passed a parity run:**
   `python tools/render_parity.py --backends d3d11,opengl` (boots the same binary
   per backend via `JCE_BACKEND`, captures the same frame, pixel-compares). D3D11
   vs OpenGL covers the HLSL-vs-GLSL split where every divergence has lived; add
   `vulkan` for SPIR-V when relevant. Use a static scene.

**Case study:** 2026-06-10 `fs_pbr.sc` built TBN as `mat3(T,B,N)` + `mul(n, TBN)`.
Correct on D3D for years; on GL/WASM the constructor packed columns not rows → the
shader applied the transposed (inverse) basis → ground normals sideways, light
pools cut in half at the light axis. One-line fix (`mtxFromCols` + `mul(M,v)`);
hours to hunt. Rules 1 + 5 each would have caught it pre-merge.

## render graph：已从公共 API 降级为内部头（2026-08-31）

`jce_rg_create()` 全树只有两个调用点，都在 `jce_rg_self_test()` 里，而
`jce_rg_self_test()` **一个调用者都没有**。也就是说 render graph 从未被实例化过。

自检第一次运行时两条用例全红，**同一个根因**：它用 `jce_rg_add_pass(rg, "shadow", NULL, NULL)`
建图，而 `add_pass` 第一行就拒绝 `fn == NULL` ⇒ 三次全返回 invalid ⇒ 空图 ⇒
`exec_count(0) == active_count(0)` ⇒ **compile 报告成功**。自检还从不检查 `add_pass` 的返回值，
所以这个失败是静默的。

**但「算法本身是对的」这句要限定**：排序只在 **read-after-write** 上成立。
`compile()` 建边时只走每个 pass 的 `reads`，**两个都写同一资源的 pass 之间没有边** ⇒
clear-then-draw 到同一目标的相对顺序未定义。

**这套 API 今天用不了**（全部实测，细节见 `jce_render_graph.h` 顶部）：
没有任何调用能把 `JceRGResource` 变成句柄；`execute` 从不 `bgfx_set_view_frame_buffer`；
它分配的 transient 纹理**无人可达**；`base_view_id` 曾硬编码为 100，同时撞进
Scene View 的点光立方体带（103..119）、Game View 的跨度、以及被 re-base 到 100 的 PostFX 链；
而它**从不调用 `jce_view_bands_claim`**，所以引擎自己的 view 归属守卫看不见它。
现在 `base_view_id` 没有默认值，`compile()` 会一直失败直到调用方
`jce_rg_set_base_view_id()` 自己选一段。

**处置：头已从 `engine/include/jce/renderer/` 移到 `engine/src/renderer/`，
从 `api_render.h` 摘除，15 处 `JCE_API` 去掉。** 公共面因此 299/301 → 298/300 个头、
3562 → 3547 个 `JCE_API` 符号，SDK 不再安装这个头。

**降级而不是删除**，是因为降级已经消除了真正的危害（向用户承诺一套用不了的能力），
同时保留那段正确的拓扑排序给后来人，而且**可逆**——重新公开一个头是一行，
恢复删掉的代码要考古。今天没有能工作的消费者，所以不存在被破坏的东西。

⚠️ **仍留给这个子系统的 owner**：`add_pass` 拒绝 `fn == NULL`，而执行路径（`:424`）
容忍它，两者不可能都是本意。要真正完成它，缺的是**资源→句柄访问器**与
**execute 里建 framebuffer**；两者都需要 bgfx 上下文才能测，而单测套件没有。


## 2026-09-01：OpenGL 的版本地板在哪（不在这个目录里）

`glsl` 的 shaderc profile 是 **120，这是对的，不要改**。它不是 GL 的版本地板：

* bgfx 在 `ShaderGL::create` 里**加载时重写 `#version`**——按源码用到的标识符在
  120/130 之间选，compute 的 430 原样保留，编译期 `MIN_VERSION >= 31` 时写 140。
  shaderc 写的那行会被丢掉。
* 这套 shaderc 把 `glsl <= 400` 交给 glsl-optimizer（只支持到 1.50），
  所以 **330 和 400 是它唯一取不到的两档**。2026-08-31 把它设成 330 的改动
  让整棵树的 OpenGL 构建挂了一天。
* `#version 140` 分支带着一串 `#define texture2DLod textureLod` 兼容宏，
  就是用来把 120 风格的源码向上适配的——bgfx 自己的 `shader.mk` 默认也是
  `GLSL_LEVEL=120`。

真正的地板是 bgfx 的编译期 `BGFX_CONFIG_RENDERER_OPENGL_MIN_VERSION`，
由 conan conf `user.jce:bgfx_graphics_tier`（stable / modern / current）选，
经 `conan/hooks/hook_bgfx_wasm_fix.py` 落到 bgfx 的 cache 变量。
三档 = **GL 3.1**/ES 3.0、4.3/3.1、4.6/3.2。**不设它不等于「自动」，等于回落到 1（GL 2.1）。**

stable 是 3.1 而不是更常见的 3.3，因为渲染器**不再跑在地板上**——它会向上爬到驱动肯给的
最高 core 版本。地板唯一剩下的职责是「还服务多老的机器」，而 3.1 是这套配置的真实下界
（bgfx 写 `#version 140` = GLSL 1.40 = GL 3.1）。实测 33→31 代价为零：三个硬 `#if` 块
（`>=40`/`>=41`/`>=31`）两档完全相同，12 处 `>=33` 全是扩展表种子、运行时 `GL_EXTENSIONS`
扫描会恢复，唯一真损失 `BGFX_CAPS_PRIMITIVE_ID`（`>=32` 时无条件给出）全树无人消费。

`tools/lint/check_gfx_api_tiers.py` 守这件事：它校验 hook 的表、
`jce_renderer_caps.c` 的预处理器断言、以及同一文件里运行时报出的 `gl[]/gles[]` 表
三者一致，并同时检查**两份** shaderc profile 表
（本目录的 `tools/compile_shaders.cmake` 与编辑器的 `jce_shadergraph_shaderc.cpp`）。

Vulkan 没有档：bgfx 运行时调 `vkEnumerateInstanceVersion` 取设备最高版本。

**选更高的档不会让它更快。** 梯子上线后，渲染器已经会爬到驱动肯给的最高 core 版本，
所以 stable 构建在有能力的卡上本来就跑 4.6。抬高档位实测只改动：
`>=40` 两个纹理格式宏、`>=41` 两个带默认值(16/128)的 limit 查询、
`>=43` 十二处其中十一处是运行时可恢复的扩展表种子（第十二处有 `KHR_debug` 兜底）——
**同时把最低要求抬高，把低于新地板的 GPU 全部拒之门外**。
档位是**支持范围**的决定，不是性能的决定。

## 2026-09-06 — 循环归组件，不归图集：`jce_sprite_player_set_loop()`

在此之前循环是**图集**的属性：`JceSpriteAnim.loop` 来自 Aseprite 的 frameTag，
`jce_sprite_player_update` 直接读它，而 `jce_sprite.h` 没有任何 player 侧的 setter
⇒ `JceSpriteAnimatorComponent.loop`（授权、序列化、画成复选框、**parser 与编辑器
Add Component 两侧默认都是 true**）**没有门可以走**。

三态：`JCE_SPRITE_LOOP_FROM_ATLAS(-1)` / `0` 一次 / `1` 循环。
`sprite_loops()` 是**唯一**把两个来源合起来的地方，不要在别处再判一次。

**打开循环会释放一个已经 finished 的 player。** 没有这一步，勾上 Loop 在动画切换前
什么都不会发生——而「什么都没发生」正是一个未接线控件的样子。

它为什么一直没被发现（两层）：加载器给它读到的**每一个** tag 以及整表回落 anim 都设
`loop = true`，所以基本上什么都在循环——**勾上没变化，取消勾选也没变化**；而
`check_component_field_consumed` 当时按整个文件匹配指针名，`jce_sr_anim.c` 里
`JceSkeletalAnimatorComponent *sa` 的 `sa->loop` 顶了这个组件的包。

## 2026-09-21 — `jce_renderer_request_screenshot_fbo()` 零调用点，以及**它不是黑窗口 P0**

全树（`git ls-files`）只出现四次：声明、定义、`contracts/abi-snapshot.txt` 里的一行、
以及 `jce_editor_scene_render.cpp:36` 的一句 **`#include` 注释**——
`/* jce_renderer_request_screenshot_fbo */`，解释这个头为什么被包含。

**一句解释「为什么 include 这个头」的注释，是「有人本来打算调用它」的最强证据，
同时它也不是一个调用者。** 与「四个被授权却从没被读过的 WheelCollider」同形。

它的 docstring 说了机制：D3D flip-model 交换链上，**Present 之后 backbuffer 拷不出来，
截图全黑**；截离屏 FBO 可靠。

**但不要把它写成黑窗口 P0 的原因——实测不成立。** 本机默认后端上 backbuffer 路径
**工作正常**：`app.runs_agree` 今天取到的是有场景内容的真实帧，外加 53089 像素的
profiler 条带，不是黑图。条件性机制解释不了无条件症状。
⇒ 现状只能说：**一个已知坏配置的、有文档的修法，躺在公共 ABI 里从没被调用过。**
这本身值得修，不需要它同时是 P0。

**我试过的接法是错的，写在这里省下一次。** 我加了
`jce_renderer_set_capture_source_fbo(uint16_t)`，让 `JCE_CAPTURE_FRAME` 走 FBO。
然后读到编辑器**已经有**一条正确的无 UI 截图路径：

```
jce_editor_game_render_screenshot()
  -> jce_editor_viewport_screenshot_submit(bridge, view_base, postfx_tex, w, h, path)
     source = postfx_tex，若无则 bridge 的 color texture
     yflip  随 source 而变（postfx RT 每个后端都是 bottom-up）
     -> jce_renderer_readback_capture_submit(source_TEXTURE, ...)
```

**被显示的那张图是一个 texture，且 y 翻转取决于它是哪一个**——
不是一个 FBO 索引能表达的。一个 `uint16_t fbo_idx` 的 setter 会去截
**bridge 的 color attachment**，而开着 postfx 时那不是屏幕上那张图。
「截到的图非空」和「截到的是被测物」又一次不是同一件事。

⇒ 要接的话，正确的缝是**宿主钩子**（宿主自己拿这一枪），不是 FBO 索引。
还要解决：readback 是异步的（`jce_renderer_readback_capture_poll` 每帧轮询），
而 `JCE_CAPTURE_FRAME` 之后进程会退出——`jce_accept.py` 用 `frames - 2`
就是为这个留的余量。

**另外：编辑器截图本身不是好证据源**，与上面是两件事。
profiler 条带的大小和位置来自 `~/.jce/imgui.ini`，一个**按用户**的文件，
determinism recipe 不钉它 ⟹ 同一台机器上两次一致，说明不了两台机器上一致。
`app.run` 现在会在结果里**明说**这件事（`evidence` / `evidence_reason`），
而不只是把 `executable_source` 记进一个没人打开的 manifest。

### 补（2026-09-21）：缝已经接上了，形状是**宿主钩子**

上面那段写「要接的话，正确的缝是宿主钩子」。已经接了：

```
jce_renderer_set_auto_capture_hook(fn, user)   公共，jce_renderer.h
jce_rcb_host_took_capture(path)                内部，本目录的头
```

`JCE_CAPTURE_FRAME` 命中时**先问宿主**，返回 false 再回落到 backbuffer。
编辑器在 `jce_editor_game_render_init` 装上，钩子转调
`jce_editor_game_render_screenshot`——也就是那条**已经正确**的无 UI 路径。

**三件容易漏的，都是「不会响的失败」：**

1. **回落不是错误路径，是正常路径。** Game View 面板关着就没有离屏目标，
   钩子返回 false，截图仍然要发生（从 backbuffer）。
   把「declined」当成「taken」会**一张图都不产出**，而且没有任何报错。
2. **shutdown 必须**先清钩子**，再销毁它读的东西**。
   `jce_editor_game_render_shutdown` 里清钩子那一行在所有 destroy 之前——
   一个活过自己状态的钩子不会大声失败，它会去读那个地址上**现在**是什么。
3. **用具名函数 + 显式 `JCE_CALL`，不要用 lambda。** `JceAutoCaptureFn` 带
   `JCE_CALL`（Windows 上是 `__cdecl`）；无捕获 lambda 转出来的函数指针用的是
   编译器**默认**约定，x64 上碰巧相同，**x86 上不同，而不同的表现是栈被写坏、
   不是编译错误**。

判据：`tests/application/test_jce_auto_capture_hook.c`，钉的是**缝**不是生产者
（生产者要 GPU 和真帧）：未装 / 装上 / **拒绝** / 清除，以及 path 和 user 指针是否完整到达。
**拒绝那一条是最重要的**——它是唯一一条「静默什么都不产出」的路径。

**续（同日，实测）：这个钩子在自动化路径上*从来不会触发*，而且它第一版还制造了一次回归。**

两趟独立 `app.run`（frames=90，第 60 帧截图）逐像素比对：

```
2560x1528  差异 34977 / 3911680 px
其中 rows 1080..1526  34977
band 之外              0        ← 和 2026-09-20 那次 53089 的结构完全一致
```

**时钟还在。** 引擎日志给出了机制本身，不靠推断：

```
game_render: [capture] Game View is 16x16 -- below 64 px, declining so the
capture falls back to the backbuffer rather than photographing a collapsed panel
```

> **更正（同日晚些，我自己写错过一次）：这里原本写的是「没有保存的布局」。**
> **布局在，而且是对的。** 查 `~/.jce/imgui.ini`（5267 bytes）：
>
> ```
> [Window][game_view]        [Window][scene_view]
> Size=1380,970              Size=1380,970
> Collapsed=0                Collapsed=0
> DockId=0x00000005,1        DockId=0x00000005,0
> ```
>
> 两个面板**共用同一个 dock 节点 `0x00000005`**，`scene_view` 是 tab 0，
> `game_view` 是 tab 1。
>
> **我据此提了第二个假说——「Game View 是背后那个没被选中的标签页」——
> 并且做了实验，实验*否定*了它。** 预先写下的判据是：把两个 tab 序号对调
> ⟹ 截图应变成 1380x970 且不再出现 decline 行。实测（备份 ini、对调、跑两趟、
> 从快照逐字节还原）：
>
> ```
> pass 1: size=(2560,1528)  bytes=1189109  decline_line=True
> pass 2: size=(2560,1528)  bytes=1189677  decline_line=True
> ```
>
> 尺寸没变、decline 行照旧、面板仍报 16x16。
>
> **但这个实验是无效的，而我是在把结论写进树之前发现的。**
> ImGui 决定哪个标签页在前，用的**不是**每个窗口那行 `DockId=<node>,<序号>`，
> 而是 dock 节点自己的 `Selected=`：
>
> ```
> [Docking][Data]
>   DockNode  ID=0x00000005 Parent=0x00000004 SizeRef=1380,1074 Selected=0x8D781F63
> ```
>
> **我对调的是序号，没动 `Selected=` ⟹ 这一趟根本没改变选中项。**
> 所以「Game View 是背后那个标签页」这个假说**既没被证实也没被否定**。
>
> 想真正测它，需要把该节点的 `Selected=` 设成 game_view 的 ImGuiID，
> 而**那个 ID 算不出来**：`ImHashStr` 不是普通 CRC32——
> `zlib.crc32("scene_view") = 0xE55BC08A`，而文件里写的是 `0x8D781F63`。
> （我是拿一个**已知答案**去校验算法才发现的；直接拿算出来的值去写，
> 就是第五个坏仪器。）
>
> **第二次实验（同日）：假说被*证实*了。**
> 不去算那个 ID，改成**把 `Selected=` 整行删掉**、并把 game_view 放到序号 0，
> 让 ImGui 的回退去选第一个标签页——**不需要任何 ID**。
>
> ```
> size=(1380,898)   decline_line=False     ← 钩子触发了
> ```
>
> **然后是这一行一直想要的那个测量**：同一个 input digest 跑两趟、钩子都触发，
>
> ```
> pass 1  762,774 bytes     pass 2  762,774 bytes
> 1380x898 (1,239,240 px)   差异: 0
> ```
>
> ⇒ **编辑器来源的自动截图，在 Game View 处于前台时是逐字节可复现的。**
> **阻塞点是布局，不是这条缝。** 钩子做的正是它该做的；
> 只是那条路径上从来没让它跑起来过。
>
> **要关掉这一行需要什么，现在是确定的**：确定性配方得钉住**哪个面板在前台**。
> `tools/jce_determinism.py` 的 `DETERMINISM` 只有四个键、没有一个碰布局。
> 而且这**不是**一个可以用容差框住的连续量——
> **哪个标签在前，决定了你拍的是哪个程序的输出。**

> **附带，且它救了一次**：写第二个实验脚本时我写了
> `io.open(INI,"w").write(edit(io.open(INI).read()))`——
> Python **先**求值 `open(...,"w")`（**截断文件**），**再**求值那个读它的参数
> ⟹ `edit()` 拿到的是它自己刚造出来的空字符串，**操作者的布局文件被清空了**。
> 是 `finally` 里的快照还原把它救回来的，并且还原后做了哈希校验。
> **快照+校验从这一刻起不再是形式**。
> 剩下的候选是**面板可见性**——`jce_panel_game_view.cpp:1140` 有
> `if (!*vis) return;`，而可见性存在 `~/.jce/editor-session.json` 的
> `panels_visible_mask` 位掩码里，**不在 imgui.ini 里**——
> 以及首帧布局未就绪时 `g.render_width` 被写入一个退化值后**再也没人重置它**。
> **下一个人请从这两条往下查，不要再去查前两条。**
>
> **错的那句有后果**：「没有保存布局」会让下一个人去找一个不存在的缺失文件，
> 而真正的阻塞点是**一个存在且正确的布局里的标签页 z 序**。两件要修的事完全不同。

⇒ **机制正确、且在这条路径上不可达**——`jce_editor_main.cpp:303` 无条件装上它，
而它永远没机会返回 true。**五条缝测试钉的是「被调用时会怎样」，没有一条钉「它被调用了」。**

**而且这条路径的不确定性比「尺寸」更深。** `tools/jce_determinism.py` 的
`DETERMINISM` 只有四个键——`JCE_FRAME_DT_FIXED` / `JCE_STREAM_SYNC` /
`JCE_TAA_JITTER_PHASE` / `JCE_WINDOW_HIDDEN`——**没有一个跟编辑器布局有关**。
所以编辑器来源的 `app.run` 依赖操作者的 `~/.jce/imgui.ini`，
**不只是面板尺寸，还包括哪个标签页在前面**。
前者是个可以用容差框住的连续量，**后者直接决定你拍的是哪个程序的输出**：
两台机器、同一个 input digest、不同的被摄物。

**第一版更糟：它只判了 `render_width == 0`，而 16 不是 0。**
于是它**接受**了那个退化面板，`app.run` 的截图从
**2560x1528 / 970 KB**（钩子上线前那一趟）变成 **16x16 / 328 bytes**。
一张「底部有时钟」的完整帧被换成了一张**什么都没有**的图——
时钟让帧不可复现，16x16 让帧**不再是证据**。

**没有任何东西拦住它**：那一趟被报成成功，而 `app.run` 自己的内容守卫返回
`has_content: null` + `"guard could not run: sampled no pixels -- the crop
window is outside the image (16x16)"`。

**而这个标签本身是错的——守卫其实跑了，而且*拒绝*了这一帧。**
`assert_has_content` 抛的是 `RuntimeError`（`jce_determinism.py:293`），
而 `observe.py:355` 的「被拒绝」分支 catch 的是 `SystemExit`、
「跑不起来」分支才 catch `Exception`。`RuntimeError` 不是 `SystemExit`
⟹ **每一次拒绝都落进「跑不起来」那一支**：

- **`has_content: false` 根本不可达**；
- 一句确定的「这帧是空的」被报成一句含糊的「我没能看」。

**这正是本节要讲的那个错误的镜像**：一个**其余取值都表示「我看了」的字段，
被用来承载「我没看成」**。而它最终怎么报都无所谓——
那个判决被 `guards.append(...)` 收进一个**全文件再无第二处引用**的列表。

⇒ 两条可复用的判据：

1. **退化值和缺失值不是一回事。** `== 0` 拦不住 `16`。凡是「尺寸/数量/长度」的
   守卫，问的应该是「这个值可能是真的吗」，不是「它是不是零」。
2. **阈值要带着它的两个实测数字一起写进代码**（这里是 64：比任何真实面板小一个数量级、
   比实测到的退化值大一个数量级）。没有证据的阈值活不过下一个人。

### 第二个洞与它的两个对照（`f676f70e`）

上面那条 64px 守卫只拦「**从没**画到可用尺寸」。另一半它拦不住：
Game View **曾经**在前台、写下 `1380x898`，之后切到背景标签页 ⟹
`jce_panel_game_view.cpp:1145` 的 `ImGui::Begin` 返回 false ⟹ 面板体不执行 ⟹
`jce_editor_game_render_frame()` 不再被调用 ⟹ **没有任何地方清掉那个尺寸**
（实测：`render_width` 只有一个赋值点、声明处初始化为 0、`g` 从不整体重置）。

于是那个值**又大又合理，描述的却是没人在画的一帧**。
⇒ **按尺寸分不开「陈旧但合理」和「活的」；只有它属于哪一帧能分开。**
修法：写尺寸时盖上 `jce_renderer_get_frame_index()`，太旧就拒绝
（`JCE_EDITOR_GAME_VIEW_EXTENT_MAX_AGE`）。

**两个对照都做了，而且必须来自同一个二进制的同一次运行：**

```
阳性（Game View 在前台）      size=(1380,898)   decline_line=False
阴性（把时间戳变异成永久陈旧）  size=(2560,1528)  decline_line=True
                              日志：age 60 > 1, written=1
```

`written=1` 是让它成为**对照**而不是巧合的那一项：尺寸检查和 `extent_written`
都通过了，**新鲜度比较是唯一还能拒绝的东西**——它拒绝了，并回落到 backbuffer。

### 这里踩到的三个坑，都不在被测代码里

1. **常数把我自己的解释否掉了。** 我带行号断言容差不可能是 0
   （`bgfx_frame()` 在 `jce_renderer.c:1878` 推进索引、捕获点在 `:1954`）。
   **强制设成 0 重建 ⟹ 照样触发**——`:1878` 是 `jce_renderer_begin_frame` 的尾巴，
   **编辑器不走的 2D 回退路径**；编辑器走 `begin_frame_3d`，唯一推进在 `:2037`、
   **在捕获之后**。age 实测 0，最终取 1（多出的一帧覆盖 2D 路径）。
   ⇒ **一个「足够大」的常数和一个「正确」的常数分不开；
   唯一测法是把它设成按自己的推理必然失败的值，然后看它没失败。**
2. **阴性对照第一次跑出来是「GUARD DEAD」，而守卫是好的。**
   检测器匹配的是 `"[capture] Game View is"`（为**尺寸**那条消息写的），
   而新消息是 `"[capture] Game View extent is from frame"`——**不是子串**。
   于是一次真实的拒绝被读成「没有拒绝」。
   **我此前已经承诺「GUARD DEAD 就把修法撤回」**，一个只打印结论的「整洁」脚本
   会让我撤掉一个能工作的守卫、并写一句说它不工作的提交信息。
   修法是**匹配 tag `"[capture]"` 而不是某一条消息的措辞**。
   ⇒ **原始日志与结论并排保留，是真相唯一可见的原因。**
3. **变异脚本只还原了源码，没有重建。** `jce_editor.exe` 会留着变异版本——
   **一个 `git status` 看不见、下一次测量会静默继承的二进制**。
   ⇒ **源码快照保护的是树，只有重建才保护树产出的东西。**
