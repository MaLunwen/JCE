# tests/AGENTS.md — JCE Unit Test Tree

## 目的
为引擎 L1-L7 + 编辑器提供 C99 单元测试，目标 ≥85% 行覆盖（按 IMPLEMENTATION_PLAN 阶段 1）。

## 框架
- **Unity** (ThrowTheSwitch, MIT) — 固定来源见 `contracts/vendor-sources.json`，由外部只读源码缓存提供，用于引擎 C99 测试。
- **doctest** (v2.4.11, MIT) — 同样使用固定来源的外部源码缓存，用于编辑器 C++17 测试。每个 `.cpp` 在包含前 `#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`。
- 由顶层选项 `JCE_BUILD_TESTS=ON` 开启；通过 CTest 注册，CI 跑 `ctest --output-on-failure`。
- C 测试用 helper `jce_add_unit_test()`；C++17 测试用 `jce_add_cxx_test()`（含 `editor/src` include + MSVC `/wd4267 /wd4244`）。

## 目录约定（镜像 `engine/src/` 拓扑）
```
tests/
├── third_party/         # legacy local sources, ignored (DO NOT modify)
├── os/core/             # L1 tests — pure / no platform deps
├── os/platform/         # L2 tests —需要 SDL/真实平台
├── renderer/            # L3 ...
├── middleware/{world,physics,audio,...}/   # L4
├── runtime/             # L5
└── application/         # L6
```

## 添加新测试（3 步）
1. 在对应层目录下建 `test_<module>.c`：
   ```c
   #include "unity.h"
   #include <jce/os/core/jce_str.h>

   void setUp(void)    { }
   void tearDown(void) { }

   static void test_jce_strcasecmp_equal(void) {
       TEST_ASSERT_EQUAL_INT(0, jce_strcasecmp("ABC", "abc"));
   }

   int main(void) {
       UNITY_BEGIN();
       RUN_TEST(test_jce_strcasecmp_equal);
       return UNITY_END();
   }
   ```
2. 在该目录的 `CMakeLists.txt` 调用 helper：
   ```cmake
   jce_add_unit_test(test_jce_str
       SOURCES test_jce_str.c
       LINK    jce_core)
   ```
3. `cmake --build . --target test_jce_str && ctest -R test_jce_str`

## 硬约束
- **C99 only**（与引擎一致）
- 测试 **不能** include SDL/bgfx/ImGui/flecs 等三方头 —— 只能 `<jce/...>` 公共 API
- 不允许 `malloc/free/fopen/printf` —— 用 `JCE_MALLOC/FREE`、`jce_fs_host_*`、`jce_log_*`
- 单测必须可在单核 / 512 MB 机器跑通（CI 基线）
- 每个 test 文件一个 `main()` + 一个 `add_test()`，便于 ctest 按需筛选

## 跨平台
- 平台特定测试（`os/platform/`）用 `#ifdef JCE_PLATFORM_*` 守卫
- 不允许出现 `_WIN32` / `__APPLE__` / `__linux__`（与引擎层一致）
- 文件 IO 走 `jce_fs_host_*`；临时路径走 `jce_fs_host_get_current_dir()` + `_ut_<suite>_<leaf>` 前缀（**引擎未提供 `jce_path_temp_dir`**，避免 OS tmp 污染）

## 当前覆盖（214 个测试可执行文件 — 与 `ctest -L unit` 一致）

> 选择标准：**纯逻辑优先**。模块若强依赖 SDL/bgfx/flecs/Box2D/Bullet/Recast/BehaviorTree.CPP/libcurl 等三方运行时，则跳过或仅测可隔离的子集（解算器、查表、几何谓词）。

| 层 | 已覆盖模块 | 备注 |
|----|------------|------|
| L1 OS-core | `jce_alloc`, `jce_allocator`, `jce_str`, `jce_path`, `jce_handle`, `jce_event`, `jce_timer`, `jce_json`, `jce_config`, `jce_i18n`, `jce_log`, `jce_jobs`, `jce_thread`, `jce_profiler`, `jce_trace`, `jce_sysinfo`, `jce_filesystem`, `jce_filesystem_host` | 覆盖无等待生产、环覆盖/丢失统计、线程/任务/等待关联与 Chrome Trace 导出 |
| L2 resource | `jce_pak_loader`, `jce_bundle_deps`, `jce_bundle_pack`, `jce_archive` | mimalloc-owned blob 必须 `jce_malloc`，不能 CRT `calloc`（所有权转移给 `jce_pak_open_owned` → 由 `JCE_FREE` 释放）。`jce_bundle_pack` 覆盖单文件 `.scene`、绝对路径虚拟化、缺失 asset hard-fail。`jce_archive` 测试经公共 API 用 writer 内存造档再读回（round-trip / 规范化 / keep-if-helps / 确定性 / 损坏检测 / 冲突 / debug 表），链 `jce_resource`+`xxHash`。 |
| L3 application | `jce_version`, `jce_subsystem` | |
| L3 renderer | `jce_camera` | bgfx 符号在链接期解析；只要测试不触发 `flush()`/`init` 即可绕过。`tests/cmake/test_sky_shader_contract.py` 静态锁定锚定天空穹顶在球面边界/外部退回无限方向采样。 |
| L3 AI | `jce_steering`, `jce_graph_astar` | 纯数学；`jce_bt`/`jce_navmesh`/`jce_nav_agent` 因 BT.CPP / Recast / Detour 跳过 |
| L3 save | `jce_snapshot` | 仅 buffer 模式；string/buffer 必须用 `jce_free` 释放（mimalloc 所有权） |
| L4 scene | `jce_lod`, `jce_space_partition`, `jce_scene_renderer_view_order`, `jce_sr_gpu_policy`, `jce_scene_recipe`, `jce_scene_compiler`, `jce_scene_transaction` | GPU policy 测试锁定小/碎工作负载的 CPU 回退、强制 GPU 与高负载 crossover；AI 场景测试覆盖严格 recipe、确定性 FrozenPlan v2、稳定父 ID/拓扑/线格式，以及隔离 ECS 层级构建、提交、健康门与回滚；`jce_scene_renderer_view_order` 确保 shadow producer 先于 scene color consumer。 |
| L4 script | `jce_script_bindings`, `jce_script_asset_text`, `jce_script_touch`, `ck_lightning_lab` | 注入 host callbacks 覆盖 VFS 相对路径校验、1 MiB 文本上限、所有返回路径释放，以及稳定的一基触点索引；项目闪电夹具锁定先导、回击与声速延迟。 |
| L5 runtime/application | `jce_ai_scene_director`, `jce_scene_generation`, `jce_runtime_script_load`, `jce_runtime_touch_input`, `ck_lightning_lab_runtime` | 使用注入 transport/cache 与 bootstrap fixture 覆盖关联校验、版本化层级 prompt、编译、事务协调、显式提交和健康结果；不访问真实网络。触控测试覆盖截断、替换、非有限值过滤、显式释放和下一帧自动释放；闪电集成测试加载真实场景并验证脚本经组件反射驱动 LineRenderer/PointLight。 |
| L4 world | `jce_time_of_day`, `jce_trigger_volume`, `jce_weather`, `jce_road_network`, `jce_spawn_manager` | `weather` 仅测 `_default()` + null-safety（其余路径触 bgfx）；link 必带 `jce_resource`（jce_world 调用 `jce_pak_*`） |
| L5 animation | `jce_anim_ik`, `jce_anim_blend_tree`, `jce_anim_sm` | `jce_skeleton`/`jce_animation` 拖入 ozz；`jce_anim_sm` 是纯 JSON 状态机 |
| L5 audio | `jce_audio_occlusion`, `jce_audio_mixer`, `jce_reverb_zones` | `jce_audio`/`jce_audio_mixer` 链接但不触发 miniaudio；mixer 是纯 bus-tree + voice-map；reverb_zones 纯 CPU blender |
| L5 physics | — | Box2D / Bullet 依赖；待写 stub 解算器子集后再补 |
| L7 editor (C++17, doctest) | `jce_editor_quat`, `jce_editor_path_util`, `jce_editor_alloc`, `jce_editor_file_util`, `jce_editor_i18n`, `jce_editor_config`, `jce_editor_assetdb`, `jce_editor_project_settings`, `jce_editor_project_render_pipeline`, `jce_editor_component_registry`, `jce_editor_layout_scene_commands`, `jce_editor_reflect_registry`, `jce_scene_camera_focus`, `jce_scene_outline_policy`, `jce_scene_view_input_policy`, `jce_hierarchy_input`, `jce_hotkeys` | 不链接 `JCE_Editor` 总目标；按需把编辑器 `.cpp` 直接编进测试 exe，再 stub 强依赖符号（如 `jce_editor_assets_get_project`）以保持低耦合。临时路径走 `jce_fs_host_get_current_dir` + `_ut_*` 前缀（**不要**用 `jce_path_temp_dir`，引擎里没有该 API）。需要 ImGui 符号时，用 `tests/editor/stub_includes/jce/tools/jce_imgui.hpp` 影子头 + `INCLUDES` 形参前置注入；助手 `jce_add_cxx_test` 已支持 `INCLUDES`（`BEFORE` 顺序）。`jce_reflect.cpp` 已拆分为纯注册表 + `jce_reflect_draw.cpp`（ImGui 绘制），让 registry 可在不引入 ImGui/editor_state 的情况下独立单测。项目 RP 测试保证项目切换时 missing/malformed RP 不继承旧 descriptor。 |

## 未来扩展候选（按价值/可行性排序）

1. CI 覆盖率作业（Windows + OpenCppCoverage）— P3
2. 扩展编辑器 C++17 测试到 `jce_editor_clipboard` / `jce_editor_history` 等纯逻辑模块 — P3
3. 扩展 ImGui stub 至 `GetIO` / `GetMainViewport` / `IM_COL32` 等，解锁 `jce_editor_toast` 覆盖（`jce_reflect` 已通过拆分 registry+draw 解锁，registry 部分已覆盖） — P3

## 重要技术注脚（避免重新踩坑）

- **`create` 工具不递归建目录**：测试新增子目录前用 `New-Item -ItemType Directory -Force -Path tests\middleware\<x>`。
- **PowerShell 启动可执行**：必须 `& .\build\...\test.exe`（前导 token 若无 `.\`，PowerShell 会按模块名解析）。
- **CMake preset bug**：`cmake --preset desktop-windows-x64-debug` 当前会报重复 preset；直接 `cmake -S . -B build\desktop\windows-x64-debug`。
- **`jce_add_unit_test` helper**：定义在 `tests/CMakeLists.txt`，自动链 Unity、加 `engine/include`、注册带 `unit` label 的 ctest，超时 60 秒。
- **MSVC/Ninja 测试链接池**：两个测试 helper 都把链接边放入 `jce_test_link_pool`；默认 `JCE_TEST_LINK_JOBS=2`，只限制大型 PDB 的并发写入，不限制编译并行度。高吞吐 CI 可在配置时显式调高。
- **Engine 层名称**（与测试 `LINK` 字段对应）：`jce_core jce_platform jce_video jce_audio jce_physics jce_animation jce_ai jce_net jce_scene jce_world jce_save jce_renderer jce_ui jce_resource jce_application`。
- **bgfx 链接但不初始化**：测试链 `jce_renderer` 时只要不调用需要 init 的 entry，链接器解析过即可（如 `jce_camera`）。
- **新增 AGENTS.md 的位置**：`tests/` 下子目录**不需要** AGENTS.md（被 `check_agents_md.py` 隐式排除）；新加测试只更新本文件。
- **mimalloc 所有权陷阱**：引擎 API 返回的 buffer/string（如 `jce_snap_read_string`、`jce_snapshot_save_to_buffer` 的 `out_buf`）由 `JCE_MALLOC`/`JCE_REALLOC` 分配，**必须**用公共 `jce_free()` 释放，CRT `free()` 会静默挂死 debug 堆。
- **跨层链接**：`jce_world` 内部调用 `jce_pak_*`（位于 `jce_resource`），任何 link `jce_world` 的测试必须同时 link `jce_resource`；同理 ECS 拉 flecs、audio 拉 miniaudio、scene_renderer 拉 bgfx — 优先选纯逻辑模块测。
- **提交规范（强制）**：所有 commit 末尾追加 `Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>`。

## 跑测一条龙

```powershell
cmake -S . -B build\desktop\windows-x64-debug
cmake --build build\desktop\windows-x64-debug --target <test_name>
ctest --test-dir build\desktop\windows-x64-debug -L unit --output-on-failure
python tools\lint\run_all.py    # 7 lints, all must pass
```

### Streaming media regression

`test_jce_read_source` covers >2 GiB offsets, bounded reads, shared ownership,
short/truncated callbacks and file/memory sources. `test_jce_audio_file` covers
real PCM counts, asynchronous seek/EOF, background peaks and close cancellation.
The optional video probe now opens a shared file source and reports its IO stats.

`test_jce_audio_stream_seek` uses explicit decoder fences to prove that PCM
from a pre-seek generation cannot leak into the new ring or reset its clock.
The optional real AudioFile probe (`JCE_TEST_AUDIO_FILE`) checks three seeks
and nonzero asynchronous peaks on user fixtures without redistributing them.

- `test_jce_mp4_source_limits`: valid metadata control, oversized moov,
  forged sample runs, short table payloads and truncated extended headers.
- Real video probe flags `JCE_TEST_VIDEO_LOOP=1` and
  `JCE_TEST_VIDEO_REQUIRE_FINAL_FRAME=1` test transport wraps and tail drain.
- SDK smoke verifies installed stream APIs and actual AV1 providers via an
  MSVC linker map; the bimg fallback must never satisfy the codec check.

`JCE_TEST_VIDEO_FINAL_PTS` supplies a known final video timestamp when the
audio track extends beyond video; EOF must not be inferred from shared duration
alone. The editor probe accepts `--expected-final-pts` for the same case.

The committed synthetic MP4 fixtures in middleware/video/fixtures have 90
silent B-frame pictures, progressive and fragmented containers. Normal
unit tests verify all output/final PTS, paused start, seek fencing and rewind
without a user asset, GPU window, ffmpeg installation or sound device. The
old EOS path returned only 87 pictures. CTTS unit vectors cover unsigned and
signed offsets, reordering and malformed run counts/payloads.
