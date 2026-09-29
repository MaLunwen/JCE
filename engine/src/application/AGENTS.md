# engine/src/application — Application / Bootstrap (L6)

> Engine boot, subsystem ownership, main entry. Top of the engine; below consumers.

## Identity

- **Layer**: L6 (Application). C99.
- **Public umbrella**: `<jce/api_app.h>` → `<jce/application/*.h>`
- **Deps**: every layer below (PUBLIC link in CMake), notably `jce_core`, `jce_platform`, renderer, scene, middleware.

## File map

| Header | TU | Role |
|--------|----|------|
| `jce_engine.h` | `jce_engine.c` | `JceEngine` lifecycle: create, destroy, frame step. Owns window/renderer/audio/input/PAK/app. `jce_engine_iterate` walks the 8 PlayerLoop phases via `jce_player_loop_run_phase` (P3-B.1) as an AUXILIARY hook surface — the real renderer / subsystem / input / scene work remains INLINE in iterate (and physics in `jce_runtime_step`) to preserve resize-watcher and modal-loop guards; this is *not* full Unity PlayerLoop parity, the phases run alongside the inline work rather than owning it. **P3-B.2 / P1-fixed-clock-unify**: `JCE_PHASE_FIXED_UPDATE` is accumulator-driven via `jce_fixed_clock_default()` (0..N steps per frame, default **60 Hz**, configurable through `jce_engine_set_fixed_hz`). That global clock is the single source of truth for the fixed cadence — the per-runtime physics clock adopts its `fixed_dt` (see runtime AGENTS rule 3), so `set_fixed_hz` now governs physics too instead of silently desyncing from the old 50 Hz default. **P3-B.3**: registers an internal lifecycle listener at boot that bridges `JCE_LIFECYCLE_LOW_MEMORY` → `jce_streaming_signal_low_memory_all()` (pairs with P3-A.2 mip-streaming hook); `jce_engine_event` translates SDL platform-lifecycle events (focus/pause/resume/low-memory/will-quit/device-reset) into `jce_lifecycle_emit` calls before delegating to the app, and `jce_engine_destroy` calls `jce_lifecycle_shutdown` after PlayerLoop teardown. |
| `jce_lifecycle.h` | `jce_lifecycle.c` | **P3-B.3** Unity-parity application lifecycle event registry (`FOCUS_GAINED/LOST`, `PAUSE/RESUME`, `LOW_MEMORY`, `WILL_QUIT`, `DEVICE_LOST/RESET`). Priority-sorted listener table (same pattern as `jce_player_loop.c`); emit is main-thread only — events originate from the SDL pump in `jce_engine_event`. Cached `is_focused` / `is_paused` state is updated before listeners fire so getters reflect the new state inside callback code. |
| `jce_main.h` | `jce_main_sdl.c` | SDL3 entry-point shim → forwards to `jce_engine_*` callbacks. Single place where `SDL_AppInit/Event/Iterate/Quit` lives. |
| `jce_app_interface.h` | (in `jce_engine.c`) | `JceAppDesc` callback table (`on_init/update/render/event/shutdown`). Consumers implement this. |
| `jce_subsystem.h` | `jce_subsystem.c` | Subsystem dependency ordering / shutdown reverse-order |
| `jce_args.h` | `jce_args.c` | argv snapshot + `--dev <assets-dir>` parser (also `JCE_DEV_ASSETS` env). Stashed first thing in `jce_engine_create`; consumers query via `jce_args_get_dev_assets`. |
| `jce_camera_controller.h` | `jce_camera_controller.c` | First/third-person camera input handler (utility for consumers) |
| `jce_screenshot.h` | `jce_screenshot.c` | Capture backbuffer → PNG via stb_image_write |
| `jce_runtime.h` | `jce_runtime.c` | Shared standalone/editor runtime state. Pointer and touch samples are frame-transient; hosts set the full input first, then replace touch state through `jce_runtime_set_touch_input()`. Touches are finite, stable-order, capped at five, and released automatically when not resubmitted on the next frame. |
| (internal) `jce_rt_script.h` | `jce_rt_script.c` | Public-runtime-to-Lua host bridge, including bounded VFS text reads and the one-based `jce.get_touch(index)` view over runtime touch state. |
| (public include) `jce_default_main.inc.h` | — | Thin default consumer shell. Samples platform pointer/touch input once per frame and submits the same normalized runtime input contract used by editor Play/Game View hosts. |
| (internal) `jce_engine_windowed_input.h` | (in `jce_engine.c`) | `jce_engine_create_windowed_input()` — the ONE place a windowed boot builds its raw input: `jce_input_create()` **plus** `jce_input_set_backend(jce_input_sdl_backend())`. It is a named function rather than two lines inside `jce_engine_create` so the install is reachable from a test with no window; that install went missing for a release while three comments claimed it existed, and no display-less runner could see it (976d38fa). Guard: `tests/application/test_jce_engine_input_backend.c`. Raw input, the action map and input record/replay are created **above** the fallback-renderer early return, so a safe-mode boot has a live `e->input` and a loaded `e->actions`. It does **not** have a populated `JceServices`: `e->svc`, `jce_subsystem_init_all` and the app's `init()` are all *below* that return, so on a safe-mode boot no app and no subsystem ever receives one. The only readers on that path are `jce_input_handle_event` / `jce_input_update` and the public `jce_engine_get_actions()`; `jce_actions_update` is below the fallback branch's return in `jce_engine_iterate`, so the action map is loaded and never evaluated there. Don't build a safe-mode consumer on `svc->actions`. |
| (internal) `jce_embedded_assets.h` | (generated) | Embedded fallback assets for JNI-desktop builds. Also declares the protected-asset key shares (`jce_embedded_pak_key_shares[64]` + `_present`) on ALL platforms; `jce_engine_create` XORs the two shares, immediately erases the temporary key, and installs it via `jce_archive_set_process_key` BEFORE the first PAK open. The shares only avoid a contiguous static key signature; they do not make a client-held key secret. |
| (internal) - | `jce_pak_key_default.c` | Zeroed default for the key-share externs. It is compiled into the normally linked `jce_pak_key_defaults` archive, outside the SDK's force-loaded core. An editor-generated `jce_generated/jce_pak_key.c` target object resolves the symbols first; otherwise the fallback member is extracted and `present == 0` skips key installation. |
| (internal) `jce_version.c` | — | Built from `jce_version.h.in` (CMake-configured) |

## Rules

1. **SDL only appears in `jce_main_sdl.c`** — nowhere else in the engine. (`os/platform` uses SDL internally too, but does not export it.)
2. **`JceEngine` is the singleton owner.** Subsystems are owned via `jce_subsystem.c` in deterministic order; shutdown is reverse.
3. **Consumer integration paths**:
   - **Standalone game**: link `JCE`, implement `JceAppDesc`, call `jce_engine_set_app_desc` before engine create. `main()` comes from `jce_main_sdl.c`.
   - **Editor-driven (Play mode)**: editor registers a `JceGameModule` (see runtime layer); engine is driven by the editor's frame loop, not SDL_App.
   - **JNI**: `engine/java/com/jce/JceRuntime.java` calls into `os/platform/jce_jni_bridge.c` which forwards to `jce_engine_*`.
4. **No game logic** lives here. Camera controller is a *utility* consumers may opt into.
5. **Config path / PAK path overrides** are set via `jce_engine_set_config_path/_pak_path` before `jce_engine_create`. Don't hard-code paths.

## Don't

- Don't add a second `main()`. The only entries are SDL3 shim, JNI bridge, and editor.
- Don't depend on consumer-specific code (examples/caged_kingdom/editor).
- Don't bypass `jce_subsystem` for init/shutdown order.

## 2026-09-06 — `rt_rigidbody_kind()`：只有一个答案

「这是什么类型的刚体」此前有**两个**答案：两条 3D spawn 路径按 `is_kinematic` / `mass` 判，
而 `rt_softbody_mirror_static` 按 `JceRigidBodyComponent.body_type` 判。那个字段
**从不被 parse**（`parse_rigidbody` 不读 `bodyType` 键）、**从不被序列化**、**没有控件**
⇒ 它在有史以来每个组件里都是 0，而 0 == `JCE_BODY_STATIC` ⇒ 镜像判定恒为「不是动态」
⇒ **每一个动态 BoxCollider 都被复制进软体世界当不可移动的地面**。

三处现在都调 `rt_rigidbody_kind()`（`jce_rt_internal.h` 里声明）。
**新增第四个需要这个判断的地方时调它，不要再抄一份三行的 if 链。**

`body_type` 仍未被它读取，理由写在 `tools/lint/component_field_baseline.json` 里：
0 同时表示「作者说静态」和「没人说过」，honour 它会冻结树里每一个动态刚体；
要让它成为权威需要一个表示「未设置」的取值，那是**编码变更**不是接线。

同一提交里 `rt_voice_should_be_3d()` 的形状相同：一个导出的谓词，三个必须一致的调用点。

## 2026-09-06 补 — `body_handle_idx`：0 是一个合法句柄

`rt_spawn_body2d` 一直把创建出的 body 索引写回组件，注释写着它 "mirrors 3D contract"
——**而 3D 侧从没实现过那个契约**。现在两条 3D 路径都调 `rt_record_body_handle()`。

**关键不是「写回去」，是 0 同时表示两件事。** `JceBodyHandle.idx` 打包了 slot 与
generation，`jce_body_valid()` 只拒绝 `UINT32_MAX` ⇒ **slot 0 / gen 0 的 body 其
idx 就是 0**，与 memset 默认值逐位相同。所以 `rt_spawn_entity_body` 在做任何判断
**之前**先盖上 `JCE_BODY_INVALID`：此后 0 是一个 body，`UINT32_MAX` 是「spawn 看过了
但没造」，只有运行时从没访问过的组件仍然是 0——而那一个确实就是「编辑器里未激活」。

Inspector 那行的判据也从 `!= 0` 改成了 `jce_body_valid()`。
**再有第三个地方要问「这个组件有 body 吗」，用 `jce_body_valid()`，不要判 0。**

## 2026-09-20 — 每脚本开销：`measured` 不是 `last_ms != 0`

脚本子系统此前是**唯一零插桩的中间件**（`jce_script.c` / `jce_script_vm.c` /
`jce_rt_script.c` 里 `JCE_PROFILE_ZONE` 各 0 个），而 `on_update` 的循环
（`jce_runtime.c` 里）本身就在 `rt_tick_gameplay` **内部**——所以它和触发器重叠、
生成密度、GAS 复制、布娃娃混合、武器计时**共用 `gameplay` 这一个桶**。
「一个脚本吃 8 ms」与「一个触发器吃 8 ms」是同一个读数。

现在两层：一个 `script` perf phase 回答「脚本到底花不花时间」，
`jce_runtime_iterate_script_costs` 回答「是哪一个」。

**三条改这段代码前必须知道的：**

1. **计量门 `jce_perf_phase_enabled()` 每帧读一次，不是每脚本读一次。**
   每脚本读它，就是把这道门存在的目的（避免开销）花在门本身上。
2. **`cost_measured` 由「计量这个动作」置位，不由数值推断。**
   关掉计量时每行都是 0.0，而一个真的不花时间的脚本也是 0.0——
   两个状态不许共用一个值。这棵树已经为「稠密表里的 0 到底是快还是没跑」付过一次账。
3. **读与重置是两个调用。** 一次会重置的读**无法被调用两次**：面板按自己的刷新率轮询，
   第二个读者（测试、日志行、automation 工具）会静默拿到面板已经消费掉的那个窗口。
   `jce_perf_phase_report()` 正是这个历史，`peek_frame()` 是后来补上的；这里从那个结局起步。

**判据是排名不是量级。** 绝对毫秒是这台机器的属性（而它被记录为不稳定）；
「忙脚本比空脚本贵」是这套度量的属性。一个在总量上正确、但把开销记到错误行上的
profiler 会在排名上红，而一台慢机器不会。


## 2026-09-20 — 二十个可增长注册表里**有一个没有 free**，而它是我上一单加的

找地方释放曲线缓存时顺手扫了 `RT_GROW_FN` 的全表：**20 个里 19 个在
`jce_runtime_destroy` 里有 `jce_free`，缺的那个是 `rt->joints`**——
`a1a245ee` 随 joint motor 一起发的泄漏。游戏里看不见（一个 runtime），
**在建几十个 runtime 的测试二进制里不是看不见**。

**要点不是这个泄漏，是扫表这个动作。** 「我刚好绊到的那一个」不是一个站得住的边界；
`RT_GROW_FN` **是一张表**，而表正是检查器读得了的东西。20 里差 1
恰好是那种「逐个改动评审都过得去、只有从表上才看得见」的密度。

**缓存的释放放在填它的那个文件里**（`rt_curve_cache_clear` 在 `jce_rt_script.c`）：
一是 `jce_runtime.c` 因此只多 2 行而不是 6 行（它 5460 行，上限 3000，
`check_file_size.py` 会为此拦你，而且拦得对），
二是**释放的那个循环和创建的那个循环不会再对「NULL 条目是什么意思」产生分歧**——
这里 NULL 是**记住了的失败**，不是空槽。

`jce.curve_eval` 的 host 回调形状是 `fallible_out`：
**「曲线求值为 0」和「曲线不存在」不能是同一个读数**。这是 `get_param`
和 profiler 的 `measured` 之后**同一课的第三次**。

## 2026-09-21 — 项目**名字**是人看的，CMake 目标和 C 函数名是**标识符**

`jce_project_create_from_template()` 把同一个 `@JCE_PROJECT_NAME@`
同时塞进三个位置：`project(... C)`、`add_executable(...)`、
以及 `static JceAppDesc ..._get_desc`。而编辑器新建项目对话框
**唯一的校验是 `strlen(project_name) > 0`**（`jce_dialog_project.cpp:583`）。

叫一个项目 "My Game"，实测 cmake 3.27.9：

```
CMake Error: Could not find cmake module file: CMakeDetermineGameCompiler.cmake
No CMAKE_Game_COMPILER could be found.
```

**一个关于空格的缺陷，报出来的是「找不到 Game 语言的编译器」。**
连字符更安静也更糟：`my-game_get_desc` 是**合法的减法表达式**，
失败从 configure 期挪到编译期，变成一堆「未声明的标识符」。

**Automation 层的 `project.create` 多年前就算了一个消毒后的 target，
并在注释里写明这是从一次失败里学到的。两个脚手架，只有一个知道。**
⇒ 规则落在**引擎**里（`project_target_from_name()`），两条路径一起拿到。

三件必须成对的事：

1. **模板要两个 token。** `@JCE_PROJECT_TARGET@` 进标识符位置，
   `@JCE_PROJECT_NAME@` 留给人看的文本（`message(STATUS ...)`、`d.name`）。
   只改一个 = 发布一个仍然坏掉的一半。
2. **manifest 要和它刚写下的文件一致。** `jce_project_new()` 把
   `target_name` 默认成 `name`——对手写项目是对的，对生成项目是错的：
   CMakeLists 上写的是消毒后的名字，manifest 写原名，
   构建就会去找一个 CMake 从没产出过的可执行文件。
3. **替换逻辑当时有两份。** `jce_project_reset_main_c()` 里手抄了一份。
   只修 walk 那条路径，「重置 main.c」这个**修复入口**会把缺陷装回去。
   已折成 `subst_mem()`，两处共用。

判据：`tests/application/test_jce_project_template_name.c`，**双向断言**——
标识符被消毒，**并且**显示名保留空格。只断言前者的话，
一个把所有名字都压成 `JceGame` 的消毒器可以全绿。

### 补（2026-09-21 同日）：把规则放进引擎的那个提交，自己制造了一次漂移

`project_target_from_name()` 是为了「两个脚手架共用一条规则」而写的，
**而我写的时候没有去比对 Automation 层里已经存在几个月的那条**：

```
"My_Game"   ->  MyGame   (project.create)     My_Game  (引擎)
"2048"      ->  2048     (project.create)     _2048    (引擎)
```

Python 抹掉 `_` 且没有首位数字防护；引擎保留 `_` 并补前缀。
**修「缺一条规则」的那次改动，引入了「两条规则不一致」。**

**两边都不会产生坏掉的工程**——`project.create` 的模板用固定的 `app_get_desc`，
没有 C 标识符暴露面，而 `2048` 是合法的 CMake 目标名。
⇒ **没有门会红、没有构建会失败、两个答案各自都站得住**，
这正是这种漂移能活下来的原因：只有在有人**去比**的时候它才存在。

定案：**保留 `_`**（在 CMake 目标和 C 标识符里都合法，抹掉它只是无谓地改写作者写的字），
Python 向引擎对齐。

**判据只能是一份跨语言共用的语料**（两条规则不可能共享代码，所以共享**测试**）：
同样六个名字、同样的期望目标，同时出现在
`test_project_target_matches_the_engine_rule`（Python）和
`tests/application/test_jce_project_template_name.c`（引擎）。
每个用例钉一条子句——普通名（**阳性对照**）/空格/下划线/连字符/首位数字/纯标点。

**而 Python 那一半还断言「这些名字确实在 C 那个文件里」。**
一份由两个文件钉住的差分，只在**两个文件都还装着同样的用例**时才算钉住；
一个只是**声称**对面覆盖了同样范围的测试，什么都没钉。
它在第一次运行就红了（`missing: ['My_Game']`），因为我先写了 Python 那半。

⇒ **一条一致性断言只在它红的那一刻证明自己。** 下次应当**故意**：
先加断言、看它红、再去满足它——和变异对照是同一条纪律，只是对象从行为换成了一致性。

**刻意没有对齐的**：`exe` 字段。两边各自**内部自洽**
（各自的 manifest 写的就是各自 CMakeLists 产出的那个文件名），
所以那是命名风格而不是漂移，选哪个是产品决定。写下来是为了让下一个人知道它不是被漏掉的。

## 2026-09-21 — 两个断裂阈值必须**各自一个 `if`**

`rt_cfg_joint_breaks` 里 `break_force` 与 `break_torque` 分成两个独立的
guarded block，不是风格选择：

```c
if (ce->break_force  > 0.0f) { ... if (imp > ce->break_force * cs->dt) return true; }
if (ce->break_torque > 0.0f) { ... if (tq  > ce->break_torque)         return true; }
```

把它们合进一个 `if` 会让**未设置的 `break_force` 悄悄关掉 `break_torque`**。
场景解析器对两者的默认值都是 **1e30**（不是 0），所以嵌套版本在默认场景里
**仍然会进去**——于是这个回归**在默认配置下看不出来**。
它只在作者显式写了 `breakForce: 0`（「这根关节拧断，但拉不断」）时才暴露。

⇒ 所以 `test_jce_joint_break_torque.c` 的断裂用例把 `breakForce` **显式写成 0**。
这不是装饰，它就是那条断言。

**单位不靠记忆。** 本包只发 Bullet 头、没有求解器源码，
「求解器存之前有没有除以 timestep」猜错就是 60 倍偏差，
而症状与「`break_torque` 还是什么都没做」**无法区分**。
所以 case 1 在**已知**的施加力矩下直接读数并做 [0.25x, 4x] 夹逼——
两个候选答案相差 60 倍，这个夹逼分得开，且不假装有 Bullet 软 6DOF 求解器
没有的精度。
