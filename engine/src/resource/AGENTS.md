# engine/src/resource — Resource / Asset Pipeline (L3-side)

> Asset loading, PAK archives, asset cooker, glTF/image decode, bundle streaming.

## Identity

- **Layer**: L3 (sits between renderer/middleware and disk I/O). C99 (+ one `.cpp` for assimp-based model import on the editor path).
- **Public umbrella**: `<jce/api_resource.h>` (and `<jce/api_streaming.h>` for world streamer).
- **Deps**: `jce_core` (PhysFS, zstd, xxHash, mimalloc), `stb_image`.

## File map

| Header | TU | Role |
|--------|----|------|
| `jce_pak_loader.h` (+ internal `jce_pak_format.h`) | `jce_pak_loader.c` | PAK archive mount/read. **Now a thin shim over Archive v1** — `jce_pak_open/find/decompress/verify/overlay/...` delegate to `jce_archive_*`, so every `.pak` the engine reads is a `JPAK` format_version=1 container. The legacy v2 byte format is fully retired (no v2 reader remains); `jce_pak_format.h` survives only for the rejection-path field offsets. |
| `jce_archive.h` / `jce_archive_writer.h` / `jce_archive_loader.h` (+ internal `jce_archive_format.h` / `jce_archive_crypto.h`) | `jce_archive_reader.c` / `jce_archive_writer.c` / `jce_archive_path.c` / `jce_archive_dict.c` / `jce_archive_crypto.c` / `jce_archive_mount.c` / `jce_archive_delta.c` / `jce_archive_loader.c` | **Archive v1** (`JPAK`, format_version=1). Plain development archives use normalized XXH3 path ids, optional debug paths, zstd/dictionaries, deterministic content dedup, mmap for eligible stored entries, XXH header checks, and per-entry CRC. Secure shipping mode derives domain-separated path/encryption/nonce/authentication subkeys from the 32-byte project key, replaces names with keyed 64-bit ids, compresses then encrypts every entry with ChaCha20 and a content-derived nonce, and authenticates header/tables/data/index with a trailing HMAC-SHA-256 tag. Secure archives reject debug paths and mixed plain entries; lookup/read stays disabled until authentication succeeds. Exact duplicate secure payloads may share ciphertext. `jce_archive_set_process_key()` installs the boot key before PAK/bundle opens; the key necessarily ships in the client, so this raises extraction cost rather than providing secrecy. Mounts, deltas, sync/async cached loading, dictionaries, and integrity APIs compose on the same format. |
| `jce_archive_cook.h` | `jce_archive_cook.c` | **Shared build-time cook** (spec §6.2/§7). One `jce_archive_cook(inputs, count, cfg, &blob, &size, &dict_count)` entry point used by every `.pak` producer (`tools/jce_pak.c`, engine `jce_bundle_pack.c`): classifies each resource by extension (STORE for already-entropy-coded media, JSON/TEXT/SHADER classes, default), trains a shared zstd dictionary per small-file class with >= MIN samples for plain archives, then drives `jce_archive_writer_*` (keep-if-helps makes misclassification harmless). Secure Archive v1 cooks always disable dictionaries because their dictionary region has no encrypted representation; this prevents trained JSON/text bytes from leaking outside encrypted entries. Returns a `jce_malloc`'d JPAK v1 blob. `dedup_content` (forwarded to the writer) coalesces byte-identical payloads into one on-disk copy. |
| `jce_asset.h` / `jce_asset_format.h` | `jce_asset_manager.c` / `jce_asset_registry.c` / `jce_asset_reader.c` / `jce_asset_loaders.c` (+ headers) | Generic asset registry, loader plug-ins, cooked-format reader |
| `jce_numeric_texture.h` | `jce_numeric_texture.c` + internal `jce_asset_writer_internal.c` | Deterministic floating-point texture assets (`R/RG/RGBA 16F/32F`) with tight-row validation, canonical little-endian pixel payloads, non-finite policy, decoded xxHash, and ordinary `.jceasset` compression/upload. The shared writer is also the sole container writer used by `jce_asset_cooker.c`. |
| `jce_bundle_format.h` / `jce_bundle_loader.h` / `jce_bundle_pack.h` / `jce_bundle_deps.h` | `jce_bundle_loader.c` / `jce_bundle_pack.c` / `jce_bundle_deps.c` | Bundle (asset group) format + dependency resolution + pack |
| `jce_image_decode.h` | `jce_image_decode.c` (uses stb_image via `engine/src/renderer/jce_stb_image_impl.c`) | PNG/JPEG/TGA decode for runtime |
| `jce_model_importer.h` | `jce_model_importer.cpp` | Editor-time mesh import (assimp). Runtime mesh uses glTF in renderer. Also exposes **per-part extraction** (`jce_model_importer_load_parts_*`): walks the node hierarchy WITHOUT `aiProcess_PreTransformVertices`, keeping separated objects distinct (one `JceModelPart` per node with world transform) — the input side of the compound-collider cook. |
| `jce_scene_contract.h` / `jce_scene_serial.h` | `jce_scene_serial.c` | Scene serialization contract + JSON IO |
| `jce_world_streamer.h` | `jce_world_streamer.c` / `jce_streaming.c` | Tile/chunk world streaming + **mip-streaming pressure hook** (P3-A.2: bridges `JceStreamingPressure` OK/SOFT/HARD → global texture mip bias 0/1/2 via `jce_texture_set_global_mip_bias`; registered as a chained hook so the user-facing pressure callback slot stays free) |
| (internal) `jce_asset_cooker.h/.c` | — | Build-time asset compilation (called from `tools/`) |
| (internal) `jce_async_pool.h/.c` | — | Async-load thread pool (uses `jce_jobs`) |
| (internal) `jce_bundle_contract_internal.h/.c` | — | Deterministic Bundle 1.1 manifest, catalog, graph snapshot, and build-report serialization |
| (internal) `jce_bundle_graph_internal.h/.c` | — | Canonical dependency edges, origin metadata, and root-to-missing diagnostics |
| (internal) `jce_tex_compress.h` | — | Texture compression dispatch (BC/ASTC) |
| (internal) `jce_shader_manager.h/.c` | — | Shader binary cache lookup (renderer-facing) |

## Rules

1. **All disk I/O via PhysFS** (`jce_filesystem.h`). Tools-only paths may use `jce_filesystem_host.h`. **No `fopen`/`<filesystem>`.**
2. **Async loads via `jce_async_pool` + `jce_jobs`.** No raw threads.
3. **Cooked vs raw**: runtime opens cooked formats only. Importers (model/image cooker) live here and in `tools/` and are NOT compiled into the shipping game.
4. **Allocations** through `jce_malloc/free`. Asset buffers handed to renderer must document ownership (move vs borrow).
5. **Editor-side caches** belong in `editor/src/scene/` — do NOT duplicate the asset cache here.
6. **glTF runtime loader** physically lives in `engine/src/renderer/jce_gltf_loader.c` (geometry close to its consumer); this layer just hands raw bytes to it.
7. **No C++ in headers.** `jce_model_importer.cpp` only — façade exposes `extern "C"`.
8. **Texture compression**: select per-platform via `jce_gpu_caps` (BC on desktop, ASTC on mobile). Don't ship uncompressed runtime textures.
9. **Pressure hooks** (`jce_streaming_add_pressure_hook`): up to `JCE_STREAMING_MAX_PRESSURE_HOOKS` (4) chained hooks fire BEFORE the single user callback set via `jce_streaming_set_pressure_callback`. Use for engine-internal layers (mip streaming, audio quality) that must not steal the user slot.

## Common tasks

| Task | Steps |
|------|-------|
| Add an asset type | New loader struct in `jce_asset_loaders.c`; register with `jce_asset_manager`; add cooker if format needs preprocessing; add file viewer in `editor/src/viewers/` |
| Add a bundle dependency rule | Extend `jce_bundle_deps.c`; update `jce_bundle_pack.c` |
| New scene component serialization | Add to `jce_scene_serial.c` AND to `engine/src/middleware/scene/jce_scene_components_json.c` |
| New texture format | Update `jce_tex_compress.h` dispatch; gate via `jce_gpu_caps` |

## Don't

- Don't call SDL or bgfx from here.
- Don't expose stb_image or assimp types in public headers.
- Don't ship the cooker/importer in release builds (they're tools).

## 2026-09-06 — `jce_atlas_pack`：纯算术在引擎里，I/O 在 cook 里

`jce_atlas_pack` 是 skyline 装箱器，**没有图像数据、没有分配、没有 I/O**。
这不是洁癖：重叠、越界、padding、确定性、拒收——**每一条在图片里都是看不出来的**
（1 px 重叠看起来像美术自己画的边），纯函数才让它们能被**断言**而不是被**看**。

**输出格式不是选择。** `jce_sprite_sheet_load_json` 已经读 Aseprite JSON，
所以运行时那一半是很久以前就发货的代码。而这句话是**闭环验证过的**，不是断言：
测试用 `jce_atlas_write_aseprite_json`（**cook 调的同一个函数**，不是它输出的手抄本）写，
再用引擎自己的 loader 读。对照：把 `frames` 键改名 ⟹「frame count does not match」——
顺带照出 **loader 对畸形文件返回非 NULL 但零帧**，所以只断言 NOT_NULL 会通过。

**三道门在这件事上依次拦住了我，每一次都是对的：**
- `check_engine_native_io.py`：引擎代码里的 `fopen/fprintf` ⟹ 改走 `jce_fs_host_write_all`
- raw-allocator：`malloc/free` ⟹ 改走 `JCE_MALLOC/JCE_FREE`
- dependency-boundaries：`#include <SDL3_image/SDL_image.h>` ⟹
  `contracts/dependency-ownership.yml` 把 image-decode-ldr 钉在**一个目录**，
  并把「direct IMG_Load at call sites」列为 forbidden alternative

**而我修第三条时先写错了一次**：为了绕开门改成写裸 `.rgba` blob——
**那是把契约违规换成了坏输出**，因为纹理路径读不了没有头的裸块。
正解是用 cook **已经链着**的 bimg（`jce_tex_write_png`，加在 `tools/jce_tex_encode.cpp`）。
**绕开一道门之前，先问它拦的是什么。**


## 2026-09-20 — 写了格式却没有读者：`jce_curve.c`，以及**把求值器搬过来而不是抄一份**

Curve Editor 自建成起就在写 `{tMin,tMax,vMin,vMax,active,channels:[{name,color,visible,keys:[{t,v,tanIn,tanOut,interp}]}]}`，
而 `engine/include`、`engine/src`、`editor/src`、`tools`、`scripts`、`scripting`、`contracts`
里搜 `JceCurve` / `jce_curve` / `tanOut` **零命中**（`tanOut` 只命中面板自己和 15 个 locale）。
设计师画得出曲线，**没有任何东西能用它**。

**求值器是搬过来的，不是抄过来的。** 面板的 `eval_channel` 现在转调
`jce_curve_eval_keys`。抄一份会得到两个**今天一致、某天不一致**的实现，
而它的症状——曲线**播放的样子和画的样子不同**——从任何一边看都看不见，
因为两边各自自洽。**同一条理由第二次生效**：面板原本还有自己的 key 读取器
（`t`/`v`/`tanIn`/`tanOut`/`interp`），两者**在写完之前就已经分叉**——
引擎把越界的 `interp` 夹成 LINEAR 而面板不夹。为此加了
`jce_curve_channel_keys()`，面板从引擎取 key 和 channel 名，
**只留下引擎故意不携带的那部分**：`visible`、`color`、视图范围——
那是「作者当时在看哪里」，不是「作者写下了什么」。

**读者必须接受写者接受的一切。** 去读面板的 loader 核对键名时，
发现它接受两种我的 parser 拒绝的文档：**pre-channels 文档**（顶层 `keys`，
面板把它包成 channel 0）和**乱序的 key**（面板会排序）。拒绝它们
= 编辑器打得开、游戏里**根本不播**——比「播得不一样」更糟的那一种。
现在两种都接受（排序而不是拒收）。

**索引对应成了契约。** 因为面板按**同一个下标**从文档里取 colour/visible，
parser 对畸形数组元素**不再跳过**，而是产出一个空 channel——跳过会让其后
每个 channel 的外观滑到错误的曲线上，而 key 仍然是对的，
**唯一的症状是两条曲线换了颜色**，没人看得出来。有断言守着。

**`dependency-boundaries` 拦了我一次而且它是对的**：
`#include <cjson/cJSON.h>` ⟹ 改走 `<jce/os/core/jce_json.h>`。
这不是让步是改善——那正是面板自己写文件用的 facade，
于是读者和写者**连 JSON 表面也只有一个**。


## 2026-09-21 — **不该存在的那两个设置**，比缺的那两个更重要

音频的 cook 策略此前整个只有一条写死的启发式（`sz < 2 MB`），**两个方向都没有
per-asset 覆盖**：2.1 MB 的脚步声集合永远保持编码、每次触发都付一次解码；
1.9 MB 的环境音循环被展开成原始 PCM 塞进 PAK。模式是现成的
（`jce_model_import_settings` / `jce_tex_encode`），音频只是**没有成员**。

**Unity 的 AudioClip 面板有 Compression Format 和 Quality，照抄是最自然的动作——
但这棵树没有音频编码器。** `jce_cook_audio` 解码成 s16 PCM 就写出去，
`JceAssetAudioInfo.format` 在每一个写入点都是 `0 = PCM_S16`。
于是「格式」下拉框只有一个可达值、「质量」滑条什么都到不了。

⇒ **一个改变不了输出的控件比没有这个控件更糟**：它看起来像那个它从不做的决定。
这正是纹理 colourSpace 复选框从 bool 改成三态之前的那个缺陷。面板改成写一行文字说明。
⇒ 判据：**加设置之前先找它的消费者**，找不到就把「缺的是编码器」写进账本，
而不是把设置加上去让账本变绿。

**转换的位置就是它的全部。** force_mono / sample_rate 走 `ma_data_converter`
（miniaudio 本来就做了解码，第二个重采样器 = 「48k→22.05k 听起来是什么样」的
第二个答案），并且**在解码之后、构建 info chunk 之前**——顺序错了会得到
一个头写 48000、字节是 22050 的片段，**以一半速度播放，而且不报任何错**。

**设置是参数，不是 `JceCookOptions` 的字段。** `JceCookOptions` 是**这次调用**的
选项（压缩等级、目标平台），这些是**这个资产**的。把 per-asset 状态塞进共享的
options 结构，正是「为一个文件设的值到达了下一个文件」的来路——
而这里两个调用方的生命周期不同：`jce_cook_file` 处理一条路径，bundle packer 在循环。

**链接器第二次报出设计问题**（第一次是 UI rect 查询）：`miniaudio.h` 此前只在
非引擎路径 include，于是引擎构建里 `ma_data_converter` 全是未声明标识符，
报出来的是一串 C2059/C4218 语法错误 + 「函数体提前结束」。
**声明在两条路径上都要有**；`MINIAUDIO_IMPLEMENTATION` 每个二进制只有一个 TU。

**`jce_cook_audio` 的调用方扫描漏了 `tests/`。** 我扫了
`engine/src tools editor/src`，漏掉 `tests/middleware/audio/` 里的三处，
改签名之后才由编译器找出来。⇒ **改内部 API 的签名时，调用方扫描要覆盖每一棵
会对着那个头编译的树，`tests/` 是其中之一。**
