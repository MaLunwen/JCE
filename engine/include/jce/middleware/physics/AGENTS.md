# engine/include/jce/middleware/physics — Physics public headers

> Public C99 surface for 3D + 2D physics. Reach via `<jce/api_physics.h>`.

## Headers

| Header | Role |
|--------|------|
| `jce_physics.h` | 3D rigid bodies, colliders, ray/sweep, collision filter, material binding, **CCD** (P3-C.3). |
| `jce_physics_types.h` | Shared POD types (vec/quat aliases, body desc enums). |
| `jce_physics2d.h` | 2D rigid bodies via box2d. |
| `jce_physics_material.h` | Standalone PhysicsMaterial asset: friction / restitution / combine, JSON I/O. |
| `jce_physics_layers.h` | 32-slot Layer Collision Matrix (Unity parity, P3-C.2): symmetric 32×32 bitmask, per-layer names, `.jce/physics_layers.json` save/load, `jce_physics_body_set_layer` helper that drives the existing collision-filter path. |
| `jce_physics_debug.h` | **P3-C.5** — Bullet `btIDebugDraw` line-sink bridge (`jce_physics_debug_set_flags/get_flags/set_line_sink/flush`) + contact-event listeners with proper BEGIN/STAY/END semantics (`jce_physics_add/remove_contact_listener`, `JceContactEventType`) + per-body opaque entity tags (`jce_physics_body_set/get_entity`). No renderer / flecs dependency — wiring is done by the caller. |
| `jce_physics_joint_query.h` | **P3-C.6** — read-only joint introspection for the editor's Joint Gizmo. `jce_physics_joint_get_info(world, body, out)` resolves the first constraint touching `body` and returns world-space anchors / primary axis / limit range + (for 6DOF) per-axis linear & angular bounds. Family enum `JcePhysicsJointKind` (NONE/BALL/HINGE/SLIDER/6DOF) is intentionally decoupled from `JceConstraintType` so editor consumers don't see Bullet-specific enums. v1 returns the lowest-index constraint when a body owns several. |
| `jce_cloth.h` | **P3-C.4** — Cloth / soft-body (Unity Cloth-component parity) via Bullet `btSoftBody`. `jce_cloth_create/destroy`, `set_wind`, `get_positions`, `anchor_to_body(node, JcePhysicsBody, pivot)`, `set_simulation_enabled` (HW-gated; default OFF). Runs in an isolated secondary `btSoftRigidDynamicsWorld` owned by `jce_cloth.cpp` — the existing rigid world is **not** upgraded, so rigid behaviour is unchanged. Cross-world anchor coupling is one-way (cloth feels rigid pose; rigid is unaffected outside anchor impulses). HW gate is driven by the renderer-layer Render Pipeline Asset (`enable_cloth`) through a callback observer registered by the physics layer on `jce_physics_create`. |

## Rules

1. Headers are pure C99 with `JCE_EXTERN_C_BEGIN/END` for C++ callers.
2. Never expose `btScalar`, `b2Vec2`, or any Bullet/box2d type here — bridge those in `engine/src/middleware/physics/jce_physics_bullet.cpp`.
3. Vectors/quaternions: use `jce_vec3` / `jce_quat` from `jce_math.h`.
4. Layer indices are `uint32_t` in `[0, JCE_PHYSICS_LAYER_COUNT)` (32). The Bullet bridge currently narrows the derived mask to `uint16_t`; widening to `uint32_t` is a follow-up so all 32 layers are addressable end-to-end.
5. **CCD (P3-C.3)** — `JceCcdMode` is Unity-parity (Discrete / Continuous / ContinuousDynamic / ContinuousSpeculative). Bullet 3 exposes only one swept-CCD knob, so the latter two are documented aliases of `CONTINUOUS`; the editor still round-trips the user's choice through `JceRigidBodyComponent::ccd_mode` so future backends can honour the distinction.
6. **Multithreaded solver (opt-in, OFF by default)** — `JcePhysicsWorldDesc::multithreaded` requests Bullet's parallel pipeline (`btDiscreteDynamicsWorldMt` + `btCollisionDispatcherMt` + `btConstraintSolverPoolMt`, driven by Bullet's built-in `btCreateDefaultTaskScheduler`, no enkiTS dependency). It is **only honoured** when the back-end is compiled with `JCE_PHYSICS_MT` **and** Bullet is built thread-safe; otherwise the flag is ignored (single-threaded world, one-time `LOG_WARN`). Enabling it is a documented two-step change: (a) build Bullet with `bt2_thread_locks=True` (defines `BT_THREADSAFE`) in `conanfile.py`, and (b) add `JCE_PHYSICS_MT` to the `jce_physics` compile definitions in `engine/CMakeLists.txt`. Left OFF because the MT island/constraint ordering is non-deterministic and conflicts with the fixed-timestep determinism the engine relies on. Code path lives in `jce_physics_bullet.cpp` behind `#if JCE_PHYSICS_MT_ACTIVE`.

## 2026-09-06 — `JceRigidBodyKind`：为什么它不是 `JceBodyType`

3D `JceRigidBodyComponent.body_type` 现在是 `JCE_RB_KIND_*`，**AUTO 在 0**。
`JceBodyType` 的 STATIC 也是 0，而 0 同时是 memset 组件、以及有史以来每个场景所带的值
（这个字段此前从不被 parse、从不被序列化）⇒ 用那个枚举编码，字段**分不出「作者说了静态」
和「没人说过」**，honour 它会冻结树里每一个动态刚体。

**编号不是随手定的，而第一版定错了。** 我按 0/1/2/3 排 AUTO/STATIC/KINEMATIC/DYNAMIC，
理由是「字段从不被 parse，所以没有文件带得了陈旧值」——**那只检查了数据通路，没检查代码通路**：
C 调用方是用 `JceBodyType` 常量经公共 setter 写这个字段的，
`tests/application/test_jce_headless_boot.c:162` 就写着 `rb.body_type = JCE_BODY_DYNAMIC`(1)，
在 0/1/2/3 下变成了 STATIC——**物体不再下落**。

所以 DYNAMIC 与 KINEMATIC **保留 `JceBodyType` 的数字**，只有 STATIC 从 0 挪开，
而 0 恰恰是那个本来就永远无法被 honour 的值。**任何既有调用方的含义都不变。**

**下次给一个「已经有公共 setter」的字段换编码时，先问代码通路。**
