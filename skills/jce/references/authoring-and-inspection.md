# 用 JCE 做东西（不是改 JCE）

> **先分清你在做哪件事。** 本文件只管「**用**引擎做出一个东西」；
> 「**改**引擎/编辑器源码」是 `SKILL.md` 主线与其余 reference 的事。
>
> | 你要做的 | 读哪里 |
> |---|---|
> | 设计场景、写玩法、驱动脚本、跑起来看画面 | **本文件** |
> | 改引擎 / 编辑器源码、过门禁、提交 | `SKILL.md` §1-§9 + 对应 reference |
>
> 一旦你开始改 `engine/**` 或 `editor/**` 的源码，**回 `SKILL.md` §4 的
> 「改动类型 → 必经步骤」表**——重装 SDK、接线点全集、收尾判据都在那边，
> 本文件不复述。

**所有数字与路径都是 2026-08-28 在 `main` 上实测的**，会过期；
命令都给了，读到不一样以命令为准（同 `references/repository-state.md` 的用法）。

---

## 1. 引擎能做什么：去哪读，以及一个必须知道的坑

```bash
python scripts/jce.py targets          # 能构建到哪些目标
ls engine/include/jce/api_*.h          # 按主题分的伞头
```

**伞头**：`api_ai` `api_ai_dispatch` `api_animation` `api_app` `api_audio`
`api_core` `api_graphics` `api_input` `api_middleware` `api_net` `api_physics`
`api_platform` `api_render` `api_resource` `api_runtime` `api_save` `api_scene`
`api_script` `api_streaming` `api_ui` `api_video` `api_world`。
（条数会漂，别写进判据；重数 `ls engine/include/jce/api_*.h`。）

> ✅ **`#include <jce/api.h>` 现在是全集，并且有门守着。**
> 2026-08-31 之前不是：299 个公共头里只有 188 个可达。补完后 299/301，
> 剩下 2 个在 `tools/lint/api_closure_exempt.txt` 里带理由豁免。
> 新加公共头**不进伞头就会红**：
>
> ```bash
> python tools/lint/check_api_closure.py          # 判据：exit 0
> python tools/lint/check_api_closure.py --list   # 可达清单
> ```
>
> ⚠️ **看到 `JCE_API` 不等于「这个符号在库里」。** 私有模块（如 `ai_dispatch`，
> `JCE_ENABLE_AI_DISPATCH` 默认 OFF）的头可能因为旧安装残留在 SDK 里，
> 而库里一个符号都没有——**编得过、链不上**。判据是库，不是头。

## 2. 设计场景

场景是 JSON（`.scene.json`，也接受 `.scene`）。顶层四个键：

```
name · uuid · entities · settings
```

每个实体：`name` · `uuid` · `parentUuid` · `enabled` · `components`。
`parentUuid` 建层级；`components` 是一个组件数组。

**不要照抄现有场景来猜字段名。** 组件 schema 从引擎源码里抽，权威在这里：

```bash
python tools/jce_scene_kit.py schema                 # 全部组件 + 全部字段
python tools/jce_scene_kit.py schema --type Camera   # 只看一个
python tools/jce_scene_kit.py check <scene.json>     # 判据：exit 0
```

`schema` 直接解析 `engine/src/middleware/scene/jce_scene_components_json.c`
的 `parse_*` / `ser_*`，所以它不会和引擎漂移。抄场景会漂：同一个概念在不同
组件里键名不同（BoxCollider 用 `centerX`/`sizeX`，CapsuleCollider 用
`cx`/`cy`/`cz`），而**引擎对不认识的键是静默忽略的**——拼错一个字段，场景照
样加载、照样保存，只是那一项永远不生效。`check` 会把它报成 warning。

**`schema` 会把 INERT 字段标出来，`check` 会对它们告警。** 那是本仓库自己的
名单（`tools/lint/component_field_baseline.json`）：这些字段能序列化、
Inspector 里也画得出来，但**引擎里没有任何代码读它们**。设了等于没设，而且
没有任何症状——场景加载、保存、渲染都和没写过一模一样。这条知识以前只到达
Inspector 的 unwired 徽章，现在也到达授权这一步。

```
farClip                number   INERT — the engine reads nothing here
```

看到 INERT 就别设它；真需要那个行为，是引擎侧要补的东西，不是场景能表达的。

**可抄的真实样例**（161 个实体，只用来看形状、不用来查字段名）：
a locally imported Halloween reference scene

项目用 `jce_project.json` 的 `startup_scene` 指定开场场景；
单次运行可用 `JCE_STARTUP_SCENE=<路径>` 覆盖。

> **找不到 `jce_project.json` 的运行时会启动默认空场景，并且什么都不报错。**
> 所以「跑起来了」不等于「加载了你的场景」——判据见 §5。

## 2b. 相机朝向与 2D 正交（2026-09-22 实测）

**相机的 forward 是世界矩阵第三列取负**（`jce_scene_camera.c`）。绕 X 转 θ：

```
forward = (0,  sin θ, -cos θ)
up      = (0,  cos θ,  sin θ)
```

代进去：

| rotX | forward | 屏幕上的「上」 |
|---|---|---|
| **+90** | (0, **+1**, 0) —— **朝天** | —— |
| **-90** | (0, -1, 0) —— 俯视 | **-Z** |
| 0 | (0, 0, -1) —— 朝 -Z | +Y |

**`rotX=+90` 是朝天不是俯视**，这一条实测踩过：截出来一张纯天空图。
`-90` 不需要躲奇异点——`up` 是从同一个矩阵读的，不走 world-up 叉乘，
所以 `-90` 是精确且良定义的，不用退到 -89 之类。

**俯视时「屏幕上方 = -Z」**，所以顶视 2D 游戏里
**「上」键必须让 z 减小**。这一条没有任何对称检查能发现：
编辑器和运行时会**一起反**，两张同样错的图比起来完全相等。
上一轮就是用户肉眼发现的，不是测试。判据只能是**绝对方向**：
按「上」，断言 z **变小**，而不是断言两次运行一致。

### 正交相机：`orthoSize` 是高度，宽度跟视口

`JceCameraComponent` 有 `orthographic`（bool）与 **`orthoSize`**（世界单位**高度**）。
**只写高度**：固定宽度会在每种窗口比例下裁得不一样，而 pose 被解析时手上没有 aspect。
`jce_camera_proj` 把 `ortho_w <= 0` 读作「按 aspect 推导」。

> **2026-09-22 之前 `orthoSize` 不存在**：场景写了 `"orthographic": true`
> 也只是解析出一个 bool，范围永远是 `jce_camera_create` 的 **800×450** 默认，
> 30×20 的棋盘因此只有一个点大。同期编辑器**拿 `fov_deg`（角度）当世界单位跨度**，
> 于是编辑器和出货 exe 对同一个正交场景**各错各的**。已修。

## 2c. UI 文本：不要点名字体

`Canvas` + `UIText` 子节点是可用的形状（`sortOrder` / `refResX,Y` / 锚点 + `anchoredX,Y`）。

**`fontPath` 留空。** 空路径 = 「这台机器的 UI 字体」，引擎去系统字体目录找。
**不要写 `fonts/JCE.ttf`**——那是 owner 手写的 b41c1 个字形，是美术素材不是 UI 字体
（owner 2026-09-22 明确说明）。它此前是 `UC_DEFAULT_FONT`，也就是说
「没选字体」会渲染成一套手写体，而示例场景又都显式点了它的名
⟹ **每个照着示例写的新场景都继承了它**。
**一个既是默认、又是唯一显眼示例的选项，会变成所有人给出的答案。**

## 3. 玩法：脚本

**7 种语言**，离线权威是 `engine/src/resource/jce_asset_ext.c` 的
`k_script_ext_table`（8 行，java 占源码+字节码两行）：

| 语言 | 扩展名 |
|---|---|
| lua | `.lua` |
| python | `.py` |
| java | `.java` / `.class` |
| cpp | `.jcecpp` |
| c | `.jcec` |
| js | **`.jcejs`**（不是 `.js`——`.js`/`.ts` 被归为项目侧 web 工具链） |
| csharp | `.cs` |

```bash
python tools/audit/check_script_language_catalog.py    # 权威读数，三方必须一致
```

项目开启脚本是一句话（`jce_script_enable()` 做完全部四步并生成注册 shim）：

```cmake
if(COMMAND jce_script_enable)
    jce_script_enable(MyGame)
endif()
```

### 脚本能调什么：`JceScriptHost`

`engine/include/jce/middleware/script/jce_script.h` 的 `JceScriptHost`
是**脚本可见能力的全集**，2026-08-28 实测 **78 个成员**。按用途粗分：

| 用途 | 成员 |
|---|---|
| 变换 / 层级 | `get_position` `set_position` `get_rotation` `set_rotation` `get_scale` `set_scale` `get_world_position` `set_parent` `get_parent` |
| 生死 / 查找 | `spawn` `destroy_entity` `find_with_tag` `find_by_name` `find_by_prefix` |
| 组件（万能通道） | `has_component` `is_component_enabled` `set_component_enabled` `comp_get_json` `comp_set_json` `render_get_json` `render_set_json` `json_free` |
| 输入 | `is_key_down` `input_button` `action_down` `action_pressed` `action_axis` `pointer_delta` `pointer_wheel` `pointer_button` `touch_count` `touch_get` `get_move` `move_axis` |
| 物理 | `raycast` `apply_impulse` `set_velocity` `get_velocity` |
| 动画 | `anim_set_float` `anim_set_int` `anim_set_bool` `anim_set_trigger` |
| 音频 | `play_sound` `play_sound_spatial` `audio_set_volume` `music_set_intensity` `music_get_intensity` `music_request_transition` |
| UI | `ui_get_slider` `ui_set_slider` `ui_get_toggle` `ui_set_toggle` `ui_set_text` `ui_get_progress` `ui_set_progress` |
| 粒子 / 线 | `particle_burst` `particle_set_emitting` `particle_set_color` `line_set_points` |
| 网络 | `net_is_server` `net_is_client` `net_spawn` `rpc_send` |
| 时间 / 相机 | `set_time_scale` `set_paused` `shake_camera` |
| 消息 / 本地化 / 其它 | `send_message` `broadcast` `log` `read_file` `loc_translate` `loc_get_locale` `loc_set_locale` `gas_activate` `gas_get` `gas_apply` `vehicle_set_input` `vehicle_get_speed` |

**`comp_get_json` / `comp_set_json` 是万能通道**：想调一个上表没有的组件属性，
先用它把组件读成 JSON 看清字段名。注意 **`comp_set` 是部分更新**，
不是整组件重写。

> **这张表会长。** 重数：读 `jce_script.h` 里 `JceScriptHost` 的函数指针成员。
> **加脚本 API 属于「改引擎」**，走 `SKILL.md` §4 ——  那是改引擎，且这个结构体是**按字节
> append-only** 的，中间插一个成员会让签名不同的指针被静默调用。

## 4. 驱动它跑（无头、可复现）

```bash
JCE_MAX_FRAMES=200 \
JCE_WINDOW_HIDDEN=1 \
JCE_MULTI_INSTANCE=1 \
JCE_LOG_FILE=$PWD/run.log \
JCE_PERF_LOG=1 \
  <你的 exe 或 build/desktop/windows-x64/release/jce_editor.exe>
```

| 变量 | 作用 |
|---|---|
| `JCE_MAX_FRAMES=N` | 跑 N 帧后自己退出——**没有它就得靠 kill，退出码不可信** |
| `JCE_WINDOW_HIDDEN=1` | 不弹窗 |
| `JCE_MULTI_INSTANCE=1` | 绕开单实例锁。**不加它，第二个进程会静默退出而不渲染任何东西** |
| `JCE_LOG_FILE=<abs>` | 出货游戏是 WIN32 无控制台进程，**stdout 是断开的**；不设这个就一行日志都拿不到 |
| `JCE_STARTUP_SCENE=<路径>` | 覆盖开场场景 |
| `JCE_BACKEND=d3d11\|vulkan\|opengl\|…` | **这是请求不是结果**，bgfx 会回落，见 §5 |
| `JCE_HEADLESS=1` | 无头 |

**要可复现**（两次运行可比）时，还要钉住这三个——它们在
`tools/jce_determinism.py` 的 `DETERMINISM` 里单一来源，**import 它，不要抄**：
`JCE_FRAME_DT_FIXED` `JCE_STREAM_SYNC` `JCE_TAA_JITTER_PHASE`。

## 5. 看画面：抓图 → 真的去看 → 机器判空

### 两条抓图路径，**不能混用**

| 路径 | 变量 | 抓的是什么 |
|---|---|---|
| 引擎 / 出货游戏 | `JCE_CAPTURE_FRAME` + `JCE_CAPTURE_PATH` | 引擎自己的帧 |
| 编辑器 backbuffer | `JCE_SHOT_FRAME` + `JCE_SHOT_PATH` | F12 那条，整个 backbuffer |

两条产出的图**不一样**，混着比对得到的差异毫无意义。

```bash
JCE_MAX_FRAMES=200 JCE_WINDOW_HIDDEN=1 JCE_MULTI_INSTANCE=1 \
JCE_CAPTURE_FRAME=198 JCE_CAPTURE_PATH=$PWD/shot.png \
JCE_LOG_FILE=$PWD/run.log  <exe>
```

**抓晚一点**：第 0 帧拍到的是加载画面。

### 然后**真的把它看一眼**

用 Read 工具读那个 `.png`——图片会被直接呈现出来。这是这条链里唯一能回答
「画面对不对」的一步，**任何数值判据都替代不了它**。

### 但先让机器排掉「什么都没画」

```python
import sys; sys.path.insert(0, "tools")
from jce_determinism import read_png, assert_has_content
assert_has_content(read_png("shot.png"), "shot.png")   # 空帧直接抛
```

**两张空帧比起来完全相等**，所以「差异 0.00000%」既可能是「一致」也可能是
「两边都没画」。这个守卫存在的原因就是后者真发生过三次。

### 三条必须核对的日志断言

```bash
grep 'jce_renderer: renderer:' run.log     # ① 引擎自己报的后端，不是你请求的那个
grep -i '<你的场景名>' run.log             # ② 场景真的加载了（否则是默认空场景）
grep 'engine: perf:' run.log               # ③ 到过稳态
```

`JCE_BACKEND` 是**请求**，bgfx 会静默回落；在错的后端上拍的图和对的一模一样。

### 一键做完全部：验收协议

```bash
python scripts/jce_accept.py --project <项目目录> --backend d3d11 --frames 200
```

八个阶段：`package` / `relocation`（拷到没有仓库在其上方的目录里跑）/ `boot`
（查退出码）/ `scene` / `backend` / `screenshot`（非空帧）/ `perf` / `listener`
（有没有开监听口）。各自 PASS / FAIL / **SKIP——SKIP 不算 PASS**。

## 6. 常见的静默失效（都实测过，都不报错）

| 现象 | 真相 |
|---|---|
| 跑起来了，但画面是空的 / 没有你的东西 | 找不到 `jce_project.json` ⇒ 启动**默认空场景**，不报错 |
| 一端渲染正常、另一端空白，两端都不报错 | **bgfx view id 撞了**。view 状态是 last-write-wins，抢同一个 id 的一方静默停止出像素。归属表 `engine/include/jce/renderer/jce_views.h` |
| HUD 全落在错的那条边上 | UGUI 锚点是**左下原点 / +Y 向上**，内部空间是**左上 / Y 向下**，差一个 Y 翻转；**X 不需要翻转**，所以看起来像「差不多对了」 |
| 第二个进程什么都不画 | 单实例锁。加 `JCE_MULTI_INSTANCE=1` |
| 一行日志都没有 | 出货游戏是 WIN32 无控制台进程，stdout 断开。设 `JCE_LOG_FILE` |
| 改了一行 Lua，行为没变 | **脚本在 PAK 里，PAK 在 exe 里——必须重链 exe** |
| 玩家一开游戏就弹 Windows 防火墙 | Tracy 在静态初始化期开监听。出货用 `jce.py sdk --profiling off` 重建 SDK 再打包 |

## 7. 越界了就回主线

以下任何一条成立，**停下，回 `SKILL.md` §4 的必经步骤表**：

- 你要改 `engine/**` 或 `editor/**` 的源码（哪怕只是注释）
- 你要加 / 改一个脚本 API（`JceScriptHost` 是按字节 append-only 的）
- 你要动 `engine/include/**`（那要重装 SDK，否则「编辑器里对、出货 exe 错」）
- 你要提交

本文件不复述那边的门禁与收尾判据；照本文件干完活直接提交，会漏掉它们。
