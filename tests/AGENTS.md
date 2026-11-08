# tests/AGENTS.md — JCE Unit Test Tree

Windows XP compatibility projects under `tests/compatibility/` are isolated
producer/OS probes, never a substitute for the full Editor or a modern SDK.
Their caches and native evidence live under `build/compat` and
`build/validation`; target compilers, host tools, pointer width, resource bytes
and actual guest execution require separate recorded checks. Keep native
window tests nonactivating and protect all user project directories in build/.

The private Core SHA fixture uses fixed known answers for empty/abc/FIPS
multi-block input and 55/56/63/64/65/1000-byte padding boundaries, seven-byte
stream updates and erased final state. It consumes the sole production Core
implementation rather than copying a hash. Archive's existing actual HMAC
fixture retains RFC 4231 short-key coverage and adds its 131-byte-key vector;
ordinary archive encryption/integrity/roundtrip checks remain in that target.
The provenance integration extends the existing complete loader/Noop
fixture; its CPU-only scope and ordinary mutations are specified by the closest
Renderer test charter.

Bundle pack positive and ordinary old-partition controls share a working
directory and may run concurrently. Their host scratch directory uses the
actual process ID plus case counter; executable function-address bits are not
unique between processes. Keep the independent archive-byte, catalog mount,
shared-threshold placement and project-policy assertions unchanged.

`test_jce_filesystem_pak_bounded` compiles the complete actual Core VFS with
real archive writer/reader and a counted transparent public decode provider.
Owned host directories prove ordinary VFS global directory precedence while
the explicit PAK entry ignores it. Metadata-only, binary sentinel, first-match
failure, invalid arguments and over-limit-before-decode are independent checks.
Whole-owner old_host and old_limit controls must fail ordinary assertions;
each target has a separate host working directory. The existing bounds/lock
fixtures privately rename this new public symbol with their other VFS entries.
Register via os/core/filesystem_pak_bounded_controls.cmake. These Engine-domain
CPU controls do not certify every asset consumer, concurrency or GPU behavior.

The actual logger retirement fixture observes final file flush and close API
returns, including forced false returns after real native IO retirement. Both
failures retain api_failed across repeated shutdown; restart resets observation,
and no_file is separate. An old-unconditional-success complete logger mutant
must fail ordinary assertions, not leak a handle or crash.

`test_jce_log_format` includes the complete logger owner and delivers messages
through the real sink, native worker and file IO. Its typed CRT port forwards
ordinary formatting, counts the single pass and provides a defined negative
return without invoking an invalid printf format. Both public entries cover
short bytes, 511/512-byte boundaries, long payloads and a Chinese UTF-8 boundary.
The complete-owner old_silent control must fail ordinary marker assertions;
CTest's WILL_FAIL result alone is not evidence of the direct executable failure.

## Shared frame-pool retirement

`test_jce_thread_retirement` includes the complete thread owner and uses actual
enki workers. Its join-forwarding port releases an already-running task only at
the native shutdown boundary. Shared lookup and recursive shutdown inside that
task must not create a second pool or deadlock. The old_join_reopen complete-TU
control omits only transient closing publication; its unexpected pool is drained
on the worker that created it before an ordinary assertion reports the failure.
Separate assertions cover retained pre-shutdown borrows, the Engine-session lazy
creation latch, and standalone/new-session recreation. These are CPU ownership
controls, not scheduler latency or GPU/process-exit proof.

## Trace clock lifetime controls

`test_jce_trace_capture_clock` uses the complete capture owner and real ring
with retained pre-start events across a native SDL restart. Independent real
performance-clock brackets bind writer start/finish to event timestamps, and
the published footer must identify its clock domain. Typed counter/frequency
ports exercise the actual shared conversion, including zero frequency, uint64
overflow and reversed endpoints. Both failed edges retain incomplete output,
join/close/free once, restore suppression and never replay payload after the
provider recovers. Cooperative and controlled threaded ownership are covered;
the worker port does not certify native scheduling. Complete-owner `old_ticks`
and `missing_failure_latch` controls must compile then fail the same ordinary
assertions. Native registration lives in `os/core/trace_capture_clock_controls.cmake`;
Web marks this native restart fixture unavailable.

`test_jce_trace_clock` compiles the actual trace owner and restarts real native
SDL with retained ring history. Both event timestamps must fit independent
performance-counter brackets and the prefix must remain present. Exact helper
vectors cover fractional floors, integers beyond double precision, wide products
and uint64 boundaries; invalid-provider events retain an unpublishable sequence
so complete-capture accounting refuses them. The ordinary complete-TU
`old_sdl_ticks` control restores only the old timestamp source and must fail
runtime assertions after compilation. No window/GPU is created; Web reports
this native restart fixture unavailable, not passed.

## Native diagnostic retirement controls

`test_jce_diagnostics_retirement` consumes the actual logger and continuous
capture owners plus the shared Engine call sequence. Native join, accepted file
bytes while the Windows write handle remains open, persisted bytes after close,
sink identity/counts, retained storage/restart and late capture events are
separate assertions. The capture join runs inside an actual nonzero service
scope; its two wait edges must have the same id and enclosing parent. Complete
capture-TU `old_join_parent` changes only wait-end's parent argument to zero and
must fail that unchanged ordinary assertion after successful compilation. This
regression can retain a zero-loss, zero-active-owner footer while producing an
invalid wait pair; footer accounting does not replace lifecycle analysis.
Other exact-owner controls omit capture/log joins or restore old async-tail and
worker-availability behavior. These CPU witnesses do not certify GPU completion,
post-footer SDL teardown, durable storage or whole-application performance.

The complete `test_jce_filesystem_host_atomic` owner also links the real logger
for opt-in OPEN detail: original TLS error before a later dependency overwrites
it, exact byte/hex prefixes (Unicode/control/empty/truncated), actual 511-byte
formatter admission at maximum widths, scope/parent and unchanged fifteen counters,
default-off/scope-zero/consumer suppression/success/isolated VFS silence, filtered
unavailability, two native thread error lifetimes, 64 whole groups, saturating
refusals and synchronous sink FS reentry. An eighth enabled group uses caller-owned
contexts: active same/foreign begin and foreign/repeated end cannot reset or detach
another owner; active VFS policy/set/unset, reader/probe replacement and clears
preserve its budget. Matching end unbinds before summary, then a new context can
bind; OFF remains unbound. Workers still join before context retirement. The
log-none target compiles snapshot work out. Complete-owner delayed-snapshot and
unmarked-prefix controls must fail
ordinary assertions; compile/link/crash is not the intended negative. Attempted
emissions never assert delivered evidence. These are CPU diagnostic boundaries,
not provider errno/optional classification, durable logging or GPU evidence.

The existing `test_jce_filesystem_host` target retains its host-I/O assertions
and calls the real public current-executable query. Added cases cover invalid,
zero/tiny output, absolute existing-file identity, exact capacity and no
truncated success. One explicit self-child mode uses the public process service
with a distinct working directory and a relative expected-path file; its path
oracle never consumes argv[0]. Polling is bounded to 2000 sleeps of 5 ms, and
teardown owns any surviving child. Unsupported targets assert false/cleared
output. These cases do not establish unrun OS/architecture support, image
renaming or exhaustive long-path/encoding/failure coverage. No second
executable recipe is added.

`test_jce_comp_set_partial` retains its real-engine partial-update cases and
includes the complete private component/merge owner with typed operation
observation ports, real registry serializers/parsers, Billboard storage and
cJSON. Its isolated exe installs CRT-forwarding JSON allocator hooks once,
before any JSON allocation, and never swaps them around existing trees.
Counted failure windows cover patch parse, outer array, first serializer object,
leaf clone/key, attachment, nested array clone and recursive propagation; rejected
construction leaves tested ECS fields unchanged and frees owned JSON. The actual
production rendering entry also rejects an observed merge-key allocation failure.
Old-extra-clone and old-failopen copies must compile and fail ordinary assertions;
their mutations apply only to the private component/merge fixture owner, while
rendering admission consumes the linked production entry. Healthy allocation
counts are printed rather than assumed as a version-independent quota. Internal
void-serializer field omission and void-parser failures remain outside this
specific guarantee; no GPU, whole-application latency or thread-scheduling claim.

## 目的
为引擎、编辑器与 installed-SDK 消费项目提供按职责分类的回归。旧 IMPLEMENTATION_PLAN 的 ≥85% 行覆盖是历史目标；没有当前覆盖率报告时不得据此声明已达标。

## 框架
- **Unity** (ThrowTheSwitch, MIT) — 固定来源见 `contracts/vendor-sources.json`，由外部只读源码缓存提供，用于引擎 C99 测试。
- **doctest** (v2.4.11, MIT) — 使用固定来源的外部源码缓存。C++ 测试 helper 默认 C++17，实际依赖需要 C++20 的目标私有声明 `cxx_std_20`；C++ 语言本身不决定 editor 域。普通 fixture 使用 `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`；需要真实 self-child 入口的 fixture 用 `DOCTEST_CONFIG_IMPLEMENT` 与唯一自有 main。
- 由顶层选项 `JCE_BUILD_TESTS=ON` 开启；通过 CTest 注册，CI 跑 `ctest --output-on-failure`。
- `os/platform/check_tcp_c99.py` uses a real Linux compiler and an unmodified
  adapter plus a temporary negative control to verify strict C99 declarations.
  Temporary compiler inputs stay under build/tests; other hosts report skipped.
- C 测试用 helper `jce_add_unit_test()`；C++ 测试用 `jce_add_cxx_test()`（含 `editor/src` include + MSVC `/wd4267 /wd4244`）。两者消费同一 domain/kind authority，不能按 C++ 自动归 editor。

## Configure-generated source fixtures

`tests/cmake/JceGeneratedFixture.cmake` owns `jce_test_write_if_changed` for
configure-generated C/C++ fixture text. The tests root includes it before module
registration. An unchanged payload keeps the existing file and timestamp;
missing files, including empty payloads, and real content changes use the same
`file(WRITE)` output behavior. Call with quoted path/content arguments.
Retain each caller's paths, configure dependencies, exact mutation checks and
target registration. Do not use this helper for runtime data fixtures whose
intent is to rewrite or exercise timestamps. The Editor trace generator's local
adapter delegates to this same owner.
No-op configure acceptance needs frozen inputs plus actual generated byte/hash
and timestamp comparisons; compile/link suppression requires a separate bounded
native build witness. Ordinary old-writer controls must fail the same timestamp
assertions, never a CMake parse/configuration error. This is build-cycle evidence,
not application or GPU frame-throughput evidence.

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
- The private sampler fixture compiles the entire production owner with authored
  fake native declarations and a shared counted port. Real headless engine
  controls link this owner with the normal static engine archives to verify
  teardown refusal and retry. No engine implementation is copied; fake controls
  do not certify native OS suspension, host recovery, pixels or GPU timing.
  Its actual collector covers a 102-thread roster including late entries 84..99;
  same-PC records at indices 0/64/127 remain separate, and a full 128-lifetime
  history still refuses new lifetimes without reusing old keys. The counted
  roster port has an independent 192-entry bound checked before filling. The
  complete-owner old_cap64 control must compile and fail ordinary coverage
  assertions; CTest WILL_FAIL alone is not proof of the intended negative.
  Registration lives in os/core/sampler_capacity_controls.cmake.
- The model texture-pool test compiles its first-party private ownership module
  directly with counted texture stubs. It verifies shared staging, distinct
  color/sampler modes, cancellation, failed uploads and one-time destruction
  without creating a graphics device or including upstream headers.
- `test_jce_prepass_target_lifecycle` compiles the complete private camera MRT
  owner with typed counted ports. Its same-source single-slot and omitted-albedo
  cleanup variants must compile and fail ordinary unchanged assertions. The
  Noop scene lifecycle fixture separately verifies renderer aliases, scene reset
  and teardown. Neither control proves GPU completion, pixels or Copy-engine cost.
- `test_jce_window_adapter` compiles the complete Windows SDL/CCD/DXGI owner
  with typed counted native, clock and allocator ports. It verifies source-LUID
  identity despite a misleading NVIDIA Output.HMONITOR, bounded CCD retries,
  strict count/name-packet validation, observed topology changes and exact
  cleanup. Clone paths may repeat the same source, but conflicting identities
  and indirect/unknown paths refuse. Distinct LUIDs with equal vendor/device
  keys remain distinct: only the pinned classic-DXGI first key match can select
  the CCD candidate. First-source acceptance, later-source refusal and retained
  CCD/backend-prediction diagnostics have unchanged assertions. Its wrong-LUID,
  old-strict-key, blind-first-key and Windows-only former DXGI-output variants
  must fail ordinary unchanged assertions. Renderer policy separately checks
  the last usable Caps::gpu index and the first out-of-range index. These controls
  do not query the desktop, load libraries or create a GPU device; native caps,
  physical display, presentation, Copy and DWM measurements remain separate.
- 每个 test 文件一个 `main()` + 一个 `add_test()`，便于 ctest 按需筛选
- `test_jce_build_tool_probe` compiles the complete private Editor probe with
  counted public process/clock ports. It checks finite retention/output admission,
  fair streams, deadlines inside continuous drains and after cleanup, exit-zero
  stdout identity, separate stderr/spawn diagnostics and one destroy/reap. The
  same-source deadline/stderr/output/exit/cleanup/final-drain wire controls must compile and
  fail ordinary unchanged assertions. Trace-on service parent/outcome closure
  and trace-off controls consume the real trace registry. These CPU controls do
  not spawn an OS child or certify GUI reachability, a compiler or tool payload.

`test_jce_build_process_completion` consumes strict extracts of the actual
  Build structs, logs, drain/release/spawn, Stop, poll and public poll wrapper,
  plus the production outcome and host-maintenance headers. Typed counted public
  process and publisher ports cover zero/nonzero cancellation, normal outcomes,
  queued/failing spawn, deferred/failed kill, finish refusal and no-draw polling.
  Real trace controls distinguish SUCCEEDED/FAILED/CANCELLED and restore parent
  identity; trace-off emits nothing. Its same-body missing-Stop wire must compile
  and fail unchanged ordinary assertions. These controls do not spawn OS children,
  draw UI, publish real EXEs or certify native minimize/kill/GPU operation.

## 跨平台
- 平台特定测试（`os/platform/`）用 `#ifdef JCE_PLATFORM_*` 守卫
- 不允许出现 `_WIN32` / `__APPLE__` / `__linux__`（与引擎层一致）
- 文件 IO 走 `jce_fs_host_*`；临时路径走 `jce_fs_host_get_current_dir()` + `_ut_<suite>_<leaf>` 前缀（**引擎未提供 `jce_path_temp_dir`**，避免 OS tmp 污染）

## 当前登记与证据边界

当前目标和变体以各目录 `CMakeLists.txt` 及本次实际生成的 CTest registry 为准，完整清单由 `ctest --test-dir <configured-tree> --show-only=json-v1` 查询。下表只给真实登记的定位入口，不是另一个 catalog、运行收据或全功能通过声明。旧表的“physics 待写”“BT/Recast 跳过”和已覆盖 history 仍列未来任务等描述已经过时，不能作为当前缺项依据。

| domain / 登记入口 | 当前目标例子 | 可以验证的范围与仍需的证据 |
| --- | --- | --- |
| engine / `os/core/CMakeLists.txt` | `test_jce_async`, `test_jce_thread_parallel_for`, `test_jce_filesystem_host_atomic` | 实际共享任务/线程、文件生命周期与定向故障控制；私有 counted ports 不等于真实 OS 故障或设备资源释放。 |
| engine / 同一 core 登记入口 | `test_jce_process_stdio`、两个实际 owner wire 负控、`test_jce_process_gui_stdio` | typed Windows/SDL 端口验证逐流策略及 cleanup 后错误保留；GUI self-child 另行检验真实输出、非零退出与 kill/reap。父句柄存在时默认 GUI 控制返回 SKIP，不能记作无控制台正控；相同源码还需消费当前 installed SDK。 |
| engine / `os/resource/CMakeLists.txt` | `test_jce_bundle_pack`, `test_jce_pak_selected_archive`, `test_jce_asset_font_params` | 公共 Bundle/Archive 与实际 host-tool 闭包、冲突、资产参数；字节/BOM 验证不代替安装 SDK、交付产品或像素验收。 |
| engine / `middleware/physics/CMakeLists.txt` | `test_jce_vehicle`, `test_jce_character_layer`, `test_jce_character_platform`, `test_jce_physics2d_layers` | 已有真实 Bullet/Box2D 公共 API 控制，包括 controller 层过滤/运动平台；不是“整个物理未测试”，也不据此认证 GUI 编辑、Play 输入或视觉。 |
| engine / `middleware/ai/CMakeLists.txt` | `test_jce_bt_library`, `test_jce_bt_exception_firewall`, `test_jce_recast_3d_query`, `test_jce_navmesh_obstacles` | 已登记真实 BT factory/tick、异常边界、Recast/Detour 路径及动态障碍；不能按旧依赖说明整体跳过。 |
| engine / `renderer/`、`middleware/scene/` | `test_jce_offscreen_trace`、各实际资源/scene 目标 | Noop 资源、精确生产 owner、计数端口和请求生命周期要分别说明；逻辑 ownership 闭合不证明 GPU 完成、物理内存释放或画面。 |
| engine / `application/`、`middleware/video/`、`middleware/llm/` | `test_jce_engine_kpi_lifecycle`, `test_jce_video_lifecycle_trace`, `test_jce_llm` | 按 fixture 分别验证公共 Noop/CPU 解码与 worker、真实进程或受控 transport；计时 header/日志控制不等于窗口呈现 FPS，LLM ports 不证明外部服务成功。 |
| editor / `editor/CMakeLists.txt` | `test_jce_editor_config_persistence`, `test_jce_editor_panel_state`, `test_jce_history_identity`, `test_jce_history_entity` | 实际文件持久化、生产 owner 与明确 typed ports；注册、编译和 CPU PASS 都不能替代可见菜单/面板/Undo/重启的实际消费。 |
| editor / 同一登记入口 | `test_jce_profiler_observation`、`test_jce_memory_snapshot`、五个实际 wire 负控 | backend interval、两线程 wait 与 latched GPU 各自计时范围；RSS 预算、tag 局部覆盖、精确完整 JSON 与 profile 下的原子发布。CPU 控制不认证 GPU 瓶颈、实际 UI 可达性或完整 allocator 覆盖。 |
| editor / 同一登记入口 | `test_jce_editor_new_project`, `test_jce_editor_default_camera`, `test_jce_editor_new_scene_failure`, `test_jce_editor_test_runner` | 默认 camera resolve/serialize-reload、项目 helper、实际失败 body ports、真实公共 process self-child；UI Save/Cancel、项目切换、Test Runner 隐藏轮询与关闭仍须当前 artifact 的 GUI 收据。 |
| consumer / `consumers/CMakeLists.txt` | `test_jce_sdk_consumer_c99`, `test_jce_sdk_consumer_scripting` | 明确选择的 installed SDK 编译及实际进程；缺 SDK 时返回 SKIP，不转 PASS。各 `tests/consumers/<project>` 的 Editor Play / EXE / Bundle 是独立模式，不能由这两个 smoke 推导全部可靠。 |

`test_jce_offscreen_trace` also queries the actual live target extent through
the public C API: asymmetric minimum/maximum clamps, unchanged reads without
resource/trace activity, failed creation/resize, retirement saturation retaining
an old target, null outputs and renderer teardown are distinct. These facts
describe admitted CPU ownership and do not establish current-frame preparation
or GPU completion. The Editor's exact source-selection body has a separate
ordinary old-requested-extent control for consuming this query in readback.

`test_jce_bgfx_profiler_trace` invokes the complete production callback vtbl
with the real trace ring and dedicated JCE OS thread. It checks copied dynamic
and literal labels, nesting and parent restoration, independent thread stacks,
bounded depth with visible omissions, disabled/suppressed positions and name
bounds. Its same-owner old-no-op variant compiles and must fail the unchanged
ordinary assertions. These CPU controls do not establish that an upstream bgfx
package enabled its optional profiler, nor do they create a GPU device or prove
GPU execution, frame presentation or graphical performance.
The same fixture exercises the private build/bridge/actual-begin witness with
capture off, once-only init/shutdown publication and saturating count controls.
It also checks that a reset preserves an unfinalized prior count as FAILED
without claiming join, and cleanup without an open attempt emits no success.
`test_jce_bgfx_profiler_attempts` consumes the complete actual window backend
attempt plus exact headless-init/allocation and window-allocation prefixes with
the complete callback owner. Counted native/allocator ports enter init-false
(including zero callbacks), API refusal and allocation cleanup; shutdown's last
callback must be included before a FAILED terminal. Each failure is once-only
and cannot acquire a subsequent successful shutdown witness. The same-body
`old_missing_terminal` variant removes only six actual finalizer wires and must
compile then fail ordinary assertions. These ports do not certify real driver
initialization failure or native shutdown latency.

`test_jce_pbr_custom_trace` consumes the complete actual custom-program
path/cache/read/link region with real host files, explicit VFS and real
Archive/PAK entries. The only controlled work is the program-create result and
the local fallback allocation port. Controls check source/parent correlation,
different requested/resolved backend keys, uint32 hash/byte halves, valid versus
unobserved outcomes, host miss/PAK success, forbidden fallback, absent entry,
allocation/decompression/program failure cleanup, cached failure/hit silence and
cache-budget refusal. Malformed precompressed bytes exercise the actual decoder.
Its same-body old_missing_blob/old_missing_program variants omit only exact
publication calls, retain scope closure and must fail ordinary assertions after
compiling. No control creates a graphics device or certifies GPU execution.

`test_jce_grass_early_gate_trace` consumes the complete bounded observer helper,
the moved actual colour-request and existing admission bodies, and the exact
draw owner begin/GrassField/empty-return/tail publication wires. Typed CPU ports
check five overlapping reasons, other refusal, GL ES tier exemption, original
draw/log/perf behavior, capture-before-history independence, exact field/witness
changes, separate views, generation reset, disabled/suppressed/refused retry and
eight-view overflow coverage. Same-body missing_publication and old_history_mask
controls must compile and fail unchanged ordinary assertions. These controls
do not execute the preceding entity filters, native scene loading or GPU draws.
`test_jce_renderer_admission` reads those facts across real Noop init, three
central CPU frame submissions and joined destruction; only an explicitly
enabled package requires nonzero real callbacks, while missing metadata stays
unknown. These facts do not register bgfx-owned threads or prove GPU work.

The actual `test_jce_pak_selected_archive` fixture pairs plain
`--strip-debug-paths` packing with inspect-only: build BOM addresses come only
from verified current inputs, while the PAK debug path table and source-less
inspection remain opaque. Its missing-BOM-source-names variant compiles the same
tool with the single report-name wire removed and must fail only the ordinary
plain-strip address case. Distinct build-owned fixture bases permit concurrent
runs. This is a host-tool/report control, not SDK install or native app acceptance.

The existing tools/tests/test_host_tool_unicode.py native CLI fixture accepts
explicit JCE_HOST_TOOLS_TEST_DIR binaries. Its suffix controls exercise the
actual jce_pak parser: 31-byte ASCII/UTF-8 and 16-value acceptance, the last
accepted suffix, case-sensitive matching and the existing empty-value no-op.
32-byte and 17-value inputs require exit 1 and a specific limit diagnostic,
with no new outputs and unchanged pre-existing output bytes/mtimes respectively.
test_jce_pak_old_exclude_suffix_wire compiles the complete same-source tool
after removing only the new refusal guards, inheriting the actual jce_pak target
sources/options/definitions/includes/links. JCE_PAK_SUFFIX_TEST_EXE selects that
explicit test-only binary for the unchanged rejection methods; retain their
ordinary `0 != 1` assertion failures. Crashes, missing tools or compile failures
do not certify this negative. The target is not installed or a second packer owner.

状态必须区分：REGISTERED（配置登记）、BUILT（实际编译/链接）、PASS（对应命令/目标与收据）、SKIP/UNAVAILABLE（明确前提缺失）、GAP（必要功能/模式没有实际证据）。普通目标 PASS 只证明该 fixture 的断言；编译成功不证明运行，typed ports 不证明原生服务，Noop 不证明 GPU 像素。运行时 contract 负控须先编译成功，再由同一实际消费断言普通失败；崩溃、编译失败或未执行不能伪作负控通过。

后续验证优先复用已有实际 owner 和 GUI 工作流：先检查编辑器入口到持久化/Play 的消费，再检查各消费项目三种运行模式及缺失依赖的明确反馈。覆盖率或缺项判断使用当前报告，不从这份定位摘要或旧阶段表推出“全部模块可靠”。

## 重要技术注脚（避免重新踩坑）

- **`create` 工具不递归建目录**：测试新增子目录前用 `New-Item -ItemType Directory -Force -Path tests\middleware\<x>`。
- **PowerShell 启动可执行**：必须 `& .\build\...\test.exe`（前导 token 若无 `.\`，PowerShell 会按模块名解析）。
- **历史 preset 故障记录**：旧阶段曾遇到重复 preset；这不是当前配置失败的默认结论。常规配置消费 `scripts/jce.py` 现有 authority；保留实际命令与错误后再定位 preset。
- **`jce_add_unit_test` helper**：定义在 `tests/CMakeLists.txt`，自动链 Unity、加 `engine/include`、注册带 `unit` label 的 ctest，超时 60 秒。
- **编辑器测试目录隔离**：共享同一 CTest 工作目录的普通/故障对照目标使用 `tests/editor/jce_test_owned_directory.h`，按进程隔离路径并仅清理本次拥有的目录。`jce_fs_host_make_unique_path` 只查询可用名称，不原子预留；不能单凭该名称承担并行测试隔离。配置/项目 fixture 在还原环境与 store owner 后再析构目录。
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
ctest --test-dir build\desktop\windows-x64-debug --output-on-failure
python -B scripts\jce.py lint    # current complete source + architecture gates
```

### Streaming media regression

`test_jce_runtime_script_cost` checks new script instances through the public
runtime API before the first update: no inherited measurements, zero calls and
zero costs with profiling disabled. Debug allocator contents must not affect
the measurement inventory, attribution or reset behavior.

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

## Explicit real-font shaping oracle
`renderer/test_jce_text_real_font.c` is headless internal-module verification.
`JCE_TEST_REAL_FONT` supplies an existing real font file read-only; missing
input is explicitly ignored, not a verified shaping PASS. No font bytes are
redistributed. Archive the supplied font hash with execution evidence. The
oracle checks Arabic contextual glyph substitution against nominal mapping,
RTL cluster direction, Latin order, nonzero advances and an independent
HarfBuzz reference, separately from Canvas screenshot review.


## Installed-SDK consumer regressions

`tests/consumers/<project>/` owns complete public-SDK regression applications,
their project-specific fixtures and tools. It is distinct from native unit
fixtures that may exercise internal contracts. Consumer applications do not
include engine/src or original third-party headers. The benchmark consumer
uses public engine counters and repeatable rendering probes; it does not own
an alternative runtime or renderer. Examples publish single-file Bundles and
provenance separately from these reproduction sources. Generate outputs under
ignored build directories and reuse compatible SDK/build caches.

`consumers/render_liveness_lab/` exercises actual grass colour/depth shader
failure, public reset recovery and visibility/caster controls through one
retained renderer and entity. It requires an isolated matching shader-OFF SDK;
its fifteen completed readbacks and no-reset negative control remain separate
from validator unit checks, Editor/Bundle parity and performance acceptance.
The consumer domain registers both CPU evidence validators; their execution
does not require an installed SDK and does not imply the GPU lab has run.
The consumer domain registers both CPU evidence validators; their execution
does not require an installed SDK and does not imply the GPU lab has run.

Script C-ABI tests select the actual target type: SHARED retains loader/export
coverage; STATIC links the real generated forwarders and checks host-copy,
short-host, version and missing-callback behavior without loading an archive.

## Classic shader boundary evidence

`tests/cmake/verify_classic_lit_power.py` uses the existing project-shader
CMake fixture and SDK compile/cache/lint recipe. Explicit source, compiler,
matching include and empty output inputs preserve before/after binaries and
logs. It emits a production-helper-derived dynamic-uniform GPU witness;
`COMPILE_PORTABILITY_PASS` does not imply finite GPU output. The corresponding
`tools/tests/test_classic_lit_power_probe.py` cases check timed-save snapshot
consistency and reject invalid witness provenance; they do not evaluate a CPU
copy of shader math. Actual shader outputs and scene pixels remain GPU tests.

## Render-pipeline request/active controls

`renderer/test_jce_render_pipeline_request_quality.c` compiles the exact
production pipeline TU with only five controlled caps-query providers. It
checks requested material quality against effective GLES/hardware cost budgets,
explicit LOW, deferred frame visibility, request preservation, full-apply
replacement, observer reentrancy/counts, supported numeric forces and cold LOW
defaults. The memoized QA force parser requires isolated CTest processes; each
extra registration carries the normal test environment and global environment
record. Preserve the real renderer tier-clamp target separately. These CPU
controls do not certify actual GPU uniform submission or visual/performance
parity; use actual CB/GL values and the tested program/artifact identity.

Offscreen lifetime controls use actual Noop resources and public GPU resource
counts. The private opaque submission seam advances real renderer frames without
including upstream headers. Cover same-frame saturation, inactive collection,
independent target unlinking, device restart and post-shutdown wrapper destruction;
these counts do not certify physical GPU completion or process memory release.

Renderer production-TU ownership controls may include the pinned bgfx C header
as a narrow internal-test exception. They intercept only the resource admission
and release calls needed to exercise first-party ownership, and use actual Noop
resources for the remaining constructor lifecycle. This exception does not apply
to SDK consumers or authorize modifying upstream code.
`renderer/test_jce_postfx_tsr_ownership.c` checks TSR history/motion and automatic
exposure reuse, resize, destruction, validation rejection, handle exhaustion,
partial creation and retry against
the pinned attachment-reference contract. Retain an independent provider-positive
control. These are CPU ownership requests, not GPU completion or vendor-OOM proof.

Editor exit sequencing reuses the private Play transaction helper and the existing
Stop authority. Its counted callbacks cover active/paused Play, ordinary Edit,
repeated exit, explicit Stop, failed construction, failed restore and pending work
ordering. Mock callback order cannot prove VM destruction; require an actual
running-editor close with script destruction and teardown records before admitting
lifecycle acceptance.

## Clock-provider ownership controls

The core Web-clock test compiles the actual first-party timer owner with isolated
public symbols and controlled native/Web clock readings. Its tiny Emscripten
declaration stub is authored test code, not a copied upstream implementation.
Dependency includes remain inside the owner TU; this narrow ownership seam is
not permitted in installed-SDK consumers. Fractional counter, conversion, timer,
tick-domain and invalid-cast controls supplement the existing native timer suite.
Installed SDK smoke records actual public clock frequency, monotonicity and
observed resolution. Native seam results cannot certify browser execution or
thread-context origins; retain actual target probe output separately.

## Completed-interval reporter regression

`test_jce_engine_perf_hitch` includes the exact application-private owner C
translation unit with the real core snapshot API, without a renderer/window.
This maintenance seam exposes its static bounded formatter to detect writes
into poisoned trailing padding; the target must not compile a second owner TU. Alternating completed
A/B values prove wall/log/phase pairing and owned payload stability. Controls
cover absent/zero samples and wall intervals, disabled/re-enabled phase gates,
skipped/nested iterations, independent ordinals,
full-capacity late registration, bounded escaped names, invalid values,
duplicate observations, window/nearest-rank policy and session reset.
`test_jce_frame_kpi` shares that owner's continuity decision and proves stale
rows are omitted after a skipped or disabled interval while fresh zero samples
survive. Exact public 376/377/378-byte names plus an empty name exercise text
boundaries and canaries; this proves neither OS modal drag nor GPU presentation.

## Bounded KPI capture regressions

Private encoder, writer and capture tests exercise the actual owned translation
units, bounded queues/scratch, native thread ownership, cooperative operation,
error propagation and rejected reentry. The public engine lifecycle fixture
links the actual engine and uses isolated CTest environments and output paths
for terminal-completion, exit reentry, ordinary completion, update, event and
manual owner-pump destruction requests. Actual exit runs once only after the
outer dispatch returns and the host retries destruction. Its headless capture contains
only the initialized CSV header; it does not certify rendered frame rows, GPU
output, storage deadlines or device memory. Window default/INI controls extend
the existing core config fixture rather than introducing another parser.

## Static-profile ctypes transport

The Python differential requires a real shared-library transport even when
shipping uses a static script API. Its private fixture compiles the actual
production marshalling sources and generated curated exports; it never changes
the shipping linkage, stages a DLL beside an application, or copies an API
implementation. Reuse the C-ABI loader/export test against that fixture and
keep explicit build dependencies for every ctypes consumer. A missing optional
Python VM does not suppress the interpreter-driven Lua/binding differential.

## Test registration authority

`tests/cmake/JceTestMetadata.cmake` is the single domain/kind owner. Engine, editor
and consumer domains follow source responsibility rather than C/C++ language.
Helpers register deferred directory finalizers; raw-only directories attach
the same owner. The root audit rejects missing metadata or mismatched labels.
Standalone SDK projects declare local test metadata without repository-helper
dependencies. `jce_test_build_dependencies(<targets>...)` routes each helper
or raw/script registration's real build inputs to both `jce_tests` and its
`jce_tests_<domain>` aggregate using that same call-site domain. Provider source
location does not classify consumption: an external cook tool can be an engine
test input, and a shared runner may serve multiple domains. The deferred audit
rejects missing all/domain edges and manually unclassified aggregate dependencies.
CLI `test --suite all|engine|editor|consumer` selects both the corresponding build
aggregate and exact CTest domain labels; explicit `--target` overrides only build
selection and still requires `--filter`. A regex is not a build-target catalog.
Default all retains the complete configured registry. Preserve legacy unit labels
and report optional unavailable suites separately. CMake NONE/touch and counted
CLI controls verify dispatch/closure, not native linking, GUI or GPU behavior.

Kinds (`unit`, `integration`, `graphical`, `contract`) describe declared fixture scope, not implementation quality. Labels `domain:engine|editor|consumer` follow owner responsibility, and `kind:<kind>` follows that explicit declaration. Preserve compatibility `unit` labels, but do not use `-L unit` as an all-suite selector: raw integration/script/variant registrations can be outside it. Counted-port, Noop, parser
and static-contract tests remain CPU/contract evidence. Actual editor and GPU
acceptance use current artifacts, explicit inputs, isolated profiles, serial
execution, captures and lifecycle outcomes. Missing evidence is unavailable.

Application KPI capture has a private JCE_DIST integration target consuming the
actual capture/encoder/writer plus real core logger and cooperative SDL IO. Its
contract mutant restores only the old stripped INFO footer and must compile,
then fail the same assertions normally (WILL_FAIL). The original fixture stays
available; these CPU controls are not graphical or timed desktop acceptance.

Editor reliability fixtures compile the complete configuration/panel owners. `test_jce_editor_config_persistence` uses one isolated process-owned profile and actual host-file/JSON readback for category migration, debounce and failed-publication recovery; its old-clear-dirty contract target must compile and fail ordinary runtime assertions. `test_jce_editor_panel_state` uses typed ports for unrelated UI providers and the real log sink for Console mirroring; visibility reinitialization, high bits, save refusal, bounded iteration and severity/toast behavior remain CPU evidence. Graphical and native OS delivery stay separate.


Test Runner controls are registered through `editor/CMakeLists.txt`: `test_jce_editor_test_runner` is editor/integration, with real public process self-children and host logs. `test_jce_editor_test_runner_old_exit_status` and `test_jce_editor_test_runner_old_completion_deadline` are editor/contract ordinary-runtime negatives. The complete private owner consumes public exclusive `jce_fs_host_write_new`; failed or incomplete log admission preserves prior results and can retain candidate evidence, while late observed completion is TIMEOUT regardless of native exit code. The two disabled-wire controls remain distinct. GUI directory scan, cancellation, hidden polling and editor shutdown require actual GUI evidence; no configured count is a pass count.


## Windows committed-region observer CPU controls

`tools/tests/test_memory_commit_probe.py` is formally consumed by the existing
`tools/lint/run_all.py` Python aggregation, reached through `scripts/jce.py lint`.
It executes the actual walker/capture owner with bounded CPU ports: committed
versus reserved region/type accounting, allocation-group metadata, invalid or
truncated/nonadvancing observations, query/time/capacity failures, identity and
QPC changes, and exactly-once handle-close requests. It does not open a live
Windows target or certify OS sampling, RSS/private-commit equality, heap ownership,
GPU residency, rendering or the 512 MiB product budget. Keep those native probe
receipts separate. No synthetic native CTest or second Python runner is added;
focused execution uses the same existing test file.

Editor cadence controls consume private authoring policy, real canonical config fields and counted public FPS/renderer ports. Viewport controls use real ImGui CPU focus/clip observations; they start no native window/GPU. Compiled setter and visibility-gate mutations must compile successfully and fail ordinary unchanged assertions. These targets are editor domain, normal unit or negative contract; their registration/native result cannot certify actual PresentMon cadence, background load, DWM/adapter copies or same-quality GPU cost.


Shared host-policy controls compile the actual lifecycle registry with faultable
allocator and counted FPS/audio ports, covering blocked/resumed simulation,
background true/false, temporary caps, nested/repeated focus, listener OOM and
unregistration. Stock default_main's actual adapter body is compiled with a
wire-removal runtime negative. Play module controls consume the private callback
owner, with an old draw-bound update runtime negative; opaque scene identities
are not real ECS or native-window tests. Foreground/paused/background policies,
module teardown and installed SDK rendering still require actual current-build
native/GUI/PresentMon acceptance; registration is not that result.

## Paused elapsed-limit controls

`test_jce_engine_minimized_autostop` consumes the complete current private timer and minimized branch, with counted counter/yield/input/capture/release ports. Its `_old_wire` and `_old_count` contract targets remove only the actual elapsed-exit wire or count a paused poll as completed work. They must compile and fail ordinary assertions; crashes are invalid controls. The shared completed/headless helper controls are not the real public-engine lifecycle receipt, and none proves actual OS minimize/restore or GPU presentation. Preserve `JCE_MAX_FRAMES` fully unset for an ordinary window-only `JCE_MAX_SECONDS` receipt.


Python exact-body compiler controls may reuse the existing C++ execute_stock
helper, but every negative caller must declare its known ordinary exit and
line-numbered stderr assertion marker. The helper requires compilation success
before execution and rejects crashes, signals and unknown nonzero outcomes.
Font controls use exit 3, explicit script-log suppression exit 2, and Play
presentation ports exit 4; all retain their original behavior conditions.
CRT assert/abort is not an ordinary runtime witness. These counted-port programs
do not prove GUI, OS event delivery or GPU presentation.

`test_jce_editor_project_context` combines the public manifest/context resolver with extracted actual dialog/scene admission bodies. Owned host directories isolate its normal and old-always-flush contract variants; counted publication ports prove only identity-gate behavior. Same-project/no-candidate/explicit Bundle preservation must not force publication; real changes and close still refuse failed publication. The existing pstate fixture separately proves real disk failure, dirty retention and retry. Configuration verifies the three SceneSerial callsites; native ports do not substitute for GUI or full scene/ECS rollback evidence.

Model-cache diagnostics compile the actual jce_sr_model_diagnostics.c owner
against real private cache records and core logger/trace. Their narrow private
bgfx include/link exception follows the existing scene-owner fixture and does
not start a graphics device. Normal controls distinguish occupied loaded/pending/
failed slots, opaque path hashes, correlated repeated FAILED outcomes, independent
owners, disabled tracing and generation rearm. The old-repeat-warning and
old-reset variants remove actual wires and must fail ordinary assertions.

tools/tests/test_scene_renderer_cache_recovery.py separately compiles actual
model lookup/poll/invalidate and grass lazy-admission/reset function bodies
against counted CPU ports. It retains complete production cache records and
uses explicit exit 4 plus line-numbered stderr markers for each removed-wire
negative. These controls cover capacity/hit preservation, async decode/upload
admission outcomes, one retry per reset generation, failed deferred-program
retirement and successful colour/depth handle retention. They do not exercise
real glTF parsing, shader IO/deformation, GPU completion or recovery pixels.

`test_jce_build_request_admission` compiles the actual manager request/refusal
prefix and actual Play acceptance/completion bodies with typed CPU ports. It
covers invalid fields, missing SDK config/tools, clean/reset failures, prior
success identity clearing and process/worker reentry preservation. Two ordinary
assertion contract variants retain old refusal state or ignore Play acceptance;
both must compile and fail. Extraction ends before asset preparation and starts
no compiler, native child, GUI or GPU; actual package relocation and UI admission
remain separate acceptance.

Bundle selected-archive CPU controls also produce an actual modern cooked Web
Bundle and invoke the complete pack-tool owner. Exact target, absent/valid/bad
sidecar, independent runtime-host refusal and non-mounted inspection remain
separate assertions. A real self-child exits 1 with the exact category/token
disagreement diagnostic; launch failures/timeouts cannot pass. The
test_jce_pak_selected_archive_old_host_target variant removes only the admitted
target dispatch and must fail ordinary assertions, preserving runtime policy.
No compiler, GPU or browser completion is inferred from these host CPU checks.

Readback lifecycle controls compile the exact queue TU and complete extracted
central jce_renderer_submit_frame owner. Counted ports cover readiness before/
at/after uint32 wrap, explicit half-range ambiguity, bounded/no-progress destroy,
central ordinal plus offscreen-retirement/instance-upload consumption, retained
CPU destinations through stop and detached-wrapper destruction after restart.
The old-direct-frame and old-raw-compare contract targets must compile and fail
ordinary Unity assertions; crashes and compile failure cannot pass WILL_FAIL.
Transition counters are checked against CPU ownership and repeated pending polls
must add no trace noise. Screenshot completion consumes the same wrap/half-range
rules with the real PNG writer; impostor controls cover actual bake admission,
readback consumption and deliberate encoder refusal. The offscreen Noop fixture
also consumes the actual renderer open/stop hooks with a live queue across
restart. These controls establish CPU ownership and API wiring, never physical
GPU transfer, residency, presentation, a fence or full-device idle completion.

The actual test_jce_bundle_inspection_mount fixture verifies borrowed Pak reads
and both mount refusals for memory/file inspection with/without a valid sidecar,
canonical tuple restoration via the existing setter, unchanged opaque-provider
ABI, NULL-provider mount-before-restore refusal, ordinary runtime Bundle/raw
Pak mounting, inspection fallback refusal against an already mounted base and
raw shared-tail cycle refusal. Its missing_mark, missing_mount_guard,
missing_fallback_guard, missing_no_provider_guard and missing_shared_tail_guard
KIND contract targets each compile one complete owner with only the specified
admission wire removed and must fail ordinary unchanged Unity
assertions. Failed compiler/linker/launch or crash outcomes do not pass negatives.
These same-handle CPU controls do not certify a decoder sandbox or GPU/browser
execution. The lock-failure fixture renames the private registration symbol when
including the complete filesystem TU, preserving its separate actual owner.

Texture cooking has a distinct actual-output integration fixture:
test_jce_cook_platform_outputs compiles full CLI dispatch and consumes the public
Bundle pack API, then reads actual JCEA texture info/pixels. It verifies portable
Web RGBA8, Android/iOS ASTC, explicit RGBA/mip overrides and actual old-cache
migration. Build-local complete legacy owners use only isolated symbol names
plus former Web policy/CLI salt/Bundle revision; they produce real old ASTC
artifacts and catalogs rather than a manually authored cache fingerprint.
Current CLI stdout is captured at a counted printf port, and Bundle logs use
the existing callback, so rebuild then warm skip/reuse are observed at the actual
owners. Shared object targets avoid repeating those legacy compilations across
contract variants. The old_web_default, old_cli_salt and old_bundle_salt variants
must compile and fail ordinary unchanged Unity assertions; timeouts, crashes or
link/configuration failures are not negative PASS. The source checks use exact
FIND/REVERSE wire counts, preserving C semicolons and same-length revision edits.
These checks start no graphics device and do not certify runtime upload/browser
pixels or mobile hardware support. Existing inspection contracts remain separate.

Cooked texture controls compile the complete actual `jce_texture.c` owner and
use the real AssetWriter, ArchiveWriter, Pak and cooked reader. Eleven CPU cases
cover RGBA8 and explicitly supported ASTC admission, valid handle zero, missing
versus observed-zero capabilities, malformed header/type/info/pixels, layout
and unknown-format refusal, upload allocation/create refusal, and correlated
integer source identity with FAILED trace closure. Only backend caps/allocation/
create/destroy are counted ports; no GPU device starts. The silent_caps and
missing_identity KIND contract variants remove one exact production wire each
and must compile and fail ordinary unchanged Unity assertions. WILL_FAIL cannot
turn compiler/linker failures or crashes into accepted negative evidence. These
controls do not certify optional WebGL format support, rendered pixels or GPU
completion; actual producer/cache controls and browser acceptance are separate.

Cooking and inspection output fixtures require a distinct CMake target namespace
for every positive/contract executable. Per-case paths are discovered through
public make_unique_path under the normalized current workspace prefix; setup
never deletes a prior path, and teardown removes only a successfully created
owned direct child. Candidate discovery is not atomic ownership: target
namespaces plus the existing cooperating native-tree lease isolate CTest jobs,
not arbitrary uncoordinated processes running the same executable. Old shared
counter-only directory evidence does not establish race-free parallel execution.

Web sample-query controls include the complete private C owner and twelve CPU
cases for real provider counts, empty lists, illegal target/format/pname,
RGB/RGBA query aliases, storage-only depth alias, integer/float/norm16 formats,
invalid values and unavailable contexts. old_uninitialized_samples and
old_query_gate remove one exact actual wire and must fail ordinary Unity
assertions. The complete actual JS bridge runs in a CPU VM with counted current
context, extension, heap/string and GL.recordError ports; context loss/restoration,
replacement and missing_error/missing_context_reset contrasts cannot be copied
into a second policy table. Node absence is unavailable, never PASS. The separate
Emscripten pointer probe uses bounded stdout receipt markers and a counted raw C
symbol to prove the actual proc-address/link-wrap seam without any graphics
context; its omitted-wrap variant returns ordinary exit 4. This narrow receipt
exception does not introduce native IO into production owners. None of these
CPU controls certifies WebGL driver caps, browser pixels, GPU timing or completed
work. C/JS query controls have engine domain and explicit unit/contract kinds.

Host read-detail controls extend the existing test_jce_filesystem_trace and
complete-owner test_jce_filesystem_host_atomic fixtures. Real VFS controls cover
successful delegation, ISOLATED refusal, explicit host paths and OVERLAY host
fallback. Existing counted SDL size/allocation/close ports cover full versus
capped short reads, first failure stage, close observations, byte-pair precision
and modeled size32 admission. The shared hash port counts actual request hashing:
disabled and consumer-suppressed reads must hash zero times and emit no events.
Missing-detail, missing-stage and unguarded-detail variants each compile the
whole production owner with only the named real wire removed; WILL_FAIL accepts
only ordinary unchanged assertions, never compile/link failure, crash or timeout.
Each concurrently registered host target has a distinct fixture prefix, then
uses public unique-path candidate discovery and cleans only its created direct
child. Candidate discovery itself is not atomic reservation. These CPU controls
do not certify application-capture coverage, physical 32-bit ABI or GPU work.

`test_jce_scene_video_phases` compiles the complete production scene-video TU
with counted public decoder, texture and asset-byte ports while consuming the
real scene/ECS container and lifecycle hooks. Its fixture includes no upstream
headers; the production TU uses the existing private flecs dependency. The exact
production renderer admission prefix is compiled with only its accessed fields;
this certifies upload admission wiring, not later render passes. Fifteen CPU
cases cover advance without upload, multi-view reuse, final one-shot frame,
path edit, resize/failure retry, remove/restart, table move and real common trace.
Four same-owner mutations are contract tests with WILL_FAIL and must compile
then fail ordinary unchanged assertions. They do not create a backend, certify
real native minimize or prove video pixels/GPU completion. Resource destroy
commands remain allowed at the existing path-change/component lifetime boundary.

`test_jce_window_web_fill` and `test_jce_window_host_fill` consume the complete
actual window TU with native SDL/public headers and typed SDL ports, selecting
only the Web fill-document branch after header inclusion. Six shared CPU cases
cover resizable/fixed/fullscreen creation, unsigned flag preservation, cached
point/pixel resize consumption, private-flag filtering and creation refusal.
The missing_flag contract variant omits only the exact actual fill wire and
must compile then fail ordinary unchanged assertions; configure/compile/link
failure or crash is not negative acceptance. These fixtures do not instantiate
an OS/browser window or certify the real Web ABI, resize events, DPR or pixels.

Background lifecycle controls compile exact complete CPU/drawable/watch/skipped-
render owner bodies with actual lifecycle and PlayerLoop registries, fixed clock,
and the shipping background property/pacing owner. Typed SDL/input/subsystem/
renderer ports replace external operations only; they do not create a native
window or GPU. The shared policy fixture compiles the shipping property state
and accessors, not a fake process-global background variable, and preserves
its prior FPS/audio port boundary. Controls cover CPU hook order/INIT frequency,
background true/false, manual/OS pause, focus-aware restore, finite/discarded dt,
zero drawable, low authored cap, paused resize and LIFO/conditional publication.
The actual SDK template's transition helpers and draw-admission prefix validate
CPU-only invalidation and once-consumed cache reset. Whole-owner old CPU hooks,
MIN-as-PAUSE, zero-drawable omission, paused resize, fixed 33ms pacing, background
publish/restore/manual, immediate cache reset and missing draw prepare variants
must compile and fail ordinary unchanged assertions. Existing elapsed/minimized
frame-limit controls retain their independent frozen-host scope. Source checks,
configured labels or successful compilation are not native/UI/performance PASS.

`test_jce_web_api_admission` consumes the complete private Web API policy, the
real public renderer version authority and a strict extract of the actual
post-init renderer decision/cleanup tail. Counted snapshot, backend accept,
shutdown/join and context-release ports prove 11 CPU cases and ordinary
wrong-namespace, missing-publish, stale-epoch and missing-init-wire failures.
They assume bgfx initialization succeeded; they do not create a backend or
prove real Web recovery. Existing JS owner controls now include actual API/GLSL
snapshots, listener/reset ownership and missed-between-reads loss epochs, with
a separate missing-epoch negative. Both suites retain the common ENV/domain
registration. Real GL version, loss, resources and pixels require browser tests.

## VFS whole-read capacity and public output controls

`os/core/test_jce_filesystem_read_all_bounds.c` includes the complete shipping VFS
TU in a private symbol namespace, with real public/private dependency declarations
and typed PhysFS/opaque-provider/allocation ports. Nine ordinary CPU cases cover
binary bytes, miss/empty/unknown length, allocation/short-read failures, optional
output, once-close/free ownership, exact PAK decode and single-buffer transfer.
SIZE32 adds two real-owner boundary cases for SIZE_MAX, wrapped larger lengths and
the last payload with sentinel headroom; no huge allocation or OS file is made.
The full historical read-all body and omitted public output reset are separate
KIND contract/WILL_FAIL controls. They must compile and fail ordinary assertions,
not crash/link-fail/timeout. No unversioned public ABI slot or second VFS pipeline
is introduced; real native IO/architecture and browser memory behavior remain
separate acceptance. Existing filesystem/trace/lock fixtures retain their owners.

## Runtime net render call-site guard

`application/test_jce_runtime_net_render_gate.c` includes the exact shipping
networking render-step block with real public session/transform/animator
declarations and typed counted ports. Four CPU cases cover bridged/unbridged
crossed with NONE/live modes; live coverage includes HOST, DEDICATED_SERVER and
CLIENT, preserving one transform then one animator call and the original alpha.
The old missing-braces block must compile and fail ordinary unchanged assertions
(KIND contract/WILL_FAIL). Existing L4 `test_jce_net_animator` seven no-session
loopback cases remain a separate real-owner regression. This call-site seam is
not a full runtime/session/transport or GPU execution certificate.

## Threaded capture coalescing controls

test_jce_trace_capture continues to compile the complete production consumer
with the real ring/Chrome encoder and host stream owner. Six additional cases
use typed clock/thread/write ports for deterministic 1ms short polls, 15/16ms
nonrenewing deadlines and long-poll clipping, threshold writes, forced stop-tail
accounting, deferred IO failure and fixed bucket/slowest-sample observations.
A fake thread token models ownership only; these timing ports do not certify
native scheduling or throughput. Existing real worker cases and full Engine
threaded/cooperative integration independently retain actual join/unregister
coverage. No counted events are dropped or committed before accepted writes.
The full-resident stop case leaves one real ring of events without an intermediate
poll, then requires complete publication, zero loss/unexported events and actual
capacity metadata. Resident-export expectations derive from JceTraceStats.capacity;
overflow still requires the same exact lost count. The tool's short-budget
mutation restores the old 32-batch drain against the current real-ring capacity
input and must fail an ordinary executed drain assertion after compilation.

The ordinary test_jce_trace_capture branch also retains
trace-screenshot-requests-observed.json from the real ring, Chrome encoder and
complete continuous writer. Exactly six identity-bearing RENDER admits cover
both modes and all three actual task outcomes; a real joined worker emits six
terminals in paired reverse order. Wide uint64 identities include values above
2^53 and UINT64_MAX. One successful lifecycle.screenshot_protocol service and
zero-loss/zero-active complete publication remain mandatory. The retained file
is for the Python lifecycle reader and mutated CPU controls; it is a protocol
and writer witness, not a PNG callback, renderer or GPU end-to-end certificate.
The optional compile-time capture variants do not register this case.

Complete-owner old_short_flush/missing_deadline/missing_stop_flush/
missing_diagnostics/old_small_budget variants compile first, then fail ordinary unchanged
assertions after owned capture cleanup; KIND contract/WILL_FAIL is explicit.
The old-small-budget target selects the same real-ring final-tail case and must
report a captured-event mismatch; its CTest expected-failure verdict alone is
not the ordinary assertion evidence.
The publication fixture seeds the actual sequence/ID owners at signed and unsigned
boundaries rather than recording billions of events. It retains finite reader
budgets, permanent exhaustion/new-consumer rejection, full-width concurrent IDs,
high task parents, nested suppression and reset-sensitive registration. Old-wrap
contrasts use only unsigned defined arithmetic; ID/task truncation and collapsed
suppression contrasts fail ordinary unchanged assertions. A host-forced static
32-bit ID fallback proves that algorithm only, not a physical 32-bit ABI or Web
runtime. UINT64 exhaustion is tested at the actual reserve helper; the public
wrapper's unconditional fatal path cannot return a recycled value.
An exact SDL-store port interposes actual producer exhaustion between the enable
setter's first validity read and final native store; the missing cold recheck
fails an ordinary enabled-state assertion. Off/on controls cover re-registration
and direct exit, preserving a single register/unregister pair. A semaphore-
quiescent old worker exits after reset while a new registration remains counted.
The old capture-generation accounting contrast fails both toggle orders; reset
cannot stand in for fixing those sequences.
Each target retains its distinct file namespace. Existing old_reopen/old_close
controls remain. Tool contract helper extraction is updated from the same
owner; optional writer diagnostics use the existing lifecycle parser with
legacy compatibility and wrong-type/bucket/time/accounting rejection cases.

## Web lost-frame admission controls

Renderer context controls consume the complete production admission/central
submit bodies with typed CPU ports; refusal must leave driver calls and
submission/retirement/instance ordinals unchanged. Existing lifecycle adapter
controls consume real private bool outcomes, with an ordinary former
pointer-only-success negative. Engine controls extract actual early/event/watch
and post-callback refusal branches; the following-stage port must stay uncalled
and terminal requests remain failed/incomplete. These narrow controls do not
boot a browser or certify every foreign callback/GPU API, actual cleanup timing,
context restoration, resource reconstruction or fences. Keep actual browser
loss as a separate fault protocol; normal Web acceptance must still reject loss.

## Scene resource preparation owner controls

`test_jce_scene_resource_prepare` compiles exact production prepare/observation,
complete async upload polls and real render/reset admission prefixes with the
actual private SceneRenderer layout. Thirteen groups use typed CPU backend,
async terminal and upload ports, real core trace and real Scene lifetime. They
cover admission, repeated views, same-round arrival, the existing model budget,
scene A→B→A, cache/reset, context, round rollback, callback policy, video
publication and legacy fallback. Three unchanged-case ordinary negatives remove
round reuse, context admission or legacy fallback; KIND contract/WILL_FAIL is
explicit. Existing fifteen full-video-owner groups retain real ECS/decoder and
texture-counted ports and now consume the same prepare body/layout. These
bounded controls do not execute the remainder of rendering, Editor UI, actual
backend upload, context reconstruction, GPU completion or performance.

## Skeletal first-view selection controls

`test_jce_scene_anim_multiview` compiles the complete current animation owner
with real Scene enumeration/authored ordering and real L5 skeleton/clip/player
sampling. Model lookup, optional JSON IO, shared-thread selection and GPU morph
are typed counted ports; sampling is deliberately serial here. Ten groups
cover disjoint and empty views, generation reuse, legacy timing, disabled
components, initial and scene-transition allocation failure/same-round retry, serial callback add/remove
snapshots and retained-buffer growth. Two unchanged-case ordinary controls
restore the actual first-view list or former pre-reserve frame stamp; KIND
contract/WILL_FAIL is explicit. The wrapper includes real headers before the
realloc port macro; no CRT declarations are renamed. These controls do not
verify imported models, parallel sampling, GPU deform/palette upload, pixels,
SimLOD camera selection, background CPU clocks or performance.

## Public GPU timing observations

`test_jce_gpu_timing_observation` includes the complete actual `jce_gpu_caps.c`
owner and the exact renderer device-context getter with typed bgfx stats ports.
Its 12 cases cover NULL/all undersizes with zero writes/queries, oversize canary,
absent context/backend, zero/negative frequency, negative/equal/reversed ticks,
forward conversion, adjacent ticks beyond double integer precision, repeated
latched samples, teardown and Web context invalidation. Three complete-owner
omitted-availability/size/context wire controls must fail ordinary assertions.
The seven actual Profiler-helper cases cover public GPU admission, unavailable
legacy-only values, raw CPU intervals and distinct waits; old-wait-math and
old-GPU-wire controls are ordinary contract failures. These CPU fixtures do not
create a device, prove physical GPU timing availability, join CPU/GPU frames or
certify GPU completion. Real SDK/Editor consumption remains separate evidence.

FS presence controls compile the complete VFS/host policy owners and private
native metadata adapter with typed result/error ports. Positive tests cover
regular/missing/nonregular/access, custom/NULL reader, isolated/overlay, PAK,
priority UNKNOWN and unavailable builtin lock; empty PhysFS search paths with
mounted PAKs, stale TLS error and fresh query errors use the same complete owner.
Fresh OK with a nonempty search path remains UNKNOWN. Two ordinary missing-wire
controls retain real trace ownership. filesystem_trace adds an owned temporary
file/miss/directory probe through the unmodified OS adapter. Mixer resolution
controls include the full runtime-audio owner, actual config parser/offline
device sends and real custom/active-VFS consumption. Missing-PAK and skipped-
UNKNOWN variants must fail ordinary assertions. Native OS, POSIX/Web execution,
SDK/ABI refresh and complete lifecycle acceptance remain distinct evidence.

## Shared application resource observations

`test_jce_engine_resource_observation` includes the complete private resource
owner and strict actual Engine boot/no-draw/active-frame wires. Counted public
memory, time/thread and qualified GPU ports plus the private non-creating Async
bridge accompany the real trace ring and Chrome encoder. Fifteen groups cover
default-off zero queries/events
and engine allocator deltas, disabled trace, zero-origin cadence, bounded
intervals, jump/rollback, current-default absence, no-draw/headless/fallback selection,
normal draw-before-read-before-end, unavailable stats, unsigned64/signed64
halves, owner-thread refusal and per-sample id/parent. The no-draw input is a
host-policy port; actual OS pause/focus/restore remains in the existing host
controls and independent native receipts. CPU fields do not certify complete
heap ownership, actual device availability, GPU cost/completion or pixels.
The real draw callback shuts down the default executor through its public API;
the observation is unavailable afterwards and reads a recreated current owner.
The existing `test_jce_async` lifecycle fixture checks the real default stats
bridge before creation, after shutdown/recreation and during its semaphore-held
closing interval. `test_jce_async_default_stats_missing_guard` compiles the
complete actual Async owner and removes only its live/closing admission; the
unchanged ordinary assertions must fail without dereferencing a stale pointer.

Same-source `missing_opt_in` restores an unconditional enable admission;
`missing_validity` treats a copied GPU snapshot as an available timer. Both
complete-owner contract targets must compile and fail unchanged ordinary
assertions. Configure-time unique-wire/order refusal, build failure or crash is
not an accepted behavioral negative. No imported recording, external attach,
new GUI route or zero-environment tool payload is delivered by these controls.


Resource zero-owner controls execute the complete private owner and actual normal
Engine wire with the zero-thread port, while leaving native trace IDs unchanged.
A separate actual Chrome encoder packet preserves raw zero. The old_zero_guard
full-owner contract variant must fail ordinary assertions. These CPU ports do
not certify a browser capture; canonical resource reporting must still prove
one named cooperative zero-thread capture through the complete footer authority.

## Process-memory provider qualification

`test_jce_mem_stats_platform` compiles the uniquely extracted actual public
mem_stats function in native/Web policy modes with a typed eight-pointer mimalloc
port. Four groups preserve large positive native values, reject Web allocator
estimates as process RSS/commit/peaks, keep elapsed/page faults, refuse NULL
before query and prevent prior native values surviving a Web snapshot. The
`old_web_estimate` contract removes only the complete actual Web qualification;
unchanged positive-value assertions must fail normally after successful compile.
The existing real allocator suite remains intact. Typed Web preprocessing is not
a Web architecture/browser run or native OS measurement; real SDK consumers and
canonical resource N/A reporting require independent artifact execution.

## Complete camera clone controls

The existing camera CPU fixture uses actual public clones to compare rolled/pole
views and perspective/fixed-width/dynamic-height projections at two aspects,
independent mutation and release after source destruction. A strict extract of
the real private value struct and complete clone body uses the existing tracked
allocation seam for NULL/no-query, OOM/source preservation and full-state retry.
The old-copy contract changes only the actual full-state assignment to a partial
position copy and must fail ordinary assertions after releasing test resources.
No public allocator override or graphics context is introduced.

## ImGui actual draw clip controls

`test_jce_imgui_clip_draw` consumes strict extracts of the complete first-party
draw function, its actual static state/texture codes and setup extent wire, with
the actual clip and texture-ID admission helpers. Authored POD/typed counted
backend ports record scissor bounds, draw state, copies, offsets and submissions
without including upstream headers or creating a GPU device. Eighteen groups
cover preserved valid rounding/state, framebuffer intersections, invalid finite
and nonfinite inputs, DisplayPos, zero/resized extents, narrowing bounds, callbacks,
texture maintenance/rejection, combined allocation failure and recording order.
The same-source old_clip_limits contract restores only the old 65535 viewport
admission limit while retaining finite checks; missing_extent removes only the
actual setup assignments. Both must compile and fail unchanged ordinary draw
assertions, never by undefined narrowing, build failure or crash. CPU ports do
not certify native rendering, pixels, font output, DPI support or GPU savings.
The same complete draw fixture verifies aggregate clip rejection observations
and no clip-counter emission for a valid draw; ordinary culling is not an error.

## Exclusive host streaming copy controls

The existing complete host-FS fixture consumes public copy_file_new through
real SDL paths, using counted source-open/size/read/write/status/allocation/close
ports only for bounded failures. It verifies real multi-chunk and empty copies,
existing files/directories, a competing real creation at the wbx boundary,
64 KiB scratch/request bounds, retained created ownership on partial IO and
independent failure of either close. Caller cleanup is conditional on created;
the production owner never deletes a candidate. The same ring verifies scoped
hash/byte/known/stage/close details and disabled/suppressed no-detail behavior.
Whole-owner missing-created, ignored-destination-close and overwrite variants
must compile and fail ordinary unchanged assertions, not crash or time out.
SIZE32 models length arithmetic only; these CPU fixtures do not qualify an
actual 32-bit OS, concurrent-source snapshot, disk durability or DLL loading.

The installed sdk_smoke and sdk_smoke_scripting consumers explicitly request
headless boot on non-Web targets. Noop selection alone is not a no-window
contract. Their PAK/API/script checks and update/quit markers remain required;
Web execution still needs its own runner and keeps its descriptor behavior.

## Bounded text-cache conflict and timing controls

`test_jce_text_shape_cache` retains shaping/itemization semantics and consumes
deterministic valid UTF-8 keys sharing a bucket for every owner address. The
complete-owner one-way control must fail ordinary unchanged reuse assertions.
Four alternating wall-timed pairs retain raw counter ticks, frequency and actual
hit/miss deltas before the unchanged 2x savings assertion. All timing pairs remain
in the report. Debug fixtures do not certify Release application CPU/GPU or
presentation performance; address-prefix arithmetic does not establish the cause
of a previous uninstrumented timing failure.

## Project boot-policy Bundle closure

The existing Engine resource test_jce_bundle_pack target now packs the standard
project RenderPipeline into its canonical runtime address. Its actual archive
and graph witnesses cover byte preservation, one-asset closure, missing-default
behavior, equal-source deduplication, conflicts/nonregular source rejection,
explicit project priority over a foreign active VFS, preserved caller shader
resolver and public PAK fallback lookup. These are packaging/lookup controls,
not renderer application or GPU timing. ES's Python staging controls separately
require explicit --project and source-hash/BOM witnesses, including an executed
old omitted-argument negative. Runtime and isolated Editor acceptance must
still observe the complete effective descriptor rather than infer it from BOM.
The same actual pack fixture verifies two-scene catalog thresholds 3 and 2:
ordinary payloads remain local below threshold and share at threshold, while the
global RP root is always shared. Every produced PAK is reopened, payload bytes
are compared, and each catalog scene mounts its RP dependency. The adjacent
bundle_partition_controls.cmake compiles the complete actual owner with only
the old below-threshold omission restored; test_jce_bundle_pack_old_partition
must fail the unchanged ordinary-payload placement assertion, not compilation.


## Audio master ownership controls

`middleware/audio/test_jce_audio_master_lifecycle.c` consumes the actual private
shared-master implementation with typed miniaudio device and clock ports, real
I32 atomics and bounded simulated callback holds. Ordinary controls restore the
old timeout release, unsafe tap replacement and lazy reopening. These are owner
semantics, not native driver timing or scheduling evidence. The separate actual
JCE_NO_AUDIO hook include fixture covers only the newly added private entries;
the historical complete disabled facade remains outside that claim.
`application/test_jce_engine_audio_retirement.c` compiles the complete Engine
owner and uses the production callback gate behind the audio service port. It
checks callback reentry before other waits, pre-tail refusal/retry, failed and
borrowed constructors, new-session admission and exit ordering. Its separate
CMake include supplies ordinary missing-barrier and missing-close controls.
Existing real-device master tap cadence and silence tests remain unchanged.

The master fixture now forwards an atomic-create fault and the real log call to
observe partial-gate cleanup, one latched allocation ERROR and next-session
recovery. Its unmarked variant leaves the thread facade uninitialized. The
complete `test_jce_audio_offline_owner_thread.c` TU uses a real native worker
after mark_main, actual miniaudio offline rendering and a forwarding free port
to observe graph destruction. Old worker-create/worker-destroy main restrictions
fail ordinary assertions with retained graph cleanup on main. This is not a
claim of concurrent public audio controls or native Web worker support.


## Shared trace JSON span controls

`os/core/trace_json_span_controls.cmake` compiles the complete actual Core codec
region and batch transaction function, separately from the ring/capture owner.
Its old-character encoder is a complete original escape/literal-prefix body
mutation, used as an uninstrumented byte oracle and ordinary negative. Real SDL
forwarding ports count explicit codec copy, strlen and format calls. All nonzero
name bytes, all event kinds, empty/47-byte/first-NUL names, limits, invalid tails,
reused storage and explicit-length overflow retain the original output and batch
rollback. The old target must compile and fail the unchanged work-count checks;
WILL_FAIL alone is not a direct execution receipt. Optional `--benchmark` runs
bounded uninstrumented ABBAABBA old/current encoding with reused buffers and
actual performance counters, excluding IO, producer threads and GPU work. These
controls do not certify lossless application capture or whole-frame improvement.


## Trace reader loss diagnostics

The real publication fixture compares observed/public cursor and event behavior
at reserved-pending, locked-busy, batch-limit, caught-up, empty and overwrite
seams. No busy-slot unlocked read or replacement producer is used. The complete
capture fixture exercises two real ring overflows, fixed first/last retention,
prior-read progress, cooperative/controlled-threaded/terminal phase labels,
unavailable observation clocks, zero-loss metadata and maximum-width serializer
storage. The controlled threaded port does not certify scheduler behavior.
`os/core/trace_reader_controls.cmake` compiles ordinary missing-metadata and
missing-first-loss variants; both must fail unchanged assertions after native
compilation and owned cleanup. Footer observations describe read detection,
not exact overwrite time, missing-event content or proven cause. Existing
lost/unexported/generation/IO and publication gates stay authoritative; the
optional typed metadata validator remains in the canonical analysis route.


## Trace JSON integer args controls

`os/core/trace_json_integer_controls.cmake` compiles actual current codec/batch
owners and a test-only exact pre-change formatter/export body from
`trace_json_integer_legacy.inc`. The shared native codec fixture verifies bytes
across all event kinds, unknown/negative enum defaults, decimal-width edges,
uint64/signed extrema, timestamp precision boundaries, deterministic mixed
whole-width inputs, names/escaping and batch storage/reuse/rollback. Checked
scratch failures and UINT64_MAX/INT64_MIN strings exercise the actual ASCII
writer. Counted forwarding observes ordinary format calls falling from two to
one; the original printf target must compile and fail that unchanged assertion.
The common timestamp snprintf and double/%.3f calculation remain literal old
code, with no integer timestamp substitute. Its optional uninstrumented ABBA
codec benchmark excludes ring producers, IO, application frames and GPU work;
codec improvement does not certify zero-loss capture or an FPS target.

`test_jce_miniaudio_com` is a Windows Engine/unit CPU fixture. Its CMake owner
extracts the complete untouched `ma_context_get_MMDevice__wasapi` function
from the current Conan-selected miniaudio header, not a checked-in algorithm.
Missing or ambiguous selected input fails configuration. Typed external ports
retain a prior SDL COM reference and distinguish S_OK/S_FALSE from failed
initialization, including CoCreate/endpoint failure and explicit-device paths.
The original0.11.22 body must fail ordinary ownership assertions after successful
compilation; do not use WILL_FAIL or accept compile failure as the regression.
Non-Windows reports this WASAPI-specific target unavailable. These controls do
not certify a physical COM device, complete worker/context teardown or Win7
application exit. Keep actual native acceptance separate.

## Selected archive single-entry read

`os/resource/test_jce_pak_selected_archive.c` exercises the complete real CLI in
self-children and public Archive/Bundle/FS owners. Existing21cases remain; added
cases retain exact compressed/stripped and typed bytes, plain empty entry versus
missing entry, secure key admission, canonical-address refusal, invalid CRC,
16MiB decoded cap, protected outputs and ambiguous zero-codec refusal. The three
complete-tool limit/CRC/overwrite mutations must fail ordinary assertions.
`test_jce_archive` independently checks empty-only writer output through strict
verify-on-open and entry CRC across index compression/debug-path combinations.
No fixture patches archive headers or relaxes the reader; these CPU controls are
not rendered, performance, durable-write or deployed-SDK certification.

## A1 XP Core compatibility probe

`compatibility/xp_core` is an explicitly limited C99/C++20 + actual
`jce_entropy.c` smoke consumer.  It does not replace regular engine/SDK
tests or establish SDL/bgfx/Editor support.  The PE gate must include
negative controls; OS runtime claims require execution on XP itself.

`compatibility/xp_os` compiles the actual C99 executable-path and legacy
display-observation owners using the same XP target toolchain. Typed path ports
exercise valid BMP/supplementary UTF-16, malformed surrogate rejection, buffer
growth and allocation balance; one path also queries the actual executable.
The validation-omission control must compile and fail at its specific ordinary
runtime assertion. The complete legacy observation owner must clear outputs and
return its typed unavailable reason without SDL/COM or a device query. Use
`scripts/jce.py xp-os-probe` for repeatable native command construction. Host
execution is distinct from matching XP guest execution and from Editor testing.

## XP Flecs callback and integer controls

`compatibility/xp_flecs` compiles the complete first-party bridge with actual
Flecs/Core declarations and typed dependency ports, plus a separate real-header
integer atomic target. Its common wrapper calls actual Core entropy. The 17
positive executions are 14 legacy bridge scenarios, the modern no-table-access
case, unsupported `FLECS_TRACK_OS_ALLOC` refusal, and the integer control.
They cover installation/foreign-table refusal, persistent registration, heap
attempt counters, callback-result/ticket ownership, rollback, synchronization,
coarse-clock units and bounded sleep. Core service ports do not create a real
Flecs world or SDL worker thread.

The six first-party variants omit result capture, omit join, restore millisecond
time units, miswire condition broadcast, miscount calloc, or omit API set.
Each must compile and fail its unchanged scenario with exit 4 and the exact
assertion marker. Compile errors, crashes, entropy failure and unrelated exits
are not passing negatives. `tools/tests/test_xp_flecs_os.py` only prepares bounded
source mutations/manifests; `tools/build/xp_flecs.py` owns repeatable native
production and optional local execution. Static imports, host CPU controls and
XP guest runs retain separate evidence; none alone qualifies the full Editor.
