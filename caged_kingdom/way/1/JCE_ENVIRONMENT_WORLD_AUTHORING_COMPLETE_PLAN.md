# JCE Environment & World Authoring — 完整技术研究与实施规划

> **目标**：使 JCE Runtime + JCE Editor 具备生产、编辑、保存、流式加载和运行现代开放世界自然环境的完整能力：大气、天空、体积云、雾、天气、光影、地形生成/雕刻/侵蚀、水体、植被、Biome 和程序化装饰。  
> **视觉目标**：RDR2 用作“环境系统整体一致性”的基准；云系统以 Horizon/Nubis 系列作为更高技术参考；水体以 Horizon Forbidden West 为高端参考。  
> **实现约束**：bgfx 是唯一生产 GPU 后端；Runtime 公共 ABI 为 C99；Editor 为 C++20 + Dear ImGui；第三方类型不得穿越 JCE 公共 ABI。  
> **说明**：UE、Frostbite、Decima、RAGE 仅作为公开技术资料参考，不作为依赖。  
> **任务性质**：必须实际实现 Runtime、Editor、资产、Cook、测试、性能验证和独立验收；不能停在 Shader Demo 或 Prototype。

---

# 1. 总体能力架构

```text
                         JCE WORLD ENVIRONMENT
                                  │
                           EnvironmentState
                                  │
        ┌─────────────────────────┼──────────────────────────┐
        │                         │                          │
 Atmosphere / Weather          Terrain                  Water
        │                         │                          │
 Sky / Sun / Moon             Heightfield              Ocean
 Aerial Perspective           Sculpt                   Lake
 Volumetric Fog               Noise                    River
 Volumetric Clouds            Hydraulic Erosion        Local Ripples
 Rain / Snow / Lightning      Thermal Erosion          Buoyancy
 Wind / Humidity              Flow/Sediment Maps       Underwater
        │                         │                          │
        └─────────────────┬───────┴───────────────┬─────────┘
                          │                       │
                       Biome                  Materials
                          │                       │
                 Vegetation / PCG      Wetness / Snow / Dust
                          │                       │
                          └───────────┬───────────┘
                                      ↓
                              Unified Lighting
                                      ↓
                               JCE RenderGraph
```

核心不是分别实现“天空 Shader、云 Shader、水 Shader”，而是建立一个共享环境状态，让云、雾、太阳、地形、水、植被和材质互相一致。

---

# 2. 统一 EnvironmentState

建议新增权威运行时状态：

```c
typedef struct JceEnvironmentState {
    double world_time_seconds;
    float day_fraction;

    JceVec3 sun_direction_ws;
    float sun_illuminance_lux;

    JceVec3 moon_direction_ws;
    float moon_illuminance_lux;

    JceVec3 wind_direction_ws;
    float wind_speed_mps;
    float wind_gust;

    float cloud_coverage;
    float cloud_density;
    float cloud_precipitation;

    float humidity;
    float fog_density;
    float precipitation_rate;
    float temperature_c;

    float global_wetness;
    float snow_amount;

    uint32_t weather_type;
    uint32_t weather_seed;
} JceEnvironmentState;
```

强制规则：

- Runtime 只有一个权威环境状态；
- 天空、云、雾、水、植被不得各维护一套天气；
- Editor 修改同一数据模型；
- Weather Controller、Timeline、脚本只驱动 `EnvironmentState`；
- Headless 可保留天气逻辑，但不加载任何 GPU 环境资源。

---

# 3. 大气 / 天空

## 3.1 推荐算法

第一主线采用 **Sébastien Hillaire 2020 — A Scalable and Production Ready Sky and Atmosphere Rendering Technique**。

核心资源：

```text
Transmittance LUT
Multi-Scattering LUT
Sky-View LUT
Aerial-Perspective LUT / 3D volume
```

物理项：

- Rayleigh scattering；
- Mie scattering；
- absorption/ozone；
- transmittance；
- multiple-scattering approximation；
- aerial perspective；
- ground-to-space parameterization。

这比只使用天空盒更适合动态昼夜和天气，也比依赖巨大高维 LUT 的旧方法更适合编辑器实时修改。

## 3.2 Editor 参数

`Environment > Atmosphere` 至少暴露：

- planet radius；
- atmosphere height；
- Rayleigh coefficients；
- Rayleigh density scale；
- Mie scattering；
- Mie absorption；
- Mie anisotropy g；
- Mie density scale；
- ozone/absorption profile；
- ground albedo；
- sun angular radius；
- sun illuminance；
- LUT quality；
- aerial-perspective quality。

Preset：

- Earth；
- Fantasy Earth；
- Mars-like debug；
- Custom。

## 3.3 曝光与光强

环境系统应统一使用近似物理意义的单位：

- Sun：lux；
- Camera：EV100 或内部等价模型；
- Atmosphere/volumetric 使用统一 radiometric convention。

不要让天空、云、雾、水各自靠艺术家任意乘亮度补偿。

---

# 4. Volumetric Fog

参考 Frostbite 2015 和 The Last of Us Part II 的 production froxel 方法。

## 4.1 Froxel Grid

```text
camera frustum
→ low-resolution 3D froxel volume
→ inject participating media
→ inject lighting/shadows
→ integrate toward camera
→ temporal reprojection
```

每个 froxel 保存或派生：

- extinction；
- scattering albedo；
- emissive；
- phase parameter；
- lighting。

Z 使用非线性/对数分片，让近景体积有更高精度。

## 4.2 媒体来源

- global height fog；
- local box/sphere fog；
- mist；
- dust；
- smoke；
- waterfall mist；
- weather humidity；
- VFX particle volumes。

## 4.3 光源

最低支持：

- sun；
- moon；
- point；
- spot；
- sun shadow；
- local shadow optional by quality tier。

## 4.4 Temporal

必须处理：

- history reprojection；
- camera cut reset；
- moving fog volume；
- ghosting rejection；
- high-speed camera。

---

# 5. 体积云

## 5.1 第一版主线：Nubis-like 2.5D Cloud

Density：

```text
density =
weather_coverage
× vertical_profile
× low_frequency_shape
- high_frequency_erosion
```

输入：

- 2D Weather Map；
- cloud type；
- height profile；
- low-frequency 3D noise；
- high-frequency erosion noise；
- curl/distortion；
- storm/precipitation field。

## 5.2 Ray March

每像素：

1. 求射线与云层范围相交；
2. empty-space skip；
3. 采样 density；
4. 计算 extinction；
5. 采样 sun lighting；
6. accumulation；
7. transmittance 很低时 early-out。

## 5.3 Lighting

V1：

- Beer-Lambert；
- Henyey-Greenstein-like phase；
- dual-lobe approximation；
- short light march；
- multiple-scattering approximation；
- artistic silver lining/powder term，必须受物理值约束。

## 5.4 性能

- half/quarter resolution；
- jitter/blue-noise；
- temporal reprojection；
- neighborhood/history clamp；
- adaptive step count；
- depth-aware upscale；
- coarse shadow；
- empty-space skipping。

## 5.5 Cloud Shadow

云必须真正影响世界：

```text
Cloud density
→ Cloud shadow texture
→ Direct sun attenuation
→ Terrain
→ Buildings
→ Vegetation
→ Water
```

不能只有天空中“看见云”，地面光照完全不变。

## 5.6 长期 High/Cinematic

**Nubis Evolved**：

- camera enters cloud；
- superstorm；
- fast motion；
- internal lightning；
- reduced temporal artifacts。

**Nubis Cubed**：

- voxel cloud；
- compressed SDF acceleration；
- fluid-simulation-based modeling；
- voxel up-rez；
- accelerated light sampling。

Cubed 作为后续研究，不得阻塞第一版 Nubis-like renderer。

---

# 6. Weather System

## 6.1 WeatherPreset

建议字段：

```text
cloud_coverage
cloud_density
humidity
fog_density
precipitation
wind_speed
wind_gust
wetness_target
snow_target
lightning_rate
temperature
```

Preset：

- Clear；
- Partly Cloudy；
- Overcast；
- Rain；
- Storm；
- Fog；
- Snow；
- Dust/Fantasy。

## 6.2 Transition

不能一键把几个效果打开。

Storm 应表现为：

```text
cloud coverage ↑
cloud density ↑
sun transmittance ↓
sky irradiance changes
fog/humidity ↑
wind/gust ↑
rain starts
ground wetness ↑
puddles/ripples ↑
water roughness ↑
vegetation bend ↑
lightning activates
audio events change
```

每个参数有：

- response time；
- curve；
- optional delay；
- deterministic seed。

---

# 7. Rain / Snow / Lightning / Wetness

## 7.1 Rain

多尺度：

- near-camera rain streak；
- mid GPU particles；
- far screen/volume approximation；
- ground splashes；
- water ripple events；
- wetness accumulation。

不做全世界真实雨滴粒子。

## 7.2 Snow

- flakes；
- wind drift；
- snow material mask；
- accumulation optional；
- footprint later。

## 7.3 Lightning

不是屏幕闪白。

流程：

```text
storm event
→ bolt path
→ local/global light flash
→ cloud internal illumination
→ volumetric response
→ bloom/exposure response
→ delayed thunder event
```

## 7.4 Wetness

统一：

```text
wetness =
weather wetness
+ water proximity/local wetness
+ puddle/depression mask
```

材质可受：

- roughness；
- albedo/darkening；
- specular；
- normal response；
- clearcoat-like response

影响，但必须按材质类型定义曲线。

---

# 8. Sun / Moon Shadows

第一版采用 4 cascade CSM/PSSM：

- practical split；
- stable cascades；
- texel snapping；
- PCF；
- receiver/normal bias；
- per-cascade culling；
- vegetation shadow distance；
- optional cached far cascade。

不要在第一阶段直接跳到复杂 virtual shadow map。

---

# 9. Sky Lighting / Environment Probe

动态天空需要给世界提供环境光。

至少：

- sky radiance cubemap；
- diffuse irradiance；
- specular prefilter。

动态更新必须 amortized：

- dirty threshold；
- staggered cubemap faces；
- diffuse faster；
- specular slower。

---

# 10. 地形权威数据

JCE Terrain 的权威数据为 **tiled heightfield**：

```text
Source Heightfield
→ Editor Generate/Sculpt/Erode
→ Tiled Height + Derived Maps
→ Cook
→ Runtime Streaming
→ GPU regular-grid LOD renderer
```

Heightfield 不能表达：

- cave；
- arch；
- overhang；
- vertical cliff detail。

这些用独立 Static Mesh/rock mesh 叠加。

---

# 11. Terrain Runtime Renderer

研究基础：Geometry Clipmaps。

推荐架构：

- source：tiled heightfield；
- runtime：camera-centered nested regular grids；
- finest center；
- coarser rings；
- morph transition；
- GPU sample height texture；
- tile cache；
- normal/slope derived texture。

若一期实现压力过大：

**先 quadtree tile LOD，再迁移 clipmap。**

必须解决：

- LOD cracks；
- tile border；
- normal seam；
- material seam；
- streaming pop。

High 不接受明显 skirt 伪影。

---

# 12. Terrain Sculpt Mode

Editor：

- Raise；
- Lower；
- Smooth；
- Flatten；
- Terrace；
- Ramp；
- Noise；
- Stamp；
- Set Height；
- Copy/Paste。

Brush：

- radius；
- hardness；
- strength；
- falloff；
- alpha；
- rotation；
- spacing；
- jitter。

流程：

```text
mouse ray
→ terrain hit
→ affected tile set
→ brush compute/CPU
→ update height
→ normals dirty
→ derived maps dirty
→ collision dirty
→ nav dirty
→ foliage/PCG dirty
→ undo command
```

---

# 13. Terrain Undo/Redo

不得每次笔刷复制整个世界高度图。

记录：

- affected tile ids；
- dirty rectangle；
- before/after block；
- compressed delta。

大型侵蚀：

- async；
- preview；
- cancel；
- commit only when done；
- one Undo command。

---

# 14. Procedural Terrain Generation

节点/操作：

- constant；
- gradient；
- Perlin/Simplex-like；
- ridged noise；
- fBm；
- domain warp；
- mask；
- curve；
- terrace；
- combine；
- smooth；
- erosion。

重要：

**Noise 只是初始形态。自然地貌必须靠雕刻 + 侵蚀 + 水系 + 艺术修正。**

---

# 15. Hydraulic Erosion

第一阶段是 Editor-only GPU/CPU compute，不做全世界 runtime erosion。

每格：

- terrain height；
- water；
- suspended sediment；
- flow velocity；
- optional hardness。

迭代：

```text
rain
→ water flow
→ sediment capacity
→ erosion/dissolve
→ sediment transport
→ deposition
→ evaporation
```

参数：

- rainfall；
- evaporation；
- erosion rate；
- deposition；
- sediment capacity；
- flow force；
- erodability；
- iterations；
- feature size。

派生输出：

- flow；
- flow direction；
- sediment；
- deposition；
- wear；
- water accumulation。

这些必须直接供：

- river placement；
- terrain materials；
- mud；
- rock exposure；
- vegetation；
- debris。

---

# 16. Thermal Erosion

依据 slope/talus：

```text
if slope > material_talus:
    move material downhill
```

用于：

- dirt；
- sand；
- scree；
- cliff base；
- ruined slope。

Hydraulic + Thermal 必须可串联。

---

# 17. Terrain Material

输入：

- height；
- slope；
- curvature；
- flow；
- sediment；
- wetness；
- biome；
- artist weights。

层：

- rock；
- dirt；
- mud；
- grass soil；
- sand；
- snow；
- ruins blend。

策略：

- steep area triplanar；
- flat area cheaper mapping；
- macro color；
- detail normal；
- distance shader LOD。

---

# 18. Road / River / Building Terrain Modifiers

Spline Modifier：

- flatten；
- carve；
- bank；
- shoulder smooth；
- paint material；
- exclusion mask。

道路/建筑必须能自动生成 vegetation exclusion，而不是手工擦草。

---

# 19. Water Body 系统

统一类型：

- Ocean；
- Lake；
- River；
- Pond/Puddle。

每个 Water Body：

- shape/spline；
- base level；
- depth；
- flow；
- wave asset；
- material；
- underwater profile；
- shoreline；
- physics。

---

# 20. Water Editor Mode

### Ocean

- world water level；
- huge boundary；
- exclusion islands。

### Lake

- polygon spline；
- level；
- shore falloff。

### River

- spline；
- width；
- depth；
- bank；
- flow；
- terrain carve。

### Pond

- local low-cost water。

必须有 debug：

- water tiles；
- depth；
- flow arrows；
- shore distance；
- body id。

---

# 21. Water Mesh

采用：

- shared water zone/tiled mesh；
- camera-centered LOD；
- body mask/info map；
- seamless transitions。

不要每个湖/河创建自己的巨大高密度 mesh。

---

# 22. Gerstner Water V1

Wave asset：

- wave count；
- seed；
- dominant direction；
- spread；
- wavelength range；
- amplitude；
- steepness；
- speed。

GPU 负责视觉 displacement。

CPU 必须有 surface query：

```c
JceWaterSurfaceSample jce_water_sample_surface(
    JceWaterWorld* world,
    JceVec3 position,
    double simulation_time);
```

返回：

- height；
- normal；
- velocity；
- body id；
- depth；
- flow。

这样 Bullet、船、角色和 AI 可与水面一致。

---

# 23. Ocean FFT V2

High/Cinematic 后续：

- wind spectrum；
- frequency-domain height；
- inverse FFT；
- displacement；
- slope/normal；
- crest/foam metric。

FFT 不阻塞 Gerstner V1。

---

# 24. Water Shading

必须：

- Fresnel；
- roughness；
- depth absorption；
- scattering；
- reflection；
- refraction；
- multi-scale normal；
- foam；
- shore fade；
- sun sparkle；
- atmosphere/sky integration。

Reflection fallback：

```text
Planar reflection (selected High bodies)
→ SSR
→ reflection probe / sky
→ optional backend RT extension
```

不能只用 SSR。

---

# 25. Foam / Shore / Caustics

Foam 来源：

- crest；
- shallow shore；
- obstacle；
- wake；
- waterfall；
- authored mask。

Caustics V1：

- projected animated caustic；
- sun direction；
- water normal；
- depth fade。

Shore：

- wave attenuation；
- wet bank；
- foam；
- debris；
- vegetation exclusion。

---

# 26. Local Physical Water

局部 shallow-water texture：

```text
height
velocity_x
velocity_z
```

用于：

- footstep ripple；
- puddle；
- wake；
- explosion；
- near-shore event。

Compute：

- propagation；
- damping；
- boundary；
- impulse。

只在 active region 模拟。

---

# 27. Buoyancy

使用 sample probes：

```text
rigid body
→ water surface query
→ submerged depth
→ upward force
→ linear/angular drag
```

Bullet 处理刚体。

不需要在 runtime 模拟完整 3D Navier-Stokes。

---

# 28. Underwater

Camera below water：

- absorption；
- scattering；
- water fog；
- caustics；
- surface underside；
- bubble/VFX；
- audio low-pass；
- hysteresis at surface。

---

# 29. Horizon Forbidden West 水体高级参考

长期研究：

```text
offline high-quality breaking-wave simulation
→ extract compact wavefront representation
→ runtime reconstruct/combine
```

这是很重要的 AAA 思路：

**昂贵的物理可以离线，Runtime 消费压缩后的可控表示。**

---

# 30. Foliage Type

数据：

```text
mesh variants
density
spacing
scale range
slope range
height range
wetness range
biome mask
align to normal
random yaw
wind strength
interaction
LOD/cull
collision policy
seed
```

---

# 31. Vegetation Scatter 输入

每个候选点可以读取：

- world position；
- height；
- normal；
- slope；
- curvature；
- terrain layer；
- flow；
- sediment；
- wetness；
- distance to water；
- distance to road；
- distance to building；
- biome；
- artist mask；
- noise。

例：

```text
reeds:
distance_to_water < 4m
wetness > 0.65
slope < 12°
```

---

# 32. PCG Graph

基础节点：

### Sources

- Terrain；
- Spline；
- Volume；
- Mesh Surface；
- Water Shore；
- Road；
- Points。

### Sampling

- Grid；
- Random；
- Poisson-like；
- Surface；
- Spline。

### Attribute

- Set；
- Math；
- Remap；
- Noise；
- Distance；
- Terrain Query；
- Biome Query。

### Filters

- Range；
- Probability；
- Mask；
- Exclusion；
- Collision；
- Density。

### Transform

- Position；
- Rotate；
- Align Normal；
- Scale；
- Snap。

### Outputs

- Mesh Instance；
- Foliage；
- Prefab；
- Decal；
- Debug Point。

---

# 33. PCG Determinism

必须：

```text
same graph version
+ same input data
+ same seed
+ same cell id
= same generated placement
```

禁止：

- wall-clock random；
- unordered nondeterministic generation；
- streaming 重新进入后位置变化。

---

# 34. Bake / Runtime / Hybrid

### Bake

- hero trees；
- city ruins；
- critical composition。

### Runtime

- grass；
- small flowers；
- generic rocks；
- debris。

### Hybrid

- trees baked；
- ground cover generated per cell。

---

# 35. Vegetation Rendering

High：

- instancing；
- compact per-instance data；
- GPU frustum culling；
- Hi-Z occlusion where available；
- draw indirect；
- LOD；
- impostor far distance。

Fallback：

- CPU cell culling；
- CPU LOD；
- regular instancing；
- lower density。

Web/old GPU 不能因为缺 compute 就没有植被。

---

# 36. Vegetation Wind

参考 Crysis / GPU Gems：

GPU vertex animation：

```text
global trunk bend
+ branch motion
+ leaf flutter
```

输入：

- Environment wind；
- gust；
- per-instance phase；
- vertex branch/leaf masks。

不要用完整 skeletal animation 驱动远处森林。

---

# 37. Local Vegetation Interaction

玩家附近：

- interaction sphere/capsule；
- bend field；
- temporary impulse；
- smooth recovery。

大型树破坏是独立 gameplay system，不让百万 foliage 进入 Bullet。

---

# 38. Biome

Biome Asset：

- climate range；
- terrain layers；
- foliage set；
- rock/debris set；
- wetness；
- snow tendency；
- PCG graphs。

Biome 可：

- artist paint；
- procedural；
- hybrid。

---

# 39. JCE Editor World Workspace

新增：

**World**

模式：

- Environment；
- Terrain；
- Water；
- Foliage；
- PCG；
- Biome；
- Debug。

---

# 40. Environment Panel

必须：

- time slider；
- sun/moon；
- atmosphere；
- exposure；
- weather preset；
- transition preview；
- fog；
- cloud；
- rain；
- wind；
- wetness；
- lightning。

---

# 41. Terrain Mode

Tabs：

```text
Manage
Sculpt
Paint
Erosion
Spline
Debug
```

支持：

- create/import；
- tile；
- resize；
- sculpt；
- material paint；
- biome paint；
- erosion；
- road/river spline；
- derived maps。

---

# 42. Water Mode

- add ocean/lake/river/pond；
- spline；
- width/depth；
- level；
- flow；
- carve；
- wave profile；
- underwater；
- debug。

---

# 43. Foliage / PCG Mode

Foliage：

- paint；
- erase；
- density；
- scale；
- masks；
- exclusions。

PCG：

- graph；
- typed pins；
- deterministic seed；
- live preview；
- node timing；
- point count；
- bake；
- regenerate；
- diff。

Editor 和 Runtime 必须共享同一 PCG 执行核心，不能维护两套规则。

---

# 44. Source / Derived / Cooked

明确三层：

### Source

- height；
- spline；
- graph；
- masks；
- rules；
- weather profile。

### Derived Cache

- normal；
- slope；
- flow；
- sediment；
- water info；
- PCG placement；
- HLOD；
- cloud noise。

### Cooked

运行时平台格式。

Derived cache：

- 可删除；
- 可重建；
- 带 source hash；
- 不成为权威。

---

# 45. Dirty Dependency Graph

例如 Terrain Sculpt：

```text
height dirty
├─ normal
├─ slope
├─ collision
├─ navmesh
├─ water shore
├─ foliage cells
├─ biome derived
└─ HLOD
```

必须异步、分优先级、允许取消过时任务。

---

# 46. Undo / Redo

所有 World Tool 操作走 Editor Command：

- TerrainBrushCommand；
- TerrainErosionCommand；
- WaterSplineCommand；
- FoliagePaintCommand；
- PcgGraphEditCommand；
- EnvironmentPropertyCommand。

Panel 不直接写底层状态。

---

# 47. 资产格式

Source：

```text
.env.json
.weather.json
.atmosphere.json
.cloud.json
.terrain.json
.terrain_layer.json
.water.json
.foliage.json
.biome.json
.pcg.json
```

Cooked：

```text
.jce_env
.jce_weather
.jce_atmos
.jce_cloud
.jce_terrain
.jce_water
.jce_foliage
.jce_biome
.jce_pcg
```

规则：

- JSON 只通过 JCE/cJSON；
- 游戏资产通过 PhysFS；
- OS 路径通过 JCE OS adapter；
- Runtime 不直接读取 Houdini/Gaea/Blender 源资产。

---

# 48. RenderGraph 建议

```text
1. Environment state update
2. Sun/Moon shadow cascades
3. Atmosphere LUT update if dirty
4. Cloud weather/density resources
5. Cloud shadow
6. Terrain depth/opaque
7. Vegetation alpha-test/opaque
8. Main lighting
9. Froxel media injection
10. Froxel lighting
11. Volumetric integration
12. Volumetric clouds
13. Water
14. Transparent/VFX
15. Aerial perspective composite
16. Exposure/Bloom/ToneMap
```

实际顺序由依赖图确认。

---

# 49. bgfx 可行性与能力分支

bgfx 官方支持 runtime caps 查询。

重点：

- `BGFX_CAPS_COMPUTE`
- `BGFX_CAPS_DRAW_INDIRECT`
- `BGFX_CAPS_DRAW_INDIRECT_COUNT`

官方 examples 已覆盖：

- instancing；
- compute N-body；
- terrain painting；
- GPU-driven rendering；
- adaptive compute tessellation；
- draw indirect；
- sparse virtual textures；
- shadows；
- HDR/IBL。

JCE 必须运行时检测能力。

禁止：

```text
if !compute:
    Environment = disabled
```

应该：

```text
if high GPU capabilities:
    advanced path
else:
    Low fallback
```

---

# 50. Quality Tiers

## LOW

- analytic/simple sky；
- height fog；
- 2D/far cloud；
- terrain LOD；
- Gerstner；
- pre-baked/simple foliage；
- CPU cull + instancing。

## MEDIUM

- physical atmosphere LUT；
- froxel fog；
- low-res volumetric cloud；
- wetness；
- Gerstner + reflection/refraction；
- GPU instancing。

## HIGH

- high-quality atmosphere；
- aerial perspective；
- Nubis-like cloud；
- cloud shadow；
- local volumetric lights；
- local shallow water；
- erosion-derived terrain masks；
- GPU-driven foliage。

## CINEMATIC

- higher volumetric samples；
- enhanced multi scattering；
- planar/RT reflection where available；
- experimental voxel clouds；
- very dense vegetation；
- slower editor erosion。

---

# 51. Headless

保留：

- EnvironmentState；
- deterministic weather；
- terrain height query；
- simplified water surface query；
- biome/PCG gameplay queries if required。

不加载：

- atmosphere LUT；
- cloud renderer；
- fog GPU；
- water renderer；
- foliage renderer。

---

# 52. 测试地图

创建：

```text
samples/world_environment_lab/
├── atmosphere
├── clouds
├── fog
├── weather
├── terrain
├── erosion
├── water
├── foliage
└── integrated
```

`integrated` 应包含：

- rocky terrain；
- sunset；
- thick storm clouds；
- aerial haze；
- vegetation；
- lightning；
- wetness；
- optional lake/river。

用于接近用户给出的 Nubis Evolved 参考图进行系统级验证。

---

# 53. Visual Regression

固定：

- camera；
- resolution；
- weather；
- seed；
- quality；
- exposure；
- backend。

记录：

- Golden Image；
- RMS/image difference；
- perceptual/SSIM-like measure；
- manual approval。

不同 GPU/driver **不要求逐像素一致**。

但：

- Environment logical state；
- PCG placement；
- terrain source；
- weather seed

应保持确定性。

---

# 54. 性能初始预算

1080p / 60 FPS High 的初始研发预算：

| 子系统 | GPU 初始目标 |
|---|---:|
| Atmosphere/Sky | 0.3–0.8 ms |
| Volumetric Clouds | 1.5–3.0 ms |
| Volumetric Fog | 0.5–1.5 ms |
| Terrain | 0.5–1.5 ms |
| Water | 0.5–1.5 ms |
| Vegetation overhead | 0.5–2.0 ms |

这是 JCE 工程预算，不是文献保证。

同时记录：

- P50；
- P95；
- P99；
- max stall；
- GPU memory；
- terrain resident tiles；
- foliage visible instances；
- streaming uploads。

---

# 55. RDR2 / Modern AAA 质量定义

不能写“全面超过 RDR2”。

拆成：

### Atmosphere

- sunset continuity；
- aerial perspective；
- sky/cloud/fog lighting consistency。

### Cloud

- volumetric shape；
- self-shadow；
- temporal stability；
- storm；
- internal lightning。

### Terrain

- no obvious random-noise landscape；
- believable erosion channels；
- natural layer placement。

### Water

- sky reflection；
- depth coloration；
- shore；
- ripple/wake；
- underwater。

### Vegetation

- plausible biome placement；
- dense rendering；
- LOD stability；
- coherent wind。

### Weather

- whole-world response；
- not only rain particles。

单项可以把 RDR2 2018 作为可超越目标；完整作品是否超过仍取决于资产、美术、灯光和内容质量。

---

# 56. Code Agent 实施顺序

## Phase 0

- 读资料；
- 审计当前 Renderer/Editor/Asset；
- capability matrix；
- environment lab；
- profiler baseline。

## Phase 1

Atmosphere + sun + exposure + aerial perspective。

## Phase 2

Froxel fog + local media + lighting。

## Phase 3

Nubis-like volumetric cloud + cloud shadow。

## Phase 4

Weather + rain + lightning + wetness + wind。

## Phase 5

Terrain tile + LOD + streaming + material + collision。

## Phase 6

Terrain Editor + sculpt + undo + import/export。

## Phase 7

Hydraulic + thermal erosion + derived maps。

## Phase 8

Water bodies + Editor + Gerstner + shading + shore + underwater + buoyancy。

## Phase 9

Foliage + Biome + PCG + instancing + wind + streaming。

## Phase 10

Integrated coupling。

## Phase 11

Low/Medium/High/Cinematic + backend fallbacks。

## Phase 12

Independent verification。

---

# 57. 强制完成规则

以下不算完成：

- 一个云 Raymarch Demo；
- 一个 Water Plane；
- 一张 Perlin Terrain；
- 一张侵蚀截图；
- 手摆 1000 棵树；
- Editor 不能保存；
- 没有 Undo；
- Standalone 不工作；
- Low fallback 不存在；
- 没有 Cooked asset；
- 没有性能测试；
- 没有独立验证。

只有：

- Runtime；
- Editor；
- Source/Cooked；
- Undo/Redo；
- Streaming；
- Debug；
- Tests；
- Fallback；
- Performance；
- Independent Verification

全部完成后才可宣告 100%。

外部硬件/SDK 真正缺失才可 `BLOCKED_EXTERNAL`；实现复杂、测试失败、性能不达标均不是停止理由。

---

# 58. 独立验证

实现完成后：

- clean worktree；
- clean shader build；
- clean cooked assets；
- new editor config；
- recreate environment lab；
- save/reopen；
- standalone；
- RenderDoc；
- visual golden；
- Low fallback；
- at least two renderer backends if available；
- performance capture。

独立验证不得只重复开发阶段单元测试。

---

# 59. 核心研究资料

以下为 Code Agent **必须阅读** 的公开资料。

## 59.1 RDR2 Integrated Atmosphere

**Creating the Atmospheric World of Red Dead Redemption 2: A Complete and Integrated Solution**  
SIGGRAPH 2019, Fabian Bauer / Rockstar.

https://advances.realtimerendering.com/s2019/index.htm

用途：理解 sky/cloud/fog/volumetrics/ambient lighting 的整体集成。

---

## 59.2 Production Sky Atmosphere

**Sébastien Hillaire — A Scalable and Production Ready Sky and Atmosphere Rendering Technique**  
Computer Graphics Forum / EGSR 2020.

https://diglib.eg.org/items/8a3e5350-18b3-46bd-9274-3add5af88c75

全文：

https://diglib.eg.org/server/api/core/bitstreams/f17c98cb-60c8-4a58-b459-181dd009ef4e/content

---

## 59.3 Unreal Sky Atmosphere

https://dev.epicgames.com/documentation/en-us/unreal-engine/sky-atmosphere-component-in-unreal-engine

用途：Editor 参数、scalability、生产接口参考。

---

## 59.4 Frostbite Unified Volumetrics

**Physically-based & Unified Volumetric Rendering in Frostbite**  
Sébastien Hillaire, SIGGRAPH 2015.

EA 官方：

https://www.ea.com/news/physically-based-unified-volumetric-rendering-in-frostbite

YouTube：

https://www.youtube.com/watch?v=ddfEnuXZijM

---

## 59.5 The Last of Us Part II Volumetric Effects

https://history.siggraph.org/wp-content/uploads/2022/08/2020-Talks-Kovalovs_Volumetric-Effects-of-The-Last-of-Us-Part-Two.pdf

用途：froxel fog、probe、sun/local light 的 production case。

---

## 59.6 Horizon Zero Dawn — Real-Time Volumetric Cloudscapes

Guerrilla / SIGGRAPH 2015.

官方：

https://www.guerrilla-games.com/read/the-real-time-volumetric-cloudscapes-of-horizon-zero-dawn

Slides：

https://advances.realtimerendering.com/s2015/The%20Real-time%20Volumetric%20Cloudscapes%20of%20Horizon%20-%20Zero%20Dawn%20-%20ARTR.pdf

---

## 59.7 Nubis, Evolved

Guerrilla / SIGGRAPH 2022.

https://www.guerrilla-games.com/read/nubis-evolved

课程页：

https://advances.realtimerendering.com/s2022/index.html

---

## 59.8 Nubis, Cubed

Andrew Schneider / SIGGRAPH 2023.

https://www.schneidervfx.com/

用途：voxel clouds、SDF acceleration、fluid-modeled cloud density、up-rez。

---

## 59.9 Unreal Volumetric Cloud

https://dev.epicgames.com/documentation/unreal-engine/volumetric-cloud-component-in-unreal-engine

---

## 59.10 GPU Gems Water

**Effective Water Simulation from Physical Models** — Mark Finch.

https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-1-effective-water-simulation-physical-models

用途：wave sum、Gerstner、dynamic normals、physical parameter intuition。

---

## 59.11 Unreal Water System

https://dev.epicgames.com/documentation/unreal-engine/water-system-in-unreal-engine

Water mesh：

https://dev.epicgames.com/documentation/unreal-engine/water-meshing-system-and-surface-rendering-in-unreal-engine

Gerstner：

https://dev.epicgames.com/documentation/en-us/unreal-engine/simulating-waves-using-the-water-waves-asset-in-unreal-engine

---

## 59.12 Horizon Forbidden West Water

**Rendering Water in Horizon Forbidden West**, Hugh Malan / Guerrilla, SIGGRAPH 2022.

课程页：

https://advances.realtimerendering.com/s2022/index.html

用途：breaking waves 与 offline → compact runtime representation。

---

## 59.13 Terrain Geometry Clipmaps

GPU Gems 2 / Microsoft Research.

https://developer.nvidia.com/gpugems/gpugems2/part-i-geometric-complexity/chapter-2-terrain-rendering-using-gpu-based-geometry

用途：nested grids、morph、huge terrain、GPU height sampling。

---

## 59.14 Unreal Hydro-Erosion

https://dev.epicgames.com/documentation/en-us/unreal-engine/landscape-hydroerosion-tool-in-unreal-engine

---

## 59.15 Houdini HeightField Erode

https://www.sidefx.com/docs/houdini/nodes/sop/heightfield_erode.html

Erosion guide：

https://www.sidefx.com/docs/houdini/heightfields/erosion.html

用途：hydraulic + thermal、多尺度、flow/sediment/debris/erodability。

---

## 59.16 GPU Hydraulic Erosion Paper

**Fast Hydraulic Erosion Simulation and Visualization on GPU**  
Xing Mei, Philippe Decaudin, Bao-Gang Hu, Pacific Graphics 2007.  
DOI: 10.1109/PG.2007.15

---

## 59.17 Sebastian Lague — Hydraulic Erosion

YouTube：

https://www.youtube.com/watch?v=eaXk97ujbPQ

用途：理解 droplet/erosion prototype。最终 JCE 必须独立实现工程版本。

---

## 59.18 Crysis Vegetation

GPU Gems 3：

https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-16-vegetation-procedural-animation-and-shading-crysis

---

## 59.19 GPU Tree Wind

https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-6-gpu-generated-procedural-wind-animations-trees

---

## 59.20 SpeedTree Rendering Reference

https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-4-next-generation-speedtree-rendering

---

## 59.21 Unreal PCG

https://dev.epicgames.com/documentation/en-us/unreal-engine/procedural-content-generation-framework-in-unreal-engine

Node reference：

https://dev.epicgames.com/documentation/unreal-engine/procedural-content-generation-framework-node-reference-in-unreal-engine

---

## 59.22 Unreal Procedural Foliage

https://dev.epicgames.com/documentation/unreal-engine/procedural-foliage-tool-in-unreal-engine

---

## 59.23 Cascaded / Parallel Split Shadows

GPU Gems 3：

https://developer.nvidia.com/gpugems/gpugems3/part-ii-light-and-shadows/chapter-10-parallel-split-shadow-maps-programmable-gpus

---

## 59.24 bgfx Examples

https://bkaradzic.github.io/bgfx/examples.html

特别查看：

- 05 instancing；
- 24 compute N-body；
- 27 terrain；
- 37 GPU-driven rendering；
- 41 adaptive compute tessellation；
- 48 draw indirect。

---

## 59.25 bgfx API / Capabilities

https://bkaradzic.github.io/bgfx/bgfx.html

必须运行时查询 capability，而不是按平台名硬猜。

---

# 60. 推荐阅读顺序

1. RDR2 integrated atmosphere；
2. Hillaire 2020；
3. Frostbite volumetrics；
4. Horizon ZD clouds；
5. Nubis Evolved；
6. Geometry Clipmaps；
7. Houdini/UE erosion；
8. GPU Gems water；
9. HFW water；
10. Crysis vegetation；
11. Unreal PCG；
12. bgfx caps/examples。

---

# 61. 版权与实现纪律

公开资料用于理解算法。

不得：

- 复制 proprietary engine code；
- 提取商业游戏 shader；
- 反编译 RAGE/Decima/Frostbite；
- 将 UE 私有代码作为依赖；
- 搬运未授权 assets。

应：

- 根据公开论文、官方演讲、公开文档独立实现；
- 使用 JCE 自己的数据结构、RenderGraph、Editor 和测试；
- 记录算法出处和实现差异。
