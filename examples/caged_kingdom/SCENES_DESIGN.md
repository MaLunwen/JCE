# Caged Kingdom — 场景设计册

> 每个场景 JSON 是**数据**（位于 `resources/assets/scenes/`，按惯例不入 git）；本文件是**设计意图**（入库）。
> 命名规范见 `SETTING_BIBLE.md` §7。
> 现有 schema：see `engine/src/middleware/scene/jce_scene_components_json.c`
> 顶层 `contract{name,major,minor}` + `scene{version, entities[]}`，entity = `{id, name, parentId, components[]}`。
>
> 组件白名单（参见 schema 源）：
> - `Transform` — pos / rot / scale (xyz)
> - `Light` — `lightType` 0=Dir / 1=Point / 2=Spot；`colorRGB` / `intensity` / `castsShadow`
> - `MeshRenderer` — `meshPath`（资源相对路径）/ `materialPath` / `meshShape`（0=cube 1=sphere 2=plane）/ inline PBR (`baseColorRGBA` / `metallic` / `roughness` / `albedoTex`)
> - `SkeletalAnimator` — `skeletonPath` / `activeClip` / `speed` / `loop` / `playing`
> - `AudioSource` — `clipPath` / `volume` / `pitch` / `spatialBlend` / `loop` / `playOnAwake`
> - `Skybox` — `hdrPath` / `rotation` / `exposure` / `useAsIbl`（需 .hdr 资源；v1 m01 不用）
> - `EditorMeta` — `name` / `tag`（编辑器辅助，非必需）

---

## 通用约定

- **ID 段位**：每幕预留 1000 段。第一幕 1000-1999，第二幕 2000-2999 …
  - m01: 1000-1099 / m02: 1100-1199 / …
- **坐标系**：右手 Y-Up。+X 东 / +Y 上 / +Z 南。玩家初始朝向 -Z（朝北）。
- **单位**：1 = 1 米。
- **基线性能**：单关场景 entity 数 ≤ 30（基线 HW 30 FPS）；复杂街区另行规划批次/cell 流式加载。

---

## 第一幕：王都余烬

### `act1_m01_wake.scene.json` — 破晓后的难民营（教学：移动/视角/物品栏）

**叙事**：玩家（Kael / BagMan）在王都遗址的废弃猎屋外苏醒。三月前的破晓之夜让这里成了难民营。远处可见王都晨光之城的残骸轮廓（v1 简化为远景墙体）。

**关卡目标**：

1. （隐式）按下移动键 → 走出猎屋投射的阴影区
2. 拾起篝火旁的"破旧背包"道具（成为 BagMan 身份）
3. 走到难民营长老处触发对话 → 进入 m02

**视觉调性**：破晓后灰白雾蒙 + 余烬橙光斜照。地面石板灰褐，墙体残破。

**Entity 清单**：

| ID | Name | 作用 | 关键组件 |
|---|---|---|---|
| 1000 | `Sun` | 主光源 | Light dir, 颜色 (1.0, 0.92, 0.78)，intensity 2.8，方向 (-0.3, -1.0, -0.4)，castsShadow=true |
| 1001 | `AmbientMusic` | 环境音 | AudioSource clipPath=`musics/AlkaKrab Music/ogg/Light Ambient 2 (Loop).ogg`，volume 0.6，loop+playOnAwake，spatialBlend 0（2D） |
| 1010 | `Ground` | 大地面（粗石） | MeshRenderer plane (meshShape 2)，scale (30,1,30)，baseColor 灰褐 (0.45, 0.40, 0.35)，roughness 0.95，albedoTex `textures/texture.jpg` |
| 1020 | `SafeHouse` | 废弃猎屋（玩家苏醒点） | MeshRenderer meshPath=`models/chalet.obj`，pos (5, 0, 2)，rot (0, -30°, 0)，scale 0.8 |
| 1030 | `RuinWall_N` | 残破石墙（北侧背景） | MeshRenderer cube (meshShape 0)，pos (0, 1, -12)，scale (20, 2, 0.5)，baseColor (0.5, 0.45, 0.4)，roughness 0.9 |
| 1031 | `RuinWall_E` | 东侧残墙 | cube，pos (12, 0.6, 0)，scale (0.5, 1.2, 8)，缺口模拟通过两段拼，本文件先用单段 |
| 1032 | `RuinPillar1` | 倒塌石柱 | cube，pos (-6, 0.3, -3)，rot (0, 0, 75°)，scale (0.8, 4, 0.8) |
| 1033 | `RuinPillar2` | 倒塌石柱 | cube，pos (-4, 0.5, 4)，rot (0, 25°, 60°)，scale (0.8, 3, 0.8) |
| 1040 | `Campfire` | 篝火（橙红光位标记） | cube，pos (-2, 0.15, -1)，scale (0.6, 0.3, 0.6)，baseColor 暖橙 (1.0, 0.55, 0.2)，roughness 0.7 |
| 1041 | `CampfireLight` | 篝火点光源 | Light lightType 1 (point)，pos (-2, 0.8, -1)，颜色 (1.0, 0.6, 0.25)，intensity 6.0，castsShadow=false |
| 1050 | `Player_BagMan` | **玩家角色**（v1 静站，待 CharacterController 接入） | SkeletalAnimator skeletonPath=`models/PSX_BagMan.glb`，activeClip 0，speed 1，loop true，playing true；pos (0, 0, 0) |

**v1 实现状态**：

- ✅ 静态场景渲染 + 玩家模型可见 + 环境音 + 主光/篝火光
- ⏳ 玩家移动控制（待 `engine/src/middleware/character/` 接入；本场景先不放 CharacterController 组件）
- ⏳ 可拾取道具 + 对话触发器（待 trigger + dialogue 系统）

**烘焙清单**（cooker 必入 PAK）：
- `models/PSX_BagMan.glb`
- `models/chalet.obj` + `textures/chalet.jpg`
- `textures/texture.jpg`
- `musics/AlkaKrab Music/ogg/Light Ambient 2 (Loop).ogg`

**ck_app.c 切换**：默认加载场景从 `scenes/main.scene` 切到 `scenes/act1_m01_wake.scene`（**留待用户确认后再改**）。

---

### `act1_m02_first_kill.scene.json` — 难民营外的伏击（教学：战斗 + Wanted 起步）

**叙事**：玩家走出 m01 安全屋投奔难民营长老的路上，撞见 2-3 名七女魔王军（薇雯麾下的"焰魔散兵"）正在抢劫一名平民。无路可避——玩家被迫第一次出手。打倒散兵后，魔王军记录玩家面孔，Wanted 起步至 1 星。

**关卡目标**：
1. 拾起路边铁棍（教学：拾取 / 武器栏）
2. 击倒所有焰魔散兵（教学：轻击 / 闪避 / 处决）
3. 平民感谢并指路 → 进入 m03（载具教学）

**视觉调性**：午后将晚，阴沉橙紫天光。地面与 m01 同石板，但巷道狭窄、视野受限，配合 cover 物体增加战术感。

**Entity 清单**（ID 段 1100-1199）：

| ID | Name | 作用 | 关键组件 |
|---|---|---|---|
| 1100 | `Sun` | 主光源 | Light dir，颜色 (1.0, 0.78, 0.60) 暖紫橙，intensity 2.2，方向 (-0.5, -0.85, -0.2)，castsShadow=true |
| 1101 | `CombatMusic` | 战斗 BGM | AudioSource clipPath=`musics/AlkaKrab Music/ogg/Action 1 (Loop).ogg`，volume 0.7，loop+playOnAwake，2D |
| 1110 | `Ground` | 巷道地面（窄长） | plane，scale (15, 1, 25)，baseColor (0.40, 0.36, 0.32)，roughness 0.95，albedoTex `textures/texture.jpg` |
| 1120 | `AlleyWall_W` | 西侧巷壁 | cube，pos (-7, 1.5, 0)，scale (0.5, 3, 22)，baseColor (0.48, 0.43, 0.38) |
| 1121 | `AlleyWall_E` | 东侧巷壁 | cube，pos (7, 1.5, 0)，scale (0.5, 3, 22)，baseColor (0.48, 0.43, 0.38) |
| 1122 | `BackWall` | 远端拦截墙（巷尾） | cube，pos (0, 1.5, 11)，scale (15, 3, 0.5)，baseColor (0.50, 0.45, 0.40) |
| 1130 | `Cover_Crate1` | 战术掩体（木箱） | cube，pos (-3, 0.5, -2)，rot (0, 15, 0)，scale (1.2, 1, 1.2)，baseColor (0.42, 0.30, 0.18)，roughness 0.85 |
| 1131 | `Cover_Crate2` | 战术掩体 | cube，pos (3, 0.5, 3)，rot (0, -20, 0)，scale (1.2, 1, 1.2)，baseColor (0.42, 0.30, 0.18) |
| 1132 | `Cover_Rubble` | 碎石堆 | cube，pos (1, 0.4, -5)，rot (0, 45, 8)，scale (2, 0.8, 1.5)，baseColor (0.55, 0.50, 0.45) |
| 1140 | `Weapon_IronStick` | 拾取武器（铁棍占位） | cube，pos (-1, 0.2, -7)，rot (0, 0, 80)，scale (0.2, 1.2, 0.2)，baseColor (0.30, 0.30, 0.32) 暗灰，metallic 0.6，roughness 0.4 |
| 1150 | `Enemy_FlameGrunt_1` | 焰魔散兵 1 | cube 占位，pos (-2, 1, 4)，scale (0.8, 1.8, 0.8)，baseColor (0.85, 0.20, 0.10) 焰红，roughness 0.7。EditorMeta tag=`enemy_flame` |
| 1151 | `Enemy_FlameGrunt_2` | 焰魔散兵 2 | cube，pos (2, 1, 5)，scale (0.8, 1.8, 0.8)，焰红色 |
| 1152 | `Enemy_FlameGrunt_3` | 焰魔散兵 3 | cube，pos (0, 1, 7)，scale (0.8, 1.8, 0.8)，焰红色 |
| 1160 | `Civilian_Bystander` | 平民（待救） | cube 占位，pos (0, 0.9, 9)，scale (0.7, 1.7, 0.7)，baseColor (0.65, 0.60, 0.50) 灰布。EditorMeta tag=`civilian` |
| 1170 | `Player_BagMan` | 玩家 | SkeletalAnimator skeletonPath=`models/PSX_BagMan.glb`，pos (0, 0, -10)（巷口起始） |

**v1 实现状态**：
- ✅ 静态场景 + 玩家可见 + 战斗 BGM + 主光照
- ⏳ 敌人 AI（待 `engine/src/middleware/ai/` 接入；占位 cube 用 EditorMeta tag 标记）
- ⏳ 武器拾取系统（待 interactable trigger）
- ⏳ Wanted 系统（待 ck-layer 游戏状态模块）
- ⏳ 处决 / hitstop / 粒子（待战斗 FX 接入）

**烘焙清单**：
- `models/PSX_BagMan.glb`
- `textures/texture.jpg`
- `musics/AlkaKrab Music/ogg/Action 1 (Loop).ogg`

### `act1_m03_first_run.scene.json` — TBD

教学：载具（魔法马车）。

### `act1_m04_resistance.scene.json` — TBD

抵抗军接触，认识塞拉。

### `act1_m05_smuggle.scene.json` — TBD

第一次走私任务（解锁拉斯帕兹）。

### `act1_m06_arrival.scene.json` — TBD

抵达拉斯帕兹，进入主场景。

---

## 技术实验场景

### `lightning_lab.scene.json` — 拉普拉斯介电击穿闪电实验

**用途**：独立于主线的可复现实验场景，用于验证 JCE 的场景脚本、
LineRenderer、瞬态光照、后处理、天气和空间音频。编辑器 Play 与发行运行时
都执行 `scripts/lightning_lab.lua`，场景本身不依赖项目专有 C 逻辑。

**模拟流程**：

1. 在 31 x 25 的电势求解平面上设置云端/地面边界，并将结果映射为具有
   空间相关性的三维曲折通道；
2. 分帧执行有硬预算的 Gauss-Seidel 松弛，求解离散拉普拉斯方程；
3. 按介电击穿权重 `P_i proportional to phi_i^1.65` 生长负先导，保留自然分支；
4. 根据云端起点、导体高度和水平距离选择接闪目标；下降先导进入连接区后，
   从目标顶部生成向上流光，两者接触后才建立导电主通道；
5. 首次回击由地向云传播，随后以确定性的 35-90 ms 间隔生成 1-3 次
   dart leader / 后续回击；白热核心、蓝紫外晕、分支余辉和云内散射光分别
   渲染，避免用单根等宽蓝线代替整个放电过程；
6. 依据观察者到通道各段的距离，以 343 m/s 安排近场爆裂与较远通道滚雷，
   高频爆裂先到，低频尾声后到。

**性能与确定性**：每帧最多生长 4 个网格单元，每个单元最多 4 轮松弛；
通道至少发展 82 个节点后才允许接地，124 节点为软上限并带强制接地兜底。
随机序列使用固定 Park-Miller
生成器和 strike serial 种子，因此同一次序列可复现，不使用全局 `math.random`。
CPU 只负责低频场求解、三维几何细分和事件时序；每段细分具有父方向惯性，
避免逐点独立白噪声造成的锯齿。GPU 负责线段、天气、光照和后处理绘制。
真实先导/回击的微秒级过程低于普通显示器帧周期，因此案例提供“可见但受限”
的时间扩展：先导约 0.25 s，首次回击约 0.035 s；相位次序与传播方向保持正确。

**Entity 清单**（ID 段 9000-9099）：

| ID | Name | 作用 | 关键组件 |
|---|---|---|---|
| 9000 | `LightningLabRoot` | 实验根节点 | Transform + EditorMeta |
| 9001 | `StormCamera` | 固定观察镜头 | Camera + VirtualCamera，look-at 导雷区 |
| 9010 | `StormGround` | 湿润试验地面 | plane，50 m x 50 m，低粗糙度深灰材质 |
| 9011-9015 | `GroundGrid_*` | 地面导体/尺度参照 | 扁平金属 cube |
| 9020-9022 | `LightningRod_*` | 接闪目标 | 细长金属 cube |
| 9030-9032 | `StormCloud_*` | 云层轮廓 | 扁平 sphere，粗糙深灰材质 |
| 9040 | `LightningLeader` | 分帧先导主通道 | LineRenderer，蓝紫细线 |
| 9041 | `LightningReturnStroke` | 地面回击 | LineRenderer，白蓝宽线 |
| 9042-9047 | `LightningBranch_0..5` | 放电分支 | LineRenderer |
| 9048 | `LightningCore` | 饱和白热导电核心 | 窄 LineRenderer |
| 9049 | `LightningHalo` | 电离通道外晕 | 宽低透明 LineRenderer |
| 9053 | `LightningUpwardStreamer` | 导体向上连接流光 | LineRenderer，地向云生长 |
| 9050 | `LightningImpactLight` | 落点瞬态照明 | Point Light，无阴影 |
| 9051 | `LightningCloudLight` | 云底散射闪光 | Point Light，无阴影 |
| 9052 | `LightningImpactGlow` | 落点发光球 | sphere + emissive PBR |
| 9054-9055 | `LightningCloudScatter_A/B` | 云体多点散射 | Point Light，无阴影 |
| 9060 | `LightningDirector` | 模拟生命周期 | Script `scripts/lightning_lab.lua` |

**资产清单**：

- `scripts/lightning_lab.lua`
- `scripts/lightning_lab.lua.deps.json`
- `sounds/lightning_lab_thunder_crack.wav`（项目内确定性程序合成，原创）
- `sounds/lightning_lab_thunder_roll.wav`（项目内确定性程序合成，原创）

**验收**：打开场景后进入 Play，约 1 秒出现低亮度阶梯先导；导体向上流光与
下降先导连接后，白热回击从地面向云端传播，并出现至少一次强度递减的后续
回击。雷声按观察距离分成爆裂和滚雷到达；脚本公开入口
`ck_lightning_trigger` 可立即开始下一次放电。

### `black_hole_lab.scene.json` — Kerr 黑洞科学可视化实验

**用途**：独立于主线的广义相对论科普与数值验证场景。场景以 Kerr 时空的
零测地线反向积分为权威图像来源，展示引力透镜、光子俘获、参考系拖曳、
吸积盘高阶像、引力红移和相对论多普勒增亮。不得用黑色球体、径向 UV 扭曲
或手绘亮环冒充求解结果。

**模型范围**：默认采用 `chi = 0.8` 的旋转超大质量黑洞和
Page-Thorne 薄吸积盘；`chi = 0` 为 Schwarzschild 基准。内部使用
`G = c = M = 1`，质量只负责映射物理长度、时间和盘温标。科学界面必须区分
事件视界、临界曲线、黑洞阴影、光子环和引力透镜环；不把 EHT 的观测厚环
直接称作理论光子环。v1 不模拟 GRMHD、喷流、日冕、偏振和视界内部。

**求解与验证**：GPU 使用有界 RK4 积分 Kerr 分离测地线方程，并逐射线报告
俘获、盘命中、逃逸或无效结果，以及步数和势函数残差。CK 的宿主测试目标
另用双精度 Kerr-Schild Hamiltonian + 自适应 RK45 作为独立参考。必须恢复
Schwarzschild 的 `r_h = 2M`、`r_ph = 3M`、`b_c = 3 sqrt(3) M`、
`r_ISCO = 6M`，并通过 `64 x 64` 浮点诊断目标的 4096 条射线进行 CPU/GPU
对比；8 位截图不得代替数值残差读回。

**架构边界**：JCE 只提供通用 HDR Fullscreen Effect、项目 Shader 编译、
项目优先的 Shader/PAK 解析、通用数值纹理、异步类型化诊断回读、受限 JSON
脚本读取、序列化、相机对齐/驾驶、编辑器通用检查器和生命周期管理。Kerr
方程、观察者标架、吸积盘模型、配置、Shader、预设、Canvas HUD 和验证数据
全部位于 `caged_kingdom`。交互由 CK 场景 Lua 脚本驱动，因此编辑器默认 Game
Module 与运行时消费同一套项目 PAK、脚本和 Shader，不要求编辑器静态链接 CK
C 代码。场景序列化经验证的默认参数快照，使编辑态无需执行项目脚本也能运行
真实 Shader；Scene View 保持独立自由相机，选择 `KerrObserverCamera` 后可通过
通用对齐/驾驶命令核对标准构图。Play 前由同一 JSON 配置重新校验并绑定。

**Entity 清单**（ID 段 9100-9199）：

| ID | Name | 作用 | 关键组件 |
|---|---|---|---|
| 9100 | `BlackHoleLabRoot` | 实验根节点 | Transform + EditorMeta |
| 9101 | `KerrObserverCamera` | ZAMO 观察相机，坐标单位为 `M` | Camera + VirtualCamera |
| 9110 | `KerrCoordinateAnchor` | 编辑器可选原点代理，游戏视图不渲染 | Transform + EditorMeta |
| 9120 | `KerrFullscreenEffect` | 项目 Kerr Shader 的通用 HDR 入口 | FullscreenEffect（required） |
| 9130 | `KerrLabDirector` | CK 配置、预设、相机和参数绑定 | Script `scripts/black_hole_lab.lua` |
| 9140 | `KerrScienceHUD` | 科学标签、参数与诊断切换 | ECS Canvas + 控件 |
| 9150 | `CriticalCurveOverlay` | 临界曲线、视界与 ISCO 解释层 | CK Canvas 诊断覆盖层 |
| 9160 | `ReferenceRayProbe` | 烘焙参考摘要与可视射线探针，不冒充运行时自动验证 | Script + Canvas |

**计划资产**：

- `scenes/black_hole_lab.scene.json`
- `black_hole/kerr_lab.json`
- `black_hole/reference_summary.json`（完整语料仅生成到构建报告目录）
- `scripts/black_hole_lab.lua` 与 `.deps.json`
- `textures/black_hole/starfield_reference.png`
- `textures/black_hole/disk_flux_lut.jceasset`（通用 `R32F` 数值纹理容器）
- `textures/black_hole/blackbody_rgb_lut.jceasset`（通用 `RGBA16F` 数值纹理）
- `shaders/black_hole/include/ck_kerr_lensing_solver.sc`
- `shaders/black_hole/fs_ck_kerr_lensing_{128,192,320,640}.sc`
  （四档项目包装入口共享同一求解器正文，构建后进入 PAK）

**验收**：默认、Schwarzschild、正向/反向自旋和近侧视预设均可交互；
科学诊断能显示射线结果、红移、像阶、步数和残差。相同后端、视口、配置哈希
与帧号下，编辑器 Game View 和发行运行时输出一致。缺少 required Shader 或
纹理时必须给出资源与后端诊断，不得仅显示黑屏。科普界面采用观察、比较、
诊断三种紧凑工作态，公式、单位、模型假设和引用可核对，且不遮挡临界曲线。
完整理论、数值误差预算、通用 API 与跨后端测试见
`docs/superpowers/specs/2026-08-12-scientific-kerr-black-hole-lab-design.md`。

---

## 历史场景（保留）

- `main.scene.json` — 烟测场景（Sun+Ground+BagMan）。**v1 保留作 smoke test**，不入主线。
- `fps_demo.scene.json` — 引擎特性 demo。
- `showcase.scene.json` — 引擎完整能力展示。

---

## 附录 A：场景串联机制设计（v1 草案，未实施）

> **目标**：m01 走完后过场切到 m02，不重启游戏。玩家身上的状态（HP / 武器 / 货币 / Wanted / 任务进度）跨场景保留。
> **本节为设计记录**，落地代码将分为引擎层补 API + ck 层加 `ck_scene_director` 两个 PR。

### A.1 现状回顾（ck_app.c L266-292）

- 初始化期一次性 `jce_scene_create()` + `jce_scene_serial_load_vfs()` + `jce_scene_renderer_create()`
- 写死候选 `scenes/main.scene` / `scenes/main.scene.json`
- 无运行期场景切换路径
- 引擎暴露 API（`engine/include/jce/middleware/scene/jce_scene.h`）：
  - ✅ `jce_scene_create()` / `jce_scene_destroy()` / `jce_scene_serial_load_vfs()`
  - ✅ `jce_scene_destroy_entity()`
  - ❌ 缺 `jce_scene_clear()`（保留 scene 对象，清空 entities）

### A.2 设计要点

#### A.2.1 切换原语

两种实现路径：

| 方案 | 做法 | 优缺点 |
|---|---|---|
| **A. Destroy-Recreate** | `destroy(scene_renderer) → destroy(scene) → create(scene) → load(...) → create(scene_renderer)` | 简单；但每次重建 renderer 浪费 GPU/asset cache |
| **B. Clear-Reload** ✅ 推荐 | 新增引擎 API `jce_scene_clear(JceScene*)` 仅清 entities；renderer 保留 + invalidate 缓存 | 切换快；可服用 PAK / mesh cache。需引擎补 1 个 API |

→ **决定**：先做 A（v1 落地最少改动），引擎层后续补 `jce_scene_clear` 后切到 B。

#### A.2.2 触发条件

每个场景**入口数据**带"目标"和"完成条件"，由 ck 层任务系统解释：

```jsonc
// 草案：scenes/act1_m01_wake.scene.json 加顶层 quest 元数据
{
  "contract": {"name": "jce.scene", "major": 1, "minor": 0},
  "ck_quest": {                          // <-- ck 层扩展，引擎忽略
    "questId": "act1_m01_wake",
    "objectives": [
      { "id": "leave_safehouse", "type": "trigger_zone",
        "center": [0, 0, -8], "radius": 3.0 }
    ],
    "onComplete": { "loadScene": "scenes/act1_m02_first_kill.scene.json" }
  },
  "scene": { ... }
}
```

→ ck 层 `ck_scene_director` 读 `ck_quest` 字段；引擎 schema 不强校验未知顶层 key（**待引擎确认**，若 strict 则改放独立 `scenes/<id>.quest.json` 旁文件）。

#### A.2.3 状态持久化（跨场景）

引入 ck 层 `CkPlayerState`（**纯数据 struct**，与 entity 解耦）：

```c
/* examples/caged_kingdom/src/game/ck_player_state.h (待新增) */
typedef struct CkPlayerState {
    float    hp, hp_max;
    float    armor;
    int      currency_crystal;
    int      currency_kingmark;
    int      wanted_stars;       /* 0..5 */
    uint32_t weapon_flags;       /* bitmask: fist / iron_stick / runic_xbow / ... */
    char     active_quest[64];   /* 当前任务 id */
    /* 不放 entity / transform —— 那些每场景重建 */
} CkPlayerState;
```

切场景时 ck_scene_director：
1. 从旧 scene 的 `Player_BagMan` entity 提取并更新 `CkPlayerState`
2. destroy 旧 scene
3. load 新 scene
4. 在新 scene 找到（按 EditorMeta tag=`player` 的 entity）→ 把 `CkPlayerState` 写回

#### A.2.4 过场转换（fade）

- ck 层维护 `transition_alpha`（0..1）
- 流程：trigger → tween alpha 0→1 (250 ms 黑屏) → swap scene → tween alpha 1→0 (250 ms 显现)
- 引擎/编辑器侧不需要改；ck 层在 `on_render` 末尾叠加全屏黑色 quad（透明度 = alpha）
- v1 黑屏即可，后续可换十字溶解 / 卡通涂抹

#### A.2.5 数据驱动 quest graph

可选独立文件 `examples/caged_kingdom/resources/assets/quests/main_storyline.json`：

```jsonc
{
  "version": 1,
  "act1": {
    "m01_wake":      { "next": "m02_first_kill", "scene": "scenes/act1_m01_wake.scene.json" },
    "m02_first_kill":{ "next": "m03_first_run",  "scene": "scenes/act1_m02_first_kill.scene.json" },
    "m03_first_run": { "next": "m04_resistance", "scene": "scenes/act1_m03_first_run.scene.json" },
    "m04_resistance":{ "next": "m05_smuggle",    "scene": "scenes/act1_m04_resistance.scene.json" },
    "m05_smuggle":   { "next": "m06_arrival",    "scene": "scenes/act1_m05_smuggle.scene.json" },
    "m06_arrival":   { "next": "m07_rats",       "scene": "scenes/act1_m06_arrival.scene.json" }
  },
  "act2": { ... }
}
```

→ `ck_scene_director` 启动时加载此文件，按 `wanted_act/m` 查找下一场景，**避免在每个 scene JSON 里硬编码 next**。

### A.3 落地 TODO（顺序）

1. 引擎层（可独立 PR）：
   - [x] 补 `jce_scene_clear(JceScene*)` —— 清空 entities 但保留 scene 句柄 ✅ commit `b943230`
   - [x] 确认 scene serial loader 是否忽略未知顶层 key（如 `ck_quest`）✅ `engine/src/resource/jce_scene_serial.c` 仅读取 `contract` + `scene`，其他顶层 key 静默忽略，无需放开 strict
2. ck 层（v1 主体改动）：
   - [x] `examples/caged_kingdom/src/game/ck_player_state.h/.c` —— 玩家持久状态 ✅ commit `123c632`
   - [x] `examples/caged_kingdom/src/game/ck_scene_director.h/.c` —— 场景切换器，封装 destroy→load→inject ✅ commit `123c632` + 接入 `ae4a8d0` + 持久 fs/quests/triggers + tick 重写 `57114ee`
   - [x] `examples/caged_kingdom/src/game/ck_quest_graph.h/.c` —— 加载 `quests/main_storyline.json`，提供 next-scene 查询 ✅ commit `9183d08`
   - [x] `examples/caged_kingdom/src/game/ck_trigger.h/.c` —— trigger_zone 类型检测（XZ 圆盘，进入 radius 触发，单次发火）✅ commit `57114ee`
   - [ ] `examples/caged_kingdom/src/game/ck_fade.h/.c` —— 过场 fade 渲染
   - [x] ck_app.c：把 init 的 scene_load 提取为 `ck_scene_director_load_initial(...)` ✅ `ae4a8d0`；启动改走 `ck_scene_director_load_start`，update tick 已接 ✅ `e93926a`
3. 资产（**本地文件，遵循 .gitignore 约定不入库**；只入库 `.md` 设计文档）：
   - [x] 生产 `examples/caged_kingdom/resources/assets/quests/main_storyline.json`（act1 6 关 + act2 m01_rats 占位）✅ 本地
   - [x] 给每个 scene JSON 加 `ck_quest.objectives`（trigger_zone center+radius）✅ 本地：m01 `leave_safehouse` (5,0,2) r=2.5；m02 `exit_alley` (-8,0,0) r=3.0
4. 文档：
   - [ ] SCENES_DESIGN.md 每关补 "进入条件" / "完成条件 → 下一关" 两行
   - [ ] AGENTS.md 提及 `ck_scene_director` 是 ck 层场景切换权威

### A.4 不做（v1.0 范围外）

- ❌ 场景流式加载（cell-based streaming）—— ROADMAP §2 提及，但留给 B 区拉斯帕兹大地图，本机制仅做关卡式切换
- ❌ 联机场景同步 —— ROADMAP §9，主机推进，客户端被动跟随，本机制设计阶段仅考虑单机
- ❌ 存档跨平台序列化 —— ROADMAP §13，独立模块
