/*
 * jce_sr_ribbon.h  The camera-facing triangle ribbon shared by LineRenderer
 * and TrailRenderer -- and the material -> texture resolution that makes
 * their authored material_path mean something.
 *
 * WHAT WAS WRONG.  JceLineRendererComponent.material_path and
 * JceTrailRendererComponent.material_path were authored, serialized, shown in
 * the Inspector with a material picker, and read by nothing: sr_draw_ribbon
 * submitted through jce_renderer_get_program_color(), which has no sampler at
 * all.  Every ribbon in every project drew as a flat vertex-colour gradient no
 * matter which material was picked.
 *
 * WHY THE OBVIOUS WIRE WOULD NOT HAVE SHIPPED.  The reflex is
 * `sr_resolve_texture(sr, lr->material_path)`.  That resolver has two halves:
 * in the editor it forwards to a host callback (the asset cache, which parses
 * .mat.json); in the shipping runtime it hands the path to
 * jce_texture_decode_cpu(), whose extension whitelist -- .png .jpg .tga .dds
 * .ktx .ktx2 .bmp .hdr .webp .psd .gif .jceasset -- does not contain .json.
 * So a designer who used the shipped picker would see a textured ribbon in the
 * editor and an untextured one in the exe, for 100% of authorable values.  A
 * wire that is dead in the shipping configuration is the same defect the field
 * gate exists to catch, moved one layer down.
 *
 * WHAT IS DONE INSTEAD is what the MeshRenderer path already does: resolve the
 * material file to the IMAGE path its albedo slot names, and hand THAT to
 * sr_resolve_texture -- whose runtime half accepts it.  The material read goes
 * through jce_pbr_material_load_json_vfs(jce_fs_get_active(), ...) with a host
 * fallback, the same two-branch shape sse_file_exists() uses in the scene
 * loader, so the same file is found in a PAK-only exe and in the editor.
 *
 * SCOPE OF "MATERIAL" HERE.  One base-colour texture, multiplied by the
 * authored colour gradient (fs_textured.sc is literally `texel * v_color0`).
 * That is Unity's default meaning for these two components -- an unlit,
 * Sprites-style material tinted by the gradient -- not a full JcePbrMaterial.
 * A ribbon does not get normal maps, roughness or lighting, and an alpha-cut
 * ribbon material would need its own discard shader, which does not exist yet.
 *
 * SPLIT OUT of jce_sr_environment.c like jce_sr_veg_material.h was: that file
 * is size-frozen with 22 lines of headroom, and jce_sr_internal.h is frozen at
 * exactly its baseline, so neither is a place to spend budget on this.  The
 * two functions below are the DECIDING halves -- no bgfx, no JceSceneRenderer
 * -- so they can be asserted headlessly.  A submit that decides inside itself
 * can only be checked by rendering, and a ribbon with the wrong UVs looks like
 * a ribbon with the right UVs until it moves.
 *
 * Layer: Middleware/scene.  Internal to the scene renderer.
 */

#ifndef JCE_SR_RIBBON_H
#define JCE_SR_RIBBON_H

/* Deliberately NOT including jce_sr_internal.h: neither function below touches
 * bgfx or JceSceneRenderer, and that header drags in <bgfx/c99/bgfx.h>, which
 * a headless test cannot compile against.  A "pure half" that can only be
 * included by something holding a GPU device is not a pure half. */
#include <stdbool.h>
#include <stddef.h>

/* Resolve an authored material_path to the image path a sampler can load.
 *
 *   "Effects/Trail.mat.json"  ->  the albedoMap it names, made whole against
 *                                 the material's own directory
 *   "Textures/spark.png"      ->  itself (an image authored directly; the
 *                                 Inspector picker allows both)
 *
 * Reads through jce_fs_get_active() when a VFS is mounted and the host
 * filesystem otherwise, so the answer is the same in the editor and in a
 * PAK-only exe.  Returns false when the path is empty, the material cannot be
 * read, or it names no base-colour texture -- `out` is then empty and the
 * caller draws the ribbon untextured, exactly as before this file existed.
 *
 * NOT CACHED.  sr_ribbon_texture() memoises; this stays a pure request so a
 * test can call it against a fixture without a JceSceneRenderer.
 */
bool sr_material_albedo_path(const char *material_path, char *out, size_t cap);

/* Normalised CUMULATIVE ARC LENGTH at each point: out_u[i] = (length of the
 * polyline up to point i) / (total length), in [0,1].  For `loop`, the total
 * includes the closing segment, so the closing vertex lands exactly at 1.0.
 *
 * WHY NOT THE INDEX.  The colour and width lerp uses i/(pts-1), which is fine
 * for a gradient but wrong for a texture: it gives every segment an equal
 * share of the image regardless of how long it is, so the texture compresses
 * on short segments and stretches on long ones.  Worse for the trail, whose
 * points expire from the FRONT (rt_trail_step) and are renumbered -- under an
 * index parameterisation the whole texture slides one segment along the ribbon
 * every time the oldest point dies, which reads as the material swimming
 * against the motion.  Arc length is Unity's LineTextureMode.Stretch and does
 * not move when a point is dropped except by the length that point carried.
 *
 * Writes min(pts, cap) entries and returns that count; 0 on bad input.  A
 * degenerate polyline (total length 0) yields all zeros rather than NaN.
 */
int sr_ribbon_arc_u(const float (*points)[3], int pts, bool loop,
                    float *out_u, int cap);

#endif /* JCE_SR_RIBBON_H */
