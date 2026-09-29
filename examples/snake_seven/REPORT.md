# 贪吃蛇 —— 按 `.docs/way/贪吃蛇提示词.txt` 重做

规格 §19 要求把【已完成实践】和【待验证假设】分开，并且明确写着
**不得把「代码已经生成」等同于「游戏已经实际运行成功」**。
本文按那个要求写：下面每一条要么附带它是怎么被跑出来的，要么被归进假设。

## 一、模块划分：一种语言一个模块

规格 §13 的逻辑结构（Snake / Food / GameBoard / GameManager / UI）叠加
"每种驱动语言负责一个核心模块"：

| 规格模块 | 语言 | 文件 | 负责 |
| --- | --- | --- | --- |
| Snake | Lua | `resources/scripts/snake.lua` | 逻辑时钟、速度曲线、蛇头移动 |
| Input | JavaScript | `resources/scripts/input.jcejs` | 读键、禁反向、每 tick 一次转向 |
| Food | Python | `resources/scripts/food.py` | 空闲格枚举、吃、加分、增长 |
| GameBoard | Java | `resources/scripts/GameBoard.java` | 30×20 边界判定 |
| SnakeBody | C++ | `native/snake_body.cpp` | 身体历史、跟随、自撞判定 |
| GameManager | C | `native/snake_manager.c` | 状态机、分数重置、最高分、重开 |
| UI | C# | `csharp/Hud.cs` | SCORE / BEST / 状态文字 |

七个模块**互不调用**。它们通过实体的 **position** 交换状态，
因为那是七套绑定表面唯一都有的东西。契约在 [`BLACKBOARD.md`](BLACKBOARD.md)，
**一个槽位只有一个写者**。

## 二、规格常量（§11 / §14）

| 项 | 规格 | 代码里的出处 |
| --- | --- | --- |
| 棋盘 | 30 × 20 | `tools/gen_scene.py` `COLS, ROWS` |
| 初始长度 | 4（蛇头 + 3 节） | `snake_manager.c` `INITIAL_LENGTH` |
| 初始间隔 | 0.18 s | `snake.lua` `STEP_INIT` |
| 最小间隔 | 0.07 s | `snake.lua` `STEP_MIN` |
| 食物分值 | 10 | `food.py` `FOOD_SCORE` |

## 三、这一轮在引擎/编辑器里改的东西

这些不是贪吃蛇的代码，是做贪吃蛇的过程中撞出来的引擎缺陷。
全部已提交（`dbff406e` 及后续），门禁与构建是它们的判据。

1. **正交相机没有尺寸可给。** 场景写 `"orthographic": true` 只解析出一个 bool，
   范围永远是 `jce_camera_create` 的 800×450 默认 —— 30×20 的棋盘只有一个点大。
   编辑器另外**拿 `fov_deg`（角度）当世界单位跨度**，于是编辑器和出货 exe
   对同一个正交场景各错各的。新增 `orthoSize`（世界单位高度，宽度跟视口比例）。
2. **编辑器的打包器一个共享库都不拷。** 带脚本的游戏打出来**起不来**：
   `nethost.dll: cannot open shared object file`，exit 127，一行输出，
   而构建打印 `package staged at: …` 并报成功。
3. **`JCEScriptEnable.cmake` 的目录 staging 嵌在文件 staging 的循环里。**
   CMake 不报错，效果是**没有 stage 文件的项目一个目录都不拷**。
4. **C# 桥把游戏程序集装进 `AssemblyLoadContext.Default`**，
   而 hostfxr 把桥本身装在隔离 ALC 里 ⟹ C# 脚本全部静默消失。
5. **UI 默认字体是 `fonts/JCE.ttf`**（owner 手写的 b41c1 字形，是美术素材）。
   空 `fontPath` 现在解析成**系统 UI 字体**。

## 四、验收

`verify.py` 逐条跑规格 §19 的 20 条。每条有**三种**结局：

- **PASS** —— 游戏做对了
- **FAIL** —— 游戏做错了
- **VOID** —— **这条没测成**，它对游戏**不构成任何证据**

VOID 单独计数，因为只有 PASS/FAIL 两种结局的检查器，会把
「我没量」印成「我量过了，没问题」。

判据来自游戏自己每 tick 输出的一行 `SNAKETRACE`（以及停摆时的 `SNAKEIDLE`），
一行里带齐判断那一 tick 所需要的全部字段，
**所以任何一条结论都不需要把两行拼起来**。

> 结果见本文件末尾「实测结果」一节 —— 那一节由 `python verify.py` 产出，
> 不手写。

## 五、可手动运行的目录

```
dist\games\SnakeSeven-release-x86_64\SnakeSeven.exe
```

双击即可，**不需要任何环境变量**。目录 48 MB，其中 exe 31 MB。

**它不是单个文件，而且不能是。** 资产确实全在 exe 里（PAK 内嵌，
`pak=yes loose=no`），但三门语言的运行时不是我们的东西，没法嵌：

| 与 exe 同级 | 谁需要 | 能不能嵌进 exe |
| --- | --- | --- |
| `jce_script_api.dll` | c / cpp 的 C ABI | 可以去掉（见下） |
| `python312.dll` + `jce_script_python/` | python | **不能** —— CPython |
| `jce_script_java.dll` + `jce_script_java_classes/` | java | **不能** —— JVM |
| `nethost.dll` + `JceScript.dll` + `.runtimeconfig.json` | csharp | **不能** —— .NET |
| `SnakeScripts.dll` | 本项目的 C# 模块 | 随 .NET |

`-DSNAKE_SINGLE_FILE=ON` 可以得到**一个 exe**，代价是只剩
**lua / js / c / cpp 四种**——被丢掉的三种正是解释器不归我们所有的三种。
这是取舍，不是缺陷。

**操作键**

| 键 | 作用 |
| --- | --- |
| 方向键 / WASD | 转向（不能直接反向） |
| Enter / Space | 开始、重开 |
| Esc / P | 暂停 / 继续 |
| Q | 从暂停或结束回主菜单 |

## 六、实测结果（`python verify.py`，2026-09-22）

```
PASS  1. game starts                            exit 0, assets mounted
  PASS  2. menu enters the game                   15 ticks after ENTER; first state=1
  PASS  3. snake moves by itself                  15 distinct head cells with no steering
  PASS 10. wall ends the game                     GAMEOVER with wall=1 at head (30, 10)
  PASS 18. no movement after death                13 idle samples after death, 1 distinct head cells [(30, 10)]
  PASS 19. body stays connected                   39 adjacent pairs across 15 ticks, all distance 1
  PASS 20. food stays in bounds                   15 ticks, food always within 0..29 x 0..19
  PASS  6. food never inside the snake            15 ticks, food never on head or a traced segment
  PASS  4. arrow keys steer correctly             UP decreased z at tick 5 (10 -> 9)
  PASS  5. cannot reverse into itself             15 ticks, the head never stepped back onto its own previous cell
  PASS  8. eating adds 10 points                  score 0 -> 10 at tick 16
  PASS  7. eating grows the snake                 len 3 -> 4
  PASS  9. food respawns after being eaten        food (23, 18) -> (12, 13)
  PASS 15. best score is recorded                 snake_best.txt holds 10 after a run that scored 10
  PASS 12. pause stops the logic                  7 idle samples at PAUSED, 1 distinct head cells [(19, 10)]
  PASS 13. resume continues the game              15 ticks total, 14 of them past the paused cell
  PASS 14. restart resets the game                after restart: score=0 len=3 (want 0 / 3)
  PASS 16. speed does not follow frame rate       8 ticks at dt=1/60 vs 4 at dt=1/120 over the same 120 frames (ratio 2.00, want ~2.0)
  PASS 11. self-collision ends the game           self verdict raised with the head at (23, 17)
  PASS 17. no runaway memory over a long run      17 samples over the run, settled RSS 550.8 MB -> 551.0 MB (+0.0%)
========================================================================
PASS 20   FAIL 0   VOID 0   of 20 criteria
```

### 【已完成实践】

上面 20 条，**每一条都是把游戏真的跑起来量出来的**，不是读代码推断的。
证据是游戏自己每 tick 打的 `SNAKETRACE` / `SNAKEIDLE` 行，输入走 `.jirc` 回放，
时间步固定（`JCE_FRAME_DT_FIXED`）所以 tick 数在两趟之间可比。

另外这些也是跑出来的，不在 20 条里：

- **七种语言全部装载**：`c cpp java js lua python csharp`（出货 exe 的装载日志）。
- **UI 用系统字体**：`ui_canvas: default UI font: C:\Windows\Fonts\segoeui.ttf`，
  以及 `ECS-UI: 1 canvas(es) presented on first render`。
- **门禁**：`jce.py lint` = **88/88，0 skipped**，`AUDIT PASSED`；
  提交之后再跑一次 `run_architecture_audit.py` 仍然 `AUDIT PASSED`。

### 【待验证假设】

诚实列出**没有**被这一轮实测覆盖的部分：

1. ~~编辑器 Play 与出货 exe 的逐帧一致性~~ —— **已测，已一致**（见下节）。

2. **只在 Windows / D3D12 上跑过。** 系统字体探测对 macOS / Linux 写了路径列表，
   **没有在那两个平台上执行过**。
3. **食物铺满棋盘（获胜）那条路径没有被触发。** `food.py` 里有「没有空闲格」的
   分支并会打印，但 600 格填满需要的时长远超验收跑的窗口。
4. **`-DSNAKE_SINGLE_FILE=ON` 这一轮没有重新构建。** 四语言单文件模式的结论
   来自 CMakeLists 的机制，不是本轮的实测。
5. **最高分跨进程持久化只验了写。** `snake_best.txt` 确实被写成 10；
   **重新启动后把它读回来**这一半没有单独构造用例（读的代码路径在
   `gm_read_best()`，但验收里每趟都会先删掉那个文件）。

## 七、画面验证（2026-09-22，真的去看了）

20 条判据全绿**之后**才第一次抓图去看，结果立刻发现一个判据看不见的缺陷：

- **菜单那张：棋盘正中间一个孤零零的绿方块。** 蛇头没有和身体一起停到场外，
  于是它压在「PRESS ENTER TO START」上，读起来像个渲染 bug。
  **没有任何一行 trace 承载「菜单长什么样」**，所以 20 条里没有一条会因此变红。
  已修（`snake.lua` 在 MENU 状态把头停到场外，开新局时把 y 钉回 0——
  不能沿用读回来的 y，那时候它正是停车高度）。

修完重抓三张，逐张看过：

| 截图 | 看到的 |
| --- | --- |
| 菜单 | `SNAKE` + `PRESS ENTER TO START / ARROWS / WASD TO STEER`，棋盘干净，SCORE/BEST 在位 |
| 进行中 | 30×20 棋盘与四面墙、**4 格蛇**（头在右、两只眼睛）、红色食物、SCORE/BEST |
| 结束 | `GAME OVER` + `SCORE 0  BEST 0  ENTER PLAY AGAIN`，蛇头正撞在右墙上 |

字体是系统 UI 字体（`segoeui.ttf`），不是 `fonts/JCE.ttf` —— 肉眼可辨，
日志也印了 `ui_canvas: default UI font: C:\Windows\Fonts\segoeui.ttf`。

棋盘左右留黑边是**正确的**：正交高度 22、16:9 ⟹ 视野宽 39.1 世界单位，
而棋盘宽 30，占 77%。换个窗口比例棋盘不会变形，因为只钉了高度。

**这一节的教训**：判据齐全不等于看过。
`SNAKETRACE` 能证明每一 tick 的状态对，证明不了**屏幕上那一帧对**。

## 八、编辑器 ↔ 运行时（2026-09-22 实测）

**同一段输入，编辑器 Play 与出货 exe 产生逐 tick 完全相同的序列**
（15 个 tick，tick/state/score/len/head/food/wall/self/body 全部相同）。

到达这个结果之前，编辑器只跑 **3/7** 种语言。四个原因，全部已修：

| 问题 | 真相 |
| --- | --- |
| c / cpp 不跑 | `jce_editor_script_modules_reload()` 挂在 **全树无人 define 的宏** `JCE_EDITOR_HAVE_SCRIPT_CPP` 下，永远编译成空实现 |
| c / cpp 仍不跑 | 只有**项目对话框**调那个 reload；**从场景反查工程**那条路接管了 root、资产、渲染设置，唯独不重载脚本模块 |
| java 不跑 | 树内 class path 指向 `<out>`，而 `build_java.py` 产物在 `<out>/classes` —— **差一层目录** |
| csharp 不跑 | 托管程序集**没有任何 manifest 字段**可声明 ⟹ 新增 v5 `script_assemblies` |
| cpp 加载了却什么也不做 | `JCE_CPP_SCRIPT_CLASS` 用**虚函数成员指针比较**判断是否 override；GCC 把它编码成 **vtable 索引**，而 override 与基类同索引 ⟹ 七个钩子全是 `nullptr`。MSVC 编码成 thunk 地址所以那边是对的 |

**最后一条是全场最贵的，因为它的表象是「两个程序不一样」而不是「编译器不一样」。**
引擎注册了模块、解析出类、构造了实例、打印了
`script: loaded 'scripts/SnakeBody.jcecpp' (cpp)` —— 然后一次也没调用它。
所有层都报成功，只有**效果**是缺的。

定位它靠的是给那个模块一个能说话的渠道：C++ 脚本面**没有 log 绑定**，
所以「跑了但什么都没找到」和「根本没跑」是同一个观察。
加了一个三态标记写进场景实体：
`(0,0,0)` on_start 没跑 / `(99,0,0)` on_start 跑了 on_update 没跑 /
`(5,t,n)` 都跑了。读出 `0,0,0`，一次排除掉我剩下的全部假设。

**判据不是「装载了几种语言」。** 修好 java 之前，蛇一路跑到 `head=(53,10)`
——直接穿过第 29 列的墙，因为**撞墙规则就在那门缺席的语言里**。
少一门后端看起来不像少一门后端，看起来像一个物理 bug。

## 九、视觉一致性:trace 一致不等于画面一致

逻辑对上之后我才去比像素,结果是**五个场景全部逻辑 PASS、全部像素 FAIL,
99.989% 的像素不同**。

然后我没有继续读数字,而是**去看那张图**:一条横贯画面的地平线,
右边缘一小片绿。那是俯视的棋盘,被从站立高度、以近乎水平的角度看着。

**真因**:`jce_editor_game_render.cpp` 里 Play 的相机**只问 VCam 系统**——

```cpp
if (play_active) {
    jce_vcam_system_evaluate(scene, frame_dt, &output, &has_vcam);
    if (has_vcam) { /* 设置位置/朝向/fov */ }
}
```

没有 VirtualCamera ⟹ **没有任何东西应用场景里那个 authored `Camera` 组件**,
`g.camera` 保持 `game_reset_camera_internal` 的默认值:
位置 (0, 1.7, 6)、看向原点、60° 透视——**一个站立视角**。
而出货游戏调的是 `jce_scene_camera_apply_primary()`,
这个调用在 `jce_default_main.inc.h` 里,**编辑器里一次都没有**。

所以两边**用两个不同的相机渲染同一个场景,而逻辑一模一样**。

### 这一条教了两件事

1. **trace 和画面互相不能替代,而我差点以为可以。** 在比像素之前,
   我已经根据「逐 tick 相同」宣布过编辑器与运行时一致——按那个证据这话是真的。
   **相机不在 trace 里。** 如果这个 harness 只比逻辑,它会永远绿着。
2. **99.989% 正是一个*正确*的比较失败时该有的样子。** 如果用「像素均值」,
   同一个暗色场景的两个角度平均下来会是一个很小、很好辩解的数——
   而蛇错一格本来就只有几百个像素对 921,600,均值会把它抹掉。
   所以判据是**有多少个像素不同**,不是平均差多少。

修法照运行时的顺序:**先应用 authored 主相机,VCam 再在它之上覆盖**。
同时补上解析失败的告警——场景没有 primary Camera 时会说出来,
而不是安静地拿一个没人授权的相机画出一幅看着还挺合理的画面。

## 十、画面一致性：从 99.989% 到 12.5%，和两个被对照打死的假设

第九节修掉相机之后，像素差异从 99.989% 降到 76.6%，然后卡在那里。
下面是它最终被拆开的过程，四层，每一层都是一个真缺陷。

### 第一层：编辑器在给每一个场景补它文件里没有的状态

`snake.scene.json` **一个字没提 ambient**。但引擎的环境光解析
（`jce_sr_draw.c`）按**场景有没有 rendering-settings 组件**分支：
有 → 用它并加 sky fill；没有 → 平坦的 `(1,1,1)×0.15`、不加 fill。

而 `jce_editor_panel_postfx_tick()` **每帧无条件跑**（不管面板开没开，
`jce_editor_layout.cpp` 的注释自己写了这件事），而它拿的是一个会
`ensure` 的 *mut* 访问器 ⇒ **编辑器打开任何场景的第一帧就把组件造了出来**，
没有任何用户动作。两个 host 于是把同一个文件走了两条分支：

```
game:   ambient from engine-default rgb=(1.0000, 1.0000, 1.0000) x0.1500 sky_fill=no
editor: ambient from override       rgb=(0.0713, 0.1029, 0.1760) x1.0000 sky_fill=yes
```

修法统一成一句：**读就不要造**。面板改成先 seed 一份副本，
和种子 `memcmp` 不同才落盘（用 `memcmp` 而不是信任 `changed` 标志：
一个写了却不报告 change 的控件，否则它的编辑会被静默丢掉）。

> 拆函数那一步不是重构：`ensure_rendering_settings()` 的**第一段**（在 guard 之前）
> 是三个**项目级**纹理开关，加载路径是**顺带**拿到它的。
> 我第一版直接删掉 ensure 调用，**把它一起删了，而且静默**。

### 第二层：两个 host 读截图时刻用的不是同一个钟

游戏的排程按**进程时间**；编辑器把排程 tick 给了一个 **play state**，
只在 Play 跑着时前进，而 Play 从第 30 帧开始。同一个 `"2.5"` 指的是
**同一局游戏里相差 30 帧的两个时刻**。

**这在逻辑比较里不可见** —— tick 序列完全相同，而序列就是 `ticks()` 比的全部。
它只在像素里出现，形状是**蛇在编辑器里多走了三格**，读起来像一个渲染差异。

### 第三层（最大的一个）：两个 host 根本不在同一个图形后端上

到这里我写下过两个听起来无懈可击的解释，**两个都被对照打死**：

| 假设 | 对照 | 结果 |
| --- | --- | --- |
| 「TAA 地板，两边 jitter 序列独立」 | 同一二进制跑两遍；截图差一帧、差两帧 | **0 / 921,600，最大通道差 0** ⇒ 根本没有地板 |
| 「时序错位」 | 换成静止帧（MENU，画面不动） | 差异**一点没少**（53.30%） |

真相是给 `jce_camera_proj` 加的一行 log-on-change：

```
game:   mode=ortho aspect=1.777778 half=(19.5556, 11.0000) near=0.1 far=100 homogeneous_ndc=0
editor: mode=ortho aspect=1.777778 half=(19.5556, 11.0000) near=0.1 far=100 homogeneous_ndc=1
```

**投影的每一个数都一样，只有 `homogeneous_ndc` 不同** —— 而它是后端属性。
回头查日志里的后端行：**游戏 Direct3D 12、编辑器 OpenGL**，
因为 `~/.jce/editor-preferences.json` 里写着 `"renderer": "OpenGL"`（本机持久化偏好），
而游戏没有这份偏好、走 auto 阶梯。

⇒ **此前每一次逐像素比较都是两套光栅器在比，而没有任何一行字说出这件事。**

### 第四层：钉住后端之后立刻冲出来的第二个缺陷

编辑器在 D3D12 上**一张图都没拍出来**。原因：`jce_editor.cpp` 把截图排程
tick 给的是**性能计数器算出来的 `dt`**，不是 `JCE_FRAME_DT_FIXED`。
D3D12 第一帧要 ~950 ms 编 shader，墙钟花掉了而模拟没有，
400 帧预算跑完、排程还没走到要求的那一秒。

而日志里那句话是 **`deterministic Game View capture enabled`** ——
**一个用自己的名字断言了它不具备的性质的特性**。
`jce_editor_play.cpp:707` 早就为 Play 接过同一个开关，注释写着
「the editor's Play was simply never wired to it」——这是同一条链上的下一个钟。已修。

### 最终读数（`python examples/snake_seven/parity.py`，6 个场景×4 条判据）

**PASS 18 / FAIL 6 / VOID 0 of 24**

| 判据 | 结果 |
| --- | --- |
| 逻辑（逐 tick trace） | **6/6 PASS** |
| 后端 | **6/6 PASS**（两边都是 Direct3D 12） |
| 环境光 | **6/6 PASS**（两边都是 `engine-default rgb=(1,1,1) ×0.15 sky_fill=no`） |
| 像素 | **0/6**，12.50%–14.34% 不同 |

像素还剩的那一点，**已经量到了机制一级**：

- **3D pass 整体偏移 (+1, −3) px**（沿墙做亚像素边缘拟合，三个采样列一致到 ±0.005 px）；
- **屏幕空间 UI（HUD）没有偏移**（“SCORE 0” 包围盒 x[28..119] 两边逐像素相同）；
- **投影已证明逐值相同**（上面那两行，钉住后端后 `homogeneous_ndc` 也相同）。

⇒ 剩下的是**编辑器 offscreen target 的 viewport 原点**，不是投影、不是相机、不是 UI 层。
这一条**没修**，它是一条带着精确幅度和已排除项的待办，不是一句「画面有点不一样」。

### harness 本身学到的三件事

1. **噪声地板要每跑重测。** 每个场景多跑一趟**游戏自己**，
   判据是「编辑器与游戏的差异 ≤ 游戏与它自己的差异」。
   没有它，「485,147 个像素不同」是一个没有参系的数字；
   **一个由看起来正确的机制论证出来的容差会把整件事判成绿的。**
2. **第一个场景是静止帧。** `menu-static` 全程不按键，截图时刻不再重要，
   比的纯粹是「画得一样吗」。其余场景把它和「钟对齐了吗」混在一起，
   **而这两种失败在一个像素计数里长得一模一样**。
3. **VOID 也要留现场。** 原来只在 FAIL 时保留图和日志，
   而 VOID（截图缺失）**恰恰是最需要日志的那一种**。

## 十一、第二轮：12.50% → 2.98%，剩下的那一点有名字了

第十节停在「3D pass 整体偏移 (+1, −3) px，指向 offscreen target 的 viewport 原点」。
**那个推论是错的**，而拆穿它的方法和前面一样：给一个不可见的量装一条日志。

### 先排除相机

把位置 / yaw / pitch 加进 `jce_camera_proj` 那一行之后，两边逐位相同：

```
pos=(0.00000, 20.00000, 0.00000) yaw=-0.000000 pitch=-1.553343
```

相机、投影、viewport 全部证明相同 ⇒ 差异在 **post 链**里。

### 真因：TAA 只在编辑器里跑

- `jce_scene_renderer_taa_begin_frame()` 是唯一能在共享 pipeline 上开 TAA 的路径，
  全树**只有一个调用者**：编辑器的 Scene view。
- 出货 main loop 还多一层：`any_effect || has_fullscreen` 为假时**整个
  `jce_postfx_apply()` 都不调** —— 而一个不 author rendering settings 的场景正是这种。
- 编辑器 Game View 却只看 `r.taa` cvar，而默认 render pipeline 写着 `taa=1`。

⇒ **Game View 在做时域抗锯齿，而玩家永远拿不到。**
实测（两边都钉在 D3D12）：`JCE_CVAR="r.taa=0"` 强制两边关掉之后，
同一张静止帧 **12.50% → 2.98%**，墙边缘 **3.003 px → 0.000 px**。

所以那个 (+1, −3) px 不是 viewport 原点，是 TAA。
**我把它读成了刚性平移，而平移的原因猜错了。**

修法是一道**奇偶性门**，不是对 TAA 的评价：Game View 的 TAA 现在挂在
「运行时能不能也 resolve」上，今天是 false，写成**带理由的具名常量**而不是删掉分支 ——
哪天有人把运行时接上，翻回来是一行。Scene view 保留 TAA：它是 authoring 视图，
不声称自己是「出货长什么样」。

并且它现在是一条**并列判据**：第一次跑就直接说出
`game off (no post chain), editor on` —— 这正是那个 12.50% 一直在说、却说不出口的话。

### 剩下的 2.98% 是什么：一条已经写在代码里的结构性差异

剩余像素里 **54% 是同一种**：游戏画 `(22,23,26)`、编辑器画纯黑 `(0,0,0)`——
即**游戏给棋盘外边缘做了抗锯齿，编辑器没有**，分布在上下两条带加一圈轮廓。

`jce_offscreen_target.c:156` 自己写着原因：
**「NO MSAA, deliberately, and the MSAA SETTINGS DO NOT REACH HERE」**——
MSAA 配的是 **backbuffer**，offscreen target 是单采样的，开 MSAA 需要额外一道 resolve。
没有 postfx 时出货游戏直接画到 backbuffer（`msaa=2`），
而编辑器 Game View **永远**画到 offscreen target。

⇒ 这一条是**已知且故意**的结构差异，不是一个被忽略的缺陷；
而且它**只对不用 postfx 的项目成立**（一旦 author 了 postfx，游戏也走 offscreen，两边就一致）。
没有把它“修”掉：给 offscreen target 开 MSAA 是一件带 resolve pass 的真工作，
不是顺手能做的。

### 最终读数

**PASS 24 / FAIL 6 / VOID 0 of 30**（六个场景 × 五条判据）

| 判据 | 结果 |
| --- | --- |
| 逻辑 | **6/6 PASS** |
| 后端 | **6/6 PASS**（Direct3D 12） |
| TAA | **6/6 PASS**（两边 `off (no post chain)`） |
| 环境光 | **6/6 PASS** |
| 像素 | **0/6**，**2.62%–3.03%**（本轮之前是 12.50%–14.34%） |

整个会话的静止帧轨迹：
**99.989% → 76.6% → 52.6% → 12.50% → 2.98%**，
每一步都是一个具名的机制，不是一个被调松的阈值。

## 十二、第三轮：把 TAA 接进运行时（上一节的结论已被推翻）

第十一节的修法是「让 Game View 不跑 TAA，因为运行时也不跑」。
owner 选了另一个方向：**把运行时接上**。于是那一节的「剩下 2.98%」不再是当前读数。

### 四个门都看不见 TAA

`jce_scene_renderer.h` 把 TAA 写成一个**调用方驱动的两点接口**（渲染器既不拥有主
颜色 pass 的 view transform，也不拥有 `jce_postfx_apply()`）。全树只有编辑器 Scene view
在驱动它。接运行时要动四处，因为 **TAA 不是 `JCE_POSTFX_*` 里的一种**，
三个独立的门都看不见它：

1. `want_bridge` —— 只开 TAA 的场景会走 direct-to-backbuffer，**那里根本没有 post 链**；
2. `taa_begin_frame` 放在颜色 pass 的 view transform 之前，把返回的 jittered proj 交给 `prepare_keep`；
3. `jce_postfx_apply` 前的 `any_effect` 门 —— 否则会**抖了却没人解**，比不开 TAA 更糟；
4. `taa_end_frame`，**无条件**调 —— 「previous」必须在 TAA 关着的帧也往前走，
   否则重新打开后的第一帧会用一个陈旧的相机做重投影。

### 然后像素变差了 —— 而这不是回归

TAA 两边都开之后，>1 像素从 4,378 涨到 **68,592**。
原因写在 `jce_taa.c` 自己的注释里：Halton 索引对**帧号**确定，
而截图时刻的帧号在两个 host 上不同（编辑器加载帧数不定、第 30 帧才进 Play）。
**两个都正确地 resolve TAA 的 host，依然会采样不同的亚像素偏移。**

`JCE_TAA_JITTER_PHASE` 就是为这个存在的（注释里写着 tools/visual_diff 也设它）。
`parity.py` 像钉后端一样钉住它。

### 结构性差异（>1 像素）的完整轨迹

| 状态 | >1 像素 | 最大通道差 |
| --- | --- | --- |
| 编辑器 OpenGL vs 游戏 D3D12 | 57,337 | 105 |
| 同后端，TAA 只有一边开 | 18,534 | 105 |
| 同后端，两边都强制关 | 4,378 | 86 |
| **同后端，两边都开 + 钉相位** | **1,285** | **64** |

⇒ **接上 TAA 后的结构一致性是整个会话里最好的**，比「两边都关掉」还好三倍。

剩下的是一个 **±1 人群**：两个 host 在截图前累积的**帧数不同**，
所以 TAA 历史的深度不一样。像素那一行因此仍然对着 game-vs-game 的0 报 FAIL——
**没有去改它**：真正动了的那个数字是「两张图在结构上差多少」。

> 前一节写的「剩下的 ~3% 是 MSAA」仍然成立，但它描述的是
> **TAA 关掉时**的残余。TAA 开着时两边都走 offscreen，这一条自己就消失了 ——
> 正如那一节自己预言的：「一旦 author 了 postfx，游戏也走 offscreen」。
