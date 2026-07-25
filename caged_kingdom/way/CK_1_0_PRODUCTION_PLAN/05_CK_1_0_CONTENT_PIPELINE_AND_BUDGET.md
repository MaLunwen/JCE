# Caged Kingdom 1.0 — 内容管线与预算

## 1. 资产流程

```text
需求登记
→ 概念/参考
→ 灰盒
→ 高低模/拓扑
→ UV/纹理/材质
→ 骨架/动画
→ 碰撞/LOD
→ 导入
→ Prefab
→ 场景验证
→ 性能验证
→ License 与来源归档
→ PAK
```

任何资产不得跳过归属、License、运行时验证、性能和命名检查。

## 2. 源格式与引擎格式

| 类别 | 源格式 | 编译 | 运行时 |
|---|---|---|---|
| 网格 | glTF/GLB，OBJ 仅遗留 | Assimp/cgltf + meshoptimizer | `.jce_mesh` |
| 骨架/动画 | glTF/FBX | ozz-animation | `.ozz` |
| 纹理 | PNG/TGA/EXR/HDR | Texture Tool | `.jce_tex` |
| 材质 | 编辑器数据 | JCE | `.mat` |
| 音频 | WAV | 转码 | Ogg/Opus |
| UI | PNG/Aseprite/JSON | Sprite pipeline | `.jce_sprite` |
| 场景 | Editor | Serializer | `.scene.json` |
| Prefab | Editor | Serializer | `.prefab.json` |

运行时不得读取 DCC 源文件。

## 3. 命名

场景：

- `area_a_hub_ruin.scene.json`
- `area_b_hub_market.scene.json`
- `act1_m01_wake.scene.json`
- `int_garage_haro.scene.json`
- `boss_vivian_west.scene.json`

模型：

- `models/char/kael.jce_mesh`
- `models/enemy/guard_street.jce_mesh`
- `models/env/ruin_wall_broken_01.jce_mesh`

材质与纹理：

- `materials/char/kael_body.mat`
- `textures/char/kael_body_albedo.jce_tex`
- `textures/char/kael_body_normal.jce_tex`
- `textures/char/kael_body_orm.jce_tex`

## 4. 性能预算

目标最低配置暂定：

- Windows 10/11；
- 4 核 CPU；
- 8 GB RAM；
- 集显或较低端独显；
- 720p/30 FPS Low。

推荐暂定：

- 6 核 CPU；
- 16 GB RAM；
- GTX 1060/1650 或同级；
- 1080p/60 FPS Medium。

最终配置必须实测后冻结。

### 4.1 帧预算

60 FPS：

- 总帧：16.67 ms；
- 游戏逻辑：≤3 ms；
- 物理：≤2 ms；
- AI：≤2 ms；
- 渲染提交：≤3 ms；
- GPU：≤14 ms；
- Streaming Pump：平均 ≤0.5 ms，P99 ≤2 ms。

30 FPS Low：

- 不允许持续超过 33.3 ms；
- Streaming 不应造成明显超过 100 ms 的停顿。

### 4.2 内存

建议：

- Low 游戏进程 ≤3 GB；
- 推荐场景 ≤5 GB；
- Low GPU 纹理预算 ≤1.5 GB；
- 当前分区 + 相邻分区常驻；
- 远区卸载；
- 音乐流式；
- 不一次解压全部 PAK。

## 5. 网格预算

角色：

- Kael/Boss LOD0：50k–100k triangles；
- 关键 NPC：30k–60k；
- 普通 NPC/敌人：20k–50k；
- LOD1 约 50%；
- LOD2 约 20%。

载具：

- LOD0：30k–70k；
- LOD1：50%；
- LOD2：20%。

环境：

- 单模块尽量低于 20k；
- 大建筑拆模块；
- 重复件实例化；
- 禁止城市成为单一网格。

## 6. 材质与 Draw Call

- 环境使用 Atlas；
- 平民使用材质变体；
- 相同 Shader 统一参数；
- 普通道具尽量 1 材质；
- 角色 2–4 材质；
- 不为颜色变体复制 Shader；
- 透明材质严格控制；
- Emissive 和 Decal 有独立预算。

每个区域建立 Draw Call 与 GPU 时间基线。

## 7. 碰撞

- 角色：Capsule；
- 车辆：简化 Compound；
- 建筑：盒/凸包；
- 道具：简单碰撞；
- 不用渲染网格作全量复杂碰撞；
- Trigger 与实体碰撞层分离；
- Navmesh 障碍明确；
- 任务目标不能被物理推入不可恢复位置。

## 8. LOD 与流式

B 区建议 Cell：

- 中央集市；
- 西区；
- 剧院住宅；
- 过渡道路；
- 室内独立加载。

规则：

- 玩家 Cell 常驻；
- 相邻 Cell 预取；
- 远区卸载；
- 任务目标 Cell 提前锁定；
- Boss 场独立；
- 卸载前保存必要状态；
- 远距建筑使用 HLOD。

## 9. 版权与来源

每个外部资产记录：

- 原始名称；
- 作者；
- 来源 URL；
- 下载日期；
- License；
- 商用许可；
- 修改要求；
- 署名要求；
- 原文件哈希；
- 项目虚拟路径。

禁止来源不明模型、从商业游戏提取资产、无商业授权音乐、不明声音克隆、盗版字体和仅写“网络下载”。

## 10. 完成定义

### 模型

形体、拓扑、UV、材质、LOD、碰撞、Pivot、Scale、导入、场景表现、性能和 License 全部通过。

### 动画

骨架、循环、Root Motion、事件、Blend、武器挂点、Retarget、30/60 FPS 全部通过。

### 音频

无削波、循环点、Loudness、空间化、压缩、分组、License和字幕对应通过。

### UI

720p/1080p/1440p、16:9、缩放、键鼠、手柄、字体缺字、安全区、颜色可读和焦点导航通过。

## 11. 灰盒优先

顺序：

1. Graybox；
2. Gameplay；
3. 测试；
4. 最终美术；
5. Polish。

禁止先完成高精度 Boss 模型，再发现 Boss 场机制不可玩。

## 12. 变更控制

任何资产扩展提交必须包含对应玩法/任务、首次出现、复用范围、路径、预算、优先级、来源和新增成本。不接受“以后可能用到”作为 R0/R1 理由。