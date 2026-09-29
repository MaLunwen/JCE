/*
 * jce_material_override.h  Per-renderer overrides of a shared material.
 *
 * A JceMeshRenderer carries an inline PBR block AND may name a .mat.json in
 * material_path.  Before this existed, every load path copied the material's
 * factors over the inline ones unconditionally, so tinting three of two
 * hundred rocks that share Rock.mat.json was discarded on the next open --
 * silently, with the authored value still sitting in the scene file.  The
 * only workaround was one .mat.json per variant, which multiplies material
 * count, breaks batching keys, and defeats the point of a shared material.
 *
 * material_override_mask records which factors the author set on THIS
 * renderer.  Unity spells the same idea MaterialPropertyBlock: per-instance
 * overrides and a shared material coexist, and the override survives a
 * material swap.  Zero -- what every scene written before this carries --
 * means the material wins outright, which is the old behaviour exactly.
 */

#ifndef JCE_MATERIAL_OVERRIDE_H
#define JCE_MATERIAL_OVERRIDE_H

#include <jce/middleware/scene/jce_scene.h>
#include <jce/renderer/jce_pbr_material.h>

JCE_EXTERN_C_BEGIN

/* Bits of JceMeshRenderer.material_override_mask.  A set bit means "the
 * author set this factor on this renderer; do not overwrite it from the
 * material file". */
#define JCE_MR_OVERRIDE_BASE_COLOR   (1u << 0)
#define JCE_MR_OVERRIDE_METALLIC     (1u << 1)
#define JCE_MR_OVERRIDE_ROUGHNESS    (1u << 2)
#define JCE_MR_OVERRIDE_EMISSIVE     (1u << 3)
#define JCE_MR_OVERRIDE_NORMAL_SCALE (1u << 4)
#define JCE_MR_OVERRIDE_AO_STRENGTH  (1u << 5)
/* Clearcoat and sheen together, as ONE bit and not four: the two lobes are
 * a single authoring decision ("this instance is the lacquered one"), and a
 * coat roughness without a coat is not a state anyone asks for.  APPENDED. */
#define JCE_MR_OVERRIDE_LOBES        (1u << 6)

/* Every override bit that is defined today.  A renderer whose mask is this
 * takes nothing from its material file. */
#define JCE_MR_OVERRIDE_ALL          0x7Fu

/* Copy `mat`'s PBR factors into `mr`, SKIPPING every factor `mr` overrides.
 *
 * This is the single authority for that rule: the runtime scene loader, the
 * editor's path-repair pass, and the inspector's material load/reload all
 * call it, so a renderer cannot lose an override through whichever path
 * happens to touch it.  Only the six masked factors are handled -- textures,
 * alpha mode, cutoff, priority and double-sided are the caller's business
 * because they differ per call site.
 *
 * Both pointers must be non-NULL; the call is a no-op otherwise. */
/* The INVERSE direction, and the single authority for it: fill a
 * JcePbrMaterial's factors from a renderer, for the draw that is about to
 * happen.  `out` must already be jce_pbr_material_default(); this writes the
 * authored factors over it and touches no texture handle, because which
 * texture a draw binds is a per-path decision (SSAO substitutes the AO map,
 * the texture-array batcher leaves albedo to the array layer) and the factors
 * are not.
 *
 * WHY IT IS A FUNCTION.  This block was written out FOUR times in
 * jce_sr_draw.c, three of them byte-identical and the fourth also copying
 * render_priority -- so three of the four MeshRenderer draw paths silently
 * dropped a field.  (Harmlessly, as it happens: those three are opaque-only
 * and render_priority orders transparents. That is luck, not design, and the
 * next field would not have been so lucky -- adding UV tiling to four hand
 * copies is exactly how the fifth one gets missed.) */
JCE_API void jce_mesh_renderer_to_pbr(const JceMeshRenderer *mr,
                                      JcePbrMaterial *out);

JCE_API void jce_mesh_renderer_apply_material_pbr(JceMeshRenderer *mr,
                                                  const JcePbrMaterial *mat);

/* How many factors this renderer overrides (0..6).  For UI that has to say
 * so, and for tests that assert a mask round-tripped. */
JCE_API int jce_mesh_renderer_override_count(const JceMeshRenderer *mr);

JCE_EXTERN_C_END

#endif /* JCE_MATERIAL_OVERRIDE_H */
