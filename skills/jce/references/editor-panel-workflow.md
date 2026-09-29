# JCE 编辑器面板工作流

编辑器是 C++20，且它本身就是一个 L7 消费方（和用户游戏同等地位）——
它只能经 `<jce/...>` 伞头访问引擎，`check_editor_consumer_purity.py` 在守这条。

## 0. 先问：真的需要一个新的顶层面板吗

近期的实际趋势是把新能力做成**现有 workbench 的一个 tab**，而不是新顶层窗口。
顶层面板的代价是下面整张接线表，其中 `editor/src/ui/jce_editor_layout.cpp`
（**3461 行**，全仓合并冲突最高发点）要改 6 处。

先找一个语义相近的既有面板，做成它的 tab。确实需要独立窗口时才走 §2。

## 1. 硬约束：`JceEditorPanel` 只能 APPEND

枚举在 `editor/src/ui/jce_editor_panels.h`（成员数重测：`grep '^    JCE_PANEL_' editor/src/ui/jce_editor_panels.h | grep -vc JCE_PANEL_COUNT`，2026-08-27 为 60，含一个冻结保留槽，它在位图里同样占一位），
注释自己写明：

> `APPEND ONLY before JCE_PANEL_COUNT` —— 可见性位图**按序号序列化**。

**重排 = 把每个已有用户存下来的面板开关洗牌。**
「顺手按字母整理一下」是这条规则最常见的违反方式，
而它不会报错、不会有测试红，只会让用户下次打开编辑器时布局是乱的。

## 2. 新增一个顶层面板的完整触点（在 `JCE_PANEL_WORLD_STREAMING` 上端到端追踪得出）

**这是全集，不得自行裁剪、不得把其中任何一项标注为「可选」。成本不是裁剪理由。**

| # | 文件 | 改什么 |
|---|---|---|
| 1 | `editor/src/panels/jce_panel_<name>.cpp` | 新文件；调 `jce_editor_panel_visible_ptr(JCE_PANEL_<NAME>)` |
| 2 | `editor/src/ui/jce_editor_panels.h` | 枚举**追加**一个成员 + 声明绘制函数 |
| 3 | `editor/src/ui/jce_editor_panels.cpp` | `s_visible[JCE_PANEL_<NAME>] = false;`（默认可见性） |
| 4 | `editor/src/ui/jce_editor_layout.cpp` | **6 处**，见下 |
| 5 | `editor/src/panels/jce_panel_search.cpp` | `kPanelKeys[]` 增一行（面板搜索可达性） |
| 6 | `editor/resources/assets/i18n/` 下 **15 个** locale JSON | 同一个 key，15/15 齐 |
| 7 | `editor/src/panels/guide/` 对应章节 | 用户指南里的「打开该面板」按钮 |

`editor/src/ui/jce_editor_layout.cpp` 内的 6 处：

> **2026-08-28 复核仍准确**（`world_streaming` 实测落在 897 / 959 / 1650 / 1915 / 2342 / 2903）。
>
> **数它的时候别只 grep `JCE_PANEL_<NAME>`。** 六类里只有三类含这个记号
> （① 命令函数、③ Window 菜单、⑥ 可见性判断），另外三类用的是字符串
> `"###<name>"` 或 `window.<camelCase>`。只按枚举记号数会读到 **3**，
> 然后把一条正确的条目判成过时——我 2026-08-28 就差点这么改。
> 正确做法：`grep -n '<name>\|<NAME>' editor/src/ui/jce_editor_layout.cpp`，把行读出来。
>
> 另：**这个数不是所有面板的常量**。带热键或特殊停靠的面板更多——
> 同日实测 `PROFILER` 10 处、`CONSOLE` 5 处。抄一个**同类**的既有面板，别抄计数。

1. `cmd_show_<name>_()` 命令函数
2. 命令面板表行 `{ "window.<name>", "Toggle Window: ...", "Window", cmd_show_<name>_ }`
3. Window 菜单 `panel_toggle(jce_editor_i18n("window.<name>"), JCE_PANEL_<NAME>, "###<name>")`
4. `ImGui::DockBuilderDockWindow("###<name>", right_id)` 默认停靠
5. tab 邻居表 `{ "###<name>", { "###...", "###...", NULL } }`
6. 可见性判断 + `jce_editor_panel_default_pose("<name>")` + 绘制调用

**`editor/CMakeLists.txt` 不需要改** —— 它用
`file(GLOB_RECURSE EDITOR_SOURCES CONFIGURE_DEPENDS ...)`，新 `.cpp` 自动进。

## 3. i18n：真 API 是 `jce_editor_i18n()`

```cpp
jce_editor_i18n("window.worldStreaming")
```

- 全仓 `jce_editor_i18n(` 的命中数以千计（重测：`git grep -o 'jce_editor_i18n(' -- '*.c' '*.cpp' '*.h' '*.hpp' | wc -l`）——每加一条 UI 字符串就变，别把它写进判据。
- `editor_t(` 命中 **0** 处：**这个 API 不存在**。记着它的是两份章程（`editor/src/core/` 与 `editor/src/panels/` 各自的 `AGENTS.md`，共三处提及）——章程本身是在的，被它们描述的那个函数不在。
- locale 目录下有 **15** 个 JSON：
  `de en es fr it ja ko pl pt_br ru sw tr uk zh_cn zh_tw`。
- 键必须 15/15 齐；`tools/lint/run_all.py` 里的 `i18n_audit.py`、
  `i18n_hardcoded.py`、`check_i18n_dup_values.py` 三个检查器在守。

## 4. 新增一个 ECS 组件的真实路径

**`jce_reflect` 不是主路径。** 有三份 `AGENTS.md` 把它当主路径，
但 `jce_reflect_builtin.cpp` 实际只注册了一个类型。按文档走会走错路。

真实链条：引擎侧组件注册 → `editor/src/core/` 的组件注册表 →
组件默认值 → 手写的 `draw_comp_*` 绘制函数。

**注意默认值有多份**（引擎的 JSON parse 回退是权威，编辑器另有一份）。
历史上发生过 Point Light radius 5.0 → 10.0 的静默漂移。
改默认值时把所有副本一起改，或者更好——把它们合成一处。

## 5. 「编辑器完整消费引擎」的当前真相（实测 2026-08-26）

```bash
python tools/lint/check_editor_consumption.py --report
```

| | 数值 |
|---|---|
| 从 `<jce/api.h>` 可达的 `JCE_API` 符号 | **2600**（18 个伞头） |
| 编辑器里出现过该标识符 | **903** |
| 已交代的豁免（每条带证据） | **85** |
| 未被编辑器消费 | **1612** |
| **`<jce/api.h>` 到不了的公共头** | **110 个**（其符号不在 2600 内 ⇒ 实际缺口更大） |

**「未被编辑器消费」不等于「没人用」。** 那 1612 个再分五态（下表是 2026-08-27 实测）：

| 状态 | 数量 | 含义 |
|---|---|---|
| 用户项目在用 | **0** | 默认不扫用户项目，见下 |
| 引擎内部在用 | 见下方重测命令 | 内部设施（`jce_sr_anim.c`、`jce_gltf_loader.c` 之类），活的 |
| 只有 `tests/` / `tools/` 在用 | 见下方重测命令 | 被测试覆盖，但没有任何产品到达 |
| 只经宏包装到达 | **7** | 标识符扫描看不见的一类，见下 |
| **通用面上哪里都没有** | **293** | **这才是无条件的 built-but-unwired 债** |

⇒ **债是 293，不是 1612。** 实测教训：`api_ui.h` 的 67 个里 45 个是 ck 在用
（编辑器 Game View 走的是另一套 `jce_ui_canvas_*`）；`api_animation.h` 的 62 个里
55 个有引擎内部调用者。把任一组直接读成债，会错约 **8 倍**。

报告里有全部 293 个的具名列表（`## Referenced NOWHERE` 一节）。

**这个数字被连续修正过六次：1697 → 1612 → 467 → 252 → 245 → 293。**
最后一步（245 → 293）不是又发现了什么，而是**判据的口径变了**：
用户项目退出扫描面（owner 2026-08-27），原 `ck` 桶的 94 个按「最活优先」
重新落桶——46 个同时被引擎或测试命名，只有 48 个真的掉进 NOWHERE。
**`unconsumed` 总数 1612 不变，所以棘轮一动不动**：它跟的是每个伞头的
`unconsumed`，下面所有桶都只是同一个总数的分解。
在那之前一步（252 → 245）是发现**标识符扫描看不穿宏包装**：7 个 `jce_profile_*`
只经宏到达，被报成死代码。**复核时先跑一次 `check_editor_consumption.py` 取当值，
不要抄这里的数字**——它是 2026-08-27 的实测。
每一次都不是重新测同一件事，而是问了一个之前没人问的问题：
先是「有没有带理由的豁免」，再是「ck 和引擎内部算不算」（这一问 2026-08-27 有了定论：**用户项目在用不构成编辑器消费的证据**，
所以它们退出了扫描面；机制没删，名单挪进了 `tools/lint/user_project_trees.txt`，
该文件入库且**默认无条目**，加一行就能把某棵树作为独立报告列带回来），
最后是「`tests/` 和 `tools/` 扫了没有」——最后一步发现前一版的清单
**46% 是假阳性**（215/467）。
⇒ **一个「有多少东西不存在」的计数，只和你列举过的查找地点一样可靠。**

这个门禁是**棘轮**：只在某个伞头变差时 FAIL。新增引擎能力却不接编辑器，
会被它抓住。确有理由不接（例如通过 ECS 组件声明式驱动）时，
把符号写进 `tools/lint/editor_consumption_exempt.txt`，**同一行必须写理由**——
没有理由的裸符号不算豁免。

注意该指标的口径：标识符出现 ≠ 用户可达。**`consumed` 是上界，`unconsumed` 才是硬证据。**

## 6. 体量与拆分

| 文件 | 行数 |
|---|---|
| `editor/src/ui/jce_editor_layout.cpp` | 3461 |
| `editor/src/core/jce_build_manager.cpp` | 2931 |
| `editor/src/core/jce_editor_state.cpp` | 2422 |
| `editor/src/panels/jce_panel_scene_view.cpp` | 2203 |

软上限 ~2000 行、硬上限 ~3000 行。上面四个都已越线。
**本轮政策：不拆存量，但新代码不得让它们更长**——
能放进新 TU 的就放新 TU。风格与阈值见 `references/code-style.md`。

## 7. 收尾

```bash
python tools/lint/run_all.py
cmake --build build/desktop/windows-x64 --target jce_tests
ctest --test-dir build/desktop/windows-x64 -L unit -j 8
python tools/lint/check_editor_consumption.py
```

`editor/src/panels/AGENTS.md` 等叶子契约是 **gitignored** 的——
更新它（本机 agent 会读），但**不要**把它写进 `git add` 清单，
也不要把「它已提交」写进 DONE 判据。见 `references/git-and-worktrees.md`。

## 8. 红旗

| 念头 | 现实 |
|---|---|
| 「顺手把面板枚举按字母排一下」 | 位图按序号序列化，重排会洗掉所有用户的布局 |
| 「命令面板那一处加了要多 15 个 i18n 键，先跳过」 | 清单是全集，不得裁剪 |
| 「按 `AGENTS.md` 用 `editor_t()`」 | 该 API 全仓 0 处，用 `jce_editor_i18n()` |
| 「按 `AGENTS.md` 走 `jce_reflect` 加组件」 | 它不是主路径 |
| 「新 `.cpp` 要加进 `editor/CMakeLists.txt`」 | `GLOB_RECURSE`，不用 |
| 「ctest 数目和改动前一样，说明没破坏」 | 陈旧构建目录下会读到一堆 `Not Run` |
