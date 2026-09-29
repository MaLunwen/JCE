/*
 * jce_sr_sprites.c  Scene-renderer 2D pass: sprite animator, sprite,
 * billboard (split from jce_sr_draw.c).
 *
 * WHY IT MOVED.  jce_sr_draw.c is one of the nine files frozen over the
 * 3000-line cap by tools/lint/check_file_size.py, sitting exactly ON its
 * baseline: the previous two commits each landed by compacting comments to
 * buy back the lines they needed, and the one after that would have had
 * nothing left to sell.  The ratchet is not asking for shorter comments, it
 * is asking for this -- and a 2D draw path wedged into the middle of the 3D
 * type-dispatch ladder was the obvious thing to lift out.
 *
 * WHAT IT IS.  One function, called from the entity loop exactly where the
 * three `if` blocks used to sit.  Each of them ended in `continue` (this
 * entity is a 2D draw, nothing further applies); here that is `return true`
 * and the caller does the `continue`.  Nothing else changed: same order,
 * same guards, same early-outs.
 *
 * ORDER MATTERS and is preserved: these three run BEFORE the mesh paths and
 * before `if (!mesh || !cfg->draw_opaque) continue;`, so an entity that is
 * both a sprite and a mesh renders as a sprite, exactly as before.
 */

#include "jce_sr_sprites.h"

/*
 * Draw entity `e` if it is a 2D drawable.  Returns true when it handled the
 * entity and the caller should skip the rest of the ladder.
 *
 * `model` is the entity world matrix the caller already computed; `kc_fast`
 * is the caller's fast-kind cull flag, passed through unchanged so the fast
 * path still skips all three tests with one compare.
 */
bool sr_draw_2d_entity(JceSceneRenderer *sr, JceScene *scene, JceEntity e,
                       const JceSceneRenderConfig *cfg, bool kc_fast,
                       jce_mat4 model, const JceCamera *camera,
                       uint16_t view_id)
{
    /* ── Sprite animator path (2D, P1 #16) ───────────────────── */
    /* Frame time was advanced once this frame by sr_update_sprite_anims;
       here we consume the cached player's current frame, convert its pixel
       rect to a UV sub-rect, and submit the quad to the sprite batch. */
    if (!kc_fast && cfg->draw_sprites && sr->sprite_batch &&
        jce_scene_has_sprite_animator(scene, e) &&
        jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_SPRITE_ANIMATOR)) {
        int slot = sr_find_sprite_anim(sr, (uint32_t)e);
        JceSpritePlayer *pl =
            (slot >= 0) ? sr->sprite_anim[slot].player : NULL;
        const JceSpriteSheet *sheet =
            (slot >= 0) ? sr->sprite_anim[slot].sheet : NULL;
        const JceSpriteFrame *fr =
            pl ? jce_sprite_player_current_frame(pl) : NULL;
        if (fr) {
            JceSpriteAnimatorComponent *sac =
                jce_scene_get_sprite_animator(scene, e);
            /* Resolve the sheet image: the component's explicit image path
               wins; fall back to the atlas's meta.image. */
            const char *img = (sac && sac->sheet_path[0])
                ? sac->sheet_path
                : (sheet ? jce_sprite_sheet_image_path(sheet) : NULL);
            bgfx_texture_handle_t st = { UINT16_MAX };
            uint32_t iw = 0, ih = 0;
            if (img && img[0]) {
                JceTexture t = sr_resolve_texture(sr, img);
                if (jce_texture_valid(t)) {
                    st.idx = t.idx;
                    jce_texture_get_size(t, &iw, &ih);
                }
            }
            if (!BGFX_HANDLE_IS_VALID(st)) { st = sr->white_tex; iw = ih = 1; }

            float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
            if (iw > 0 && ih > 0 && fr->w > 0 && fr->h > 0) {
                u0 = (float)fr->x / (float)iw;
                v0 = (float)fr->y / (float)ih;
                u1 = (float)(fr->x + fr->w) / (float)iw;
                v1 = (float)(fr->y + fr->h) / (float)ih;
            }

            /* No tint/sort: white + NEUTRAL key (0 is order -32768). */
            JceTexture sjt; sjt.idx = st.idx;
            jce_sprite_batch_add(sr->sprite_batch, sjt, model.raw[0],
                                 u0, v0, u1, v1, 0xFFFFFFFFu, jce_sprite_sort_key(0, 0));
            return true;
        }
    }

    /* ── Sprite path ─────────────────────────────────────────── */
    if (!kc_fast && cfg->draw_sprites && sr->sprite_batch &&
        jce_scene_has_sprite_renderer(scene, e) &&
        jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_SPRITE_RENDERER)) {
        JceSpriteRendererComponent *spr = jce_scene_get_sprite_renderer(scene, e);
        if (spr) {
            bgfx_texture_handle_t st = { UINT16_MAX };
            if (spr->sprite_path[0]) {
                JceTexture t = sr_resolve_texture(sr, spr->sprite_path);
                if (jce_texture_valid(t)) st.idx = t.idx;
            }
            if (!BGFX_HANDLE_IS_VALID(st)) st = sr->white_tex;

            float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
            if (spr->flip_x) { float tmp = u0; u0 = u1; u1 = tmp; }
            if (spr->flip_y) { float tmp = v0; v0 = v1; v1 = tmp; }

            const float *sc = spr->color;
            uint8_t r8 = (uint8_t)(sc[0] * 255.0f);
            uint8_t g8 = (uint8_t)(sc[1] * 255.0f);
            uint8_t b8 = (uint8_t)(sc[2] * 255.0f);
            uint8_t a8 = (uint8_t)(sc[3] * 255.0f);
            uint32_t abgr = ((uint32_t)a8 << 24) | ((uint32_t)b8 << 16)
                          | ((uint32_t)g8 << 8) | (uint32_t)r8;

            JceTexture sjt; sjt.idx = st.idx;
            jce_sprite_batch_add(sr->sprite_batch, sjt, model.raw[0],
                                 u0, v0, u1, v1, abgr, jce_sprite_sort_key(spr->sorting_layer, spr->sorting_order));
            return true;
        }
    }

    /* ── Billboard path (camera-facing textured quad) ─────────────
     * Authored-but-never-drawn before this. Reuses the sprite batch (same
     * textured-quad shader — no new shader) with a camera-facing model
     * matrix built from the camera basis (FULL) or world-Y-locked (Y_AXIS).
     * The sprite batch's local quad is the centered unit square in XY, so
     * col0 = right*size.x, col1 = up*size.y places a size-scaled quad. */
    if (!kc_fast && cfg->draw_opaque && sr->sprite_batch && camera &&
        jce_scene_has_billboard_renderer(scene, e)) {
        static int s_bb_cid = -2;
        if (s_bb_cid == -2) s_bb_cid = jce_component_find("BillboardRenderer");
        bool bb_on = (s_bb_cid < 0) || jce_scene_comp_enabled(scene, e, s_bb_cid);
        JceBillboardRendererComponent *bb =
            bb_on ? jce_scene_get_billboard_renderer(scene, e) : NULL;
        if (bb && bb->visible) {
            bgfx_texture_handle_t bt = { UINT16_MAX };
            if (bb->texture_path[0]) {
                JceTexture t = sr_resolve_texture(sr, bb->texture_path);
                if (jce_texture_valid(t)) bt.idx = t.idx;
            }
            if (!BGFX_HANDLE_IS_VALID(bt)) bt = sr->white_tex;

            jce_vec3 pos = { model.col[3].x, model.col[3].y, model.col[3].z };
            jce_vec3 right, up;
            if (bb->mode == JCE_BILLBOARD_Y_AXIS) {
                jce_vec3 to_cam = jce_v3_sub(jce_camera_get_position(camera), pos);
                to_cam.y = 0.0f;
                float l = jce_v3_len(to_cam);
                jce_vec3 fwd = (l > 1e-5f) ? (jce_vec3){ to_cam.x/l, 0.0f, to_cam.z/l }
                                           : (jce_vec3){ 0.0f, 0.0f, 1.0f };
                up    = (jce_vec3){ 0.0f, 1.0f, 0.0f };
                right = jce_v3_cross(up, fwd);
            } else {
                right = jce_camera_get_right(camera);
                up    = jce_camera_get_up(camera);
            }
            float sx = bb->size[0] != 0.0f ? bb->size[0] : 1.0f;
            float sy = bb->size[1] != 0.0f ? bb->size[1] : 1.0f;
            const float *bc = bb->color;
            uint32_t bbgr = ((uint32_t)(bc[3] * 255.0f) << 24)
                          | ((uint32_t)(bc[2] * 255.0f) << 16)
                          | ((uint32_t)(bc[1] * 255.0f) << 8)
                          |  (uint32_t)(bc[0] * 255.0f);
            if (bb->texture_path[0]) {
                /* Textured: sprite batch, which draws with the TEXTURED
                 * program, so this tint IS applied and the quad is unlit. */
                float world[16] = {
                    right.x * sx, right.y * sx, right.z * sx, 0.0f,
                    up.x    * sy, up.y    * sy, up.z    * sy, 0.0f,
                    0.0f, 0.0f, 1.0f, 0.0f,
                    pos.x, pos.y, pos.z, 1.0f
                };
                JceTexture bjt; bjt.idx = bt.idx;
                jce_sprite_batch_add(sr->sprite_batch, bjt, world,
                                     0.0f, 0.0f, 1.0f, 1.0f, bbgr, jce_sprite_sort_key(0, 0));
            } else {
                /* Solid colour: camera-facing quad via the color program
                 * (unlit, tinted — renders in the pre-postfx offscreen where
                 * the sprite batch's lit/untinted look would otherwise show). */
                bgfx_vertex_layout_t bl;
                bgfx_vertex_layout_begin(&bl, bgfx_get_renderer_type());
                bgfx_vertex_layout_add(&bl, BGFX_ATTRIB_POSITION, 3, BGFX_ATTRIB_TYPE_FLOAT, false, false);
                bgfx_vertex_layout_add(&bl, BGFX_ATTRIB_COLOR0,   4, BGFX_ATTRIB_TYPE_UINT8, true, false);
                bgfx_vertex_layout_end(&bl);
                bgfx_transient_vertex_buffer_t bvb;
                bgfx_transient_index_buffer_t  bib;
                if (bgfx_alloc_transient_buffers(&bvb, &bl, 4, &bib, 6, false)) {
                    struct BbVtx { float x, y, z; uint32_t abgr; };
                    struct BbVtx *bv = (struct BbVtx *)bvb.data;
                    uint16_t *bidx = (uint16_t *)bib.data;
                    jce_vec3 rx = jce_v3_scale(right, sx * 0.5f);
                    jce_vec3 uy = jce_v3_scale(up,    sy * 0.5f);
                    jce_vec3 c0 = jce_v3_sub(jce_v3_sub(pos, rx), uy);
                    jce_vec3 c1 = jce_v3_sub(jce_v3_add(pos, rx), uy);
                    jce_vec3 c2 = jce_v3_add(jce_v3_add(pos, rx), uy);
                    jce_vec3 c3 = jce_v3_add(jce_v3_sub(pos, rx), uy);
                    bv[0].x=c0.x; bv[0].y=c0.y; bv[0].z=c0.z; bv[0].abgr=bbgr;
                    bv[1].x=c1.x; bv[1].y=c1.y; bv[1].z=c1.z; bv[1].abgr=bbgr;
                    bv[2].x=c2.x; bv[2].y=c2.y; bv[2].z=c2.z; bv[2].abgr=bbgr;
                    bv[3].x=c3.x; bv[3].y=c3.y; bv[3].z=c3.z; bv[3].abgr=bbgr;
                    bidx[0]=0; bidx[1]=1; bidx[2]=2; bidx[3]=0; bidx[4]=2; bidx[5]=3;
                    bgfx_set_transient_vertex_buffer(0, &bvb, 0, 4);
                    bgfx_set_transient_index_buffer(&bib, 0, 6);
                    jce_mat4 bid = jce_m4_identity();
                    bgfx_set_transform(bid.raw[0], 1);
                    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                                   BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS |
                                   BGFX_STATE_BLEND_ALPHA, 0);
                    JceShaderHandle csh = jce_renderer_get_program_color(sr->renderer);
                    bgfx_program_handle_t cprog; cprog.idx = csh.idx;
                    if (BGFX_HANDLE_IS_VALID(cprog))
                        bgfx_submit(view_id, cprog, 0, BGFX_DISCARD_ALL);
                }
            }
            return true;
        }
    }

    return false;
}
