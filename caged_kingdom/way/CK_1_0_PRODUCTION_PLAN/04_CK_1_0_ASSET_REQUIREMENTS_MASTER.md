# Caged Kingdom 1.0 — 资产需求总表

> 本文件只列 2027-01 首发需要的资产。长期七魔王资产继续保留在 `CK_Asset_Manifest_v1.0.md`，但不进入本次 Release Blocking 范围。

## 1. 状态与优先级

状态：

- ✅ 已有且可用；
- 🔶 已有但需加工；
- ⬜ 待制作或采购；
- ⛔ 首发延期。

优先级：

- R0：切片或主线必需；
- R1：首发质量必需；
- R2：不足时可延期或降低复杂度。

## 2. 目录

```text
caged_kingdom/resources/assets/
├── models/
├── skeletons/
├── anims/
├── textures/
├── materials/
├── vfx/
├── audio/
├── ui/
├── sky/
├── prefab/
└── scenes/
```

所有运行时路径必须小写、正斜杠、无绝对路径；进入 PAK 前必须有来源和 License 记录。

## 3. 骨架

| ID | 用途 | 规格 | 状态 | 优先级 |
|---|---|---|:--:|:--:|
| `sk_humanoid` | 玩家、NPC、普通敌人、两 Boss | 约 55 骨；含武器、背部、车辆和可选 VFX 挂点 | ⬜ | R0 |

首发不制作四足、翼族、巨型和米拉异形骨架。

挂点：

- `socket_hand_r`
- `socket_hand_l`
- `socket_back`
- `socket_hip_l`
- `socket_vehicle`
- `socket_fx_head`
- `socket_fx_chest`

验收：

- 所有人形模型可重定向；
- 无骨骼比例爆炸；
- 武器挂点稳定；
- LOD 切换不丢蒙皮；
- Root Motion 策略统一。

## 4. 人形动画

### 4.1 通用移动

| Clip | 说明 | 优先级 |
|---|---|:--:|
| `idle` | 主待机 | R0 |
| `idle_look` | 待机变体 | R1 |
| `walk_fwd` | 前行 | R0 |
| `walk_back` | 后退 | R1 |
| `strafe_l/r` | 横移 | R1 |
| `run_fwd` | 奔跑 | R0 |
| `sprint` | 冲刺 | R0 |
| `turn_l90/r90` | 原地转向 | R0 |
| `jump_start/loop/land` | 跳跃 | R0 |
| `dodge_f/b/l/r` | 四向闪避，可镜像/旋转复用 | R0 |
| `fall_loop/hard_land` | 高处落下 | R2 |

### 4.2 受击与死亡

- `hit_front` — R0
- `hit_back` — R1
- `hit_l/r` — R2
- `stagger` — R0
- `knockdown` — R0
- `getup` — R0
- `death_a` — R0
- `death_b` — R2
- `block` — R0
- `parry_react` — R2

### 4.3 武器套

徒手：

- `unarmed_combat_idle`
- `unarmed_atk_1`
- `unarmed_atk_2`
- `unarmed_atk_3`
- `unarmed_heavy`

钝器：

- `blunt_combat_idle`
- `blunt_atk_1`
- `blunt_atk_2`
- `blunt_heavy`

短剑：

- `blade_combat_idle`
- `blade_atk_1`
- `blade_atk_2`
- `blade_atk_3`
- `blade_heavy`

短弩：

- `xbow_aim_idle`
- `xbow_aim_walk`
- `xbow_fire`
- `xbow_reload`
- `xbow_draw`
- `xbow_holster`

符文枪：

- `gun_aim_idle`
- `gun_aim_walk`
- `gun_fire`
- `gun_reload`
- `gun_draw`
- `gun_holster`

### 4.4 交互与载具

- `interact_pickup`
- `interact_use`
- `open_door`
- `talk_gesture_a/b`
- `sit_down`
- `sit_idle`
- `stand_up`
- `vehicle_enter_l`
- `vehicle_exit_l`
- `vehicle_drive_idle`
- `vehicle_steer_l/r`

### 4.5 Boss 专属

薇雯：

- `vivian_intro`
- `vivian_fireball`
- `vivian_sweep`
- `vivian_lava_cast`
- `vivian_stagger`
- `vivian_death`

米拉：

- `mira_intro`
- `mira_vanish`
- `mira_clone`
- `mira_dagger_combo`
- `mira_reveal`
- `mira_death`

目标总量约 65–80 个 Clip。允许通过镜像、Additive、IK、程序化朝向和商业动画包重定向减少原创工作量，但必须核验授权。

## 5. 角色模型

### 5.1 玩家与关键 NPC

| 资产 | 路径 | 方案 | 状态 | 优先级 |
|---|---|---|:--:|:--:|
| Kael | `models/char/kael.jce_mesh` | `BagMan.glb` 补绑骨、材质、LOD | 🔶 | R0 |
| Sera | `models/char/npc_sera.jce_mesh` | 模块化人形 | ⬜ | R0 |
| Haro | `models/char/npc_haro.jce_mesh` | 通用男体 + 独立头/服装 | ⬜ | R0 |
| Rat | `models/char/npc_rat.jce_mesh` | 通用男体 + 独立头/服装 | ⬜ | R0 |
| Lily | `models/char/npc_lily.jce_mesh` | 通用女体 + 独立头/服装 | ⬜ | R0 |

### 5.2 平民模块

最低：

- 男体 1；
- 女体 1；
- 头部 4–6；
- 发型 6；
- 上衣 6；
- 下装 4；
- 鞋 3；
- 材质换色 8 组；
- 可选恶魔角/耳/皮肤附件 4。

目标通过组合得到 20+ 视觉变体，而不是制作 20 个独立角色。

### 5.3 敌人

| 原型 | 路径 | 复用方式 |
|---|---|---|
| 街头守卫 | `models/enemy/guard_street.jce_mesh` | 共享人形 |
| 符文弩手 | `models/enemy/guard_ranged.jce_mesh` | 守卫体 + 弩 |
| 重装骑士 | `models/enemy/knight_heavy.jce_mesh` | 独立装甲附件 |
| 焰魔士兵 | `models/enemy/flame_soldier.jce_mesh` | 人形 + 火焰材质 |
| 影刺客 | `models/enemy/shadow_assassin.jce_mesh` | 人形 + 幻影材质 |

### 5.4 Boss

| Boss | 路径 | 方案 |
|---|---|---|
| 薇雯 | `models/boss/witch_flame.jce_mesh` | 人形骨架、独立服装和火焰附件 |
| 米拉 | `models/boss/witch_phantom.jce_mesh` | 人形骨架、面具/披风/幻影材质 |

`sachiel.obj` 异形真身标记为 R2 后续增强。

### 模型验收

- 正确比例和面朝向；
- 骨架绑定；
- UV/法线；
- Toon 材质；
- 简化碰撞；
- LOD；
- 无版权问题；
- 可在正式场景加载。

## 6. 车辆

| 资产 | 路径 | 状态 | 优先级 |
|---|---|:--:|:--:|
| 魔法马车 | `models/vehicle/cart_magic.jce_mesh` | ⬜ | R0 |
| 符文车 | `models/vehicle/runecar.jce_mesh` | ⬜ | R0 |

每辆至少包含车身、可转轮、灯光/符文发光材质、碰撞体、LOD0–LOD2、驾驶座挂点、货物挂点、音频锚点和粒子锚点。

## 7. 武器与投射物

| 资产 | 路径 | 优先级 |
|---|---|:--:|
| 木棍 | `models/weapon/stick.jce_mesh` | R0 |
| 铁棍 | `models/weapon/rod.jce_mesh` | R1，可与木棍共网格换材质 |
| 短剑 | `models/weapon/shortsword.jce_mesh` | R0 |
| 符文短弩 | `models/weapon/crossbow.jce_mesh` | R0 |
| 弩箭 | `models/projectile/bolt.jce_mesh` | R0 |
| 符文枪 | `models/weapon/runegun.jce_mesh` | R0 |

拳套无独立模型。每件武器需要手持/背部挂点、材质、UI 图标、地面拾取姿态、命中定义和武器数据资产。

## 8. 环境套件

### A 区废墟套件

建议约 40–45 件：

- 地块 6；
- 完整墙 4；
- 破墙 6；
- 拱门 3；
- 柱 3；
- 断桥/台阶 4；
- 瓦砾堆 6；
- 帐篷 3；
- 木栅 3；
- 难民火堆 2；
- 抵抗军标志 3；
- 远景建筑 4。

### B 中央集市套件

约 50–60 件：

- 路面 8；
- 人行道 6；
- 建筑立面 8；
- 转角建筑 4；
- 门窗模块 8；
- 摊位 6；
- 布棚 5；
- 招牌 8；
- 传送阵基座 1；
- 电台塔/中继 2。

### 西部工业套件

约 50 件：

- 仓库外壳 4；
- 熔炉 3；
- 管道直/弯/三通 10；
- 铁架 6；
- 平台与楼梯 8；
- 烟囱 3；
- 熔岩沟 4；
- 冷却管 4；
- 路障 5；
- Boss 场地模块 8。

### 剧院与住宅套件

约 50 件：

- 住宅立面 8；
- 剧院立面 1；
- 剧院内部模块 12；
- 后台模块 8；
- 舞台 3；
- 面具/旗帜/灯牌 10；
- 小巷模块 6；
- 安全屋外壳 2。

统一要求：网格化尺寸、Pivot 一致、可吸附、LOD、碰撞、Toon 材质、遮挡测试和 Streaming Cell 归属。

## 9. 室内资产

| 室内 | 最低资产 |
|---|---|
| A 安全屋 | chalet 加工、床、存档台、箱子、灯 |
| Haro 改装铺 | 工作台、武器架、车位、工具、符文设备 |
| Rat 黑市 | 柜台、任务板、货架、帘门、违禁品 |
| 竞技场 | 擂台、闸门、看台、计分牌 |
| B 安全屋 | 床、装备架、存档台、车位 |
| 剧院后台 | 化妆镜、道具箱、布景、后台走廊 |

每个室内先完成灰盒和玩法，再增加装饰。

## 10. 通用道具

首发建议 50–70 件，允许材质变体：

木箱、木桶、货箱、火把、路灯、符文灯、桌椅、床、柜、货架、摊位商品、瓶罐、绳索、路障、标牌、海报、武器架、工具、熔炉容器、舞台道具、面具、垃圾堆、难民用品、抵抗军旗、女魔王旗和任务货物。

## 11. 拾取与交互物

| 资产 | 路径 | 优先级 |
|---|---|:--:|
| 晶币 | `models/pickup/crystal.jce_mesh` | R0 |
| 王玺金 | `models/pickup/kingmark.jce_mesh` | R0 |
| 治疗瓶 | `models/pickup/heal.jce_mesh` | R0 |
| 弹药包 | `models/pickup/ammo.jce_mesh` | R0 |
| 任务货箱 | `models/pickup/mission_crate.jce_mesh` | R0 |
| 王国遗物 | `models/pickup/relic.jce_mesh` | R1 |
| 电台碎片 | `models/pickup/radio_fragment.jce_mesh` | R1 |

## 12. 材质与纹理

风格：

- Toon；
- 高饱和；
- 清晰明暗分区；
- 避免写实噪声；
- 统一描边或 Fresnel；
- 皮肤、布料、金属、石材和符文发光有固定参数范围。

纹理：

- Albedo；
- Normal；
- ORM；
- 可选 Emissive；
- 角色 2K，英雄可 4K 但必须评估；
- 环境 1K–2K；
- 道具 512–1K；
- BC7/ASTC/ETC2；
- DirectX 法线约定。

材质控制：

- 玩家 3–5；
- 关键 NPC 2–4；
- 普通敌人共享；
- 每环境套件 10–20 主材质；
- 道具 Atlas；
- Decal 合图。

## 13. VFX

### 通用战斗

普通命中、重击、格挡、闪避残影、击杀、拾取、治疗、任务完成、Wanted 升级、弩箭命中、枪口和弹着。

### 火焰族

火球、地面火区、熔岩、薇雯护盾、冷却破盾、Boss 阶段转换和焰魔身火。

### 幻影族

隐身、分身、传送、暗场、真身揭露和米拉死亡。

### 载具与环境

轮尘、刹车尘、碰撞火花、车辆损坏烟、符文推进、火把、烟囱、环境尘和雾。

目标约 25–35 个正式 VFX 定义。

## 14. 音频

### 环境

A 废墟、难民营、B 集市昼/夜、西区工业、剧院外/内、安全屋、改装铺、黑市、竞技场和两个 Boss 场。

### 战斗 SFX

徒手、钝器、短剑、短弩、符文枪、受击、格挡、闪避、死亡、火焰和幻影。

### 载具

每辆至少包含 Idle、Low、High、加速、刹车、碰撞轻/重、损坏、进入/退出和符文启动。

### UI

菜单、确认、取消、购买、错误、任务、Wanted、收集和存档。

### 音乐

最低：

- 3 电台 × 3 首 = 9；
- 主菜单 1；
- A 区氛围 1；
- 城市任务循环 2；
- 追逐 1；
- 薇雯 1；
- 米拉 1；
- 结局 2，可为主题变体。

约 17–18 个音乐条目。首发不做全语音，只制作 Boss 短句、守卫 Barks 和主持人口播，所有音频必须确认商用权。

## 15. UI 与图标

页面：

- 主菜单；
- 新游戏/继续；
- 存档；
- 设置；
- 暂停；
- HUD；
- 地图；
- 任务日志；
- 物品栏；
- 装备；
- 商店；
- 改装；
- 安全屋；
- 电台；
- 结局；
- Credits；
- 错误/确认。

HUD：

- 生命；
- 弹药；
- 当前武器；
- 晶币；
- 王玺金；
- Wanted 1–5；
- 任务目标；
- 小地图；
- 载具生命/速度；
- Boss 生命；
- 交互提示；
- 字幕。

图标约 70–100 个。字体必须覆盖中文和拉丁，使用 OFL 或自有版权，并有缺字回退。

## 16. 天空、灯光与后处理

A 区：

- 黄昏 HDR；
- 暖橙主光；
- 暖灰雾；
- 中 Bloom；
- 轻颗粒。

B 区首发只做白天、黄昏、夜晚三套关键状态，而不是完整连续物理天空。需要城市天空、夜间路灯、霓虹、西区红橙工业和剧院紫金基调。

## 17. Prefab

角色与敌人：

- `prefab/char/player_kael`
- `prefab/char/npc_sera`
- `prefab/char/npc_haro`
- `prefab/char/npc_rat`
- `prefab/char/npc_lily`
- `prefab/char/civilian`
- `prefab/enemy/guard`
- `prefab/enemy/ranged`
- `prefab/enemy/knight`
- `prefab/enemy/flame`
- `prefab/enemy/assassin`
- `prefab/boss/vivian`
- `prefab/boss/mira`

载具：

- `prefab/vehicle/cart_magic`
- `prefab/vehicle/runecar`

系统：

- Pickup；
- Mission Trigger；
- Area Trigger；
- Door；
- Spawn Point；
- Vendor；
- Radio Zone；
- Safehouse；
- Wanted Spawn；
- Checkpoint；
- Streaming Cell；
- Save Point；
- Boss Barrier。

## 18. 总量摘要

| 类别 | 首发目标 |
|---|---:|
| 骨架 | 1 |
| 动画 Clip | 65–80 |
| 独立关键角色 | 7 |
| 模块化平民基础体 | 2 + 组件 |
| 普通敌人原型 | 5 |
| Boss 模型 | 2 |
| 载具 | 2 |
| 武器/投射物 | 6 |
| 环境套件 | 4 主套件 |
| 室内 | 6 |
| 通用道具 | 50–70 |
| 拾取物 | 7 |
| VFX | 25–35 |
| 音乐 | 17–18 |
| SFX | 120–180 |
| UI 页面 | 15+ |
| UI 图标 | 70–100 |
| 天空/光照状态 | A 1 套，B 3 套 |

## 19. 可降级方案

进度不足时可：

- 平民变体 20→10；
- 减少建筑内部装饰；
- 电台每频道 3→2 首；
- 第二死亡动画延期；
- Boss 过场改为引擎内镜头；
- B 区昼夜改为黄昏/夜两个状态；
- 收集物 50→20；
- 米拉异形真身取消；
- 语音只保留 Boss 和电台短句。

不得降级：主线关键模型、武器命中反馈、Boss 攻击可读性、HUD、存档、任务标识、碰撞/LOD和版权记录。