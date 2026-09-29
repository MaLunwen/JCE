# Caged Kingdom — 资产明细清单 / Asset Manifest v1.0

> 本文件是 **生产层**清单，扩展 `SETTING_BIBLE` 的 §13（资产→场景映射）。
> 设计层权威仍是 `SETTING_BIBLE`；本清单从其 canon 派生，**不得反向定义设定**。
> 任何新资产进 PAK 前，先在 `SETTING_BIBLE §6` / 本清单登记归属（沿用既有纪律）。
> 状态图例：✅ 已有 ／ ⬜ 待制 ／ 🔶 已有但需补工（如 .obj 需重拓扑/绑骨）。
> 优先级：**P0**＝垂直切片一必需（A 区废墟 + m01→m03）；**P1**＝切片二（B 枢纽一分区 + 首个 Boss）；**P2**＝主线铺量；**P3**＝后期/收尾。

---

## 0. 怎么读这份清单（三个前提）

**前提一：资产管线（源格式 → 引擎格式）。** 美术产出的是源文件，进引擎前由工具编译为引擎专用格式，运行时只读编译产物（与归档/VFS 一致）。各类映射如下。

| 类别 | 源格式（DCC 产出） | 编译工具 | 引擎格式 | 备注 |
|---|---|---|---|---|
| 网格 | `.glb` / `.gltf` / `.obj` | assimp + meshoptimizer + LOD | `.jce_mesh` | 静态/蒙皮均可；自动生成 LOD |
| 骨架 | glTF skeleton | ozz-animation | `.ozz`(skeleton) | 一套骨架被多角色共享 |
| 动画 | glTF/FBX clip | ozz-animation | `.ozz`(clip) | **按骨架分组复用**，见 §2 |
| 纹理 | `.png`（albedo/normal/ORM 三合一） | texconv | `.jce_tex`(BC7/ASTC/ETC2) | DirectX 法线约定 |
| 材质 | — | 编辑器 | `.mat` | 引用纹理 + shader |
| 精灵/UI 图 | `.png` + `.json`(Aseprite) | — | `.jce_sprite` | 2D/HUD |
| 特效 | 粒子/着色定义 | bgfx shaderc | `.bin` + vfx 定义 | 见 §8 |
| 音频 | `.wav` | — | `.ogg` / `.opus` | 免版税基线 |
| 字体 | `.ttf`(OFL) | — | 直接打包 | 仅用 OFL/CC0/自制 |

**前提二：虚拟路径根布局**（小写、正斜杠，与引擎归一化一致）。

```
examples/caged_kingdom/resources/assets/
├── models/      (.jce_mesh)
├── skeletons/   (.ozz skeleton)
├── anims/       (.ozz clip，按骨架分子目录)
├── textures/    (.jce_tex)
├── materials/   (.mat)
├── vfx/         (特效定义)
├── audio/       (amb/  music/  sfx/  vo/)
├── ui/          (hud/  icon/  font/)
├── sky/         (HDR skybox)
├── prefab/      (.prefab.json，见 BIBLE §12)
└── scenes/      (.scene.json，见 BIBLE §7)
```

**前提三：共享骨架经济学（最重要）。** 不要给每个角色单独做动画。下表骨架被一组角色共享，动画一次制作、全员复用。这是把"几十个角色"的动画工作量压到可行范围的唯一办法。

| 骨架 ID | 共享角色 | 动画来源 |
|---|---|---|
| `sk_humanoid` | Kael、全部人形 NPC、人形敌人、人形态女魔王 | §2.1 人形动画库（做一次） |
| `sk_quadruped` | 战狼、巨蜥坐骑、霜狼、火蜥蜴 | §2.2 四足库 |
| `sk_winged` | 飞龙坐骑、飞行恶魔、雷鸢 | §2.3 翼族库 |
| `sk_giant` | 石巨人、（树魔可复用+改） | §2.4 巨型库 |
| `sk_unique_*` | 每个有独特形体的 Boss（米拉/sachiel、树魔等） | §2.5 各 Boss 专属 |

---

## 1. 骨架库（Skeletons）— 先做这一层

骨架是动画的地基；先定骨架，动画和蒙皮才能落地。人形骨架是重中之重，全游戏大半角色都绑它。

| 骨架 | 路径 | 用于 | 复杂度 | 状态 | 优先级 |
|---|---|---|---|:--:|:--:|
| 人形 | `skeletons/sk_humanoid.ozz` | Kael + 人形 NPC/敌人/女魔王 | ~55 骨，含手指、武器挂点(右手/左手/背) | ⬜ | **P0** |
| 四足 | `skeletons/sk_quadruped.ozz` | 战狼/巨蜥/霜狼/火蜥蜴 + 骑乘点 | ~35 骨 | ⬜ | P1 |
| 翼族 | `skeletons/sk_winged.ozz` | 飞龙/飞行恶魔/雷鸢 + 骑乘点 | ~45 骨，含翼/尾 | ⬜ | P2 |
| 巨型 | `skeletons/sk_giant.ozz` | 石巨人/树魔 | ~30 骨 | ⬜ | P2 |
| 米拉(异形) | `skeletons/sk_unique_phantom.ozz` | 米拉真身（`sachiel.obj` 形体） | 自定义 | ⬜ | P2 |

> 武器挂点（socket）规范：人形骨架须含 `socket_hand_r`、`socket_hand_l`、`socket_back`、`socket_hip_l`，供武器/工具吸附（见 §6）。

---

## 2. 动画片段库（Animation Clips）— 你点名要的核心

按骨架分组。每个片段标注：是否循环（loop）、复用范围、优先级。**人形库做一次，复用到所有人形角色**。

### 2.1 人形动画库（`anims/humanoid/`）— 绑 `sk_humanoid`

人形动画再细分四组：移动、近战、远程、通用/载具。武器不同会改变"持握待机 + 攻击"动作，但移动是共享的——所以按"武器套"组织攻击动作，移动只做一份。

**A. 移动（全角色共享，最高优先）**

| 片段 | loop | 说明 | 优先级 |
|---|:--:|---|:--:|
| `idle` | ✓ | 站立待机 | **P0** |
| `idle_look` / `idle_shift` | ✓ | 待机变体（环顾/换重心），防呆板 | P1 |
| `walk_fwd` | ✓ | 前行 | **P0** |
| `walk_back` | ✓ | 后退 | P1 |
| `walk_strafe_l` / `_r` | ✓ | 横移 | P1 |
| `run_fwd` | ✓ | 奔跑 | **P0** |
| `sprint` | ✓ | 冲刺 | P1 |
| `turn_l90` / `turn_r90` / `turn_180` | ✗ | 原地转向 | **P0**(至少 90°) |
| `move_start` / `move_stop` | ✗ | 起步/急停过渡 | P1 |
| `jump_start` / `jump_loop` / `jump_land` | 部分 | 跳跃三段 | P1 |
| `crouch_idle` / `crouch_walk` | ✓ | 潜行（`m13` 幻影潜入需要） | P1 |
| `dodge_roll_back`（或四向） | ✗ | 闪避翻滚 | P1 |
| `fall_loop` / `hard_land` | 部分 | 坠落/落地 | P2 |

**B. 近战套（按武器分套，各套含自己的待机+攻击）**

| 套 | 片段 | 说明 | 优先级 |
|---|---|---|:--:|
| 徒手 `unarmed_` | `combat_idle`, `atk_1`, `atk_2`, `atk_3`(三连), `atk_heavy` | 开场拳脚 | **P0** |
| 钝器 `blunt_` | `combat_idle`, `atk_1`, `atk_2`, `atk_heavy` | 木棍/铁棍 | P1 |
| 利刃 `blade_` | `combat_idle`, `atk_1`, `atk_2`, `atk_3`, `atk_heavy` | 短剑/长剑/符文剑 | P1 |
| 通用受击 `hit_` | `hit_front`, `hit_back`, `hit_l`, `hit_r`, `stagger`, `block`, `parry` | 全近战共享 | **P0**(front+死亡) |
| 倒地 | `knockdown`, `getup`, `death_a`, `death_b` | 全角色共享 | **P0** |

**C. 远程套（按武器分套）**

| 套 | 片段 | 说明 | 优先级 |
|---|---|---|:--:|
| 短弩 `xbow_` | `aim_idle`, `aim_walk`, `fire`, `reload`, `draw`, `holster` | 远程起手 | P1 |
| 符文枪 `gun_` | `aim_idle`, `aim_walk`, `fire`, `reload`, `draw`, `holster` | hitscan | P2 |
| 法杖 `staff_` | `aim_idle`, `cast_a`, `cast_b`, `channel`, `draw`, `holster` | 弹道+魔法 | P2 |

**D. 通用 / 交互 / 载具（全角色共享）**

| 片段 | 说明 | 优先级 |
|---|---|:--:|
| `interact_pickup` / `interact_use` / `open_door` | 拾取/使用/开门 | **P0**(pickup) |
| `sit_down` / `sit_idle` / `stand_up` | 坐（NPC、对话） | P1 |
| `talk_gesture_a/b` | 说话手势（NPC 用） | P1 |
| `vehicle_enter_l` / `_r` / `vehicle_exit` | 上/下车 | **P0**(enter+exit) |
| `vehicle_drive_idle` / `vehicle_steer_l` / `_r` | 驾车 | **P0**(idle) |
| `mount_get_on` / `mount_ride_idle` / `mount_gallop_pose` | 骑乘坐姿（配 §2.2 坐骑动作） | P1 |
| `fly_ride_idle` / `fly_ride_fwd` | 骑飞行物坐姿 | P2 |
| `emote_taunt` / `emote_cheer` | 嘲讽/庆祝（黑色幽默调性） | P3 |

> 人形库总量：移动 ~16 + 近战 ~22 + 远程 ~16 + 通用 ~14 ≈ **~68 片段**；其中 **P0 约 18 片段**即可支撑切片一。

### 2.2 四足动画库（`anims/quadruped/`）— 绑 `sk_quadruped`

| 片段 | loop | 说明 | 优先级 |
|---|:--:|---|:--:|
| `idle`, `walk`, `run_gallop` | ✓ | 基础移动 | P1 |
| `atk_bite`, `atk_pounce` | ✗ | 攻击（敌对个体） | P1 |
| `hit`, `death` | ✗ | 受击/死亡 | P1 |
| `ridden_idle`, `ridden_gallop` | ✓ | 被骑乘（坐骑用，与 Kael `mount_*` 同步） | P1 |

> ~9 片段。火蜥蜴/霜狼为敌对版（用 atk），战狼/巨蜥为坐骑版（用 ridden）。

### 2.3 翼族动画库（`anims/winged/`）— 绑 `sk_winged`

| 片段 | loop | 说明 | 优先级 |
|---|:--:|---|:--:|
| `idle_ground`, `takeoff`, `fly_loop`, `fly_fwd`, `land` | 部分 | 飞行循环 | P2 |
| `fly_atk_swoop`, `breath_attack` | ✗ | 俯冲/吐息（飞龙/泽芙兵种） | P2 |
| `hit_air`, `death_fall` | ✗ | 空中受击/坠落 | P2 |
| `ridden_fly_idle`, `ridden_fly_fwd` | ✓ | 飞龙坐骑被骑乘 | P2 |

> ~11 片段。

### 2.4 巨型动画库（`anims/giant/`）— 绑 `sk_giant`

| 片段 | 说明 | 优先级 |
|---|---|:--:|
| `idle`, `walk`, `slam`, `ground_pound`, `hit`, `death` | 石巨人/树魔（格罗兵种） | P2 |

> ~6 片段。树魔可复用并改根/枝动作。

### 2.5 Boss 专属动画（`anims/boss/<witch>/`）— 各 Boss 单独

每位女魔王需一套专属招式，按 §2.3 武器特征。人形态女魔王（薇雯/诺尔/泽芙/索菈/泽尼丝）可**复用 `sk_humanoid` 移动**，仅做专属攻击；米拉用 `sk_unique_phantom`。

| Boss | 专属片段（示意） | 绑骨架 | 优先级 |
|---|---|---|:--:|
| 薇雯(烈焰) | `intro`, `cast_fireball`, `lava_summon`, `phase2`, `hit`, `death` | sk_humanoid | P1 |
| 米拉(幻影) | `intro`, `vanish`, `clone_spawn`, `dagger_flurry`, `reveal`, `death` | sk_unique_phantom | P2 |
| 诺尔(寒霜) | `cast_freeze`, `ice_spike`, `phase2`, `hit`, `death` | sk_humanoid | P2 |
| 格罗(大地) | `slam`, `quake`, `summon_golem`, `hit`, `death` | sk_giant 或 humanoid | P2 |
| 索菈(血潮) | `drain`, `poison_mist`, `summon_parasite`, `death` | sk_humanoid | P2 |
| 泽芙(风暴) | `lightning`, `tornado_aoe`, `fly`, `death` | sk_humanoid+winged | P2 |
| 泽尼丝(终焉) | `time_warp`, `mixed_cast`, `phase×3`, `final` | sk_humanoid | P3 |

> Boss 动画 ~6/位 × 7 ≈ **~42 片段**（分布在 P1–P3）。

---

## 3. 角色模型（Characters）

### 3.1 玩家与剧情 NPC

| 资产 | 路径 | 绑骨架 | 动画 | LOD | 状态 | 优先级 | 归属 |
|---|---|---|---|:--:|:--:|:--:|---|
| Kael（玩家） | `models/char/kael.jce_mesh`（源 `BagMan.glb`） | sk_humanoid | 人形全库 | 3 | 🔶(需确认是否带骨/补绑) | **P0** | 玩家 |
| 塞拉 Sera | `models/char/npc_sera.jce_mesh` | sk_humanoid | idle/talk/walk/sit | 2 | ⬜ | P1 | 抵抗军王女 |
| 老符匠 哈罗 | `models/char/npc_haro.jce_mesh` | sk_humanoid | idle/talk/work | 2 | ⬜ | P1 | 改装铺 |
| 黑市头子 老鼠 | `models/char/npc_rat.jce_mesh` | sk_humanoid | idle/talk | 2 | ⬜ | P1 | 黑市 |
| 流浪诗人 莉莉 | `models/char/npc_lily.jce_mesh` | sk_humanoid | idle/talk/sit | 2 | ⬜ | P2 | 电台/支线 |
| 白龙后裔 维尔 | `models/char/npc_vel.jce_mesh` | sk_humanoid | idle/talk/过场 | 2 | ⬜ | P2 | 三幕揭示 |

### 3.2 平民（人群，可换色复用）

| 资产 | 路径 | 绑骨架 | 动画 | 状态 | 优先级 |
|---|---|---|---|:--:|:--:|
| 人类平民（男/女各一，材质换色） | `models/char/civ_human_m/f.jce_mesh` | sk_humanoid | 移动+flee+sit+talk | ⬜ | P1 |
| 恶魔平民（人魔混居） | `models/char/civ_demon.jce_mesh` | sk_humanoid | 同上 | ⬜ | P2 |

> 人群靠"少量模型 × 多套换色材质 × 共享动画"营造密度，勿逐个建模。

### 3.3 敌人 / 治安（5 级 Wanted + 各区兵种）

| 资产 | 路径 | 绑骨架 | 动画 | 状态 | 优先级 |
|---|---|---|---|:--:|:--:|
| 街头守卫(W1) | `models/enemy/guard_street.jce_mesh` | sk_humanoid | 复用近战库 | ⬜ | **P0** |
| 重装符文骑士(W2) | `models/enemy/knight_rune.jce_mesh` | sk_humanoid | 复用近战库 | ⬜ | P1 |
| 焰魔士兵(W3/西区) | `models/enemy/flame_soldier.jce_mesh` | sk_humanoid | 近战+投火 | ⬜ | P1 |
| 飞龙巡查(W4) | `models/enemy/dragon_patrol.jce_mesh` | sk_winged | 翼族库 | ⬜ | P2 |
| 女魔王亲卫(W5) | `models/enemy/witch_guard.jce_mesh` | sk_humanoid | 精英近战 | ⬜ | P2 |
| 火蜥蜴(西区) | `models/enemy/fire_lizard.jce_mesh` | sk_quadruped | 四足敌对 | ⬜ | P1 |
| 影刺客(剧院区) | `models/enemy/shadow_assassin.jce_mesh` | sk_humanoid | 近战+潜行 | ⬜ | P2 |
| 人偶(剧院区) | `models/enemy/puppet.jce_mesh` | sk_humanoid | 近战 | ⬜ | P2 |
| 霜狼(冰川) | `models/enemy/frost_wolf.jce_mesh` | sk_quadruped | 四足敌对 | ⬜ | P2 |
| 石巨人/树魔(森林) | `models/enemy/stone_golem.jce_mesh` | sk_giant | 巨型库 | ⬜ | P2 |
| 吸血恶魔/寄生体(沼泽) | `models/enemy/blood_fiend.jce_mesh` | sk_humanoid | 近战+吸血 | ⬜ | P2 |
| 飞行恶魔/雷鸢(空港) | `models/enemy/storm_kite.jce_mesh` | sk_winged | 翼族库 | ⬜ | P2 |

### 3.4 女魔王 Boss（英雄级资产，各唯一）

| Boss | 路径 | 绑骨架 | 动画 | 状态 | 优先级 |
|---|---|---|---|:--:|:--:|
| 薇雯 烈焰 | `models/boss/witch_flame.jce_mesh` | sk_humanoid | §2.5 薇雯 | ⬜ | P1 |
| 米拉 幻影 | `models/boss/witch_phantom.jce_mesh`（源 `sachiel.obj`） | sk_unique_phantom | §2.5 米拉 | 🔶(需重拓扑+绑骨) | P2 |
| 诺尔 寒霜 | `models/boss/witch_frost.jce_mesh` | sk_humanoid | §2.5 诺尔 | ⬜ | P2 |
| 格罗 大地 | `models/boss/witch_earth.jce_mesh` | sk_giant/humanoid | §2.5 格罗 | ⬜ | P2 |
| 索菈 血潮 | `models/boss/witch_blood.jce_mesh` | sk_humanoid | §2.5 索菈 | ⬜ | P2 |
| 泽芙 风暴 | `models/boss/witch_storm.jce_mesh` | sk_humanoid+winged | §2.5 泽芙 | ⬜ | P2 |
| 泽尼丝 终焉 | `models/boss/witch_zenith.jce_mesh` | sk_humanoid | §2.5 泽尼丝 | ⬜ | P3 |

---

## 4. 载具与坐骑（Vehicles & Mounts）

| 资产 | 路径 | 类型 | 动画/部件 | 状态 | 优先级 |
|---|---|---|---|:--:|:--:|
| 魔法马车 | `models/vehicle/cart_magic.jce_mesh` | 陆地通勤 | 轮子转动、符文发光 | ⬜ | **P0** |
| 符文车 | `models/vehicle/runecar.jce_mesh` | 陆地高速 | 轮/悬挂 | ⬜ | P2 |
| 战狼坐骑 | （复用 `models/enemy`→坐骑材质）+ sk_quadruped | 骑乘 | §2.2 ridden | ⬜ | P1 |
| 巨蜥坐骑 | `models/vehicle/mount_lizard.jce_mesh` | 骑乘 | §2.2 ridden | ⬜ | P2 |
| 符文船 | `models/vehicle/boat_rune.jce_mesh` | 水上 | 浮动 | ⬜ | P3 |
| 飞龙坐骑 | `models/vehicle/dragon.jce_mesh` | 空中(三幕) | sk_winged ridden | ⬜ | P2 |
| 飞艇 | `models/vehicle/airship.jce_mesh` | 空中(四幕) | 螺旋/帆、多座 | ⬜ | P2 |

---

## 5. 武器与投射物（Weapons & Projectiles）

武器为吸附到 `socket_hand_r`/`_back` 的静态网格；远程附带投射物模型。

| 资产 | 路径 | 挂点 | 投射物 | 状态 | 优先级 |
|---|---|---|---|:--:|:--:|
| 拳套 | （无模型/仅手） | hand | — | ✅(无需) | **P0** |
| 木棍 / 铁棍 | `models/weapon/stick.jce_mesh` / `rod.jce_mesh` | hand_r | — | ⬜ | P1 |
| 短剑 / 长剑 | `models/weapon/shortsword.jce_mesh` / `longsword.jce_mesh` | hand_r/back | — | ⬜ | P1 |
| 符文剑 | `models/weapon/runesword.jce_mesh` | hand_r/back | — | ⬜ | P2 |
| 符文短弩 | `models/weapon/crossbow.jce_mesh` | hand_r | `proj/bolt.jce_mesh` | ⬜ | P1 |
| 投掷符 | `models/weapon/throwrune.jce_mesh` | hand_r | 同模型飞行 | ⬜ | P2 |
| 符文枪 | `models/weapon/runegun.jce_mesh` | hand_r | hitscan(仅枪口VFX) | ⬜ | P2 |
| 法杖 | `models/weapon/staff.jce_mesh` | hand_r | `proj/spellbolt.jce_mesh` | ⬜ | P2 |

---

## 6. 场景 / 建筑模块（Environment Kits，按区）

环境靠**模块化 + 实例化**搭建，下面给"套件"而非逐砖枚举；每套件含若干可复用件。

| 套件 | 路径前缀 | 含件（示意） | 用于场景 | 状态 | 优先级 |
|---|---|---|---|:--:|:--:|
| A 废墟套件 | `models/env/ruin_*` | 残墙、断拱、地块、瓦砾堆、难民营帐篷、木栅 | A 区全部 | ⬜ | **P0** |
| 通用道具 | `models/prop/*` | 火把、路灯、木箱、木桶、招牌、桌椅、货架 | 全区 | ⬜ | **P0**(火把/箱) |
| B 中央集市 | `models/env/b_market_*` | 摊位、布棚、人魔混居民居、传送阵基座 | B 枢纽核心 | ⬜ | P1 |
| B 西区(工业) | `models/env/b_west_*` | 熔炉、管道、铁架、熔岩渗出 | 西区/薇雯 | ⬜ | P1 |
| B 剧院区 | `models/env/b_theater_*` | 剧院立面、招牌、面具、后台、竞技场 | 剧院/米拉/竞技场 | ⬜ | P2 |
| B 空港 | `models/env/b_sky_*` | 飞艇坞、塔台、风向标 | 空港/泽芙 | ⬜ | P2 |
| B 码头 | `models/env/b_dock_*` | 栈桥、货箱、系缆桩 | 码头/走私 | ⬜ | P2 |
| C 浮空遗迹 | `models/env/c_float_*` | 浮岛平台、阶梯、传送门、裂隙水晶 | C 区/阶梯 | ⬜ | P3 |
| 野外·冰川 | `models/env/wild_ice_*` | 冰柱、冻岩、可燃油桶 | 诺尔 | ⬜ | P2 |
| 野外·魔森林 | `models/env/wild_forest_*` | 巨木、藤蔓、菌、地刺点 | 格罗 | ⬜ | P2 |
| 野外·沼泽 | `models/env/wild_swamp_*` | 枯木、水洼、毒气孔 | 索菈 | ⬜ | P2 |

### 6.1 室内场景专用资产

| 室内 | 路径 | 核心件 | 来源 | 状态 | 优先级 |
|---|---|---|---|:--:|:--:|
| 第一安全屋 | `models/int/safehouse_a.jce_mesh` | 猎屋内饰、床、存档台 | `chalet.obj` | 🔶(内饰补件) | P1 |
| 哈罗改装铺 | `models/int/garage.jce_mesh` | 工作台、武器/载具陈列架 | ⬜ | ⬜ | P1 |
| 老鼠黑市 | `models/int/blackmarket.jce_mesh` | 任务板、违禁商品架 | ⬜ | ⬜ | P2 |
| 地下竞技场 | `models/int/arena.jce_mesh` | 擂台、看台、闸门 | ⬜ | ⬜ | P2 |

---

## 7. 拾取物 / 交互件（小物件）

| 资产 | 路径 | 用途 | 状态 | 优先级 |
|---|---|---|---|:--:|
| 晶币 Crystal | `models/pickup/crystal.jce_mesh` | 通用货币拾取 | ⬜ | **P0** |
| 王玺金 KingMark | `models/pickup/kingmark.jce_mesh` | 黑市货币 | ⬜ | P1 |
| 通用道具/货箱货物 | `models/pickup/crate_item.jce_mesh` | 任务货物/走私 | ⬜ | **P0** |
| 快速旅行标记(四阶) | `models/prop/fasttravel_*.jce_mesh` | 驿站/传送阵/飞龙台/飞艇坞 | ⬜ | P1 |

---

## 8. 特效（VFX，按元素族）

特效以 shader + 粒子定义实现；按七女魔王元素族组织，玩家技能/通用共用部分资源。

| VFX 族 | 路径前缀 | 含效（示意） | 关联 | 状态 | 优先级 |
|---|---|---|---|:--:|:--:|
| 通用 | `vfx/common_*` | 拾取闪光、命中火花、血溅、尘、枪口闪 | 全程 | ⬜ | **P0**(命中/拾取) |
| 符文/魔法 | `vfx/rune_*` | 符文辉光、魔法投射、施法环 | 武器/UI | ⬜ | P1 |
| 火/熔岩 | `vfx/fire_*` | 火球、熔岩、燃烧 | 薇雯/西区/火把 | ⬜ | P1 |
| 冰/霜 | `vfx/frost_*` | 冰刺、冻结、寒雾 | 诺尔 | ⬜ | P2 |
| 雷/风暴 | `vfx/storm_*` | 闪电、龙卷 | 泽芙 | ⬜ | P2 |
| 土/震 | `vfx/earth_*` | 地刺、碎石、震波 | 格罗 | ⬜ | P2 |
| 影/幻 | `vfx/shadow_*` | 隐身、分身残影 | 米拉 | ⬜ | P2 |
| 血/毒 | `vfx/blood_*` | 吸血束、毒雾 | 索菈 | ⬜ | P2 |
| 裂隙/时间 | `vfx/rift_*` | 裂隙辉光、时间扭曲 | 泽尼丝/C 区 | ⬜ | P3 |
| 龙息 | `vfx/dragonbreath` | 玩家终极被动 | 三幕解锁 | ⬜ | P3 |

---

## 9. 音频（Audio）

| 资产 | 路径 | 类型 | 来源/授权 | 状态 | 优先级 |
|---|---|---|---|:--:|:--:|
| A 区环境音 | `audio/amb/ruin.ogg` | 环境 | `ambient.ogg` | ✅ | **P0** |
| B 区昼/夜环境音 | `audio/amb/city_day.ogg` / `city_night.ogg` | 环境 | ⬜ 自制/CC0 | ⬜ | P1 |
| C 区/异界环境音 | `audio/amb/rift.ogg` | 环境 | ⬜ | ⬜ | P3 |
| 野外环境音(冰川/森林/沼泽) | `audio/amb/wild_*.ogg` | 环境 | ⬜ | ⬜ | P2 |
| 电台曲库(≥6 频道首曲) | `audio/music/radio_*/` | 音乐 | `AlkaKrab/*` 候选 + OFL/CC0/自制 | 🔶(部分候选) | P1 |
| 战斗 SFX 库 | `audio/sfx/combat/*` | 音效 | ⬜ 自制/CC0 | ⬜ | **P0**(基础打击/受击) |
| 脚步/移动 SFX | `audio/sfx/foot/*` | 音效 | ⬜ | ⬜ | P1 |
| 载具 SFX | `audio/sfx/vehicle/*` | 音效 | ⬜ | ⬜ | P1 |
| 魔法/技能 SFX | `audio/sfx/magic/*` | 音效 | ⬜ | ⬜ | P2 |
| UI SFX | `audio/sfx/ui/*` | 音效 | ⬜ | ⬜ | P1 |
| 语音 VO | `audio/vo/*` | 配音 | ⬜（可后期/留空） | ⬜ | P3 |

> 授权基线：原创音乐须登记版权；公共域频道仅用 CC0；敌方"魔语"无字幕可用合成声。

---

## 10. UI / 2D / 字体

| 资产 | 路径 | 内容 | 状态 | 优先级 |
|---|---|---|---|:--:|
| HUD 套件 | `ui/hud/*` | 生命条、Wanted 星级(5)、小地图框、晶币计数 | ⬜ | **P0** |
| 图标集 | `ui/icon/*` | 武器/物品/任务/货币图标 | ⬜ | P1 |
| 世界标记 | `ui/icon/marker_*` | 任务点/快速旅行/商店 头顶标记 | ⬜ | **P0**(任务点) |
| 菜单/对话框 | `ui/menu/*`（RmlUI `.rml/.rcss`） | 暂停、商店、对话、地图 | ⬜ | P1 |
| 字体·拉丁 | `ui/font/latin.ttf` | 英文/数字（OFL，如 Caveat/Permanent Marker 风） | ⬜ | **P0** |
| 字体·中文 | `ui/font/cjk.ttf` | 中文（OFL，如 霞鹜文楷/思源黑体） | ⬜ | **P0** |

---

## 11. 天空盒 / HDRI（按 BIBLE §11 环境套件）

| 资产 | 路径 | 用于 | 状态 | 优先级 |
|---|---|---|---|:--:|
| A 黄昏 | `sky/a_dusk.hdr` | A 区（固定黄昏） | ⬜ | **P0** |
| B 晴/阴/夜（昼夜循环） | `sky/b_day.hdr` / `b_overcast.hdr` / `b_night.hdr` | B 区 | ⬜ | P1 |
| C 异界 | `sky/c_rift.hdr` | C 区（恒光） | ⬜ | P3 |
| 野外·冰川/森林/沼泽 | `sky/wild_*.hdr` | 三幕野外 | ⬜ | P2 |

---

## 12. 现有资源复用矩阵（沿用 BIBLE §6）

| 现有资源 | 归属 | 编译目标 | 需补工 |
|---|---|---|---|
| `BagMan.glb` | 玩家 Kael | `models/char/kael.jce_mesh` | 确认/补人形骨架 + 蒙皮 |
| `chalet.obj` | 第一安全屋 | `models/int/safehouse_a.jce_mesh` | 补室内件、重拓扑 |
| `sachiel.obj` | 米拉真身 | `models/boss/witch_phantom.jce_mesh` | 重拓扑 + 绑 sk_unique_phantom |
| `ambient.ogg` | A 区环境音 | `audio/amb/ruin.ogg` | 直接用 |
| `AlkaKrab/*` | B 区电台候选 | `audio/music/radio_*/` | 确认授权后选用 |

---

## 13. 垂直切片 P0 清单（先只做这一撮！）

下面是跑通**切片一**（A 区一小片废墟 + `m01_wake`→`m02_first_kill`→`m03_first_run`）所需的全部 P0 资产。做完这一撮，移动/战斗/Wanted/载具四大基础即闭环；**在此之前不要开任何其他区的美术大工程**。

**骨架与动画（最先做）**
- ⬜ `sk_humanoid` 骨架（含武器挂点）
- ⬜ 人形 P0 动画 ~18 片段：`idle`、`walk_fwd`、`run_fwd`、`turn_l90/r90`、`unarmed_combat_idle/atk_1/2/3/heavy`、`hit_front`、`knockdown`、`death_a`、`interact_pickup`、`vehicle_enter/exit`、`vehicle_drive_idle`

**模型**
- 🔶 Kael（`BagMan.glb`→补骨/蒙皮）
- ⬜ 街头守卫 `guard_street`（复用人形动画）
- ⬜ 魔法马车 `cart_magic`
- ⬜ A 废墟套件（残墙/拱/地块/瓦砾/帐篷）
- ⬜ 通用道具：火把、木箱
- ⬜ 拾取物：晶币、货箱货物
- ✅ 拳套（无需模型）

**特效 / 音频 / UI / 天空**
- ⬜ 通用 VFX：命中火花、拾取闪光
- ✅ A 区环境音（`ambient.ogg`）
- ⬜ 基础战斗 SFX（打击/受击）
- ⬜ HUD：生命条、Wanted 星级、晶币计数；任务点头顶标记
- ⬜ 字体：拉丁 + 中文（OFL）
- ⬜ 天空：A 黄昏 HDR

> 这份 P0 清单约 **1 套骨架 + ~18 动画 + ~7 模型 + 少量 VFX/SFX/UI/天空**，是"最小可玩"的资产边界。

---

## 14. 规模速览（全量预估，便于排期）

| 类别 | P0 | 全量预估 |
|---|:--:|:--:|
| 骨架 | 1 | ~5–6 |
| 动画片段 | ~18 | ~135（人形68 + 四足9 + 翼11 + 巨6 + Boss42） |
| 角色模型（玩家/NPC/平民/敌人/Boss） | 2 | ~29 |
| 载具/坐骑 | 1 | ~7 |
| 武器/投射物 | 1 | ~12 |
| 场景模块套件 | 2 | ~11 套（每套多件） |
| 室内模型 | 0 | ~4 |
| 拾取/交互件 | 2 | ~5 |
| VFX 族 | 1 | ~10 |
| 音频条目 | ~3 | ~11 类（每类多文件） |
| UI/字体 | ~4 | ~6 |
| 天空盒 | 1 | ~5–7 |

> 结论：全量是一项巨大的内容工程；**先交付 §13 的 P0 切片**，验证引擎与玩法后再按 BIBLE §15 的优先级横向铺量。每完成一幕做一次资产回归核对，并把新资产回写 `SETTING_BIBLE §6` 登记。

— end —
