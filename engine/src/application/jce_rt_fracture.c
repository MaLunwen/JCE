/*
 * jce_rt_fracture.c  Runtime fracture / destruction + physics draw-distance
 * module (split from jce_runtime.c).
 *
 * Voronoi box-shatter of a broken entity into dynamic convex-hull fragment
 * bodies (deferred POST-step via rt_flush_pending_fractures), plus the
 * physics draw-distance pass that lazily spawns / destroys small far static
 * colliders around the player (rt_drive_draw_distance).  Pure move from the
 * monolithic runtime: cross-module entry points are declared in
 * jce_rt_internal.h, everything else stays file-static here.  No behaviour
 * change.
 */

#include "jce_rt_internal.h"

/* ── Fracture / destruction (DESTRUCTION/FRACTURE) ────────────────────────
 *
 * On break, the entity's intact body is destroyed and replaced by N dynamic
 * convex-hull fragment bodies from a deterministic Voronoi box-shatter of the
 * entity's AABB.  Strictly POST-step (deferred via rt_flush_pending_fractures)
 * so Bullet is never mutated mid-solve.  Default off: only entities authoring
 * an ENABLED JceFracture component are ever queued, so a scene with no
 * fracturable entities keeps the byte-identical frame path.
 */

/* Compute the entity's LOCAL-space half-extents (about the collider/transform
 * centre) used as the shatter box.  Mirrors rt_spawn_entity's collider sizing
 * so the fragment cloud matches the body that was there.  `out_center_ofs` is
 * the collider centre offset (local, pre-rotation, already scaled). */
static void rt_fracture_local_box(JceScene *scene, JceEntity e,
                                  const JceTransform *tf,
                                  jce_vec3 *out_half, jce_vec3 *out_center_ofs)
{
	jce_vec3 half   = jce_v3(0.5f, 0.5f, 0.5f);
	jce_vec3 center = jce_v3(0.0f, 0.0f, 0.0f);

	JceBoxColliderComponent     *box = jce_scene_get_box_collider(scene, e);
	JceSphereColliderComponent  *sph = jce_scene_get_sphere_collider(scene, e);
	JceCapsuleColliderComponent *cap = jce_scene_get_capsule_collider(scene, e);

	if (box) {
		half = jce_v3(0.5f * box->size[0] * tf->scale.x,
		              0.5f * box->size[1] * tf->scale.y,
		              0.5f * box->size[2] * tf->scale.z);
		center = jce_v3(box->center[0] * tf->scale.x,
		                box->center[1] * tf->scale.y,
		                box->center[2] * tf->scale.z);
	} else if (sph) {
		float smax = tf->scale.x;
		if (tf->scale.y > smax) smax = tf->scale.y;
		if (tf->scale.z > smax) smax = tf->scale.z;
		float r = (sph->radius > 0.0f ? sph->radius : 0.5f) * smax;
		half = jce_v3(r, r, r);
		center = jce_v3(sph->center[0] * tf->scale.x,
		                sph->center[1] * tf->scale.y,
		                sph->center[2] * tf->scale.z);
	} else if (cap) {
		jce_vec3 cs = jce_v3_abs_safe_scale(tf->scale);
		float cr_scale = cs.x > cs.z ? cs.x : cs.z;
		float cr = (cap->radius > 0.0f ? cap->radius : 0.3f) * cr_scale;
		float ch = (cap->height > 0.0f ? cap->height : 1.0f) * cs.y;
		half = jce_v3(cr, 0.5f * ch, cr);
		center = jce_v3(cap->center[0] * tf->scale.x,
		                cap->center[1] * tf->scale.y,
		                cap->center[2] * tf->scale.z);
	} else {
		half = jce_v3(0.5f * tf->scale.x, 0.5f * tf->scale.y, 0.5f * tf->scale.z);
	}
	if (half.x <= 0.0f) half.x = 0.5f;
	if (half.y <= 0.0f) half.y = 0.5f;
	if (half.z <= 0.0f) half.z = 0.5f;

	*out_half       = half;
	*out_center_ofs = center;
}

/* Destroy + untrack the intact body for entity `e` (if any), returning its
 * pre-destroy linear + angular velocity through out params (zeroed when there
 * was no dynamic body).  Compacts bodies[] (swap-with-last) so no dangling
 * handle survives — the same lifecycle rt_track_body feeds. */
static bool rt_destroy_body_for_entity(JceRuntime *rt, JceEntity e,
                                       jce_vec3 *out_lin, jce_vec3 *out_ang)
{
	*out_lin = jce_v3(0.0f, 0.0f, 0.0f);
	*out_ang = jce_v3(0.0f, 0.0f, 0.0f);
	for (int i = 0; i < rt->body_count; ++i) {
		if (rt->bodies[i].entity != e) continue;
		JceBodyHandle b = rt->bodies[i].body;
		if (jce_physics_body_is_dynamic(rt->physics, b)) {
			*out_lin = jce_physics_body_get_velocity(rt->physics, b);
			*out_ang = jce_physics_body_get_angular_velocity(rt->physics, b);
		}
		jce_physics_body_destroy(rt->physics, b);
		/* swap-with-last compaction */
		rt->bodies[i] = rt->bodies[rt->body_count - 1];
		rt->body_count--;
		return true;
	}
	return false;
}

/* Destroy + untrack the static body for a draw-distance entry on despawn.
 * Each dd entry owns exactly one static body, so the first bodies[] match is
 * it; uses the same swap-with-last compaction as rt_destroy_body_for_entity
 * (no out-velocity — these are static). */
static void rt_dd_destroy_body(JceRuntime *rt, JceEntity e)
{
	for (int i = 0; i < rt->body_count; ++i) {
		if (rt->bodies[i].entity != e) continue;
		jce_physics_body_destroy(rt->physics, rt->bodies[i].body);
		rt->bodies[i] = rt->bodies[rt->body_count - 1];
		rt->body_count--;
		return;
	}
}

/* Per-frame physics draw-distance pass (big-world spawn): spawn deferred small
 * static colliders within PHYS_DD_RADIUS of the player, despawn them past
 * PHYS_DD_RADIUS + PHYS_DD_HYST.  O(dd_count) cheap XZ distance tests once per
 * frame.  No-op (byte-identical) when no entity was deferred (dd_count == 0),
 * or when the player position is unavailable (state preserved this frame). */
void rt_drive_draw_distance(JceRuntime *rt)
{
	if (!rt->physics || rt->dd_count == 0 || !rt->scene) return;

	/* NO PLAYER => FAIL OPEN, spawn everything.
	 *
	 * This used to `return`, leaving deferred bodies uncreated forever in any
	 * scene without a player character -- a menu, a cutscene, a headless
	 * dedicated server, or simply the frames before a character spawns.  That
	 * was survivable while deferral only ever caught an explicitly-static
	 * Rigidbody, which is rare.  It stopped being survivable when a collider
	 * WITHOUT a Rigidbody started producing a static body, because that is how
	 * ordinary level geometry is authored: the deferral would then hold back
	 * the floor, the walls and the props of every playerless scene, and the
	 * bug being fixed there would simply have moved here.
	 *
	 * The radius test is an optimisation around a player.  With no player
	 * there is nothing to optimise around, so the world gets its collision --
	 * which is exactly the behaviour before deferral existed.  Once a player
	 * does appear, the normal in/out test below takes over and despawns what
	 * is far. */
	jce_vec3 pp;
	if (!jce_runtime_get_player_position(rt, &pp)) {
		for (int i = 0; i < rt->dd_count; ++i) {
			DDEntry *de = &rt->dd[i];
			if (de->spawned) continue;
			if (rt_spawn_entity_body(rt, rt->scene, de->entity,
			                         /*allow_defer=*/false))
				de->spawned = true;
		}
		return;
	}

	const float r_in  = PHYS_DD_RADIUS;
	const float r_out = PHYS_DD_RADIUS + PHYS_DD_HYST;
	const float in2   = r_in  * r_in;
	const float out2  = r_out * r_out;

	for (int i = 0; i < rt->dd_count; ++i) {
		DDEntry *de = &rt->dd[i];
		float dx = de->center.x - pp.x;
		float dz = de->center.z - pp.z;
		float d2 = dx * dx + dz * dz;       /* horizontal (XZ) only */
		if (!de->spawned) {
			if (d2 < in2 && rt_spawn_entity_body(rt, rt->scene, de->entity,
			                                     /*allow_defer=*/false))
				de->spawned = true;
		} else if (d2 > out2) {
			rt_dd_destroy_body(rt, de->entity);
			de->spawned = false;
		}
	}
}

/* Perform one entity's fracture body-swap.  Called only from the deferred
 * flush (POST-step). */
static void rt_perform_fracture(JceRuntime *rt, JceEntity e)
{
	if (!rt->physics || !rt->scene) return;
	if (!jce_scene_has_transform(rt->scene, e)) return;
	JceFractureComponent *fc = jce_scene_get_fracture(rt->scene, e);
	if (!fc || !fc->enabled) return;   /* gate: only enabled fracturables */

	JceTransform *tf_local = jce_scene_get_transform(rt->scene, e);
	if (!tf_local) return;

	/* WORLD: the fragments are free bodies placed in the world, so the box
	 * they are cut out of has to be the world one.  A fracturable parented to
	 * anything shattered into pieces that appeared at its local offset from
	 * the origin -- see the note at the head of rt_spawn_entity.  A root
	 * entity is byte-identical. */
	JceTransform tf_world = *tf_local;
	jce_scene_get_world_pose(rt->scene, e, &tf_world.position,
	                         &tf_world.rotation, &tf_world.scale);
	const JceTransform *tf = &tf_world;

	/* Entity AABB (local half-extents about the collider centre). */
	jce_vec3 half, center_ofs;
	rt_fracture_local_box(rt->scene, e, tf, &half, &center_ofs);

	/* Destroy the intact body first (frees its handle + untracks), capturing
	 * its velocity so fragments inherit momentum. */
	jce_vec3 inh_lin, inh_ang;
	bool had_body = rt_destroy_body_for_entity(rt, e, &inh_lin, &inh_ang);
	if (!had_body) {
		/* Nothing to break (no body) — leave the entity intact. */
		LOG_INFO(LOG_TAG, "fracture: entity %llu has no body to shatter",
		         (unsigned long long)e);
		return;
	}

	int n = fc->fragment_count;
	if (n < 1)   n = 1;
	if (n > 256) n = 256;
	float density = fc->density > 0.0f ? fc->density : 1000.0f;

	/* Local-space shatter box centred on the collider centre. */
	float bmin[3] = { -half.x, -half.y, -half.z };
	float bmax[3] = {  half.x,  half.y,  half.z };

	float *seeds = (float *)jce_malloc((size_t)n * 3u * sizeof(float));
	if (!seeds) return;
	jce_fracture_scatter_seeds(bmin, bmax, n, fc->seed, seeds);

	JceFractureResult fr;
	int cells = jce_fracture_box(bmin, bmax, seeds, n, &fr);
	jce_free(seeds);
	if (cells <= 0) {
		jce_fracture_free(&fr);
		return;
	}

	int spawned = 0;
	for (int ci = 0; ci < fr.cell_count; ++ci) {
		JceFractureCell *cell = &fr.cells[ci];
		if (cell->vcount < 4 || cell->volume <= 0.0f) continue;

		/* Re-base cell verts so the body origin is at the cell CENTROID (the
		 * convex hull is expressed relative to the body origin). */
		float verts[JCE_FRACTURE_MAX_CELL_VERTS * 3];
		for (int v = 0; v < cell->vcount; ++v) {
			verts[v * 3 + 0] = cell->verts[v][0] - cell->centroid[0];
			verts[v * 3 + 1] = cell->verts[v][1] - cell->centroid[1];
			verts[v * 3 + 2] = cell->verts[v][2] - cell->centroid[2];
		}

		/* Cell centroid in WORLD space: collider centre offset + cell centroid,
		 * rotated by the entity rotation, added to the entity world position. */
		jce_vec3 local_c = jce_v3(center_ofs.x + cell->centroid[0],
		                          center_ofs.y + cell->centroid[1],
		                          center_ofs.z + cell->centroid[2]);
		jce_vec3 world_c = jce_v3_add(tf->position,
		                              jce_q_rotate(tf->rotation, local_c));

		JceColliderChild child;
		memset(&child, 0, sizeof child);
		child.shape        = JCE_SHAPE_CONVEX_HULL;
		child.rotation     = jce_q_identity();
		child.vertices     = verts;
		child.vertex_count = (uint32_t)cell->vcount;

		JceCompoundBodyDesc desc;
		memset(&desc, 0, sizeof desc);
		desc.type        = JCE_BODY_DYNAMIC;
		desc.position    = world_c;
		desc.rotation    = tf->rotation;
		desc.mass        = density * cell->volume;   /* refined by set_mass below */
		desc.friction    = 0.5f;
		desc.children    = &child;
		desc.child_count = 1;

		JceBodyHandle body = jce_physics_body_create_compound(rt->physics, &desc);
		if (!jce_body_valid(body)) continue;

		/* set_mass recomputes the inertia tensor from the actual hull shape. */
		jce_physics_body_set_mass(rt->physics, body, density * cell->volume);

		/* Inherit the parent's velocity + a small outward kick from the centre
		 * so the pieces visibly separate. */
		jce_vec3 out_dir = jce_v3_sub(world_c,
		                              jce_v3_add(tf->position,
		                                         jce_q_rotate(tf->rotation, center_ofs)));
		out_dir = jce_v3_normalize(out_dir);
		jce_vec3 v0 = jce_v3_add(inh_lin, jce_v3_scale(out_dir, 1.5f));
		jce_physics_body_set_velocity(rt->physics, body, v0);
		jce_physics_body_set_angular_velocity(rt->physics, body, inh_ang);

		/* Track each fragment as a dynamic body bound to the SAME entity so the
		 * existing transform write-back / interpolation lifecycle owns it.  The
		 * scene renderer still draws the entity's mesh at its transform; a real
		 * shattered VISUAL (per-fragment meshes) is a documented follow-up. */
		if (rt->body_count >= rt->body_cap && !rt_grow_bodies(rt))
			break;
		JceTransform ftf = *tf;
		ftf.position = world_c;
		rt_track_body(rt, e, body, &ftf, (uint8_t)JCE_BODY_DYNAMIC);
		spawned++;
	}

	jce_fracture_free(&fr);

	/* Disable further fracture of this entity (it is already broken). */
	fc->enabled = false;

	LOG_INFO(LOG_TAG, "fracture: entity %llu shattered into %d fragment bodies",
	         (unsigned long long)e, spawned);
}

/* Flush the deferred fracture queue.  Runs once per frame after the fixed-step
 * loop so all body destroy/create happens POST-step.  No-op when empty. */
void rt_flush_pending_fractures(JceRuntime *rt)
{
	int n = rt->pending_fracture_count;
	if (n <= 0) return;
	for (int i = 0; i < n; ++i)
		rt_perform_fracture(rt, rt->pending_fractures[i]);
	rt->pending_fracture_count = 0;
}
