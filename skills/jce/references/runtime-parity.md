# JCE 编辑器视口 ↔ 运行时一致性

要求是：**编辑器游戏视口与运行时逻辑完全一致**。这在本仓库不是靠「模仿」实现的，
而是靠四条真实共享的接缝。改动落在接缝上时，**改一侧必须同时验另一侧**。

## 1. 四条共享接缝（实测）

| # | 接缝 | 证据 |
|---|---|---|
| 1 | **编辑器自己就是一个 `JceApp`** | `editor/src/jce_editor_main.cpp` 用 `JCE_MAIN(editor_app_get_desc)` —— 与任何用户游戏同一入口形态 |
| 2 | **Play 与出货共用同一个 `JceRuntime`** | `editor/src/core/jce_editor_play.cpp` 调 `jce_runtime_create()`；实现在 `engine/src/application/jce_runtime.c` |
| 3 | **共用同一个场景渲染器与相机** | `jce_camera_third_person_follow` 同时被 `editor/src/scene/jce_editor_game_render.cpp` 与 `engine/src/renderer/jce_camera.c` 这条链使用 |
| 4 | **共用同一套输入提交契约** | `jce_runtime_set_pointer_input` / `_set_touch_input` / `_set_actions`，两侧都走 `engine/src/application/jce_runtime.c` |

⇒ **不要在编辑器里重写一份游戏逻辑。** 需要新行为时，加进 runtime，两侧同时得到它。

## 2. 已知一致性缺口（源码自己承认的）

- **world streaming 有两份实现。** `editor/src/core/jce_editor_play.cpp` 的注释
  写明「engine gap — `jce_runtime_step` has no streaming，所以 Play harness 自己养一个」。
  ⇒ 任何与流式加载有关的行为，**编辑器里对不代表出货 exe 里对**，反之亦然。
- **资源解析路径不同。** 编辑器 Play 会做多根探测（项目目录、松散树、资产库），
  出货 exe 挂的是**内嵌 PAK ＋ exe 同级的 cooked 目录**（`JceProject.cooked_assets`，缺省 `resources/_cooked`），且是 **loose 覆盖 PAK** 的优先级（`jce_default_mount_runtime_assets`）；编辑器 Play 走的是多根探测。两侧解析都不同。

  这条在**两个方向**上都会害人：排查「打包后加载失败」时会漏掉旁边那棵 loose 树；反过来，开发机上 exe 旁恰好留着 `_cooked` 时 loose 会盖过 PAK，得到「我这台机器上打包版是对的」的假绿。**验收单文件 exe 前先确认 exe 旁没有 `_cooked` 残留**，否则你测的是 loose 不是 PAK。
  单文件验收判据见 `references/user-project-sdk.md` §5。

- **`JceRuntimeDesc` 的字段两侧填得不一样**（2026-08-31 实测，已上门禁）。
  两条路径各自手写一份 desc：
  `engine/include/jce/application/jce_default_main.inc.h`（出货）与
  `editor/src/core/jce_editor_play.cpp`（Play）。**18 个字段里 11 个只在一侧被设。**
  最贵的三类：

  | 字段 | 当时的状况 |
  |---|---|
  | `navmesh_path` · `saves_dir` | 只有 Play 设 ⇒ 出货游戏**没有导航网格**、**存档无处可写**，而同一个场景在 Play 里寻路正常 |
  | `fixed_timestep` · `gravity` · `gravity2d` · `solver_iterations` · `sleep_threshold` · `disable_auto_physics` · `max_frame_dt` | 只有 Play 设，且**七个全部来自项目设置** ⇒ 出货游戏用运行时内置默认值，**忽略设计师调的每一个物理与时间值**。出货的那个包，手感不是调它时的那个手感 |
  | `locale` · `locales_dir` | 只有出货设 ⇒ 设计师**无法在 Play 里预览本地化后的游戏文本**（这一条今天仍是缺口，带理由记在豁免文件里） |

  根因不是"漏写几行"：**`JceProjectSettings` 是编辑器私有类型**
  （`editor/src/core/jce_project_settings.h`），`grep -rn ProjectSettings engine/` 为空
  ⇒ 出货运行时**结构上看不到那些值**。修法是让出货侧读**编辑器写出的那个文件**
  （每个用户项目下由编辑器生成的 `project-settings.json`，在该项目的 `.jce/` 目录里；
  它是运行时产物，不在本仓库中），而不是读它写出前的那个类型。

  ⇒ **门：`python tools/lint/check_runtime_desc_parity.py`**（已在 `run_all.py` 里）。
  它有两条规则：① 任一字段只在一侧被写即 FAIL，除非在
  `tools/lint/runtime_desc_parity_exempt.txt` 里**带同行理由**豁免；
  ② 出货读取器问的每一个 JSON 键，编辑器写入器必须仍在写——
  单边更新一个两侧契约，比两侧都不更新更糟。
  两条规则的阴性对照都实测能红。

  **它计的是"写"不是"提及"**：第一版计任何提及，阴性对照没红——删掉
  `rd.navmesh_path = …` 之后，旁边那行 `LOG_INFO("navmesh: %s", rd.navmesh_path)`
  还在，计数不为零。*阴性对照能过的门不是门。*

发现新的缺口时，写进这一节，不要私下在一侧打补丁。

## 2b. 脚本语言的编辑器/运行时缺口（2026-09-22 实测并修）

**同一个场景文件，出货 exe 跑 7 种语言，编辑器 Play 只跑 3 种。** 三个独立原因，
都不报错，都只表现为「这个模块好像没生效」：

| 语言 | 编辑器里为什么不跑 | 修法 |
|---|---|---|
| c / cpp | `jce_editor_script_modules_reload()` **整个挂在 `JCE_EDITOR_HAVE_SCRIPT_CPP` 下，而全树没有任何地方 define 它**（5 处出现，全是 `#if defined`）⟹ 永远编译成「deliberately silent」的空实现 | 改用 `JCE_SCRIPT_LINKED_CPP`——`jce_script_enable()` 真的会把它加到 target 上 |
| c / cpp（第二层） | 只有**项目对话框**调那个 reload。从**场景**反向找到工程（`follow_scene_project_root`）会接管 root、资产基准、渲染设置，**唯独不重载脚本模块** | 在那条路径上也调一次 |
| java | 树内 `JCE_SCRIPT_JAVA_CLASS_PATH` 指向 `<out>`，而 `build_java.py` 的 docstring 明写产物在 **`<out>/classes/`** ⟹ 差一层目录 | 指到 `.../classes` |
| csharp | 托管程序集**没有任何 manifest 字段**可声明 ⟹ 编辑器无从加载 | 新增 v5 `script_assemblies`，编辑器按它加载 |

**为什么这三条能活这么久：上面那一半是好的。** VM 通过共享 shim 注册，
所以编辑器**报告** c/cpp/java 可用、扩展名也解析得到；失败在下一层，
而且措辞是「NO NATIVE SCRIPT MODULE IS LOADED IN THIS PROCESS」——
读起来像**项目忘了构建模块**，不像**编辑器根本没有加载模块的能力**。

> java 那条的欺骗性最强：报错说「Build the scripting classes with
> `scripting/java/build_java.py`」——而 class **已经**编译好了，就躺在
> 它指向的位置下面一层。**一条正确的建议，指向一个不存在的问题。**
> 出货包不受影响（SDK 的安装布局是对的），所以同一个场景
> **java 在出货 exe 里能跑、在编辑器里不能**。

### 判据：比 tick 序列，不比「跑起来了」

编辑器无头 Play：`JCE_KPI_AUTOPLAY=1`（第 30 帧进 Play）+ `JCE_WINDOW_HIDDEN=1`
+ `JCE_MAX_FRAMES` + `JCE_INPUT_REPLAY`。**工程是从
`~/.jce/editor-session.json` 的 `last_project` 打开的，没有命令行或环境变量能覆盖**
——所以改它之前先按 sha256 备份，`finally` 里还原，**并且验证还原后逐字节相同**。

两侧跑同一段 `.jirc`，比**每个 tick 的完整状态**。修好后实测
15 tick 全同（tick/state/score/len/head/food）。
**只数「装载了几种语言」不够**：缺 java 时蛇照样跑，只是永远不死
——`head=(53,10)`，越过 29 列还在走，因为撞墙规则就在那门缺席的语言里。

## 2c. 画面一致性：trace 一致**不蕴含**画面一致（2026-09-22 实测）

逐 tick 的 trace 全同、而两侧的画面 **99.989% 的像素不同**，是实测出来的组合。
原因是**相机不在 trace 里**：

| 差异 | 真相 |
|---|---|
| 相机 | `jce_editor_game_render.cpp` 的 Play 相机**只问 VCam 系统**。场景没有 VirtualCamera ⟹ 没有任何东西应用 authored `Camera` 组件，`g.camera` 保持 `game_reset_camera_internal` 的默认值（站立视角 (0,1.7,6)，60° 透视）。出货游戏调 `jce_scene_camera_apply_primary()`，**这个调用编辑器里一次都没有** |
| 清屏色 | Game View 的 offscreen target 硬编码 `0x202028FF`（编辑器灰蓝），而运行时的相机栈清成 `0x000000FF`。棋盘不铺满时，**全部不同像素里 23% 就是这一种颜色** |
| 环境光 | 编辑器**无条件**把 Lighting 面板的 ambient 作为 **override** 推给渲染器；运行时走自然路径。表现为整块地面色差几个单位（实测 `(55,60,67)` vs `(49,56,69)`） |

⇒ **两个 harness，谁也不能替谁**：
trace 一致而画面不同是可能的（相机只在一边生效），
画面一致而逻辑不同也是可能的（跑到一半之前两帧看起来都一样）。

## 2c-1. **先问两个 host 在不不同的后端上**（2026-09-22 实测，这一条排在所有像素分析之前）

本机实测：**游戏 Direct3D 12、编辑器 OpenGL**。
原因不是缺陷，是 `~/.jce/editor-preferences.json` 里有
`"renderer": "OpenGL"`（编辑器日志明写 `policy=Vulkan remembered=Auto
host-override=OpenGL`），而游戏没有这份偏好，走 auto 阶梯。

⇒ **此前每一次逐像素的编辑器↔运行时比较，都是两套光栅器在比，
而没有任何一行字说出这件事。** 实测代价：静止帧 53.30% 像素不同；
把两边都铉到 D3D12 之后降到 **12.50%**。

### 怎么拿到这个结论的（方法比结论值钱）

两个听起来无懈可击的解释先后被对照打死：
「TAA 地板」（同一二进制跑两遍 = **0 / 921,600**，截图差一帧、差两帧还是 0）、
「时序错位」（换成静止帧，差异一点没少）。
最后是给 `jce_camera_proj` 加的一行 log-on-change：

```
game:   mode=ortho aspect=1.777778 half=(19.5556, 11.0000) near=0.1 far=100 homogeneous_ndc=0
editor: mode=ortho aspect=1.777778 half=(19.5556, 11.0000) near=0.1 far=100 homogeneous_ndc=1
```

**投影的每一个数都一样，只有 `homogeneous_ndc` 不同** ——
而它是后端属性（GL 是 [-1,1]，D3D/Metal 是 [0,1]）。
一个差异要看它的**输入**，不要看它的输出。

### 写进 harness 的两件事（只做一件不够）

1. 用 `JCE_BACKEND` **铉住两边**（`jce_renderer.c:875`，接受
   `auto/d3d11/d3d12/vulkan/opengl/gles/metal/noop`）；
2. 并把「两边**实际到达**的后端」升成一条并列判据，
   从各自日志里解析 `jce_renderer: renderer: <名字>`。
   **只铉不验，等于把一个可能失败的假设写成了前提。**

## 2c-2. 「deterministic Game View capture」的钟用的是墙钟（2026-09-22 实测并修）

`jce_editor.cpp` 把截图排程 tick 给的是**性能计数器算出来的 `dt`**，
不是 `JCE_FRAME_DT_FIXED`。后果：同一个编辑器、同一个场景、同样 400 帧预算，
在 OpenGL 上拍到了、在 **D3D12 上一张都没拍到** ——
D3D12 第一帧要 ~950 ms 编 shader，墙钟花掉了而模拟没有，
排程永远走不到要求的那一秒。

而日志里那句话是 **`deterministic Game View capture enabled`** ——
**一个用自己的名字断言了它不具备的性质的特性**。
`jce_editor_play.cpp:707` 早就为 Play 接过同一个开关，注释写着
「the editor's Play was simply never wired to it」——
**这是同一条链上的下一个钟**，已修。

> 教训：一个说自己 deterministic 的东西，它的每一个钟都要数一遍。
> 接了 Play 不等于接了截图，而没接的那一个只会在**别的机器、别的后端上**发作。

## 2c-3. TAA 只在编辑器里跑（2026-09-22 实测并修）

两个 enable 点，**两个都在编辑器侧**：

```bash
grep -rn "jce_postfx_set_taa(" --include='*.c' --include='*.cpp' .   # 排除 dist/
grep -rn "taa_begin_frame" --include='*.c' --include='*.cpp' .
```

- `jce_scene_renderer_taa_begin_frame()` 是唯一能在**共享** pipeline 上开 TAA 的路径，
  全树只有一个调用者：编辑器的 **Scene view**。
- 编辑器 **Game View** 在自己的 `g.postfx` 上直接开，只看 `r.taa` cvar。
- 出货 main loop 还多一层 gate：`any_effect || has_fullscreen` 为假时
  **整个 `jce_postfx_apply()` 都不调**——而一个不 author rendering settings 的场景正是这种。

⇒ 当时 **没有任何出货构建会 resolve TAA，而 Game View 会。**

> **2026-09-22 已接：**`jce_default_main.inc.h` 现在按契约驱动 TAA。
> 要动四处，因为 **TAA 不是 `JCE_POSTFX_*` 里的一种**，三个独立的门看不见它：
> `want_bridge`（只开 TAA 的场景会走 direct-to-backbuffer，那里没有 post 链）、
> `taa_begin_frame` 放在颜色 pass 的 view transform 之前、
> `jce_postfx_apply` 前的 `any_effect` 门（否则**抖了却没人解**，比不开更糟）、
> 以及**无条件**调用的 `taa_end_frame`。
>
> 然后像素反而变差（>1 像素 4,378 → 68,592），**不是回归**：
> Halton 索引对**帧号**确定，而两个 host 在截图时刻的帧号不同
> （编辑器加载帧数不定、第 30 帧才进 Play）。
> `JCE_TAA_JITTER_PHASE` 就是为此而存（`jce_taa.c` 的注释里写着
> `tools/visual_diff.py` 也设它）；钉住后 **68,592 → 1,285**，
> 最大通道差 **105 → 64** —— 整个会话里最好的结构一致性，
> 比「两边都关掉」还好三倍。
>
> 剩下的是一个 **±1 人群**：两个 host 在截图前累积的帧数不同，
> TAA 历史的深度不一样。

> **2026-09-22 已接：** 现在按契约驱动 TAA。
> 要动四处，因为 **TAA 不是  里的一种**，三个独立的门看不见它：
> （只开 TAA 的场景会走 direct-to-backbuffer，那里没有 post 链）、
>  放在颜色 pass 的 view transform 之前、
>  前的  门（否则**抖了却没人解**）、
> 以及**无条件**的 。
>
> 然后像素反而变差（>1 像素 4,378 → 68,592），**不是回归**：
> Halton 索引对帧号确定，而两个 host 在截图时刻的帧号不同。
>  就是为此而存（ 注释里写着 visual_diff 也设它）；
> 钉住后 **68,592 → 1,285**，最大通道差 **105 → 64** —— 整个会话里最好的结构一致性。

实测（两边都钉在 D3D12）：`JCE_CVAR="r.taa=0"` 强制两边关掉，
同一张静止帧 **12.50% → 2.98%**，墙边缘 **3.003 px → 0.000 px**。
这是 Play 与出货之间**单一最大**的差异。

> **我在这条上错了两次，两次都是因为读代码而不是量。**
> 先说「TAA 从不在出货构建里跑」——真的，但我紧接着说它解释不了观测，
> 因为我看到 Game View 也挂在 `r.taa` 上、而项目没有 `.rp.json`。
> **错在默认 render pipeline 就写着 `taa=1`**，它会反过来 `jce_cvar_set_bool(r.taa, 1)`。
> 日志里那一行 `render_pipeline: applied: ... taa=1` 一直在那里。

### 判据：让引擎说出它到底 resolve 没有

`jce_postfx_apply()` 现在在**变化时**打一行 `taa resolve: on|off`（`JCE_LOG_TAA=1` 升到 WARN）。
从 **apply()** 而不是 `set_taa()` 打，因为比较在乎的是画那一帧时生效的状态；
而且**一个从不调 `set_taa()` 的 host 会什么都不说**，在日志里和「关掉了」不可区分。

`parity.py` 把它升成并列判据，并且把「一方沉默」读作
**`off (no post chain)`而不是 VOID**——沉默是一个真正的答案；
**只有两边都沉默才是 VOID**（那意味着仪器没进二进制，这趟量的是仪器不是引擎）。

## 2c-4. 剩下的 ~3% 是 MSAA，而它已经写在代码里了

把 TAA 拉平之后，剩余像素里 **54% 是同一种**：
游戏画 `(22,23,26)`、编辑器画纯黑 `(0,0,0)` —— **游戏给外边缘做了抗锯齿，编辑器没有**。

`jce_offscreen_target.c:156` 自己写着：
**「NO MSAA, deliberately, and the MSAA SETTINGS DO NOT REACH HERE」**——
MSAA 配的是 **backbuffer**，offscreen target 单采样，开它需要额外一道 resolve。
没有 postfx 时出货游戏直接画到 backbuffer（`msaa=2`），而 Game View **永远**画到 offscreen。

⇒ 这是**已知且故意**的结构差异，并且**只对不用 postfx 的项目成立**：
一旦 author 了 postfx，游戏也走 offscreen，两边就一致了。

## 2d. 「项目设置」整类是编辑器专属（2026-09-22 实测）

`JceProjectSettings` 定义在 `editor/src/core/jce_project_settings.cpp`，
**`engine/src` 里没有任何一行读它**。它驱动的三个进程级开关：

```
jce_texture_set_colour_space        Graphics > Colour Space（Linear/Gamma）
jce_texture_set_quality_mip_bias    Quality  > Texture Quality
jce_texture_set_aniso_override      Graphics > Anisotropic Textures
```

判据（**逐个跑，不要凭印象**）：

```bash
grep -rn "jce_texture_set_colour_space" engine/src editor/src tools --include='*.c' --include='*.cpp'
```

**2026-09-22 已修，这一段的结论变了，留着是为了记住怎么找到的。**
当时三个调用点**全部**在 `editor/`（`jce_editor_scene_rendering_defaults.cpp`
与 `jce_panel_project_settings.cpp`），所以一个项目改了 Colour Space
只在编辑器里生效。现在 `jce_default_main.inc.h` 的
`s_default_apply_project_settings()` 也读它们了。

修法照搬了旁边早就存在的那一半：**读编辑器写的那个文件
（`.jce/project-settings.json`），而不是它写出来的那个类型**——
`JceProjectSettings` 是编辑器类型，运行时根本看不到。
Time/Physics 七个字段 2026-08-31 就是这么补上的，这次只是同一个函数里多一个
`graphics` / `quality` 块。

> **教训不在结论，在它为什么安静：**引擎自己的默认
> （`s_colour_space_linear = 1`）**恰好等于**编辑器的默认，
> 所以只有**改过**这个值的项目才会分岔。
> 一个只在非默认配置下才错的缺陷，在所有默认配置的测试里都是绿的。

> 它为什么一直没被发现：这三个开关改的是纹理**解释方式**，
> 表现为整幅画面的一个平移色偏 —— 和曝光、tonemap、gamma 差异**用肉眼分不开**，
> 而两边都「看起来正常」。只有把两个 host 的同一帧逐像素相减才看得见。

## 2e. 环境光解析有三个来源，而只有一个是场景状态

`jce_sr_draw.c` 按顺序取：**override（编辑器专用传输）→ 场景 rendering settings →
引擎兜底 `(1,1,1) x 0.15`（且**不**加 sky fill）**。

2026-09-22 之前，编辑器对**每一次场景加载**调
`jce_editor_scene_ensure_rendering_settings()`，它在场景**没有**该组件时
按 Project Settings **造一个**出来 —— 于是：

- 任何不 author 灯光的场景，编辑器都用一个**文件里没有、出货 exe 无从获得**的 ambient 渲染；
- `current_scene_rendering_settings()` 这个**读**访问器自己也调 ensure，
  所以连 override 这条路都在伪造。

已改：读访问器不再 ensure；三条**加载**路径不再 ensure（新建/demo 场景仍然给默认值，
那是 author 出来的、会被保存）；`jce_editor_lighting_get_ambient` 现在返回 `bool`，
没有 authored settings 时编辑器**不推 override**，两个 host 落到同一条兜底分支。

> **拆函数那一步不是重构。** `ensure_rendering_settings()` 的第一段是
> §2d 的三个项目级纹理开关，**在 guard 之前**，加载路径是**顺带**拿到它的。
> 直接删掉 ensure 调用会把它一起删掉，而且**静默**——
> 两件无关的事共用一个入口。现在是 `jce_editor_scene_apply_project_texture_state()`。

### 看得见的判据：让引擎自己说它解析成了什么

`jce_sr_draw.c` 现在在**解析值变化时**打一行：

```
scene_renderer: ambient from <override|scene|engine-default> rgb=(...) intensity=... sky_fill=<yes|no>
```

**level 由 `JCE_LOG_AMBIENT` 决定，这不是风格选择**：`JCE_DIST` 把 `LOG_INFO`
编译掉，而 `JCESDKHelpers.cmake` 给**每个 SDK 消费者 exe** 加 `JCE_DIST=1` ——
所以 INFO 那一行在编辑器里读得到、在出货游戏里是哑的，
**正好在这条比较最需要它开口的那一半上没有声音**。
两个 host 都设 `JCE_LOG_AMBIENT=1` 才拿得到。

### 怎么比像素

编辑器的 `jce_editor_kpi_game_capture` **默认就是 1280×720**，
和游戏的 `JCE_KPI_GAME_SHOTS` 一致，而且它拍的是 **Game View 面板本身**，
不是整个编辑器窗口 —— 所以**没有性能面板的时钟带要 mask**
（`tools/visual_diff.py` 要 mask 是因为它拍的是整窗）。两边直接可比。

**判据是「有多少个像素不同」，不是均值。** 1280×720 上任何局部差异的均值都接近 0：
蛇错一格只有几百个像素对 921,600。**均值会把「两个不同的游戏」读成「基本一样」。**

### `JCE_KPI_GAME_SHOTS` 的秒数在两个 host 上不是同一个钟

游戏没有 Play 这个概念，它的排程按**进程时间**走。
编辑器把排程 tick 给了一个 **play state**
（`jce_editor.cpp` → `jce_editor_kpi_game_capture_global_tick(capture_play, dt)`），
所以**编辑器的钟只在 Play running 时前进**，而 Play 从编辑器第 30 帧开始
（`JCE_KPI_AUTOPLAY`，`jce_editor.cpp:1018` 的 `++s_editor.autoplay_frame == 30u`）。

⇒ 同一个 `"3.0"` 在两边指的是**同一局游戏里相差 30 帧的两个时刻**。

**这个偏差在逻辑比较里是不可见的**：tick **序列**完全相同，而序列就是
`ticks()` 比的全部。它只在像素里出现，形状是
**蛇在编辑器里多走了三格** —— 读起来完完全全像一个渲染差异，而它不是。

实测：3 格 × 0.18 s/格 = 0.54 s ≈ 30 帧 × 0.016666 = 0.50 s（加上一两帧启动）。
`parity.py` 现在给编辑器的截图时刻减去 `AUTOPLAY_FRAME * DT`。

> **一般化：任何「按秒数取样」的跨 host 比较，先问这两个秒数是谁的秒数。**
> 这里两边读的是同一个环境变量、同一个字符串、同一份解析代码，
> **不同的只有谁在给它喂 dt**。

### 判据不是「装载了几种语言」

修好 java 之前，蛇一路跑到 `head=(53,10)` —— 直接穿过第 29 列的墙。
**撞墙规则就在那门缺席的语言里。** 少一门后端不像少一门后端，像一个物理 bug。

## 3. 验收：两侧都要跑

```bash
# ① 编辑器 Play 一侧
JCE_MULTI_INSTANCE=1 JCE_WINDOW_HIDDEN=1 JCE_FRAME_DT_FIXED=0.016666 \
JCE_DBG_FOCUS_GAME=1 JCE_DBG_AUTOPLAY=30 JCE_MAX_FRAMES=400 \
JCE_SHOT_FRAME=300 JCE_SHOT_PATH=<abs>/play.png \
  build/desktop/windows-x64/release/jce_editor.exe

# ② 出货 exe 一侧（同一场景、同一帧、同样钉死）
JCE_MULTI_INSTANCE=1 JCE_WINDOW_HIDDEN=1 JCE_FRAME_DT_FIXED=0.016666 \
JCE_MAX_FRAMES=400 JCE_CAPTURE_FRAME=300 JCE_CAPTURE_PATH=<abs>/ship.png \
  <project>/build/.../<game>.exe
```

> **`JCE_DBG_FOCUS_GAME` 只把 Game 视图前置，它不会按 Play。**
> 无头进入 Play 只有两条路：`JCE_DBG_AUTOPLAY=N`（第 N 帧按下 Play，N 必须小于 `JCE_SHOT_FRAME` 并给场景加载留余量）与 `JCE_KPI_AUTOPLAY`；
> 其余七个 `jce_state_play()` 调用点全是 ImGui 按钮或控制台命令。
> 少了它，拍到的是**停在 STOPPED 的编辑器**——runtime 没在步进，同一段里强调「必设」的 `JCE_FRAME_DT_FIXED` 也无从生效，
> 而两侧都会产出看起来很合理的 PNG。**判据：日志里必须出现 autoplay 进入 Play 的那一行**，否则这张图不是 Play 的图。


**`JCE_FRAME_DT_FIXED` 是必设的。** 编辑器 Play 曾经是墙钟驱动的：
同一构建的两次运行差 mean 31.58 / 584275 个像素超阈，而真实 A/B 差异只有 135 个像素——
**噪声是信号的 3000 倍**。现在 Play 认这个变量（`editor/src/core/jce_editor_play.cpp`），
但只有你设了它才生效。

**注意两侧用了不同的截图钩子**（编辑器 `JCE_SHOT_*` vs 出货 `JCE_CAPTURE_*`）。
两者都是 backbuffer 族，但**编辑器那张里有 ImGui**——所以两侧的图不能整幅相减。
可比的是：视口区域的裁剪、或者行为量（位置、朝向、帧时、draw 数），不是整幅像素。

**更可靠的一致性判据是行为量而不是像素**：同一输入序列下的实体位姿、
物理落点、动画状态机所处状态。这些可以用 `.jirc` 回放驱动（它驱动引擎侧输入，
见 `references/evidence-verification.md` §7）。

## 4. 改动清单

改到下列任一处时，本 skill 生效：

- `engine/src/application/jce_runtime.c` / `jce_runtime_boot.c`
- `engine/src/runtime/`（`jce_game_module.c`、`jce_player_loop.c`、`jce_coroutine.c`…）
- `engine/src/renderer/jce_camera.c`
- `editor/src/core/jce_editor_play.cpp`
- `editor/src/scene/jce_editor_game_render.cpp`
- 任何 `JceGameModule` / `JceAppDesc` 形状

## 5. 红旗

| 念头 | 现实 |
|---|---|
| 「编辑器里试出来是对的」 | 流式与资源解析两侧不同；出货 exe 只走 PAK |
| 「在编辑器里加一份实现更快」 | 那正是 streaming 双实现的来历 |
| 「Play 模式截图对比就行」 | 不设 `JCE_FRAME_DT_FIXED` 时噪声是信号的 3000 倍 |
| 「两侧截图直接相减」 | 编辑器那张含 ImGui。比区域或比行为量 |
| 「相机手感不对，先调阻尼常数」 | 先找活着的写入路径；这条链上曾有多条互相打架的写入 |
