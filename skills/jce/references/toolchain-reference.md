# JCE 工具链速查

纯查表。流程见 `references/build-and-gate.md`、`references/evidence-verification.md`。

## 1. `tools/` 全表（2026-08-27 实测 14 个 `.py` + 6 个 `.c` + 1 个 `.cpp`；条数会变，重测 `ls tools/*.py tools/*.c tools/*.cpp`）

| 工具 | 用途 |
|---|---|
| `tools/envshot.py` | A/B 像素主力：`capture` / `compare` / `flicker` / `list` |
| `tools/visual_diff.py` | 可门禁的 A/B，自测噪声底（`--floor-reps` `--max-floor` `--floor-only`） |
| `tools/render_parity.py` | 跨后端一致性；噪声底 / 空帧守卫 / 后端断言 |
| `tools/jce_determinism.py` | **唯一**的确定性配方与判读器：`DETERMINISM`、`actual_backend()` / `assert_backend()`、`assert_has_content()`、`read_png()`、`PERF_RE` / `parse_perf()`。**import 它，不要复制** |
| `tools/perf_bench.py` | 帧时/内存，带「编辑器存活」与「日志陈旧」守卫 |
| `tools/shader_lint.py` | 着色器跨后端可移植性（**需要路径参数**） |
| `tools/verify_colour_space_chain.py` | 颜色链字节级验收（importer → sidecar → cooker → mip 字节） |
| `tools/colour_vs_distance.py` | 按**真实逐像素深度/阴影**分箱归因（图像行数不是距离） |
| `tools/gen_pak_report.py` | PAK 归档报告（HTML） |
| `tools/gen_binsize_report.py` | 二进制体积报告 |
| `tools/jce_scene_kit.py` | 场景构造辅助：`schema`（从引擎源码抽全部组件+字段，并标出 INERT 字段）· `check`（lint 场景，exit 0 为判据）· `dump` · `measure` · `shot` · `frames`。**授权场景前先跑 `schema`，不要照抄现有场景猜字段名** |
| `tools/sort_i18n.py` | i18n JSON 排序 |
| `tools/dilate_alpha.py` | 纹理 alpha 膨胀 |
| `examples/caged_kingdom/tools/gen_graveyard_demo.py` | 场景生成（内部） |
| `tools/jce_pak.c` | PAK 打包器（host 工具） |
| `tools/jce_cook.c` | 资产烹饪器 |
| `tools/jce_bin2obj.c` | 二进制转目标文件（单 exe 嵌入链的一环） |
| `tools/jce_cook_catalog.c` | 烹饪目录 |
| `tools/jce_tex_compress.c` · `tools/jce_tex_encode.cpp` | 纹理压缩 / 编码 |
| `tools/miniaudio_impl_cook.c` | 音频烹饪期实现 |

**`tools/audit/` 已重新入库**（owner 决定 2026-08-27）。16 个模块 + 2 份契约数据。
在此之前它整树 gitignored、磁盘上只剩 6 个孤儿测试，而它们 import 的模块全部缺失。

| 项 | 状态（2026-08-27 实测） |
|---|---|
| `tools/audit/run_architecture_audit.py` | **入库**，编排 14 道门，判据是 `AUDIT PASSED` |
| `tools/audit/check_abi_snapshot.py` | **入库**，基线 `contracts/abi-snapshot.txt`（4658 条声明 / 299 个公共头）|
| 其余 `check_*` / `find_*` / `run_dedup_audit.py` | **入库**（条数重测 `git ls-files tools/audit/ \| wc -l`） |
| `tools/audit/cdecl.py` | **不存在，且这是刻意的**：`tools/scriptgen/cdecl.py` 是本仓库唯一的 C 声明解析器（5e232821 的决定）|
| `tools/audit/AGENTS.md` · `tools/audit/tests/`（6 个测试，115 个用例）| 在磁盘，**不入库**（`.gitignore:171`）|

> 这一节曾经写着那三个脚本「不在」，并给每行加了 `skills-lint: allow` 让检查器
> 别管。2026-08-27 它们回来之后，**那三个豁免正是让断言烂在这里的原因**——
> 一条被豁免的断言不会被任何机器复核。加豁免时要问：**它失效时谁会发现？**

**权威文档是每个工具自己的模块 docstring**，不是 `tools/AGENTS.md` ——
后者没有记录 `envshot.py` / `visual_diff.py` / `render_parity.py` / `perf_bench.py`
这四个最重要的工具。


## 1b. `jce.py` 的子命令（2026-08-31 实测 `--help`）

指针曾指向 `references/build-and-gate.md`「见那里的子命令表」——**那份里没有这张表**，
只有两处顺带提到 `smoke`。`check_skills.py` 抓不到这种悬空：文件存在，它只验名词
不验被指的内容真的在。表放在这里。

| 子命令 | 作用 |
|---|---|
| `sdk` | 构建 + 安装可再分发 SDK。`--variant release\|dist\|both`、`--profiling on\|off`（Tracy，见 `user-project-sdk.md` §5c）、`--patented-codecs`、`--script-java`、`--smoke` |
| `smoke` | 用纯 C99 消费方对着现成 SDK 构建并运行 —— **C ABI 回归门** |
| `app` | 经 SDK 构建一个项目（消费方路径） |
| `build-project` | **2026-08-31 新增。** cook + 构建 + 打包一个项目，走的是**编辑器 Build 按钮同一条流水线**（无头驱动编辑器 exe，经 `JCE_HEADLESS_BUILD_*` 契约）。此前那套契约在 `scripts/`+`tools/`+`cmake/`+`CMakeLists.txt` 里**零命中**——能力完整，但只有人手点得动。**注意：进程退出码不承载结果**，判据是编辑器写出的 `jce.editor.headless-build-result.v1` JSON 的 `state` 字段；该子命令会先删陈旧结果再跑，否则一次崩溃会读到上一次的成功 |
| `editor` | 构建一方编辑器 |
| `host-tools` | 构建 host 的 `jce_pak` / `jce_cook` / `jce_bin2obj` |
| `cook` | 烹饪项目的原始资产。**不暴露 `--platform`**，写死宿主平台 |
| `serve` | 伺服 web 构建 |
| `lint` | 跑 lint 套件**并跑架构审计**（2026-08-31 起；`--lints-only` 回到旧行为）。此前它只跑 `run_all.py`，于是驱动脚本与 `CLAUDE.md` §6 的收尾流程对「lint」的含义不一致，而被漏掉的那一半正是守着**被跟踪的** `contracts/abi-snapshot.txt` 的那一半 |
| `test` | **2026-08-31 新增。** 先构建 `jce_tests` 再跑 `ctest -L unit`。顺序不可颠倒：陈旧构建目录会把未构建的 exe 报成 `Not Run`，长得像失败 |
| `targets` | 打印桌面目标矩阵 |
| `checksums` | 把 dist 下的编辑器二进制聚成一份可校验的 `SHA256SUMS` |
| `package` | 落地可分发包：`package editor` / `package game <项目>` / `package sdk`。`game` 的 `project` 是**位置参数**，不是 `--project` |

条数与 flag 都会变，**重测**：`python scripts/jce.py --help`、
`python scripts/jce.py <子命令> --help`。

> **`JCE_HEADLESS_BUILD_*` 的脚坑：激活条件是「任一变量被设」，而合法条件是「九个全设」。**
> 所以 shell 里残留一个（比如上一次手工试验导出的 `JCE_HEADLESS_BUILD_PROJECT`），
> 之后**手动启动的编辑器会静默进入无头构建模式**、判请求 Invalid、写一份失败结果然后退出
> ——看起来像编辑器打不开。`jce.py build-project` 不受影响（它九个全设、逐个覆盖），
> 但手工调试那套契约之后记得清干净。判定：`env | grep JCE_HEADLESS_BUILD`。

## 2. `scripts/`

| 路径 | 用途 |
|---|---|
| `scripts/jce.py` | **唯一构建逻辑源**。子命令表见本文件 §1b（条数会变，判据是 `--help`） |
| `scripts/windows/` `scripts/linux/` `scripts/macos/` | 零逻辑薄壳，同名同参转发 |
| `scripts/build-android.bat` | Android（ndk / sdk / arch / `--clean`） |
| `scripts/build-web.bat` | Emscripten |
| `scripts/serve-web.py` | 本地起 web 构建 |
| `tools/lint/run_all.py` | **检查器条数见 `repository-state.md`统一入口**（2 项可 SKIP） |
| `tools/lint/check_editor_consumption.py` | 编辑器消费覆盖棘轮（本套件新增） |
| `tools/lint/check_skills.py` | skill 自身的机械校验（本套件新增） |
| `tools/coverage/` | 覆盖率（Windows/OpenCppCoverage 可用，其它平台是 stub，未接门禁） |

**新增构建脚本只能落 `scripts/`。**

## 3. 环境变量目录

### 捕获
| 变量 | 作用 |
|---|---|
| `JCE_CAPTURE_FRAME` / `JCE_CAPTURE_PATH` | 引擎 backbuffer 一次性截图，任何消费方可用 |
| `JCE_SHOT_FRAME` / `JCE_SHOT_PATH` | 编辑器 F12 等价物（backbuffer）；FRAME 支持 `N,STRIDE` |
| `JCE_WINCAP_FRAME` / `JCE_WINCAP_PATH` | 整窗（ImGui-FBO）—— **与上两者不是同一张图** |
| `JCE_REC_FRAMES` / `JCE_REC_PATH` | 无头录屏 |
| `JCE_KPI_FRAME_LOG` / `JCE_KPI_FRAME_COUNT` / `JCE_KPI_SHOT` | 逐帧 KPI CSV + 一次性 PNG |
| `JCE_KPI_GAME_SHOTS` | 按播放时钟定时截图（最多 8 个，秒数严格递增） |

### 运行模式
| 变量 | 作用 |
|---|---|
| `JCE_HEADLESS=1` | bgfx **Noop** 后端 —— **无像素**；编辑器会 exit 1 |
| `JCE_WINDOW_HIDDEN=1` | 真 GPU、隐藏窗口 —— **像素验证用这个** |
| `JCE_MULTI_INSTANCE=1` | 绕过单实例锁。**对已构建产物一律显式设它** |
| `JCE_BACKEND` | `auto\|d3d11\|d3d12\|vulkan\|opengl\|gles\|metal\|noop`；**必须从日志断言真实选中的后端** |
| `JCE_MAX_FRAMES` | 到帧数自动退出；失焦时仍继续迭代 |

### 确定性
`JCE_FRAME_DT_FIXED` · `JCE_STREAM_SYNC` · `JCE_TAA_JITTER_PHASE` ·
`JCE_CSM_FAR_INTERVAL` · `JCE_BENCH_CAM` ·
`JCE_DBG_VISTA` 及 `JCE_DBG_VISTA_TX/TY/TZ/_DIST/_PITCH/_YAW/_FAR/_SPIN` ·
`JCE_DBG_FOCUS_SCENE` / `JCE_DBG_FOCUS_GAME`（**必设其一**）

### 场景与自动驾驶
`JCE_SCENE` · `JCE_DBG_AUTOPLAY` · `JCE_DBG_AUTOWALK` · `JCE_DBG_AUTOLOOK` ·
`JCE_DBG_OVERVIEW` · `JCE_DBG_SWITCH_SCENE` · `JCE_DBG_VIEWJSON`

### 录制回放
`JCE_INPUT_RECORD` / `JCE_INPUT_REPLAY`（**两个都设时 replay 胜**；作用域见
`references/evidence-verification.md` §7）· `JCE_AID_RECORD` / `JCE_AID_REPLAY`

### 性能与诊断
| 变量 | 作用 |
|---|---|
| `JCE_PERF_LOG=1` | **布尔，不是路径**。目的地是 `JCE_LOG_FILE` |
| `JCE_LOG_FILE` | 日志落点 |
| `JCE_TRACE=1` / `JCE_TRACE_EXPORT` | Chrome/Perfetto 追踪导出 |
| `JCE_SAMPLER` / `JCE_SAMPLER_ALL` / `JCE_SAMPLER_DELAY` / `JCE_SAMPLER_OUT` | 采样剖析（Windows） |
| `JCE_HITCH_FACTOR` | 卡顿检测倍数 |
| `JCE_RDOC_FRAME` | RenderDoc 抓帧 |
| `JCE_CVAR` | 在注册期钉住 cvar，之后所有写入被拒 |
| `JCE_RP_FORCE` | 渲染特性强制开关，压过所有其它来源 |

### 校验 oracle（证明快路径与暴力路径一致）
`JCE_CULL_VERIFY` · `JCE_CULL_KPI` · `JCE_DBG_VERIFY_SORT` ·
`JCE_PARALLEL_VERIFY` · `JCE_VIEW_BAND_CHECK` · `JCE_DBG_CSM_LOG` ·
`JCE_HIZ_DIAG` · `JCE_MLCULL_DIAG` · `JCE_TEXARR_DIAG` · `JCE_INST_DIAG` ·
`JCE_SHADER_DIAG` · `JCE_DBG_LEAKSTATS` · `JCE_SHUTDOWN_DIAG`

### 脚本
`JCE_SCRIPT_LANGUAGE`（**进程级全局覆盖，不是回退**）

## 4. 产物落点

| 产物 | 路径 |
|---|---|
| 交互式截图 | `~/.jce/screenshots/jce_screenshot_<时间戳>.png` |
| 交互式录屏 | `~/.jce/recordings/rec_<时间戳>.mkv`（VP9 + Opus） |
| 出货 exe 的截图 / 录屏 | exe 同级的 `screenshots/` · `recordings/` |
| envshot | `build/envshots/<name>.png` + 同名 `.json` 清单 |
| visual_diff | `.jce/visual_diff/` |
| perf_bench | `.jce/perf_bench/` 下每次运行一个 log |
| render_parity | `build/parity/parity_<backend>.png` |
| SDK | `dist/sdk/<triple>/` |
| 编辑器分发 | `dist/editor/<triple>/` |

## 5. SDK 目录契约

```
dist/sdk/<triple>/
├── VERSION.txt      commit: / host: / variant: / tools: / scripts:
├── bin/             host 工具
├── include/         公共头安装副本
├── lib/             库 + lib/cmake/JCE
└── share/           引擎资源 + shader PAK
```

已有 triple：`win32-x86_64` · `win32-x86_64-debug` · `win32-x86_64-dist` ·
`win32-aarch64` · `wasm` · `wasm-dist`。

**`dist/` 是产物不是源码。** 要改 SDK 的行为，改
`cmake/JCESDKHelpers.cmake` 一类的源，然后 `python scripts/jce.py sdk` 重装。
`dist/` 与 `reports/` 都是 gitignored。

## 6. 已批准依赖（新增需 owner 批准 + 登记 `THIRD_PARTY_LICENSES.md`）

SDL3 · SDL_image（平台/图像 IO）· bgfx（图形后端）· PhysFS（VFS）·
mimalloc（分配）· **enkiTS（任务调度，`jce_async.*` 建在它之上）** ·
Tracy（剖析，dist 强制关）· flecs（ECS）·
Recast & Detour · behaviortree.cpp（AI）· miniaudio（音频）·
Bullet3 · Box2D（物理）· ozz-animation（骨骼）·
RmlUI · harfbuzz · freetype（游戏内 HUD/排版）· ImGui（**仅编辑器**）·
cJSON · stb_image · assimp · cgltf · zstd · xxHash ·
dav1d · libvpx · libwebm · Opus · Ogg（免版税影音）· protobuf · enet（网络）。

数学统一内置 `jce_math`（cglm 已彻底移除，不要再引）。
异步统一 `engine/src/os/core/jce_async.c`，它是 **enkiTS 之上的结构化封装**
（`#include <enkiTS/TaskScheduler_c.h>`）。**`jce_jobs.c` 不存在**——那是旧名，
`.github/copilot-instructions.md` 仍这么写。

专利编解码器（fdk-aac / OpenH264 / libhevc）可选，
`JCE_ENABLE_PATENTED_CODECS=ON` 才编译，**dist 构建强制 OFF**。

## 7. 已知断链

- `engine/include/jce/resource/jce_bundle_pack.h` 的文件头逐字点名
  `tools/jce_bundle_pack.c` 作为两条出口之一，**该文件不存在**， <!-- skills-lint: allow -->
  根 `CMakeLists.txt` 也没有对应 `add_executable`。
  ⇒ headless / CI 产 `.jbundle` 的路只存在于注释里；
  目前只有编辑器 in-process 一条出口。
- `tools/coverage/` 在非 Windows 上是 stub，且未接任何门禁。
