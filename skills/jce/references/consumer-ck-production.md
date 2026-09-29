# 主消费方 ck 的生产上下文（时间窗与代码边界）

> **这一份是约束，不是需求来源。**
>
> SKILL.md 的第一句仍然成立：**首位永远是通用引擎与编辑器**。ck 的需要
> **不构成**改引擎 API、放宽审计门、或往 skill 里写项目特例的理由。
> 这一份存在的唯一理由是两件**只有知道才不会做错**的事：
>
> 1. **时间窗** —— ck 有一份硬排期，落在引擎上的改动有「早做省事 / 晚做返工」之分；
> 2. **代码边界** —— ck 自己的计划书划了一条 JCE↔ck 的线，而这条线上有一处
>    **已实测为假**的条目。不知道它，会以为引擎里有一个其实不存在的子系统。
>
> 反过来才对：ck 暴露出的缺口，**补在引擎的通用面上**，不带 ck 的名字。

---

## 1. ck 现在是什么形状（实测 2026-08-31, `main`, `280874c5`）

```bash
git ls-files caged_kingdom | wc -l          # 56
wc -l examples/caged_kingdom/src/game/*.c            # 2,828 行，13 个源文件
```

| 目录 | 内容 |
|---|---|
| `examples/caged_kingdom/src/game/` | `ck_app` · `ck_engine_smoke` · `ck_player_state` · `ck_quest_graph`（245 行）· `ck_scene_director` · `ck_trigger` |
| `examples/caged_kingdom/way/CK_1_0_PRODUCTION_PLAN/` | **8 份计划书，2394 行**，是首发范围的权威 |
| `examples/caged_kingdom/ui/` | RmlUI：`main_menu.rml` · `pause_menu.rml` · `loading.rml` · `ck_base.rcss` |
| `examples/caged_kingdom/resources/assets/` | 目前只有 black_hole 一组（场景 + Lua + 4 个 `.jceasset` 纹理） |
| `examples/caged_kingdom/shaders/black_hole/` | 6 个分辨率变体的 Kerr 透镜 fs + 一个 solver include |

⇒ **ck 今天基本是空的**：有框架、有玩法设计文档、有一个黑洞技术演示，
**没有首发内容**。所以判据不是「ck 现在用不用得上」，而是
**「当 ck 真的开始写玩法与量产内容时，引擎会不会在某处卡住」**。

## 2. 时间窗（`way/CK_1_0_PRODUCTION_PLAN/06_*.md`）

| 里程碑 | 日期 |
|---|---|
| 开发开始 | **2026-09-01** |
| 内容完成 | 2026-11-30 —— **此后禁止新增系统 / 区域 / Boss / 主线 / 载具类 / 武器类** |
| Beta | 2026-12-01 |
| 内容冻结 | 2026-12-15 |
| 代码冻结 | 2027-01-08 —— 此后只修 P0/P1 |
| 发布目标 | 2027-01-29 |

**对引擎工作的含义**：任何会改变玩法代码形状的引擎改动
（公共 API 形状、组件 schema、存档段划分、脚本 ABI、单机↔LAN 的会话模型）
**越晚落地，返工面越大**。2026-11-30 之后落地的引擎新系统，ck 用不上了。

首发规模（`00_*.md` 冻结）：12 主线 · 8 支线 · 4 类活动 · 2 Boss · 5 敌人原型 ·
5 武器类 · 2 载具 · 2 结局 · 6–8 小时主线。**首发不做联机**。

## 3. 代码边界（`07_*.md` §6）—— 以及它当中已实测为假的一条

ck 自己的计划书把这些列为**「通用能力属于 JCE」**：

> Renderer · ECS · Animation · Physics · Audio · UI · Input · Save ·
> World Streaming · **Mission Framework** · Asset · AI Dispatch ·
> Networking 基础（首发不启用多人）

以及**「CK 专属」**：角色数据 · 武器数值 · Wanted 规则 · 任务 · 经济 · Boss ·
地图 · 对话 · 结局 · 动态委托 Schema。**不得把 CK 任务或七魔王规则硬编码进 JCE 通用层。**

### 3a. `Mission Framework` 在引擎里不存在（2026-08-31 实测）

```bash
grep -rli "jce_mission\|jce_quest" engine/include engine/src   # 0 个文件
```

同样为 0 的还有：`jce_dialogue` · `jce_inventory` · `jce_interact` ·
`jce_objective` · `jce_waypoint` · `jce_checkpoint` · `jce_achievement`。
（`jce_dialog` 有 2 个文件命中，是 **os 层的宿主文件对话框**，不是对白系统。）

ck 自己有一份 `examples/caged_kingdom/src/game/ck_quest_graph.c`（245 行）。

⇒ **不要以为引擎里有一个任务/对白/背包子系统而去找它。** 同时注意：
这条边界表说 Mission Framework 属于 JCE，而 §6 又禁止把 CK 任务硬编码进通用层——
**这两句合起来是一个尚未落地的引擎工作项，不是一个已有设施**。
真要做，它必须做成通用的（数据驱动的图/状态机 + 事件），不带 ck 的名字。

### 3b. `05_*.md` 点名的运行时格式，引擎不产出

计划书的「源格式 → 引擎格式」表写的运行时格式里，这三个在树里**零命中**：

```bash
grep -rl "\.jce_mesh\|\.jce_tex\|\.jce_sprite" engine/src engine/include tools   # 空
```

引擎实际用的是 `.jceasset`（见 `examples/caged_kingdom/resources/assets/textures/black_hole/*.jceasset`）
与 `.scene.json` / `.prefab.json` / `.mat`。
⇒ **计划书里的格式名是规划意图，不是引擎现状。** 按它去找 cooker 会一无所获。

### 3c. 性能预算：两套基线并存，都对，口径不同

| 来源 | 目标 |
|---|---|
| `AGENTS.md` §2「设备基线」 | 单核 · **512 MB** · 无独显 · 2008/2010+ |
| ck `05_*.md` §4 | 4 核 · **8 GB** · 集显 · 720p/30 Low；进程 ≤3 GB |

**不冲突**：引擎章程是「跑得动的下限」，ck 是「一个真实游戏的最低配」。
引擎的 512 MB 基线**是真实现的、且会自动探测**（不是散文）：

```c
/* engine/src/application/jce_engine.c:1500 */
bool lm = (ram_mb > 0 && ram_mb < 2048) || cores <= 1;   /* SDL_GetSystemRAM / GetNumLogicalCPUCores */
```

配套设施：`JceMachineClass{AUTO,LOW,FULL}`（`os/core/jce_config.h`）、
`JceQualityTier{LOW,MED,HIGH,ULTRA}`（`renderer/jce_quality_preset.h`）、
`jce_alloc_low_mem_mode()`、`jce_thread_pool_default_workers() = cores-1` 钳 [1,8]。
⇒ 报告里**不要**把 512 MB 基线写成「只是文档里的一句话」——它有代码、有自动探测、有分级。

## 4. 读这一份时的纪律

- **不得**因为「ck 需要 X」就往 `engine/include/` 加一个只有 ck 会用的 API。
  缺口要补在通用面上：问「Unity/UE/Godot 在这里的通用形状是什么」，照那个做。
- **不得**因为 ck 的排期紧就放宽门禁、跳过 SDK 重装、或把绝对判据改写成相对基线。
  时间压力**从来不是**豁免理由（SKILL.md §8）。
- **不得**把 ck 的规划文档当引擎现状引用。它写的是**意图**；引擎现状只能由
  `engine/` 下的代码与命令 exit code 决定。§3a/§3b 就是两个现成的反例。
- ck 的 `way/` 与 `src/` 在 `main` 上**是被跟踪的**（56 个文件），
  与 `tests/` / `docs/` / `AGENTS.md` 不同——引用它们不需要加「不入库」注脚。
  验证：`git ls-files examples/caged_kingdom/way/CK_1_0_PRODUCTION_PLAN/07_CK_1_0_CODE_AGENT_EXECUTION_PLAN.md`

## 5. 重测这一份的命令

```bash
git ls-files caged_kingdom | wc -l
wc -l examples/caged_kingdom/src/game/*.c
grep -rli "jce_mission\|jce_quest" engine/include engine/src        # 期望：无输出
sed -n '1,20p' examples/caged_kingdom/way/CK_1_0_PRODUCTION_PLAN/06_CK_1_0_SCHEDULE_TEST_AND_RELEASE.md
```

日期与规模若与 `way/` 下的文档不符，**以文档为准并回来改这一份**——
文档是权威，这里只是索引。
