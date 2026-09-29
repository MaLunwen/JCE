/*
 * jce_sr_ribbon.c  See jce_sr_ribbon.h.
 *
 * Moved out of jce_sr_environment.c unchanged except where noted: the ribbon
 * now carries a texcoord and can submit through the textured program when the
 * component's material_path resolves to an image.  With no material (or a
 * material that names no base-colour texture) the geometry, the state word and
 * the colour program are the ones that were there before, so an untextured
 * ribbon renders exactly as it did.
 */

#include "jce_sr_ribbon.h"
#include "jce_sr_internal.h"

#include "renderer/jce_renderer_internal.h"   /* program_textured (pos+col+uv) */

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>
#include <jce/renderer/jce_pbr_material.h>

#include <bgfx/c99/bgfx.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ── material_path -> image path ───────────────────────────────────── */

static char sr_ribbon_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool sr_ribbon_ends_with_ci(const char *s, const char *suffix)
{
    if (!s || !suffix) return false;
    size_t sl = strlen(s), tl = strlen(suffix);
    if (tl > sl) return false;
    s += sl - tl;
    for (size_t i = 0; i < tl; i++)
        if (sr_ribbon_lower(s[i]) != sr_ribbon_lower(suffix[i])) return false;
    return true;
}

/* Exists, through whichever filesystem is authoritative right now.  Same two
 * branches as sse_file_exists() in the scene loader, for the same reason: a
 * PAK-only exe has no host paths and the editor has no VFS mounted unless a
 * bundle preview is open. */
static bool sr_ribbon_exists(const char *p)
{
    if (!p || !*p) return false;
    const JceFileSystem *fs = jce_fs_get_active();
    if (fs) return jce_fs_exists(fs, p);
    return jce_fs_host_exists_file(p);
}

/* Make a texture reference from inside a material file whole.
 *
 * jce_pbr_material_load_json() hands back the string as written -- it resolves
 * the material's SHADER siblings but deliberately not its textures, because
 * different callers anchor them differently.  Unity-exported material files
 * name "Textures/x.png" relative to the project root while engine-native ones
 * name a sibling, so both anchors have to be tried; this is the same ladder
 * the scene loader walks for MeshRenderer.albedoTex.
 *
 * Falls back to the reference verbatim when nothing on the ladder exists: the
 * asset may simply not be cooked yet, and sr_resolve_texture() retries a path
 * it could not find, so guessing wrong here costs a frame, not the texture. */
static void sr_ribbon_anchor_texture(const char *mat_path, const char *tex,
                                     char *out, size_t cap)
{
    out[0] = '\0';
    if (!tex || !tex[0]) return;

    if (jce_path_is_absolute(tex) || sr_ribbon_exists(tex)) {
        snprintf(out, cap, "%s", tex);
        return;
    }

    /* jce_path_parent, not a local copy: the scene loader already carries one
     * (sse_path_parent_inplace) and the dedup detector counted a second as a
     * regression the moment this file was added -- correctly, since the public
     * helper aliases in place and has the same contract. */
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", mat_path);
    if (jce_path_parent(dir, sizeof(dir), dir)) {
        char cand[1024];
        /* (1) sibling of the material file. */
        if (jce_path_join(cand, sizeof(cand), dir, tex) &&
            sr_ribbon_exists(cand)) {
            snprintf(out, cap, "%s", cand);
            return;
        }
        /* (2) up to four levels above it (Unity layout: Materials/ and
         *     Textures/ are siblings under the project root). */
        for (int up = 0; up < 4; up++) {
            if (!jce_path_parent(dir, sizeof(dir), dir)) break;
            if (jce_path_join(cand, sizeof(cand), dir, tex) &&
                sr_ribbon_exists(cand)) {
                snprintf(out, cap, "%s", cand);
                return;
            }
        }
    }
    snprintf(out, cap, "%s", tex);
}

bool sr_material_albedo_path(const char *material_path, char *out, size_t cap)
{
    if (!material_path || !material_path[0] || !out || cap == 0) return false;
    out[0] = '\0';

    const bool is_mat = sr_ribbon_ends_with_ci(material_path, ".mat.json") ||
                        sr_ribbon_ends_with_ci(material_path, ".mat");
    if (!is_mat) {
        /* An image authored directly.  The Inspector picker offers both, and
         * a path that is already a texture must not be run through a material
         * parse that would only fail. */
        snprintf(out, cap, "%s", material_path);
        return true;
    }

    /* ".mat" is an accepted alias for ".mat.json" (the scene loader accepts
     * it too, because Unity exports name materials without the second
     * extension). */
    char mat[512];
    snprintf(mat, sizeof(mat), "%s", material_path);
    if (sr_ribbon_ends_with_ci(mat, ".mat") && !sr_ribbon_exists(mat)) {
        char alias[520];
        snprintf(alias, sizeof(alias), "%s.json", mat);
        if (sr_ribbon_exists(alias)) snprintf(mat, sizeof(mat), "%s", alias);
    }

    JcePbrMaterial m;
    char tex[5][256];
    const JceFileSystem *fs = jce_fs_get_active();
    /* The VFS call is FIRST and the host call is the fallback, which is the
     * order jce_fs_get_active()'s ISOLATED policy asks for.  Measured while
     * writing this: the second line alone would have sufficed, because
     * jce_fs_host_read_all() itself redirects through the active VFS when one
     * is installed (jce_filesystem.h documents the hook) -- a negative control
     * that disabled the first line left the mounted-VFS test green.  Both are
     * kept anyway: the redirection is a reader hook that jce_fs_set_active()
     * happens to install, while _vfs names the filesystem outright, and a
     * caller that installs a custom reader must not silently lose the bundle. */
    bool ok = fs && jce_pbr_material_load_json_vfs(fs, mat, &m, tex);
    if (!ok) ok = jce_pbr_material_load_json(mat, &m, tex);
    if (!ok) return false;

    sr_ribbon_anchor_texture(mat, tex[0], out, cap);
    return out[0] != '\0';
}

/* ── arc-length parameter ─────────────────────────────────────────── */

static float sr_ribbon_seg_len(const float a[3], const float b[3])
{
    const float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
    return sqrtf(dx*dx + dy*dy + dz*dz);
}

int sr_ribbon_arc_u(const float (*points)[3], int pts, bool loop,
                    float *out_u, int cap)
{
    if (!points || !out_u || pts < 1 || cap < 1) return 0;
    const int n = (pts < cap) ? pts : cap;

    float total = 0.0f;
    for (int i = 0; i + 1 < pts; i++)
        total += sr_ribbon_seg_len(points[i], points[i + 1]);
    if (loop && pts > 1)
        total += sr_ribbon_seg_len(points[pts - 1], points[0]);

    if (!(total > 0.0f)) {          /* also catches NaN */
        for (int i = 0; i < n; i++) out_u[i] = 0.0f;
        return n;
    }

    const float inv = 1.0f / total;
    float run = 0.0f;
    for (int i = 0; i < n; i++) {
        out_u[i] = run * inv;
        if (i + 1 < pts) run += sr_ribbon_seg_len(points[i], points[i + 1]);
    }
    return n;
}

/* ── material → texture, memoised on the renderer ──────────────── */

/* A ribbon asks for its texture every frame and the answer costs a file read
 * plus a JSON parse, so it is cached -- on sr->prog_cache, the renderer's
 * existing material-path cache, NOT in a file-scope table.  The first version
 * of this used a static, and tools/audit/find_similar_code.py + the
 * global-state freeze caught it as two separate regressions on the same day:
 * a duplicated path helper and one more process-wide mutable.  Both were
 * right.  Renderer-owned state also gets the lifetime for free -- a cache
 * keyed by material path is only valid for the project the renderer was
 * created for, and a file-scope one would have had to notice a bundle preview
 * swapping jce_fs_get_active() underneath it.
 *
 * ENTRIES ARE CREATED BY sr_resolve_custom_program AND ONLY BY IT.  If this
 * function created them too, a material shared between a ribbon and a mesh
 * would be cached with program = "none" by whichever asked first, and the
 * mesh would silently lose its Shader Graph shader.  So we ask that function
 * first (it caches, so this costs one lookup) and then fill in the albedo
 * half of the entry it made.  It only makes entries for .mat.json, which is
 * what the Inspector's material picker writes; an authored image path costs
 * nothing to "resolve" anyway, and the ".mat" alias is legacy. */
static JceTexture sr_ribbon_texture(JceSceneRenderer *sr, const char *mat_path)
{
    JceTexture invalid = { UINT16_MAX };
    if (!sr || !mat_path || !mat_path[0]) return invalid;

    (void)sr_resolve_custom_program(sr, mat_path);

    char *slot = NULL;
    for (int i = 0; i < sr->prog_cache_count; i++) {
        if (strncmp(sr->prog_cache[i].path, mat_path,
                    sizeof(sr->prog_cache[i].path)) != 0)
            continue;
        if (sr->prog_cache[i].albedo_done)
            return sr->prog_cache[i].albedo[0]
                 ? sr_resolve_texture(sr, sr->prog_cache[i].albedo)
                 : invalid;
        slot = sr->prog_cache[i].albedo;
        sr->prog_cache[i].albedo_done = true;
        break;
    }

    char local[256];
    char *img = slot ? slot : local;
    const size_t cap = 256;

    /* When the material cannot be read from here, hand the AUTHORED path to
     * sr_resolve_texture and let it try.
     *
     * This is not the editor/shipping split this file exists to avoid, and the
     * distinction is worth stating because it looks like one.  A cooked
     * runtime has a VFS mounted and material vpaths are project-relative
     * inside it, so sr_material_albedo_path() answers above and this line
     * never runs.  It runs in the editor on a loose source project, where the
     * authored path is relative to the SCENE FILE and the base directory that
     * would anchor it is thread-local to the loader and cleared when the load
     * finishes (jce_scene_serial_set_base_dir(NULL)) -- unreachable from a
     * draw.  The editor's own resolver does have that context, so asking it is
     * strictly better than drawing untextured, and the exe is unaffected
     * either way. */
    if (!sr_material_albedo_path(mat_path, img, cap))
        snprintf(img, cap, "%s", mat_path);

    return img[0] ? sr_resolve_texture(sr, img) : invalid;
}

/* ── the ribbon ───────────────────────────────────────────────────── */

/* Pack a float rgba [0..1] into bgfx 0xAABBGGRR. */
static uint32_t sr_line_abgr(const float c[4])
{
    float r = c[0] < 0.0f ? 0.0f : (c[0] > 1.0f ? 1.0f : c[0]);
    float g = c[1] < 0.0f ? 0.0f : (c[1] > 1.0f ? 1.0f : c[1]);
    float b = c[2] < 0.0f ? 0.0f : (c[2] > 1.0f ? 1.0f : c[2]);
    float a = c[3] < 0.0f ? 0.0f : (c[3] > 1.0f ? 1.0f : c[3]);
    uint32_t ri = (uint32_t)(r * 255.0f + 0.5f);
    uint32_t gi = (uint32_t)(g * 255.0f + 0.5f);
    uint32_t bi = (uint32_t)(b * 255.0f + 0.5f);
    uint32_t ai = (uint32_t)(a * 255.0f + 0.5f);
    return (ai << 24) | (bi << 16) | (gi << 8) | ri;
}

/* Transform a point by a column-major jce_mat4 (w = 1). */
static jce_vec3 sr_line_xf(const jce_mat4 *m, float x, float y, float z)
{
    jce_vec3 r;
    r.x = m->col[0].x*x + m->col[1].x*y + m->col[2].x*z + m->col[3].x;
    r.y = m->col[0].y*x + m->col[1].y*y + m->col[2].y*z + m->col[3].y;
    r.z = m->col[0].z*x + m->col[1].z*y + m->col[2].z*z + m->col[3].z;
    return r;
}

/* Shared camera-facing TRIANGLE RIBBON (PT_LINES produces nothing in the editor
 * pre-postfx offscreen; triangles via the color program DO render).  Width-
 * expanded quads, colour + width lerped along the polyline, optionally looped.
 * Points are transformed by `model` (identity => already world).  No new shader.
 * Shared by the Line renderer (loopable, local/world) and the Trail renderer
 * (open, world-space captured points).
 *
 * `tex` invalid => the colour program, unchanged.  Valid => vs/fs_textured,
 * which is `texel * v_color0`: the authored gradient tints the material rather
 * than replacing it, which is what Unity's default does for both components. */
static void sr_draw_ribbon(JceSceneRenderer *sr, const JceCamera *camera,
                           uint16_t view_id, const jce_mat4 *model,
                           const float (*points)[3], int count, bool loop,
                           float w0, float w1,
                           const float c0[4], const float c1[4],
                           JceTexture tex)
{
    if (count < 2) return;
    int pts = count;
    if (pts > JCE_LINE_MAX_POINTS) pts = JCE_LINE_MAX_POINTS;
    int segs = loop ? pts : (pts - 1);
    if (segs < 1) return;

    /* Decide the program ONCE, before anything is written: a half-armed
     * texture (valid handle, missing sampler uniform) must fall all the way
     * back to the colour program rather than submit a textured draw with
     * nothing bound, which samples black. */
    bgfx_program_handle_t prog_tex = jce_renderer_get_program_textured(sr->renderer);
    JceUniformHandle      uh       = jce_renderer_get_tex_uniform(sr->renderer);
    bgfx_uniform_handle_t sampler; sampler.idx = uh.idx;
    bgfx_texture_handle_t btex;    btex.idx    = tex.idx;
    const bool textured = jce_texture_valid(tex) &&
                          BGFX_HANDLE_IS_VALID(prog_tex) &&
                          BGFX_HANDLE_IS_VALID(sampler);

    float uarc[JCE_LINE_MAX_POINTS];
    const int n_u = sr_ribbon_arc_u(points, pts, loop, uarc,
                                    (int)(sizeof(uarc) / sizeof(uarc[0])));

    uint32_t vcount = (uint32_t)segs * 4u;   /* quad per segment */
    uint32_t icount = (uint32_t)segs * 6u;

    /* TEXCOORD0 is declared even for an untextured ribbon.  bgfx binds only
     * the attributes the bound program declares, so vs_color ignores it; one
     * layout keeps the two cases on a single code path, which is the only way
     * a control on the textured path also exercises the untextured one. */
    bgfx_vertex_layout_t layout;
    bgfx_vertex_layout_begin(&layout, bgfx_get_renderer_type());
    bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_POSITION,  3, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_COLOR0,    4, BGFX_ATTRIB_TYPE_UINT8, true,  false);
    bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_TEXCOORD0, 2, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&layout);

    bgfx_transient_vertex_buffer_t tvb;
    bgfx_transient_index_buffer_t  tib;
    if (!bgfx_alloc_transient_buffers(&tvb, &layout, vcount, &tib, icount, false))
        return;

    struct SrLineVtx { float x, y, z; uint32_t abgr; float u, v; };
    struct SrLineVtx *v = (struct SrLineVtx *)tvb.data;
    uint16_t *idx = (uint16_t *)tib.data;

    jce_vec3 eye = camera ? jce_camera_get_position(camera)
                          : jce_v3(0.0f, 0.0f, 1.0e4f);

    uint32_t vi = 0, ii = 0;
    for (int s = 0; s < segs; ++s) {
        int   ia = s;
        int   ib = (s + 1) % pts;
        float ta = (pts > 1) ? (float)ia / (float)(pts - 1) : 0.0f;
        float tb = (loop && s == segs - 1)
                     ? 1.0f
                     : ((pts > 1) ? (float)ib / (float)(pts - 1) : 0.0f);

        /* Colour and width keep the index parameter they have always used --
         * changing them would restyle every existing ribbon.  Only the
         * texture coordinate is arc-length, because only it is a mapping onto
         * an image and only it swims when points expire. */
        float ua = (ia < n_u) ? uarc[ia] : ta;
        float ub = (loop && s == segs - 1) ? 1.0f
                                           : ((ib < n_u) ? uarc[ib] : tb);

        jce_vec3 wa = sr_line_xf(model, points[ia][0], points[ia][1], points[ia][2]);
        jce_vec3 wb = sr_line_xf(model, points[ib][0], points[ib][1], points[ib][2]);

        jce_vec3 dir = jce_v3_sub(wb, wa);
        float    dl  = jce_v3_len(dir);
        if (dl < 1e-6f) continue;
        dir = jce_v3_scale(dir, 1.0f / dl);
        jce_vec3 mid = jce_v3_scale(jce_v3_add(wa, wb), 0.5f);
        jce_vec3 vdir = jce_v3_sub(mid, eye);
        float    vl = jce_v3_len(vdir);
        vdir = (vl > 1e-6f) ? jce_v3_scale(vdir, 1.0f / vl) : jce_v3(0,0,1);
        jce_vec3 side = jce_v3_cross(dir, vdir);
        float    sl = jce_v3_len(side);
        if (sl < 1e-6f) { side = jce_v3(0,1,0); sl = 1.0f; }
        side = jce_v3_scale(side, 1.0f / sl);

        float wda = w0 + (w1 - w0) * ta;
        float wdb = w0 + (w1 - w0) * tb;
        /* Minimum width is a SCREEN-SPACE quantity, not a world one.
         *
         * This used to be `if (wda < 0.05f) wda = 0.05f;` -- 0.05 WORLD units,
         * unconditionally.  A floor is right (a ribbon thinner than a pixel
         * aliases or vanishes) but metres are the wrong unit for it: 0.05 m is
         * 5% of the frame in a one-metre shot and invisible in a 500 m one.
         *
         * Measured cost of the old floor: every authored width in space/ was
         * below it -- net 0.008, tether 0.018, scene lines 0.010 and 0.015 --
         * so all five rendered identically at 0.05.  The net's eight meridians
         * came out 6.25x too thick, about 29% of the bag's silhouette, and a
         * deliberate "make the tether thinner" edit (0.030 -> 0.018) changed
         * exactly zero pixels because both clamp to the same floor.
         *
         * The floor now asks for ~1.2 px at this segment's own distance:
         *   px_per_m = vp_h / (2 * dist * tan(fov_y/2))
         * so an authored width is honoured whenever it is resolvable, and only
         * a genuinely sub-pixel ribbon is widened.  vl is |mid - eye|, already
         * computed above for the billboard basis. */
        {
            /* JceCamera is opaque in this TU -- go through the accessor. */
            const float cam_fov = camera ? jce_camera_get_fov(camera) : 0.0f;
            const float fov_y = (cam_fov > 1.0f) ? cam_fov : 60.0f;
            const float vp_h = (sr && sr->last_vp_h) ? (float)sr->last_vp_h
                                                     : 1080.0f;
            const float half = tanf(fov_y * 0.5f * 3.14159265358979f / 180.0f);
            float min_w = 0.0f;
            if (vl > 1e-6f && half > 1e-6f)
                min_w = 1.2f * (2.0f * vl * half) / vp_h;
            if (wda < min_w) wda = min_w;
            if (wdb < min_w) wdb = min_w;
        }
        jce_vec3 sa = jce_v3_scale(side, wda * 0.5f);
        jce_vec3 sb = jce_v3_scale(side, wdb * 0.5f);

        float ca[4], cb[4];
        for (int c = 0; c < 4; ++c) {
            float d = c1[c] - c0[c];
            ca[c] = c0[c] + d * ta;
            cb[c] = c0[c] + d * tb;
        }
        uint32_t col_a = sr_line_abgr(ca), col_b = sr_line_abgr(cb);

        jce_vec3 p0 = jce_v3_add(wa, sa), p1 = jce_v3_sub(wa, sa);
        jce_vec3 p2 = jce_v3_add(wb, sb), p3 = jce_v3_sub(wb, sb);
        uint32_t base = vi;
        v[vi].x=p0.x; v[vi].y=p0.y; v[vi].z=p0.z; v[vi].abgr=col_a; v[vi].u=ua; v[vi].v=0.0f; ++vi;
        v[vi].x=p1.x; v[vi].y=p1.y; v[vi].z=p1.z; v[vi].abgr=col_a; v[vi].u=ua; v[vi].v=1.0f; ++vi;
        v[vi].x=p2.x; v[vi].y=p2.y; v[vi].z=p2.z; v[vi].abgr=col_b; v[vi].u=ub; v[vi].v=0.0f; ++vi;
        v[vi].x=p3.x; v[vi].y=p3.y; v[vi].z=p3.z; v[vi].abgr=col_b; v[vi].u=ub; v[vi].v=1.0f; ++vi;
        idx[ii++]=(uint16_t)base;     idx[ii++]=(uint16_t)(base+1); idx[ii++]=(uint16_t)(base+2);
        idx[ii++]=(uint16_t)(base+1); idx[ii++]=(uint16_t)(base+3); idx[ii++]=(uint16_t)(base+2);
    }
    if (ii == 0) return;

    bgfx_set_transient_vertex_buffer(0, &tvb, 0, vcount);
    bgfx_set_transient_index_buffer(&tib, 0, ii);
    jce_mat4 ident = jce_m4_identity();
    bgfx_set_transform(ident.raw[0], 1);   /* positions are already world */

    /* Depth-TEST against opaque geometry (so a ribbon behind a tree/rock is
     * occluded) but do NOT write depth: these are translucent ribbons (wind
     * streaks, lightning, trails).  A blended primitive that writes Z punches a
     * depth hole that rejects any transparent content drawn AFTER it at greater
     * depth — the wind lines then appear to "cut through" / erase the rain,
     * smoke and particles behind them.  Matches the reference WindLines material
     * (transparent:true, depthWrite:false). */
    /* WRITE_Z matters as much as the depth test here. The ribbon is thin,
     * effectively opaque along its core, and there is no guarantee it is
     * submitted after every opaque mesh it crosses. Without a depth write,
     * anything drawn later simply overwrites it: an orbit line unmistakably
     * in front of a planet disappeared the moment the planet's disc was
     * behind it, while the same line stayed visible against empty space.
     *
     * The two paragraphs above contradict each other and both are kept,
     * because the second one won on evidence and the first one records the
     * cost of that decision: WRITE_Z is what ships, and a ribbon material with
     * cut-out alpha will need a discard shader of its own before it can be
     * both textured and correctly composited. */
    uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                   | BGFX_STATE_WRITE_Z
                   | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_MSAA | BGFX_STATE_BLEND_ALPHA;
    bgfx_set_state(state, 0);   /* no cull: ribbon is double-sided */

    bgfx_program_handle_t prog;
    if (textured) {
        bgfx_set_texture(0, sampler, btex, UINT32_MAX);
        prog = prog_tex;
    } else {
        JceShaderHandle sh = jce_renderer_get_program_color(sr->renderer);
        prog.idx = sh.idx;
    }
    if (BGFX_HANDLE_IS_VALID(prog))
        bgfx_submit(view_id, prog, 0, BGFX_DISCARD_ALL);
}

/* Line Renderer — colour/width-lerped ribbon, loopable; local points ride the
 * entity transform, world-space points draw directly. */
void sr_draw_line_renderer(JceSceneRenderer *sr, JceScene *scene,
                           EntityList *list, JceEntity e,
                           const JceCamera *camera, uint16_t view_id)
{
    (void)list;
    JceLineRendererComponent *lr = jce_scene_get_line_renderer(scene, e);
    if (!lr || lr->position_count < 2) return;
    jce_mat4 model = lr->use_world_space ? jce_m4_identity()
                                         : jce_scene_get_world_matrix(scene, e);
    sr_draw_ribbon(sr, camera, view_id, &model, lr->positions, lr->position_count,
                   lr->loop, lr->width_start, lr->width_end,
                   lr->color_start, lr->color_end,
                   sr_ribbon_texture(sr, lr->material_path));
}

/* Trail Renderer — the runtime-captured world-space point trail drawn as an open
 * ribbon.  Capture/growth happens in the runtime (rt_update_trails); here we
 * just draw whatever points the buffer currently holds (also previews a scene's
 * serialized trail in the editor). */
void sr_draw_trail_renderer(JceSceneRenderer *sr, JceScene *scene,
                            EntityList *list, JceEntity e,
                            const JceCamera *camera, uint16_t view_id)
{
    (void)list;
    JceTrailRendererComponent *tr = jce_scene_get_trail_renderer(scene, e);
    if (!tr || tr->point_count < 2) return;
    jce_mat4 ident = jce_m4_identity();   /* trail points are already world */
    sr_draw_ribbon(sr, camera, view_id, &ident, tr->points, tr->point_count,
                   false, tr->width_start, tr->width_end,
                   tr->color_start, tr->color_end,
                   sr_ribbon_texture(sr, tr->material_path));
}
