/*
 * jce_planar_reflection.h -- render the scene again, mirrored through a plane.
 *
 * A reflection probe is a cube captured once from a point, so a mirror or a
 * still lake shows the room from the WRONG place and never moves with the
 * camera.  Screen-space reflections move correctly but can only reflect what
 * is already on screen, so a lake reflects nothing above the horizon.  A
 * planar reflection is the third answer: mirror the camera through the plane,
 * render the scene from there into an offscreen target, and sample it back
 * projectively.  Unity ships it as a Planar Reflection Probe, Unreal as a
 * Planar Reflection actor; Godot 4 has neither.
 *
 * TWO LIMITS, both forced by measurement rather than chosen:
 *
 *   IT IS ONE FRAME LATE.  bgfx executes views in ascending id order and the
 *   surface that samples the reflection is drawn in the scene's COLOUR view,
 *   which is view_id_base + 0 -- so no base-relative id can render before it.
 *   The reflection therefore renders into a reclaimed absolute band ABOVE
 *   every base and is consumed by the next frame.  On a fast camera pan the
 *   reflection lags by one frame; standing still it is exact.  The
 *   alternative was to move the water pass out of the colour view, which
 *   reorders it against every other transparent thing in the scene.
 *
 *   IT REFLECTS ONTO WATER, NOT ONTO ARBITRARY PBR SURFACES.  fs_pbr declares
 *   SIXTEEN samplers in slots 0..15 with none free -- measured with
 *   `jce.py shader-inspect engine/shaders/pbr/fs_pbr.sc`, not assumed -- and
 *   bgfx's sampler ceiling is 16.  fs_water has seven free, which is where
 *   this lands.  A general mirror material needs its own shader; that is a
 *   shader, not a wire, and the ledger row says so.
 *
 * The second render is a REDUCED one (JceSceneRenderConfig::reduced_pass): no
 * shadows, no transparent pass, no post-processing and no screen-space
 * effects.  The last is not an aesthetic choice -- see that field.
 *
 * Layer: renderer (Layer 3) -- public.
 */
#ifndef JCE_PLANAR_REFLECTION_H
#define JCE_PLANAR_REFLECTION_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_gfx_types.h>
#include <jce/resource/jce_pak_loader.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JcePlanarReflection JcePlanarReflection;
typedef struct JceScene            JceScene;
typedef struct JceSceneRenderer    JceSceneRenderer;
typedef struct JceCamera           JceCamera;

typedef struct JceRenderer JceRenderer;

typedef struct {
    JceRenderer *renderer;
    /* The bridge's own view id, which is also the base the reflection render
     * is given.  Must be ABOVE every scene base -- see the one-frame note. */
    uint16_t     view_id;
    /* Render target size.  A reflection is read through a rough surface, so
     * half the viewport is the usual choice and the default. */
    int          width;
    int          height;
    /* Appended: the archive the APPLY pass loads its program from.  NULL
     * means "mirror only" -- the render still works and the texture is still
     * handed out, which is what the water path has always used.  A caller who
     * wants jce_planar_reflection_apply() has to say where the shaders live,
     * and gets a clean no-op rather than a crash when it does not. */
    const JcePakArchive *pak;
} JcePlanarReflectionDesc;

JCE_API JcePlanarReflection *jce_planar_reflection_create(
    const JcePlanarReflectionDesc *desc);
JCE_API void jce_planar_reflection_destroy(JcePlanarReflection *pr);

/* Render the scene mirrored through the horizontal plane at world height
 * `plane_y`, from `camera`'s mirrored position, into the internal target.
 *
 *   view_id_base : the reflection's OWN base.  It must be above every scene
 *                  base, because this runs after the main colour pass -- see
 *                  the one-frame note in the file header.
 *
 * Does nothing and leaves the previous frame's texture in place when the
 * camera is below the plane: a reflection of a surface you are under is the
 * refraction, and drawing the mirrored world there is worse than not. */
JCE_API void jce_planar_reflection_render(JcePlanarReflection *pr,
                                          JceSceneRenderer *sr,
                                          JceScene *scene,
                                          const JceCamera *camera,
                                          float plane_y,
                                          uint16_t view_id_base,
                                          float dt_sec);

/* The colour target.  Invalid (idx == UINT16_MAX) until the first render. */
JCE_API JceTextureHandle jce_planar_reflection_texture(
    const JcePlanarReflection *pr);

/* The view-projection the last render used, for the surface shader to project
 * the texture with.  Identity before the first render. */
JCE_API jce_mat4 jce_planar_reflection_view_proj(const JcePlanarReflection *pr);

/* THE SAME MIRROR, THROUGH ANY PLANE.
 *
 * `plane_point` is any point on it and `plane_normal` its normal, which is
 * normalised here rather than demanded normalised -- a caller reading a
 * surface normal off a mesh should not have to round it first.  A zero-length
 * normal is refused and the previous frame's texture is left alone.
 *
 * `consumer_aspect` is the aspect ratio of the view that will SAMPLE this
 * reflection, not of the render target.  The two are different numbers and
 * using the wrong one is invisible in the mirror image and wrong everywhere
 * it is read: the target is 640x360 (1.778) while a viewport is whatever the
 * user dragged it to (1.521 here).  MEASURED: the mirror's view-projection
 * came out as the main one composed with the plane reflection -- correct to
 * the last digit in y -- with its x row scaled by exactly 1.521/1.778, so
 * every projective lookup landed 14% toward the centre of the image and the
 * mirror showed the wrong part of the world.  Pass <= 0 to fall back to the
 * target's own aspect, which is what the shape of the render target says and
 * what this did before.
 *
 * jce_planar_reflection_render() above is this function with the horizontal
 * plane y = plane_y, and stays because water is horizontal and says so at the
 * call site.  A floor is horizontal too; a mirror on a wall is not, and a
 * probe the user can rotate had to be able to say which way it faces. */
JCE_API void jce_planar_reflection_render_plane(JcePlanarReflection *pr,
                                                JceSceneRenderer *sr,
                                                JceScene *scene,
                                                const JceCamera *camera,
                                                jce_vec3 plane_point,
                                                jce_vec3 plane_normal,
                                                float consumer_aspect,
                                                uint16_t view_id_base,
                                                float dt_sec);

/* What the mirror is applied to, and why it is a separate call.
 *
 * The reflection above is a TEXTURE; something has to decide which pixels it
 * belongs on.  Sampling it from a material shader is how this engine did it
 * first, and it reaches exactly the materials that have a free sampler slot
 * -- fs_pbr has none of sixteen, so that meant water and nothing else.
 *
 * This composites in SCREEN SPACE instead, over every pixel that lies on the
 * plane, faces the same way and sits inside the influence box, whatever
 * material drew it.  It reads the depth buffer and the SSR G-buffer normal
 * (whose alpha carries roughness) and blends premultiplied "over", the same
 * blend the SSR composite uses -- so a polished floor, a wet road and a still
 * lake all get it and none of them needed a shader written for them.
 *
 *   view_id       : must be ABOVE the view that drew the scene colour.
 *   dst_fb        : the colour framebuffer to blend into.  A struct handle,
 *                   not a bare uint16_t: the public-abi raw-handle ratchet
 *                   refuses new bare indices in renderer headers, and it is
 *                   right to -- an index that is interchangeable with a view
 *                   id at compile time eventually gets passed as one.
 *   depth_tex     : the scene's depth buffer for this frame.
 *   normal_tex    : fs_gbuffer's MRT[0]; invalid means "no G-buffer this
 *                   frame", and this returns without drawing rather than
 *                   guessing a normal for every pixel.
 *   inv_view_proj : the MAIN camera's, to rebuild world position from depth.
 *   thickness_m   : how far off the plane a pixel may sit, metres.
 *   max_angle_deg : how far its normal may tilt from the plane's.
 *   intensity     : scales the mirrored colour, 1 = as rendered.
 *   max_roughness : above this the surface scatters too much to mirror.
 *   box_centre / box_half : the influence volume, world space.
 *   edge_fade_m   : how far inside the box the reflection fades out, so the
 *                   boundary is not a line drawn across a continuing floor.
 *   dst_width/height : the destination view rect, in pixels.  NOT optional and
 *                   not inferred: a fullscreen pass whose view rect was never
 *                   set inherits whatever the last user of that bgfx view id
 *                   left there, and this pass SHARES its id with the SSR
 *                   composite -- so it would be correct in every frame SSR
 *                   also ran and silently draw into a stale rect in the rest.
 *
 * Does nothing at all when the last render was skipped (camera under the
 * plane) or when the pak in the desc was NULL. */
JCE_API void jce_planar_reflection_apply(JcePlanarReflection *pr,
                                         uint16_t view_id,
                                         JceFrameBufferHandle dst_fb,
                                         JceTextureHandle depth_tex,
                                         JceTextureHandle normal_tex,
                                         const jce_mat4 *inv_view_proj,
                                         jce_vec3 plane_point,
                                         jce_vec3 plane_normal,
                                         float thickness_m,
                                         float max_angle_deg,
                                         float intensity,
                                         float max_roughness,
                                         jce_vec3 box_centre,
                                         jce_vec3 box_half,
                                         float edge_fade_m,
                                         uint16_t dst_width,
                                         uint16_t dst_height);

/* NO resize() and NO valid() HERE ON PURPOSE.  The target is re-prepared at
 * its size on every render, so a separate resize would be a second way to say
 * the same thing; and "did it render" is already answered by
 * jce_planar_reflection_texture() returning an invalid handle.  Two functions
 * nobody would call is exactly the debt this audit is about. */

JCE_EXTERN_C_END

#endif /* JCE_PLANAR_REFLECTION_H */
