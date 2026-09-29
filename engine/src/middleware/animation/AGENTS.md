# engine/src/middleware/animation — Animation (L4)

> Skeleton, blend tree, state machine, IK. Powered by ozz-animation.

## Identity

- **Layer**: L4. C99 + one C++ bridge.
- **Public umbrella**: `<jce/api_animation.h>` → `<jce/middleware/animation/jce_*.h>`
- **Deps (PRIVATE)**: `ozz-animation`, `bgfx` (for skinning palette upload helpers).

## File map

| Header | TU | Role |
|--------|----|------|
| `jce_animation.h` | `jce_animation.c` (+ `jce_animation.h` internal twin) | Top-level animator + clip playback |
| `jce_skeleton.h` | `jce_skeleton.c` | Skeleton hierarchy, bind-pose, joint indices |
| `jce_anim_blend_tree.h` | `jce_anim_blend_tree.c` | 1D/2D blend trees |
| `jce_anim_sm.h` / `jce_anim_sm_binding.h` | `jce_anim_sm.c` / `jce_anim_sm_binding.c` | State machine + parameter binding |
| `jce_anim_ik.h` | `jce_anim_ik.c` | Two-bone / FABRIK IK |
| (internal) `jce_anim_ozz.h` | `jce_anim_ozz.cpp` | Bridge to `ozz::animation::*` |

## Rules

1. **ozz is private.** `ozz::*` types live only in `jce_anim_ozz.cpp`. Public headers expose `JceAnimator*`, `JceSkeleton*` opaque.
2. **Skinning palette** is written into a bgfx uniform buffer in the bridge; renderer consumes it via `engine/src/renderer/jce_skinned_mesh.c`. Keep that handshake stable.
3. **Clip data** loaded from cooked ozz format via `resource/` layer — don't import FBX at runtime (editor uses assimp for import only).
4. **State machine** is data-driven; states/transitions defined in JSON (`scene_components_json` style). Conditions evaluate over animator parameters.
5. **Math** = `jce_math`. Convert ozz `Float4x4` at the boundary.

## Don't

- Don't expose ozz types in public headers.
- Don't run IK every frame on off-screen characters — gate via `jce_lod` / culling.
- Don't add a second skinning path.

## Blendshapes: two files, and the split is load-bearing

`jce_morph.c` is the maths and the CPU evaluator and holds **no GPU
resources**, deliberately: the import and evaluation path is then unit-testable
with no renderer context, and `tests/middleware/animation/test_jce_morph.c`
exercises it against an in-memory glTF string.

`jce_morph_gpu.{c,h}` is the other half — the compute program, the
per-`JceMorphData` device buffers, and the dispatch. It lives **here and not in
`engine/src/renderer/`** even though it is nothing but GPU resources: its cache
key is `JceMorphData`, a middleware type, and the renderer may not include
middleware (`check_layer_dependencies`). The layer rule is right; the module is
what moved.

`jce_morph_internal.h` is the one seam between them: a destroy hook that
`jce_morph_data_destroy` calls so the GPU cache can be invalidated wherever a
`JceMorphData` dies, **without `jce_morph.c` knowing a GPU exists**. Do not
replace it with a call at each destroy site — those sites are renderer files
(the layer rule again), there are two today and an unknown number tomorrow, and
a pointer-keyed cache that outlives its key does not fail loudly: it hands the
next allocation at that address somebody else's vertices.

**The two evaluators must agree.** `engine/shaders/particles/cs_morph_deform.sc`
is written against `jce_morph_apply` line for line, including the detail that a
normal is renormalised ONLY when the data carries normal deltas. Change one and
change the other. The ablation that measures it is `JCE_MORPH_CPU=1`, and the
fixture is `examples/caged_kingdom/tools/gen_morph_probe_glb.py` — authored because this tree carried
the whole feature and not one .glb with a `targets` array in it.


## 2026-09-21 — clip 只能由导入产生：**结构性缺口是格式，不是编辑器**

`jce_anim_clip_create` 全树只有两个调用点（`jce_gltf_loader.c:1201`、
`jce_model_importer.cpp:1254`），**没有任何序列化器**。于是一个 `JceAnimClip`
只能靠导入模型产生，任何东西都存不下它：clip 不能脱离它的 rig 发布、
没有共享的 locomotion 库、手写不了一个 clip。
而 `JceSkeletalAnimator` 是**按名字**引用 clip 的（`clip_name` / `clip_names[8]`），
`jce_sr_anim.c` 的两个解析器都只扫模型、找不到就返回 NULL。

⇒ **先做打关键帧的 UI，会得到「编辑器画得出、什么都留不住」的东西。**
先做格式。

**读写在同一个文件里，wire 键名只写一次**（`K_TIMES` 等常量）。
一个格式的 reader 和 writer 分处两地 = 关于这个格式的两个陈述，
而 `"times"` 与 `"time"` 编译得过、往返出一个空 clip。

**target / interp 写成名字，不是枚举整数。** 那两个枚举是 src-internal 的，
数值本来可以随便改；把整数写进磁盘等于把它们冻结进每一个文件——
`.import.json` 的 `kind` 已经让我们付了这笔账。
未知的名字落到**明写的默认值**，而不是落到 switch 的 default 分支恰好是什么。

**拒收比往返更重要。** 一个没有可用 channel 的 clip 在每个时间点都采样成
**rest pose**——一个站着不动的角色，与「本来就该站着不动」**无法区分**，
下游没有任何东西会报告它。所以在门口拒收。

**两半都必须有产品消费者。** 写端 = 编辑器的「导出片段」（**不能放进
`jce_cook`**：那个工具带着 `JCE_MODEL_IMPORTER_COOK_ONLY` 编译，就是为了不链
`jce_animation`/`jce_renderer`）；读端 = 两个解析器都回退到
`<骨架目录>/<名字>.animclip.json`。**两个解析器都要回退，不能只改一个**——
作者在两处填同一个名字、只有一处生效，是更糟的半个特性。

**生命周期是唯一真正的风险。** 文件 clip 没有别的 owner（模型拥有自己的），
所以缓存在 `SrAnimInstance` 上，并在**两个**实例生命周期点释放（模型切换 + 槽位回收）——
就是 retarget map 和 aoc 已经在释放的那两处。**加到这个结构体里的资源
必须两处都释放**，否则会在作者没想到的那条路径上泄漏。
**miss 也要缓存**：两边都没有的名字否则每帧每实体 stat 一次文件系统。
