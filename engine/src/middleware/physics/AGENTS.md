# engine/src/middleware/physics — Physics (L4)

> 3D rigid-body via Bullet; 2D via box2d.

## Identity

- **Layer**: L4. C99 + one C++ bridge for Bullet.
- **Public umbrella**: `<jce/api_physics.h>` → `<jce/middleware/physics/jce_*.h>`
- **Deps (PRIVATE)**: `Bullet::Bullet` (includes `BulletSoftBody` for P3-C.4), `box2d::box2d`. CMake target is built as **CXX**. Late-bound `target_link_libraries(jce_physics PRIVATE jce_renderer)` in `engine/CMakeLists.txt` so the cloth HW gate can subscribe to the renderer's Render Pipeline Asset observer (one-way dependency only: physics → renderer at link time, runtime communication is the renderer firing a registered callback).

## File map

| Header | TU | Role |
|--------|----|------|
| `jce_physics.h` / `jce_physics_types.h` | `jce_physics.c` | Public 3D API: world, rigid body, collider, ray/sweep, **CCD modes** (P3-C.3, with sparse per-world cache so the user-selected mode round-trips Bullet's threshold-only state) |
| `jce_physics2d.h` | `jce_physics2d.c` | Public 2D API over box2d |
| `jce_physics_material.h` | `jce_physics_material.c` | Standalone PhysicsMaterial asset (`.physmat.json`): friction/restitution + combine modes, JSON load/save, applied to live bodies via `jce_physics_body_set_material` |
| `jce_physics_layers.h` | `jce_physics_layers.c` | 32-slot Layer Collision Matrix (Unity parity, P3-C.2): symmetric 32×32 bitmask + layer names, `.jce/physics_layers.json` I/O, `jce_physics_body_set_layer` derives (group,mask) for the collision-filter path (full uint32 — all 32 layers addressable, P4-E.4). The **character capsule** is filtered the same way: `JceCharacterDesc::layer` is resolved to (1<<layer, matrix row) inside `jce_physics_character_create`, and the pair is cached per character in `JceBulletCharFeel` because the ground / step / head-clearance probes are separate `rayTest`s that do not inherit the body's proxy. Before 2026-08-31 the capsule carried Bullet's own `btBroadphaseProxy` constants, which **alias layer bits** (`CharacterFilter == 32 == layer 5`, `Static|Default == layers 1,0`) — so it behaved as layer 5, touched only layers 0 and 1 whatever the matrix said, and a floor on any other layer let the player fall through the world. `tools/lint/check_physics_layer_filters.py` now forbids those constants here. |
| `jce_physics_debug.h` | `jce_physics_debug.c` (+ pair/diff/listener logic in `jce_physics.c`, `btIDebugDraw` subclass + manifold enumerator in `jce_physics_bullet.cpp`) | **P3-C.5** — debug-draw line-sink bridge (`set_flags`/`set_line_sink`/`flush`) + BEGIN/STAY/END contact events via per-frame manifold diffing in `JcePhysicsWorld` (double-buffered sorted pair arrays) + opaque per-body entity tags. Decoupled from renderer: the editor installs `jce_debug_draw_line` as the sink in `jce_panel_physics_debugger.cpp`. |
| `jce_physics_joint_query.h` | `jce_physics_joint_query.c` (+ `jce_bullet_joint_get_info_for_body` in `jce_physics_bullet.cpp`) | **P3-C.6** — joint introspection for the editor Joint Gizmo. The C front-end is a thin POD copy; the Bullet bridge walks `bw->constraints[]`, casts to `btPoint2PointConstraint` / `btHingeConstraint` / `btSliderConstraint` / `btGeneric6DofConstraint` and resolves world-space anchor / axis (X-column for slider/6DOF, Z-column for hinge per Bullet convention) / limits. World-anchored constraints are detected via `btTypedConstraint::getFixedBody()` and reported with `body_b == UINT32_MAX`. |
| `jce_cloth.h` | `jce_cloth.cpp` | **P3-C.4** — Cloth / soft-body. Owns a private secondary `btSoftRigidDynamicsWorld` (lazy-init on first `cloth_create`; the primary `btDiscreteDynamicsWorld` is **never** upgraded). Patch via `btSoftBodyHelpers::CreatePatch`, pinning via post-create `setMass(idx, 0)`, mass/stiffness/damping/iters configurable, optional wind + self-collision. Cross-world anchor uses `appendAnchor` against `btRigidBody*` looked up by `jce_bullet_body_get_rigid_native_` (internal accessor) on the "default" rigid world tracked by `jce_physics_set_default_bullet_world_`. Stepped from `jce_physics_step` via `jce_cloth_step_(dt)`; shut down from `jce_physics_destroy` when the default world dies. HW gate: a renderer-layer observer (`cloth_rpa_observer`) installed via `jce_render_pipeline_set_observer` flips `jce_cloth_set_simulation_enabled` when the active RPA changes. Self-test in `tests/middleware/physics/test_jce_cloth.c`. |
| `jce_collider_cook.h` | `jce_collider_cook.cpp` | **Per-object compound collider cook** — turns separated model parts into N child colliders combined into ONE compound shape (never a fat box spanning the gaps). Static→triangle mesh, dynamic→convex hull or **V-HACD** convex decomposition (single-header `VHACD.h` from conan `v-hacd/4.1.0`, BSD-3, `ENABLE_VHACD_IMPLEMENTATION` in this TU; CMake silences its warnings via per-file `/w`). `jce_collider_cook()` / `_cooked_free()` / `_cook_config_default()`. |
| `jce_collider_asset.h` | `jce_collider_asset.c` | Deterministic little-endian cooked-collider blob (magic `JCOL`, version 1). `jce_collider_serialize` / `_deserialize` / `_instantiate` (maps `JceCookedChild`→`JceColliderChild`→`jce_physics_body_create_compound`). |
| (internal) `jce_physics_internal.h` | `jce_physics_bullet.cpp` | C++ bridge to `btDynamicsWorld`, `btCollisionShape`, etc. |

## Rules

1. **Bullet / box2d types are private.** Public API uses `JceBody*`, `JceShape*`, `JceContact*`, plus `jce_vec3` / `jce_quat` from `jce_math`. Convert at the boundary in `jce_physics_bullet.cpp`.
2. **Fixed timestep** simulation; expose substep count. Interpolate between sim ticks for rendering — caller responsibility, document.
3. **ECS integration**: register components in `middleware/scene` (RigidBody, Collider) — physics reads transforms and writes back. Don't add a duplicate ECS world.
4. **Determinism**: don't rely on iteration order of unordered containers if you add new tracking maps.
5. **Memory**: route Bullet's allocator to `jce_malloc` (already configured in `jce_physics_bullet.cpp`).
6. **2D and 3D** are separate worlds — never mix.

## Don't

- Don't leak `btVector3` / `b2Vec2` into public headers.
- Don't run physics on the render thread.
- Don't add a third physics engine.

## 2026-09-01：Bullet 后端拆成三个 TU

`jce_physics_bullet.cpp` 曾是 3,164 行——越过 3,000 行上限并被
`tools/lint/check_file_size.py` 冻结，所以往里加一行就得先删一行。

现在的形状：

| 文件 | 内容 |
|---|---|
| `jce_physics_bullet_internal.hpp` | `struct JceBulletWorld`、`JceBulletCharFeel`、`to_bt`/`from_bt_*` 与句柄编解码 |
| `jce_physics_bullet_vehicle.cpp` | 光线投射载具控制器（188 行） |
| `jce_physics_bullet.cpp` | 其余（2,797 行，已在上限内） |

**为什么新头是 `.hpp` 而不是并进 `jce_physics_internal.h`**：后者被
`jce_physics.c` 与 `jce_physics_joint_query.c` 当 C 编译，而这个结构体的成员是
`btDiscreteDynamicsWorld*`、`btRigidBody*`。那两个 C 消费方只需要它已有的前向声明，
所以定义该进一个 C++ 专用的兄弟头，而不是藏在 `#ifdef __cplusplus` 后面。

**为什么选载具而不是角色控制器**：载具段只碰 `bw->vehicle_*`，不调用原 TU 里
任何别的函数——这是文件里最窄的一条缝。角色控制器有两倍大，且会把 body 表和
shape 表一起拽过去。

三个文件的冻结项已从 `file_size_baseline.json` 里**摘掉**（不是下调）：它们现在
都在上限以内，再越 3,000 就该按新违规报，而不是按「比基线大」报。
阴性对照已跑：把本文件补到 3,007 行，门报的是 "over AGENTS.md §11's cap of 3000"。

## 2026-09-20 — 关节驱动：Bullet 的三个强度参数，单位并不一致

`jce_physics_constraint_set_motor` 与 `jce_bullet_configurable_joint_set_drive`
对外收的都是**作者的单位**（hinge: rad/s + N·m；slider: m/s + N；6DOF 同理），
换算只在桥里做一次。**必须这样，因为 Bullet 自己不一致**——三处都从本机
Conan 缓存的 Bullet 源码读出，不是凭记忆：

| Bullet API | 它要的是 | 换算 |
|---|---|---|
| `btHingeConstraint::enableAngularMotor(.., maxMotorImpulse)` | **冲量** | `扭矩 × fixed_dt` |
| `btSliderConstraint::setMaxLinMotorForce(f)` | 力（solver 自己 `/ info->fps`） | 无 |
| `btRotationalLimitMotor::m_maxMotorForce` | 力（同上，`btGeneric6DofConstraint.cpp:777`） | 无 |

**三个里只有一个要冲量。** 弄错的后果不是报错，是一个**照常转、但只有 1/60 力气**
的马达。判据写在 `tests/middleware/physics/test_jce_joint_motor.c` 里：同一个
3 N·m 反向负载下，无关节对照 −35.9999 rad/s、0.05 N·m 马达 −35.4000
（它的贡献是 +0.6 rad/s，正好等于 0.05 N·m 在 0.1667 惯量上积两秒）、50 N·m 马达
+2.0000（达到命令速度）。**把扭矩当冲量传会让弱马达强 60 倍。**

## 2026-09-20 — `enableSpring()` 同时写 `m_enableMotor`

```cpp
void btGeneric6DofSpringConstraint::enableSpring(int index, bool onOff)
{
    m_springEnabled[index] = onOff;
    if (index < 3) m_linearLimits.m_enableMotor[index] = onOff;
    else           m_angularLimits[index - 3].m_enableMotor = onOff;
}
```

⇒ **在 VELOCITY 模式下调 `enableSpring(axis, false)`，会把刚设好的速度马达关掉。**
所以 `jce_bullet_configurable_joint_set_drive` 里**先调 `enableSpring`、再写马达状态**，
这个顺序是载荷。（反向也是安全的：spring 分支自己会把同一个标志置 true。）

反过来也要记住：**Bullet 的弹簧是*借用*限位马达生效的**。`internalUpdateSprings`
每步按胡克定律写 `m_targetVelocity` / `m_maxMotorForce`，**但从不碰 `m_enableMotor`**，
而基类只在马达使能时才发出那一行。弹簧使能而马达没开 = 每步算出一个力、一个都不施加。

## 2026-09-20 — 配置关节的 LIMITED 轴曾经等同 LOCKED

`cfg_apply_axis` 收了一个 `btScalar limit` 参数，**从来没用过**：`case 1`
（内部头定义为 LIMITED）掉进了 LOCKED 分支。于是作者写的 `linear_limit` /
`angular_limit_deg` 什么都不做，而 Inspector 照画、场景文件照存。
**函数上方的注释一直写着 `LIMITED -> setLimit(axis, -L, +L)`，只有代码不同意。**

`test_jce_configurable_joint` 的 limited-axis 用例**全程是绿的**——它断言的
「不超过 L」「比自由参照更受约束」，**对一个 LOCKED 轴同样成立**。现在它还断言
**该轴真的到得了 L**，那是唯一能区分 LIMITED 与 LOCKED 的性质。

## 2026-09-20 — 换基类会改 `getConstraintType()` 返回值

配置关节现在建成 `btGeneric6DofSpringConstraint`（是 `btGeneric6DofConstraint` 的子类，
`init()` 关掉每一根弹簧，`getInfo2` 逐轴守在那个标志后再委托给基类 ⟹ 不开弹簧时与基类逐位同行为）。
**但它的构造函数把 `m_objectType` 设成 `D6_SPRING_CONSTRAINT_TYPE`**，
而树里有三处按 `case D6_CONSTRAINT_TYPE` 分派：`set_limits`、编辑器 gizmo 读的
关节自省、以及驱动设置本身。三处都会走 `default: break`，**而构建是绿的**——
一个不再匹配的 switch 不会有任何警告。三处现在都同时接受两个 tag。

## 2026-09-21 — Bullet 的单体构造函数把**你的刚体放在 B 侧**

`btGeneric6DofSpringConstraint` 的单体重载签名是：

```cpp
btGeneric6DofSpringConstraint(btRigidBody& rbB, const btTransform& frameInB, bool);
```

参数名就是 **`rbB`**。传进去的刚体成为 **B 侧**，A 侧是 Bullet 共享的静态
`getFixedBody()`。⇒ 对每一个**锚在世界上**的关节（`connected_body == 0`，
也就是场景里大多数关节），`m_appliedTorqueBodyA` 描述的是那个**不动的世界**，
作者关心的力矩全在 `m_appliedTorqueBodyB` 里。

实测代价：`jce_bullet_constraint_applied_torque` 第一版只读 A 侧，于是
break 监视器在关节正顶着 **50 N·m** 时读到 **0.0000**——
`break_torque` 和它还没有查询函数的时候**一样惰性**，而代码逐行看都是对的。

修法是**两侧都读、取模较大者**：约束传给两侧的力矩等大反向，所以模是同一个量，
而较大的那个就是「不是退化 fixed body」的那一侧。**不要按 `has_b` 分支**——
调用方问的是「这个关节被拧得多狠」，这个问题只有一个答案，与它是用哪个构造函数
建出来的无关。

**同族警告：`enableFeedback(true)` 不等于 `setJointFeedback(...)`。**
前者让 Bullet 累积 `m_appliedImpulse`（一个合并的标量），后者才给它一个
**能分开装力与力矩**的地方。只做前者，`getJointFeedback()` 返回 NULL，
角向那一半没有任何东西可读——而这正是 `break_torque` 被写下、被序列化、
被 runtime 复制进 `ConfigJointEntry`，然后**零读者**的原因。

**这个机制现在整个住在一个文件里**：`jce_physics_bullet_con_query.cpp` 同时放
**安装**（`jce_bullet_con_register`：`enableFeedback` + `setJointFeedback` +
`addConstraint` + 登记，四件事一起）和**读取**（两个 query）。
两个创建器都只能经由它把约束放进世界——全文件只剩**一个** `addConstraint`。

分开放正是它们当初漂移的方式：安装在一个创建器上有、另一个没有，
而两个 query 都假设它有。**把安装与读取放进同一个文件，漂移才看得见。**
（顺带：`jce_physics_bullet.cpp` 因此 3015 → 2981 回到 3000 以下。
`check_file_size` 对一个**刚刚**越过上限的文件说的是「newly a god file」——
这种时候抬基线就是它警告的那种「被消音的门」，该做的是把东西搬出去。）

> **`jce_physics_bullet.cpp` 现在是 2998 行，距 3000 上限只剩 2 行。**
> 下一个往这个文件里加三行的人会拿到一个 god-file 失败，
> **而那次改动与这个文件为什么大毫无关系**。
> 正确做法是**再搬一块出去**（这个文件已经分出去两次：
> `jce_physics_bullet_vehicle.cpp`、`jce_physics_bullet_con_query.cpp`），
> **不是**去找一个可以抬的基线——门对刚越界的文件说的是
> 「newly a god file」，那种时刻抬基线就是它警告的那种被消音的门。

## 2026-09-21 — 二维碰撞层矩阵：**授权的网格，到不了任何地方**

Project Settings 里那张 32×32 的二维网格可编辑、会持久化、会被打进构建——
然后**什么都到不了**。改动前实测：

```
jce_physics2d.c 里 b2Filter / categoryBits / maskBits   0 次出现
JceBody2DDesc                                           没有层字段
JceRigidBody2DComponent                                 没有 physics_layer
jce_editor_play.cpp                                     只转发 gravity2d
jce_build_manager.cpp:595                               把**三维**矩阵写进构建，
                                                        二维没有对应那一行
```

作者设了「Player 不与 Pickup 碰撞」，存盘、出货，**每一对二维物体照撞不误**。
不报错——**看起来像关卡设计而不像 bug**，和 `Collider2D.is_trigger` 接线前
是同一种失效形态。

**判它是缺口而不是决定的依据**：三维兄弟从 P3-C.2 起就是接好的，
而全树**没有任何注释**声称二维这边是有意为之。
（对照：资产缩略图子系统同样零调用者，但**调用点的源码写着「disabled by user
request」**——所以那是决定。**零调用者这个发现必须先在调用点读过**，才能叫债。）

**两张矩阵分开、名字共用。** `s_names[32]` 一份，`s_matrix` 与 `s_matrix2d` 两份。
不是偏好：`JceProjectSettings` **本来就同时授权两张**，物理调试面板**两张网格都画**，
合并成一张等于删掉授权面**已经暴露**的一个区分。Unity 也是这么分的。

**`b2DefaultShapeDef()` 的默认值是 `categoryBits=1, maskBits=UINT32_MAX`**——
即「层 0，与一切碰撞」。所以一个**没有过滤的**二维世界
与一张**全允许的**矩阵**行为完全一致**，这正是「网格改了什么都没变」难以察觉的原因。
默认值不是错的，它只是**不是作者的**。

**Box2D 的 filter 是存在 shape 上、不是 body 上。** 于是两步构造
（`create_empty` + `add_box`）必须能拿到层号——`slot_layer` 在 `body_create`
里记录，`add_box` 继承它。**第一版漏了这一行**：`slot_layer` 只在一个
**没有任何调用者**的 setter 里被写过，于是 `add_box` 永远读到 0、
永远落在 Default 上、与一切碰撞——**在两步路径上把这个单元要修的缺陷原样重建**。
那个 setter 已删（零消费者），并专门为此留了一条用例。

## 2026-09-21 — 接触点上的东西全部走**一个**全局钩子，装在 `jce_bullet_create`

Bullet 只有 **一个** `gContactAddedCallback`。任何想影响接触的东西都必须挂在它上面，
所以拥有它的人**必须 chain 而不是 stomp**。现在它和它的全部乘客都在
`jce_physics_bullet_contact.cpp` 一个 TU 里：

| 乘客 | 守卫 | 不相关的接触付出 |
|---|---|---|
| 内边平滑（高度场对角线的鬼颠簸） | **shape** 的 user pointer（生成边信息时设的标记） | 一次指针判空 |
| 物理材质 combine（Unity 优先级 MAX>MULTIPLY>MIN>AVERAGE） | **body** 的 `userIndex2`（没材质的 body 是 0） | 一次 int 比较 |

### 装在哪里这件事本身就是一个缺陷，而它藏了很久

钩子原本**装在 `jce_bullet_body_create_heightfield` 里面，且被 `smooth_internal_edges`
守着**。⟹ **一个没有平滑高度场的世界根本没有接触回调。**
把 combine 写进那个回调然后留在原地，会在**绝大多数场景里什么都不做**，
而在**任何人都会拿来测它的那个地形场景里完美工作**。

⇒ **一个机制的安装位置必须和它做的事在一起。** 隔壁
`jce_physics_bullet_con_query.cpp` 的文件头早就把这条写下来了（它因为同一道
`check_file_size` 被切出去过两次）：「installing a mechanism and reading it are one
mechanism, and keeping them apart is exactly how they drifted」。
**答案就写在隔壁文件里，而代价从来不是没写，是没读。**

### 材质模式为什么骑在 body 上，而不是放在 world 的表里

回调是个**没有 world 指针的自由函数**，Bullet 也不提供 object→world 映射。
放一个文件作用域的 world 指针 = 一个新的全局（dedup 检测器会数，而且今天两次
正确答案都是「删掉它」）。**不需要**：Bullet **本来就**按 body 存 friction 和
restitution，缺的只有两个 2-bit 模式 ⟹ 塞进 `btCollisionObject::setUserIndex2`
（本树没人用；`setUserIndex` 是载具底盘标记，`userIndex3` 没动过）。

**两个模式都 +1 存**，于是 **0 = 「从没应用过材质」**，和 AVERAGE 区分开——
AVERAGE 是一个**真实的作者选择**，而默认构造的 body 不能被当成它。
少了这一条，没配材质的旧内容会静默改变行为。

### 已命名的行为改变

**带材质的 body 现在按作者选的规则走**，所以**默认材质 = AVERAGE**，
而此前它落到 Bullet 的 multiply。没有材质的 body 一个字节没变。
两半都有断言（`test_jce_physmat_combine.c`）。

**改这里之前：`jce_physics_material_combine()` 是规则的唯一实现，不要在回调里重写它。**
这个单元存在的全部原因就是它**从来没有调用者**。
