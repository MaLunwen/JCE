# examples/snake_seven — 七种脚本语言各驱动一个模块的贪吃蛇

这是一个**下游消费方**，不是产品。它存在的理由只有一个：
**把七种脚本语言同时放进一个真正在跑的游戏里**，所以任何一种语言悄悄不工作
都会立刻表现为游戏缺了一块，而不是表现为一份没人读的报告。

它照这个规格做：`.docs/way/贪吃蛇提示词.txt`（该目录 gitignored，
clean clone 上不存在）。

## 一种语言一个模块

| 模块 | 语言 | 文件 |
| --- | --- | --- |
| Snake（时钟 / 速度曲线 / 蛇头） | Lua | `resources/scripts/snake.lua` |
| Input（读键 / 禁反向 / 每 tick 一次转向） | JavaScript | `resources/scripts/input.jcejs` |
| Food（空闲格枚举 / 吃 / 增长） | Python | `resources/scripts/food.py` |
| GameBoard（30×20 边界） | Java | `resources/scripts/GameBoard.java` |
| SnakeBody（历史 / 跟随 / 自撞） | C++ | `native/snake_body.cpp` |
| GameManager（状态机 / 分数 / 最高分） | C | `native/snake_manager.c` |
| Hud（SCORE / BEST / 状态文字） | C# | `csharp/Hud.cs` |

七个模块**互不调用**，只通过实体 position 交换状态。
契约在 [`BLACKBOARD.md`](BLACKBOARD.md)，**一个槽位只有一个写者**。

## 动手之前必须知道的四件事

1. **`resources/scenes/snake.scene.json` 是生成的。** 改 `tools/gen_scene.py`
   然后重跑它，不要手改场景——棋盘是 30×20、身体池有 64 个近乎相同的实体，
   手维护的场景正是「一处写 30×20、另一处写 17×17」的来源。

2. **`fontPath` 是空的，这是故意的。** 空路径 = 系统 UI 字体。
   **不要**填 `fonts/JCE.ttf`：那是 owner 手写的字形，是美术素材不是 UI 字体。

3. **改了 `.cs` 要让 CMake 去 build**，别手跑 `dotnet build`：
   输出路径被钉在 `${CMAKE_BINARY_DIR}/csharp`，而 MSBuild 的默认输出路径
   跟着 `Platform` 走。两者分叉过一次，结果是**构建成功、打包成功、游戏跑起来，
   加载的却是旧程序集**。

4. **改了脚本必须先 cook 再 build。** 资产在嵌进 exe 的 PAK 里，
   `jce.py build-project` 不会替你 cook：

   ```bash
   python scripts/jce.py cook          examples/snake_seven
   python scripts/jce.py build-project examples/snake_seven
   ```

## 编辑器 Play 要的东西和出货 exe 不一样

出货 exe 把两个原生模块**链进自己**（`JCE_SCRIPT_MODULE_NO_ENTRY`），
C# 程序集用编译期绝对路径。**编辑器是另一个进程**，两样都拿不到，
所以同一个场景在编辑器里会少几种语言——而且不报错，只是那个模块不生效。

编辑器靠 `jce_project.json` 的两个数组：

| 字段 | 谁消费 | 这个工程里指向 |
| --- | --- | --- |
| `script_modules` | 编辑器 dlopen 原生模块 | `native/build/snake_{c,cpp}.dll` |
| `script_assemblies` | 编辑器交给 .NET 宿主 | `build/win32-x86_64/csharp/SnakeScripts.dll` |

同样的源码建两次（两种产物，不是冗余），但**不用你动手**——
`jce.py build-project` 会同时产出 exe 和 `native/build/snake_{c,cpp}.dll`。

> 这一步曾经要手动跑一次 `cmake --build native/build`。
> **一个只影响编辑器的手动步骤，是没人会发现自己漏掉的那一种**：
> 出货游戏两种情况下都跑满七种语言，编辑器则安静地少两种。

`native/CMakeLists.txt` 仍然能独立构建（不经过 SDK 消费方那条路时用得上），
两条路产出同一对文件。

**判据不是「编辑器起来了」，是两侧的 tick 序列相同。**
只数装载了几种语言不够：缺 Java 时蛇照样跑，只是永远不死——
撞墙规则就在那门缺席的语言里，实测 `head=(53,10)`，越过 29 列还在走。

## 验收

```bash
cd examples/snake_seven && python verify.py
```

逐条跑规格 §19 的 20 条，每条三种结局 **PASS / FAIL / VOID**。
**VOID 是「这条没测成」，不是「通过」**，单独计数。

判据是游戏自己每 tick 打的 `SNAKETRACE`（停摆时 `SNAKEIDLE`），
输入走 `.jirc` 回放，`JCE_FRAME_DT_FIXED` 固定时间步让两趟之间 tick 数可比。

**开环回放需要标定**：`JCE_INPUT_REPLAY` 不会告诉你现在第几帧，
所以 `calibrate()` 先在已知帧按一个键、看它落在第几 tick，再解出偏移。
标定必须按在**蛇还活着**的时候——它从中心朝右在第 15 tick 撞墙。

## 编辑器 ↔ 运行时一致性

```bash
JCE_LOG_AMBIENT=1 python examples/snake_seven/parity.py          # 全部场景
JCE_LOG_AMBIENT=1 python examples/snake_seven/parity.py straight # 只跑名字含 straight 的
```

把**同一段录制输入**分别喂给出货 exe 和编辑器 Play，比五样东西：

- **逻辑**：每 tick 的 trace（state / score / len / head / food / 判决 / body），逐 tick 比；
- **后端**：两边实际到达的图形后端（`jce_renderer: renderer: ...`）必须相同；
- **TAA**：引擎自己打出的 `taa resolve: on|off`，两边必须相同；
- **环境光**：引擎自己打出的「我把 ambient 解析成什么」那一行，两边必须逐字相同；
- **像素**：两侧在**同一模拟时刻**各拍一张 1280×720，逐像素比。
  编辑器那张是 **Game View 面板本身**（`JCE_KPI_GAME_SHOTS` 在编辑器里也认，
  默认就是 1280×720），不是带着面板边框的整个编辑器窗口。

**为什么要 `JCE_LOG_AMBIENT=1`：** 那一行在平时是 `LOG_INFO`，而 `JCE_DIST`
把 `LOG_INFO` 编译掉、`JCESDKHelpers.cmake` 给**每个 SDK 消费者 exe** 加
`JCE_DIST=1` —— 不设这个变量的话，它在编辑器里读得到、在游戏里是哑的，
**恰好哑在这条比较最需要它开口的那一半**。不设就拿到一条 VOID，不是一条 PASS。

### 为什么 TAA 是一条单独的判据

因为它曾经是 Play 与出货之间**单一最大**的差异，而它在像素计数里没有名字。

**2026-09-22 之前**：`jce_scene_renderer_taa_begin_frame()` 全树只有一个调用者
（编辑器 Scene view），出货 main loop 从不调 ⇒ **没有出货构建会 resolve TAA**，
而 Game View 会。现在运行时也按 `jce_scene_renderer.h` 的契约驱动它了，
两边都读 `r.taa`，这一条判据现在读 `both on`。

**一方沉默读作 `off (no post chain)`，不是 VOID**：
沉默意味着那一侧压根没跑 post 链，而 TAA 就 resolve 在那条链里，所以那是一个真正的答案。
**只有两边都沉默才是 VOID**（那意味着仪器根本没进二进制，这趟量的是仪器）。

### jitter 相位也要钉，理由和钉后端一样

TAA 两边都开之后，>1 像素反而从 4,378 涨到 **68,592**。**不是回归**：
Halton 索引对**帧号**确定，而截图时刻的帧号在两个 host 上不同
（编辑器加载帧数不定、第 30 帧才进 Play）。
`jce_taa.c` 自己的注释写着这件事，并说 `tools/visual_diff` 也设 `JCE_TAA_JITTER_PHASE`。

钉住相位后：**68,592 → 1,285**，最大通道差 **105 → 64**。

> 这**不是把判据调松**：它移除的是一个已知变量，
> 而它原本掩盖的那个问题（每个 host 到底 resolve 没有 TAA）**有单独一条判据在管**。

### 结构性差异的完整轨迹（>1 像素）

| 状态 | >1 像素 | 最大通道差 |
| --- | --- | --- |
| 编辑器 OpenGL vs 游戏 D3D12 | 57,337 | 105 |
| 同后端，TAA 只有一边开 | 18,534 | 105 |
| 同后端，两边都关 | 4,378 | 86 |
| **同后端，两边都开 + 钉相位** | **891–1,316**（六个场景） | **64** |

剩下的是一个 **±1 人群**：两个 host 在截图前累积的帧数不同，TAA 历史深度不一样。

### 噪声地板是每跑重测的，不是某个人定下的常数

每个场景多跑一趟**游戏自己**，判据是
「编辑器与游戏的差异 ≤ 游戏与它自己的差异」。
没有它的时候，「485,147 个像素不同」是一个没有参系的数字，
唯一可读的意思是「编辑器渲染得不一样」—— 而实测地板是 **0**。

> 实测：同一个二进制跑两遍，**0 / 921,600**，最大通道差 **0**；
> 截图时刻**差一帧、差两帧**，还是 **0**。
> 所以「两边 TAA jitter 相位不同，±1 是地板」这个听起来无懈可击的解释
> **是错的**，而按它定一个±1 容差会把整件事判成绿的。

### 第一个场景是静止帧，这不是凑数

`menu-static` 全程不按键，停在 MENU，画面不动。
于是**截图时刻不再重要**，比的纯粹是「编辑器画得跟出货 exe 一样吗」。
其余场景都把这个问题和「两边的钟对齐了吗」混在一起，
**而这两种失败在一个像素计数里长得一模一样**。

### 两个 host 读 `JCE_KPI_GAME_SHOTS` 用的不是同一个钟

游戏没有 Play 这个概念，它的排程按**进程时间**走；
编辑器把排程 tick 给了一个 **play state**，所以它的钟**只在 Play 跑着时前进**，
而 Play 从编辑器第 30 帧开始（`JCE_KPI_AUTOPLAY`）。
同一个 `"2.5"` 在两边指的是**同一局游戏里相差 30 帧的两个时刻**。

**这个偏差在逻辑比较里是不可见的**：tick **序列**完全相同，
而序列就是 `ticks()` 比的全部。它只在像素里出现，形状是
**蛇在编辑器里多走了三格** —— 读起来完全像一个渲染差异，而它不是。
`parity.py` 现在给编辑器的截图时刻减去 `AUTOPLAY_FRAME * DT`。

**两样都要，谁也不能替谁**：trace 一致而画面不同是可能的（相机只在一边生效），
画面一致而逻辑不同也是可能的（跑到一半之前两帧看起来都一样）。

**多个场景，不是一个。** 第一版只跑「开局→撞墙」一个场景，
它会完美通过，而转向、吃、暂停、重开**全都可能各自不一致**。
现在跑五个：直冲墙 / 上转 / 下转 / 暂停恢复 / 死后重开。

`parity.py` 会**改写 `~/.jce/editor-session.json`**（编辑器打开哪个工程只记在那里，
没有命令行或环境变量能覆盖）。它先按 sha256 备份、在 `finally` 里还原、
**并复验还原后逐字节相同**——静默失败会让操作者的编辑器几天后打开一个陌生工程，
而那时没有任何线索指回这个脚本。

**像素判据是「有多少个像素不同」，不是均值。** 1280×720 上任何局部差异的均值都接近 0：
蛇错一格也就几百个像素对 921,600，平均下来什么都不是——
「两张图基本一样」就是这么被说出口的。

## 画面

截图**不入库**（`shots/` 被忽略）——一张提交在代码旁边的截图会悄悄过期：
游戏变了而图没变时没有任何东西会失败，下一个人就把一个已经不存在的构建的样子
当成当前的样子。要看就重新抓，用
`JCE_KPI_GAME_SHOTS="秒|绝对路径[;...]"`（最多 8 条，时间必须严格递增），
配合 `JCE_INPUT_REPLAY` 把游戏真的开起来：

```bash
JCE_WINDOW_HIDDEN=1 JCE_MULTI_INSTANCE=1 JCE_FRAME_DT_FIXED=0.016666 JCE_INPUT_REPLAY=<某个 .jirc> JCE_KPI_GAME_SHOTS="0.30|<绝对路径>/menu.png;2.00|<绝对路径>/play.png" <dist 下的 SnakeSeven.exe>
```

**有些缺陷只有截图能发现**：菜单那张曾经在正中间有一个孤零零的绿方块
（蛇头没和身体一起停到场外），而没有任何一行 trace 承载「菜单长什么样」。
