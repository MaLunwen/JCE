# 一个能力是不是活的 —— 四态判定法

> `evidence-verification.md` 管的是**测量**可不可信（截图、确定性、无头、坏仪器）。
> 这一份管的是它前面那一步：**「这个能力存在吗」到底问的是什么**。
>
> 这棵树最常复发、代价最高的错误有两个方向，**两个方向都出现过很多次**：
>
> - **假阳性**：「头文件里有 / 面板里有 / 依赖里有」⟹ 报告成「有这个能力」。
> - **假阴性**：「我 grep 了一下没有」⟹ 报告成「没有这个能力」，然后去重写一份。
>
> 契约第 3 条（能复用就绝对不要重写）挡的是第二种；`AGENTS.md` §4 的 API 闭包门
> 挡的是第一种的一部分。剩下的部分只能靠下面这套判定。

---

## 1. 四态 —— 任何能力都必须落到其中一个，并各给 `file:line`

| 态 | 含义 | 怎么测 |
|---|---|---|
| `absent` | 不存在 | 见 §2 的**三重检索**；只做过一次 grep 不许判这个态 |
| `header-only` | 有声明，无实现体 | 声明在 `engine/include/`，`engine/src/` 里找不到定义，或定义体是 `return false;` / `return true;` |
| `stub` | 有实现体，但它不做事或**假装成功** | 打开函数体读完。**`return true;` 的 stub 最危险**——它让上层显示出假数据 |
| `implemented-unwired` | 实现是真的，但**没有任何产品代码调用它** | §3 的调用点计数 |
| `wired-editor-only` | 编辑器调用，出货 exe 不调用 | 调用点全在 `editor/` 下 |
| `shipped` | 出货二进制里真的会跑 | 调用点在 `engine/src` 或消费方，且**不被构建变体编译掉**（§4） |

**报告里禁止只写「有 / 没有」。** 四态之外的措辞（"基本可用""大体齐全"）在这棵树里
已经掩盖过多次「面板显示了一个假数字」。

## 2. 判 `absent` 之前必须做的三重检索

**一次 grep 判不了不存在。** 2026-08-31 一天之内我因此误报过两次，两次都错：

| 我做的 | 我得出的（错的）结论 | 真相 |
|---|---|---|
| `ls engine/include/jce/**/*character*.h` 为空 | 「没有角色控制器」 | 它是 `jce_scene.h` 里的一个**组件**，名字里没有 character 这个词做文件名 |
| `grep ecs_query_init / ECS_SYSTEM / ecs_set_id` 全 0 | 「flecs 几乎没被当 ECS 用」 | flecs **v4** 的 API 叫 `ecs_query(` / `ecs_get(` / `ecs_set(`。真实用法：`ECS_COMPONENT_DECLARE` 108 处、`ecs_query(` 17、`ecs_has(` 70 |

所以：

```bash
# ① 概念词，不是文件名——在公共头的全文里找
grep -rn "<概念词>" engine/include --include='*.h' -i | head -20

# ② 换 2-3 个同义符号名再找一次。第三方库要先确认版本的 API 拼法
#    （flecs v4 ≠ v3；bgfx 的 C 与 C++ API 名字不同）
grep -rn "<同义词1>\|<同义词2>" engine/src --include='*.c' --include='*.h' -l

# ③ 符号级，比文本准：Serena 的 find_symbol / find_referencing_symbols 在只读白名单里
```

**判 `absent` 时必须写出你试过的符号名**，否则这条结论不可复核。

反过来也一样：`internal.h` 里的东西不算公共面，但**它存在**——
`jce_component_register()` 就在 `engine/src/middleware/scene/jce_component_registry_internal.h:56`。
把它报成「引擎没有组件注册机制」是错的；正确的说法是
**「机器已经造好，只差公开」**——这两句话对应的工作量差一个数量级。

## 3. 判 `implemented-unwired` 的标准动作

```bash
# 定义点 + 全部调用点，一次看全
grep -rn "<函数名>" engine/src engine/include editor/src caged_kingdom scripting \
     --include='*.c' --include='*.h' --include='*.cpp'
```

然后**逐行分类**：

- 定义行（`engine/src/.../x.c:NNN`）→ 不算调用者；
- 声明行（`engine/include/...`）→ 不算调用者；
- 注释里的名字 → **不算调用者**。我给自检写的第一版门就是把注释当成了调用者，
  数出 6/7 而实际是 5/7，靠算不平才发现；
- `tests/` 下的调用 → **在 `main` 上不算**（`tests/` gitignored，clean clone 里不存在）；
- 被一个没有任何构建文件定义的宏挡住的调用 → **不算**。

最后一条要单独查一次：

```bash
grep -rn "<那个宏>" --include='CMakeLists.txt' --include='*.cmake' --include='*.py' . | grep -v build/
```

## 4. 判 `shipped` 要多问一句：**哪个构建变体？**

这一步最容易漏，而它决定了「玩家手上的 exe 里有没有这个东西」。

```bash
# 这个能力被什么宏门控？那个宏由谁定义？在哪个变体下定义？
grep -rn "<宏>" CMakeLists.txt cmake/*.cmake | grep -v build/
```

**已实测的三个变体级断层**（2026-08-31）：

| 宏 / 开关 | 定义处 | 后果 |
|---|---|---|
| `JCE_DIST` | `CMakeLists.txt:1587-1590`，**只在** `JCE_BUILD_VARIANT STREQUAL "dist"` 时给 `jce_core` **PUBLIC**；`cmake/JCESDKHelpers.cmake:270` 给 SDK 消费者 exe 也上 | `jce_log.h:183-198` 把六个 `LOG_*` 全变 `((void)0)` ⟹ 1477 条引擎日志语句（在 `engine/src` 下）与 93 条 ck 日志语句在 dist 二进制里**一条都不发射**；仅 18 处直调 `jce_log_write` 的幸存 |
| `/Zi + /DEBUG` | `CMakeLists.txt:1361-1372`，只给 `Release` 配置 | dist 不产 `.pdb`，且 `grep '\.pdb' scripts/ cmake/` **零命中** ⟹ 没有任何脚本归档过符号，那份写得很好的 minidump 在出货件上配不上符号 |
| `NDEBUG` | release 与 dist 都定义 | 108 处裸 `assert()` 在任何出货配置里都不执行；全树**没有** `JCE_ASSERT` 家族，所以没有可恢复断言 |

⇒ **「dist 里还剩什么」是一个必须单独回答的问题**，不能从「release 里能跑」推出来。
反过来也别过度推论：`jce_perf_phase`（53-60 个具名桶）、`debug_draw`、cvar、debug HUD
这四样**不**受 `JCE_DIST` 或 `NDEBUG` 门控，在 dist 里照样活。

## 5. 已实测的死链登记（as_of 2026-08-31 · `main` · `280874c5`）

**这张表会过期，每行都带重测命令；判据是命令的输出，不是这张表。**
它的价值不在条目本身，而在于**形状**：一个模块可以完整、优雅、有测试，同时零调用者。

| 能力 | 态 | 重测命令 |
|---|---|---|
| 开一个网络 host（整个 net 栈 12 头 / 135 API / 8861 行） | `implemented-unwired` | `grep -rn "jce_session_start_host\|jce_net_host_create" engine/src editor/src caged_kingdom scripting --include='*.c' --include='*.cpp'` —— 期望：`middleware/net/` 之外零调用；唯一调用在 `jce_session.c:795` 的自检里，被 `JCE_NET_SELF_TEST` 挡住而无构建文件定义它 |
| ozz-animation 的采样/混合 | `absent`（只当 SIMD 数学库用） | `grep -rn "ozz::animation" engine editor --include='*.cpp' --include='*.c'` —— 期望：只有 2 处，**都在注释里**（写着 once we ship .ozz）。`api_animation.h:5` 宣称的 "Uses ozz-animation ... for sampling and blending" **不成立** |
| 音频 sidechain ducking | `implemented-unwired` | `grep -rn "jce_audio_mixer_duck_advance" engine editor caged_kingdom --include='*.c'` —— 唯一调用者在 `tests/`（main 上 gitignored） |
| 音频多普勒 | `implemented-unwired` | `grep -rn "jce_audio_voice_set_velocity" engine editor caged_kingdom --include='*.c'` —— 零调用者（该函数在 `jce_audio.c` 里有 2103 与 2288 两处定义） |
| shader 热重载（整个 `jce_shader_manager` 模块） | `implemented-unwired` | `grep -rn "jce_shader_manager_create" engine editor --include='*.c' --include='*.cpp'` —— 零调用者 |
| 出货 exe 的控制台命令 | `implemented-unwired` | `grep -rn "jce_console_register_cmd" engine editor caged_kingdom` —— 零调用者；cvar 侧是活的，但唯一 UI 是编辑器面板 |
| 资产热重载 | `implemented-unwired` | `grep -rn "jce_asset_reload" engine editor --include='*.c' --include='*.cpp'` —— 零调用者 |
| `jce_ui_settings`（1009 行） | `implemented-unwired` | `grep -rn "jce_ui_settings_" engine/src editor/src caged_kingdom --include='*.c' --include='*.cpp' \| grep -v "middleware/ui/"` |
| `jce_platform_services`（成就/统计/排行/云存档位） | `implemented-unwired` | 同上形状 |
| prefab override 序列化 | `implemented-unwired` | `grep -rn "serialize_entity_tree_json_overrides" editor/src` —— 零调用者 |
| `jce_render_graph` | 死代码 | `grep -rn "jce_render_graph_" engine/src editor/src --include='*.c' --include='*.cpp'` |
| 任务 / 对白 / 背包 / 交互 / 目标系统 | `absent` | `grep -rli "jce_mission\|jce_quest\|jce_dialogue\|jce_inventory\|jce_interact\|jce_objective" engine/include engine/src` —— 期望零输出。**注意 `jce_dialog` 有命中，那是 os 层的宿主文件对话框** |
| 每用户可写目录 | `absent` | `grep -rn "SDL_GetPrefPath" engine/src engine/include` —— 零命中；`JceUserFolder` 只有 HOME/DESKTOP/DOCUMENTS/DOWNLOADS |
| exe 图标 / 版本资源 / 安装器 | `absent` | `find . -name '*.rc' -o -name '*.ico' -o -name '*.nsi' -o -name '*.wxs' \| grep -v build/` —— 四者皆 0 |
| 子系统注册表（扩展点） | `implemented-unwired` | `grep -rn "jce_subsystem_register" engine/src engine/include editor/src caged_kingdom` —— 只有定义、声明、和一句注释（`jce_physics.h:6` 写着 "the engine core never calls this directly"）。**引擎每帧对这个空注册表跑四个生命周期点** |
| 游戏模块注册表 | `implemented-unwired` | `grep -rn "jce_game_module_register" engine/src editor/src caged_kingdom` —— 零调用者；`jce_editor_register_builtin_modules()` 已被掏空成带说明的 no-op ⇒ Game View 的模块下拉框恒为 1 项 |
| 物理查询与接触回调（**对消费方是好消息**） | `implemented-unwired` | `grep -rn "jce_physics_overlap_sphere\|jce_physics_raycast_all\|jce_physics_set_contact_begin" engine/src editor/src caged_kingdom` —— 实现完整并转发到 `jce_physics_bullet.cpp:1465/1509/1534/1601`，零调用者。**做动作游戏前先看这里，不要重写** |
| 编辑器存档/读档 UI | `absent` | `grep -rn "save_to_file\|SaveGame" editor/src` —— 零命中；而 `jce_runtime_save_to_file` 与 `jce_snapshot_save_to_file` 是同一注册表的两个公共入口 |
| 命令行出包 | `absent` | `grep -rn "JCE_HEADLESS_BUILD" scripts tools cmake CMakeLists.txt` —— 零命中。编辑器有完整无头出包能力，但没有 CLI 驱动 |
| 光照贴图的运行时消费者 | `absent` | `grep -rn "lightmap" engine/src/middleware/scene engine/src/renderer/jce_pbr_material.c \| grep -vi lightmapper` —— 零命中。编辑器面板烘的是一张平面 AO 图，且引擎里真正的 `jce_lightmapper_bake_direct` 零调用者 |

### 5b. 变体级断层的第二类：编辑器与出货填不同的字段

**同一个游戏有三条互相独立、手工复制的启动/主循环实现**，它们对同一个
`JceRuntimeDesc` 填不同的字段（2026-08-31 逐字段实测）：

| 字段 | 编辑器 Play | 出货 `default_main` | ck |
|---|---|---|---|
| `navmesh_path` | 设 | 不设 | 不设 |
| `saves_dir` | 设 | 不设 | 不设 |
| `locales_dir` / `locale` | 不设 | 设 | 不设 |
| `mixer_config_path` | 设 | 设 | 不设 |

而且**编辑器完全不调 `jce_player_loop_run_phase`** ⇒ 挂在相位上的**协程**与
**异步 prefab/场景实例化**在编辑器 Play 中不运行、在出货 exe 中运行。

⇒ 判 `shipped` 时除了问「哪个变体」，还要问「**哪条启动路径**」。
重测：`grep -n "navmesh_path\|saves_dir\|locales_dir\|mixer_config_path"
engine/include/jce/application/jce_default_main.inc.h editor/src/core/jce_editor_play.cpp
examples/caged_kingdom/src/game/*.c`

## 5c. 「门的覆盖面本身没有门」—— 9 次实测

`SKILL.md` §8 说判据是命令的 exit code。**但一道门可以 exit 0 而什么都没查。**
本仓库记录过一次（`check_editor_consumption` 只打印不失败，挂了一个月），
2026-08-31 又实测出 9 处。**没有任何东西检查一个检查器是否覆盖了它声称覆盖的东西。**

| 门 | 它实际上不会红的原因 |
|---|---|
| `scripts/jce_accept.py` 的 `stage_perf` | 没有任何数值预算，全文件 `budget`/`threshold` 零命中 ⇒ **3000 ms/帧也 PASS**；且 `jce_accept.py` 本身全仓零调用者 |
| `tools/lint/i18n_hardcoded.py:142` | `scan_dirs` 硬写成 `[panels, ui, dialogs]`，而 `core/ viewers/ gizmo/ io/` 下有 20 个文件在用 ImGui。它报 0 是因为看不见 |
| `tools/lint/check_shader_sampler_slots.py` | docstring 自述只守 **stage 5**，而 16 个槽位全部已分配并有文档 |
| `tools/lint/check_env_light_authority.py` | 只 rglob `engine/`；编辑器已直接 include `jce_time_of_day.h` 而门是绿的。对照组 `check_terrain_single_owner.py` 的 SCAN_DIRS **是**含 `editor/src` 的 ⇒ 这是漏了，不是口径差异 |
| `tools/lint/check_editor_consumption.py:292-294` | `where_used()` 只减掉**与头文件同名的 `.c`**，而模块普遍拆多文件 ⇒ **符号自己的定义行被当成调用者**。实测 NOWHERE 公布 305、实际 442（低报 31%） |
| `tools/audit/run_dedup_audit.py:176-178` | 打印「DUPLICATION REGRESSIONS … Fix it」然后 **exit 0**（只有 `--check` 才 exit 1），且全仓无人调用 |
| `tools/audit/run_architecture_audit.py:76` | 一行 `gating=False` ⇒ `check_binding_parity` 的 parity 矩阵每次都跑、永远不会红 |
| `check_cull_gen_consumers.py:83-85` / `check_instance_sort_depth.py:80-82` | 主体文件缺失时打印 SKIPPED 却 `return 0`（`run_all` 记成 PASS），而它们自己定义了 `EXIT_SKIPPED=2` |
| `scripts/jce.py lint`（jce.py:1886-1887） | 只跑 `run_all.py`，**不跑那 18 个 audit** ⇒ 统一构建驱动自己与 `CLAUDE.md` §6 的收尾流程矛盾 |

另有 **5 个已入库的 audit 检查器没有任何 runner**：
`run_dedup_audit.py` / `check_singleton_ownership.py` / `check_single_decoder.py` /
`find_duplicate_symbols.py` / `find_similar_code.py` —— 在 `scripts/`、`tools/`、
`CMakeLists.txt`、`cmake/` 里零引用。

⇒ **新写或修改一道门时，第一件事是给它做阴性对照**：
故意制造一次违规，确认它真的红。`SKILL.md` §8 的
「只验证基线是绿的等于什么都没验证」在这一节有 9 个新实例。

## 5d. 最强的活性判据：**先证明它跑过，再看它跑得好不好**（2026-09-20，两次）

四态判定问「这个能力活没活」。下面这两次说明，**当被测物就在运行中时，每一个性能/正确性指标都可能在描述一个从未执行的路径**，而读数完全正常。

**一。** L1 给车写了底盘 + 四个 `WheelCollider`，§6.4 验收全绿：零穿透、零 NaN、不翻车、0.67 s 静定。**一个轮子都没被读过。** `rt_try_spawn_vehicle` 在底盘没有 enabled 的 `JceVehicleComponent` 时 `return false`，实体退化成普通刚体——**一辆用肚皮滑行的车,落地、静定、不 NaN、不翻车**。健康读数与惰性读数**逐位相同**。

**二。** 同日，布娃娃在 0.0315 / 0.08 / 0.25 三个半径（最后一个四倍过大）上读数**完全一致**。真因是 `jce_gltf_decode_cpu` 开头 `if (!pak || !asset_path) return NULL;`——**只读 PAK、无文件系统回落，且在它自己唯一那句 `LOG_ERROR` 之前就返回**。runtime 没 pak ⟹ 模型全不加载，零日志。

⇒ **做法：在指标旁边放一面只回答「它到底执行了没有」的旗子。**

* 布娃娃那面旗子是 `published_a_pose`（运行时有没有往位姿中继写过一次）。没有它，「从没生成」与「表现完美」在所有指标上一模一样。
* 车那一面靠**消融**：把四个轮子全删掉，五个指标（穿透/静定/倾角/接触数/回弹）**一个数都不动**。
* 生成条件是串联的、且每一条都静默：布娃娃有**七个**（组件在 · 组件未禁用 · `enable` 为真 · 有 physics · 有 `SkeletalAnimator` · `skeletonPath` 非空且能加载 · 模型含 skin）。**把它们逐条作为布尔量输出**，比事后猜哪一条断了便宜得多。

**同族的第三种形状**：`visible=2 total=2 culled=0` 通过了「被测物在画面里」的检查，而那张图是**全黑的**。剔除统计数的是实体，不是像素。**两道守卫（非空白 · 含被测物）谁也替代不了谁。**

## 5e. 一条「两端都没接」的账本行，关掉一半之后**不许划掉**（2026-09-20）

`jce_bundle_pack_diff` 是公开 API、一直能用，账本上却是 `behind`——
真因不是它坏了，而是**两端都没接**：树里没有任何东西**产出** `.jdiff`，
也没有任何东西**应用** `.jdiff`。

接上产出那一半之后，最容易犯的错是把那一行划掉。不能划：
**产出一个没人能应用的补丁是半个特性**，而这一半和另一半的区别，
就是「一行真的关了」和「一行看起来关了」的区别。

做法是让**返回值自己声明哪一半没接**，而不是靠账本旁边的散文：

```python
"applying_half": "NOT wired: nothing in this tree applies a .jdiff. "
                 "Producing a patch nobody can apply is half a feature, "
                 "and this says which half.",
```

这样任何消费这个工具的东西——账本、报告、下一个会话——拿到的都是
**带着缺口说明的结果**，而不是一个看起来完整的结果。
并且给它配一条变异（把 `"NOT wired:"` 改成 `"wired:"` 必须让门变红），
否则这句话哪天被顺手删掉也没人知道。

> 同族提醒：`scene.capture` / `visual.measure` 这一类「产出一份东西」的工具，
> 判据是**产出物本身被读回来验过**，不是「命令 exit 0」。
> `bundle.diff` 的对照是「同一份目录跟自己比 ⟹ identical 且三个表全空」——
> 没有这一条，一个把什么都算成 added 的差分器照样能通过
> 「added 1 / updated 1 / removed 1」。

## 6. 度量整体规模时用这两条命令，别自己数

```bash
python tools/lint/check_editor_consumption.py --user-projects examples/caged_kingdom
# 3531 个 JCE_API 可达 api.h → consumed 1196 / exempt 113 / unconsumed 2222（NOWHERE 305）
# 每伞头的未消费数在 tools/lint/editor_consumption_baseline.json 里，最差 api_scene.h 424

python tools/lint/check_file_size.py --stats
# 1099 个一方文件 / 387,929 行；13 个超 3000 行上限
```

**这两个数字是诊断，不是门槛**（`SKILL.md` §8）。它们的用途是：
当你要判一个模块「是不是负债」时，先看它在这两张表里的位置，再决定值不值得动。

## 7. 红旗

| 念头 | 现实 |
|---|---|
| 「我 grep 了，没有」 | §2 的三重检索走完了吗？换过符号名吗？查过 `internal.h` 吗？ |
| 「头文件里写着有」 | 头文件也会说谎。`api_animation.h:5` 就在说谎，写了 68 天 |
| 「面板显示了这个数」 | stub loader 返回 success 会让面板显示假数据——HEAD 的 `280874c5` 修的就是这个 |
| 「依赖表里有这个库」 | ozz 在依赖表里，实际只用了它的 SIMD 数学头 |
| 「这一行我接上了，可以划掉」 | 接上的是哪一半？另一半在返回值里自己说了吗？§5e |
| 「这个工具报参数错，我去改参数」 | 先查孤儿进程。死进程被报成 `INVALID_ARGS` 已实测一次，参数从来没错过（`automation-api.md` §6b） |
| 「它在『不可测试』清单里，所以没测过」 | 那是工具关于自己的声明，不会自己失效。清单和实际调用集合必须**不相交**，且数要平（§6d） |
| 「release 里能跑」 | dist 里呢？§4 |
| 「测试覆盖了它」 | `tests/` 在 `main` 上 gitignored，clean clone 里不存在 |
| 「它有实现，所以能力存在」 | 零调用者的实现在产品里等于不存在。§3 |
