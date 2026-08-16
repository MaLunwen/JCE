/*
 * jce_rt_physics.c  Runtime physics body-spawn module (split from
 * jce_runtime.c).
 *
 * The per-entity rigid-body materialisation helpers used by the scene-walk
 * spawn driver (rt_spawn_entity, kept in core): post-create body extras,
 * body tracking, cooked / compound / mesh / terrain collider spawn, raycast
 * vehicles, volumetric soft bodies, 2D bodies + tilemap colliders, and the
 * shared JceBodyDesc build path (rt_spawn_entity_body) reused by the
 * draw-distance + fracture passes.  Pure move from the monolithic runtime:
 * cross-module entry points are declared in jce_rt_internal.h, everything
 * else stays file-static here.  No behaviour change.
 */

#include "jce_terrain_collision_stream.h"
#include "middleware/scene/jce_terrain_cache.h"
#include "jce_rt_internal.h"

#include <jce/os/core/jce_timer.h>   /* attribute the live collider-cook hitch */

/*
 * Apply authored per-body properties that must be set AFTER body creation:
 *   - collision layer  -> broadphase group/mask via the layer matrix
 *   - per-body gravity -> world gravity * gravity_scale (0 when !use_gravity)
 *   - physics material -> friction/restitution from a .physmat.json asset
 *
 * `rb` may be NULL (e.g. a static compound with no Rigidbody). `physmat_path`
 * is the collider/body material override (may be NULL/empty).
 */
static void rt_apply_body_extras(JceRuntime *rt, JceEntity e, JceBodyHandle body,
                                 const JceRigidBodyComponent *rb,
                                 const char *physmat_path)
{
	if (!rt->physics || !jce_body_valid(body)) return;

	/* Tag the body with its entity so contact events carry entity_a/_b. */
	jce_physics_body_set_entity(rt->physics, body, (uint64_t)e);

	/* Collision layer (default layer 0 when no rigidbody). */
	jce_physics_body_set_layer(rt->physics, body,
	                           rb ? rb->physics_layer : 0u);

	/* Per-body gravity.  use_gravity=false -> factor 0 (floats).
	 * Guard an uninitialised gravity_scale (0 while gravity is enabled
	 * is contradictory -> treat as normal 1.0 so bodies don't float). */
	if (rb) {
		float gf = rb->use_gravity ? rb->gravity_scale : 0.0f;
		if (rb->use_gravity && rb->gravity_scale == 0.0f) gf = 1.0f;
		if (gf != 1.0f)
			jce_physics_body_set_gravity_factor(rt->physics, body, gf);
	}

	/* Freeze rotation (Unity FreezeRotation / Godot lock_rotation): lock all
	 * 3 angular axes so a DYNAMIC body collides linearly without tipping —
	 * how a character/enemy rigidbody stays upright.  Inert on static/kinematic. */
	if (rb && rb->freeze_rotation)
		jce_physics_body_set_angular_factor(rt->physics, body,
		                                    jce_v3(0.0f, 0.0f, 0.0f));

	/* Physics material override (host-FS .physmat.json). */
	if (physmat_path && physmat_path[0]) {
		JcePhysicsMaterial pm;
		jce_physics_material_init_default(&pm);
		if (jce_physics_material_load(physmat_path, &pm))
			jce_physics_body_set_material(rt->physics, body, &pm);
		else
			LOG_WARN(LOG_TAG, "physmat: cannot load '%s'", physmat_path);
	}
}

/* Record a spawned body + seed its TRS sync cache from the entity transform. */
void rt_track_body(JceRuntime *rt, JceEntity e, JceBodyHandle body,
                          const JceTransform *tf, uint8_t kind)
{
	BodyEntry *be = &rt->bodies[rt->body_count];
	be->entity      = e;
	be->body        = body;
	be->last_pos    = tf->position;
	be->last_rot    = tf->rotation;
	be->last_scale  = tf->scale;
	be->spawn_scale = tf->scale;
	/* Seed both interpolation endpoints to the spawn pose so the first
	 * frames before any fixed tick blend to a no-op. */
	be->prev_pos    = tf->position;
	be->prev_rot    = tf->rotation;
	be->cur_pos     = tf->position;
	be->cur_rot     = tf->rotation;
	be->center_local = jce_v3(0.0f, 0.0f, 0.0f);  /* primitive path overrides */
	be->kind        = kind;
	rt->body_count++;
}

/*
 * Try to load a precomputed (offline-cooked) compound-collider blob that
 * sits beside the model as "<model_path>.jcol".  This lets a model whose
 * compound collider was baked by `jce_cook --collider` skip the expensive
 * live VHACD / triangle-mesh cook at every scene load.  Looks in the pak
 * first (deployed builds), then the host filesystem (editor).  On success
 * `out` receives an owned cooked tree (free with jce_collider_cooked_free)
 * and the function returns true; on any miss it returns false and the
 * caller falls back to the live cook.
 */
static bool rt_try_load_cached_collider(JceRuntime *rt, const char *model_path,
                                        JceCookedCollider *out)
{
	if (!model_path || !model_path[0] || !out) return false;

	char blob_path[1024];
	int n = snprintf(blob_path, sizeof blob_path, "%s.jcol", model_path);
	if (n <= 0 || (size_t)n >= sizeof blob_path) return false;

	bool ok = false;

	/* 1. Pak-resident blob (deployed game). */
	if (rt->pak) {
		const JcePakAsset *asset = jce_pak_find(rt->pak, blob_path);
		if (asset && asset->original_size > 0) {
			void *buf = jce_malloc((size_t)asset->original_size);
			if (buf) {
				size_t got = jce_pak_decompress(asset, buf,
				                                (size_t)asset->original_size);
				if (got > 0)
					ok = jce_collider_deserialize(buf, (uint32_t)got, out);
				jce_free(buf);
			}
		}
	}

	/* 2. Host-filesystem sibling blob (editor / loose build).  The editor
	 *    never chdir's and mounts no global VFS in loose Play, so the raw
	 *    project-relative path does not resolve against the process CWD —
	 *    map the model path through the host resolver (the SAME resolution
	 *    the renderer uses for the visual mesh) and read "<resolved>.jcol"
	 *    beside it.  Falls back to the raw relative path when no resolver. */
	if (!ok) {
		char        resolved_blob[1024];
		const char *read_path = blob_path;
		if (rt->resolve_path_fn) {
			char resolved_model[1024];
			if (rt->resolve_path_fn(rt->user_data, model_path,
			                        resolved_model,
			                        (int)sizeof resolved_model)) {
				int rn = snprintf(resolved_blob, sizeof resolved_blob,
				                  "%s.jcol", resolved_model);
				if (rn > 0 && (size_t)rn < sizeof resolved_blob)
					read_path = resolved_blob;
			}
		}
		uint64_t sz = 0;
		void *buf = jce_fs_host_read_all(read_path, &sz);
		if (buf) {
			if (sz > 0 && sz <= 0xFFFFFFFFull)
				ok = jce_collider_deserialize(buf, (uint32_t)sz, out);
			jce_fs_buffer_free(buf);
		}
	}

	if (ok)
		LOG_INFO(LOG_TAG, "compound collider: loaded cached blob %s", blob_path);
	return ok;
}

/* A live cook slower than this gets its own named log line, so the stall is
 * attributable to an asset instead of showing up as an anonymous hitch. */
#define RT_LIVE_COOK_WARN_MS 2.0

/* The "you are cooking colliders inside the shipped runtime" explanation is
 * printed ONCE per process; the per-asset cost lines below repeat.  Plain bool
 * (not atomic) because every route into rt_spawn_entity is main-thread: the
 * scene-load walk, the script-spawn flush and the streamer's per-entity
 * wire-in all run inside the runtime step. */
static bool s_live_cook_warned = false;

/*
 * Cook + instantiate a body for entity `e` from a compound-collider
 * description.  Shared by the real Compound Collider component and the
 * Mesh Collider (which synthesises a single-shape description).
 * `allow_blob_cache` gates the offline ".jcol" blob lookup — the blob is
 * cooked with the compound's own settings (AUTO + static), so callers
 * whose cook settings differ (e.g. a convex mesh collider) must skip it.
 *
 * Note: the entity's TRS scale is NOT baked into the cooked shapes
 * (pre-existing jce_collider_instantiate behavior, kept for parity).
 *
 * Returns true if a body was spawned (caller then skips the regular
 * rigid-body path so the entity does not get a second body).
 */
static bool rt_spawn_cooked_body(JceRuntime *rt, JceScene *scene, JceEntity e,
                                 const JceTransform *tf,
                                 const JceCompoundColliderComponent *cc,
                                 bool allow_blob_cache)
{
	if (!cc || cc->model_path[0] == '\0') return false;

	/* Prefer a precomputed (offline-cooked) blob beside the model so we do
	 * not re-cook colliders live at every scene load.  Fall through to the
	 * live cook below on a cache miss. */
	JceCookedCollider cooked;
	bool have_cooked = allow_blob_cache &&
	                   rt_try_load_cached_collider(rt, cc->model_path, &cooked);

	if (!have_cooked) {

		/* ── HAZARD: the OFFLINE cooker running inside the SHIPPED runtime ──
		 * Everything below (an assimp parse of the whole model + jce_collider_cook)
		 * is build-time work executing on the main thread at scene load — and
		 * mid-gameplay for anything spawned later (jce.spawn, a streamed-in cell).
		 * Nothing is memoised, so N entities sharing one model pay it N times,
		 * and when the authored mode is JCE_COLLIDER_MODE_CONVEX_DECOMP it is a
		 * full V-HACD voxel decomposition: seconds on a dense mesh.
		 *
		 * It stays anyway.  The two "cheaper" options are both worse: refusing to
		 * cook above some complexity budget hands the player an entity with NO
		 * collision (fall-through beats a hitch only until you fall through the
		 * world), and substituting the box/sphere path from rt_spawn_entity_body
		 * would silently give a shipped game a different collision shape than the
		 * one it was authored and tested against.  So the cost is kept, and made
		 * loud + attributable instead.
		 *
		 * The real fix is on the content side: cook colliders offline with
		 * `jce_cook --collider` and ship the "<model>.jcol" blob beside the model
		 * so rt_try_load_cached_collider above hits and none of this runs.  Note
		 * that a CONVEX or DYNAMIC Mesh Collider passes allow_blob_cache=false and
		 * therefore lands here unconditionally, blob or not (see rt_try_spawn_mesh). */
		const uint64_t cook_t0 = jce_time_perf_counter();

		/* Load parts from the pak (deployed) or the host filesystem (editor).
		 *
		 * NOTE: this is the one runtime path that still hands a .gltf/.glb to
		 * assimp instead of cgltf.  It is tolerable — a collider consumes only
		 * per-node positions/indices/world-transform, the subset both parsers
		 * agree on, and the material/skin/morph data assimp drops is never
		 * consulted here — but it is not drift-free: the part SPLIT and the
		 * part NAMES come from assimp's node graph, and cc->detect_naming keys
		 * collider behaviour off those names, so an authored glTF node name
		 * assimp renames or a node whose primitives it merges yields a
		 * different decomposition than the cgltf renderer draws.  Closing it
		 * needs a cgltf per-node JceModelParts extractor, which does not exist
		 * yet; until then keep the extension-agnostic call. */
		JceModelParts parts;
		memset(&parts, 0, sizeof parts);
		bool loaded = false;
		if (rt->pak) {
			const JcePakAsset *asset = jce_pak_find(rt->pak, cc->model_path);
			if (asset) {
				void *buf = jce_malloc((size_t)asset->original_size);
				if (buf) {
					size_t n = jce_pak_decompress(asset, buf,
					                              (size_t)asset->original_size);
					if (n > 0) {
						const char *ext = strrchr(cc->model_path, '.');
						loaded = jce_model_importer_load_parts_memory(
							buf, n, ext ? ext : "", &parts);
					}
					jce_free(buf);
				}
			}
		}
		if (!loaded) {
			/* Resolve the model path through the host resolver (editor loose
			 * assets do not resolve against the process CWD) so the live cook
			 * reads the same file the mesh renders from. */
			char        resolved_model[1024];
			const char *load_path = cc->model_path;
			if (rt->resolve_path_fn &&
			    rt->resolve_path_fn(rt->user_data, cc->model_path,
			                        resolved_model,
			                        (int)sizeof resolved_model))
				load_path = resolved_model;
			loaded = jce_model_importer_load_parts_file(load_path, &parts);
		}
		if (!loaded) {
			LOG_WARN(LOG_TAG, "compound collider: cannot load %s", cc->model_path);
			return false;
		}

		JceColliderPart *cparts =
			(JceColliderPart *)jce_malloc((size_t)parts.count * sizeof(*cparts));
		if (!cparts) { jce_model_importer_free_parts(&parts); return false; }
		/* Snapshot the input size for the cost line below — `parts` is freed
		 * before the cook result is checked. */
		uint32_t cook_tris  = 0;
		uint32_t cook_parts = parts.count;
		for (uint32_t i = 0; i < parts.count; i++) {
			cook_tris += parts.parts[i].index_count
			             ? parts.parts[i].index_count / 3u
			             : parts.parts[i].vertex_count / 3u;
			cparts[i].name         = parts.parts[i].name;
			cparts[i].vertices     = parts.parts[i].positions;
			cparts[i].vertex_count = parts.parts[i].vertex_count;
			cparts[i].indices      = parts.parts[i].indices;
			cparts[i].index_count  = parts.parts[i].index_count;
			memcpy(cparts[i].transform, parts.parts[i].transform,
			       sizeof cparts[i].transform);
		}

		JceColliderCookConfig cfg = jce_collider_cook_config_default();
		cfg.mode          = (JceColliderMode)cc->mode;
		cfg.split         = (JceColliderSplitMode)cc->split;
		cfg.is_static     = cc->is_static;
		cfg.detect_naming = cc->detect_naming;
		if (cc->vhacd_resolution)         cfg.vhacd_resolution = cc->vhacd_resolution;
		if (cc->vhacd_max_hulls)          cfg.vhacd_max_hulls = cc->vhacd_max_hulls;
		if (cc->vhacd_max_verts_per_hull) cfg.vhacd_max_verts_per_hull = cc->vhacd_max_verts_per_hull;

		bool cooked_ok = jce_collider_cook(cparts, parts.count, &cfg, &cooked);
		jce_free(cparts);
		jce_model_importer_free_parts(&parts);
		if (!cooked_ok) {
			LOG_WARN(LOG_TAG, "compound collider: cook failed for %s", cc->model_path);
			return false;
		}

		/* Make the hazard above visible: the advisory once, the price per asset. */
		double cook_ms = jce_time_perf_to_ms(cook_t0, jce_time_perf_counter());
		if (!s_live_cook_warned) {
			s_live_cook_warned = true;
			LOG_WARN(LOG_TAG, "collider cook is running INSIDE the runtime — no "
			         "'<model>.jcol' blob was found, so this scene parses and cooks "
			         "collision geometry on the main thread at load.  Cook colliders "
			         "offline (jce_cook --collider) and ship the .jcol beside the "
			         "model to remove the hitch.");
		}
		if (cook_ms >= RT_LIVE_COOK_WARN_MS)
			LOG_WARN(LOG_TAG, "live collider cook: %.1f ms for '%s' (%u tris in %u "
			         "parts, mode %u%s) — cook this asset offline",
			         cook_ms, cc->model_path, cook_tris, cook_parts,
			         (unsigned)cfg.mode,
			         cfg.mode == JCE_COLLIDER_MODE_CONVEX_DECOMP ? ", V-HACD" : "");
	} /* !have_cooked */

	JceColliderInstanceDesc id;
	memset(&id, 0, sizeof id);
	id.position    = tf->position;
	id.rotation    = tf->rotation;
	id.friction    = cc->friction > 0.0f ? cc->friction : 0.5f;
	id.restitution = cc->restitution;
	id.is_trigger  = cc->is_trigger;

	JceRigidBodyComponent *rb = jce_scene_get_rigidbody(scene, e);
	if (rb) {
		id.mass            = rb->mass;
		id.linear_damping  = rb->drag;
		id.angular_damping = rb->angular_drag;
		if (rb->is_kinematic)      id.type = JCE_BODY_KINEMATIC;
		else if (rb->mass <= 0.0f) id.type = JCE_BODY_STATIC;
		else                       id.type = JCE_BODY_DYNAMIC;
	} else {
		id.type = JCE_BODY_STATIC;
	}

	JceBodyHandle body = jce_collider_instantiate(rt->physics, &cooked, &id);
	jce_collider_cooked_free(&cooked);
	if (!jce_body_valid(body)) return false;

	/* Layer / gravity / material — material override prefers the compound's
	 * own slot, else the Rigidbody's. */
	rt_apply_body_extras(rt, e, body, rb,
	                     cc->physmat_path[0] ? cc->physmat_path
	                     : (rb ? rb->physmat_path : NULL));

	if (rt->body_count >= rt->body_cap && !rt_grow_bodies(rt))
		return true;   /* spawned but cannot track — still skip box path */
	rt_track_body(rt, e, body, tf, (uint8_t)id.type);
	return true;
}

/*
 * Try to materialise a per-object compound collider for entity `e`.
 * Loads the referenced model WITHOUT flattening its node hierarchy, cooks
 * each part into its own child shape, and instantiates the lot as a single
 * compound body — so a model holding N separated objects yields N child
 * colliders rather than one fat hull spanning the gaps between them.
 *
 * Returns true if a body was spawned (caller then skips the regular
 * rigid-body path so the entity does not get a second body).
 */
bool rt_try_spawn_compound(JceRuntime *rt, JceScene *scene,
                                  JceEntity e, const JceTransform *tf)
{
	JceCompoundColliderComponent *cc = jce_scene_get_compound_collider(scene, e);
	if (!cc || cc->model_path[0] == '\0') return false;
	{ int cc_id = jce_component_find("CompoundCollider");
	  if (cc_id >= 0 && !jce_scene_comp_enabled(scene, e, cc_id)) return false; }
	return rt_spawn_cooked_body(rt, scene, e, tf, cc, true);
}

/*
 * Try to materialise a Mesh Collider for entity `e` (Unity MeshCollider
 * semantics: ONE shape cooked from the whole referenced mesh).  Reuses the
 * compound cook/instantiate path via a synthetic single-shape description:
 * exact triangle mesh for static bodies, convex hull when `convex` is set
 * or the body is dynamic (Bullet triangle meshes are static-only).
 *
 * Returns true if a body was spawned (caller then skips the regular
 * rigid-body path so the entity does not get a second body).
 */
bool rt_try_spawn_mesh(JceRuntime *rt, JceScene *scene,
                              JceEntity e, const JceTransform *tf)
{
	JceMeshColliderComponent *mc = jce_scene_get_mesh_collider(scene, e);
	if (!mc || mc->mesh_path[0] == '\0') return false;
	if (!jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_MESH_COLLIDER))
		return false;

	JceRigidBodyComponent *rb = jce_scene_get_rigidbody(scene, e);
	bool dynamic = rb && !rb->is_kinematic && rb->mass > 0.0f;
	bool convex  = mc->convex;
	if (dynamic && !convex) {
		LOG_WARN(LOG_TAG, "mesh collider on dynamic rigidbody requires convex; "
		         "cooking convex hull instead (entity %llu, %s)",
		         (unsigned long long)e, mc->mesh_path);
		convex = true;
	}

	/* Synthetic compound description: one shape for the whole model. */
	JceCompoundColliderComponent cc;
	memset(&cc, 0, sizeof cc);
	memcpy(cc.model_path, mc->mesh_path, sizeof cc.model_path);
	cc.mode          = (uint8_t)(convex ? JCE_COLLIDER_MODE_CONVEX_HULL
	                                    : JCE_COLLIDER_MODE_TRIANGLE_MESH);
	cc.split         = (uint8_t)JCE_COLLIDER_SPLIT_WHOLE;
	cc.is_static     = !dynamic;
	cc.detect_naming = false;
	cc.is_trigger    = mc->is_trigger;
	cc.friction      = mc->friction;
	cc.restitution   = mc->restitution;

	/* The offline ".jcol" blob beside the model is cooked AUTO + static —
	 * only shape-compatible with the static triangle-mesh case here.  A convex
	 * or dynamic mesh collider therefore ALWAYS takes the live parse+cook path
	 * in rt_spawn_cooked_body (see the hazard note there) — cooking the asset
	 * offline does NOT spare it, because the only blob the offline cooker emits
	 * beside the model is the static one. */
	bool allow_blob_cache = !convex && !dynamic;
	return rt_spawn_cooked_body(rt, scene, e, tf, &cc, allow_blob_cache);
}

/*
 * Terrain heightmap → static triangle-mesh collider (P0 terrain physics).
 *
 * Builds a triangle soup from the terrain's WxH height grid (vertex Y =
 * grid * max_height, matching the renderer) and creates ONE static body at
 * the entity transform, so characters and objects rest on the terrain
 * instead of falling through.  The geometry is copied into Bullet at create
 * time, so the temporary verts/indices are freed immediately after.
 *
 * Loaded host-filesystem first (editor Play, where the .terrain.json is loose
 * and CWD is the project root), then from the PAK (shipped game).  Returns
 * true if a terrain body was spawned.
 */
/* JceTerrainCacheLoadFn: host filesystem first (editor Play, where the
 * .terrain.json is loose and CWD is the project root), then the PAK (shipped
 * game).  The cache owns whatever this returns. */
static JceTerrain *rt_terrain_cache_load(void *ud, const char *path)
{
	JceRuntime *rt = (JceRuntime *)ud;
	char        tbuf[1024];
	const char *tpath = rt_resolve_host_path(rt, path, tbuf, sizeof tbuf);
	/* jce-terrain-owner-exempt: this IS the cache's loader callback; the
	 * cache takes ownership of the result. */
	JceTerrain *t = jce_terrain_load_file(tpath);
	/* jce-terrain-owner-exempt: same callback, PAK fallback. */
	if (!t && rt->pak) t = jce_terrain_load_from_pak(rt->pak, path);
	return t;
}

/* JceTerrainCollisionSampleFn over a tile-aware JceTerrain.  Samples on the
 * tile's own grid, INCLUDING the far edge (span covers [0, tile_world_size]),
 * so adjacent collision tiles share their boundary row exactly and no seam
 * opens between them. */
static bool rt_terrain_collision_sample(void *ctx, uint32_t tile_x,
                                        uint32_t tile_z, uint32_t span,
                                        float origin_x, float origin_z,
                                        float tile_world_size,
                                        float *out_heights)
{
	JceTerrain *t = (JceTerrain *)ctx;
	(void)tile_x; (void)tile_z;
	if (!t || span < 2u || !out_heights) return false;

	const float step = tile_world_size / (float)(span - 1u);
	for (uint32_t z = 0; z < span; ++z) {
		for (uint32_t x = 0; x < span; ++x) {
			const float wx = origin_x + (float)x * step;
			const float wz = origin_z + (float)z * step;
			out_heights[(size_t)z * span + x] =
			    jce_terrain_sample_height(t, wx, wz);
		}
	}
	return true;
}

bool rt_try_spawn_terrain(JceRuntime *rt, JceScene *scene, JceEntity e,
                                 const JceTransform *tf)
{
	JceTerrainComponent *tc = jce_scene_get_terrain(scene, e);
	if (!tc || tc->terrain_path[0] == '\0') return false;
	if (!jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_TERRAIN)) return false;

	/* BORROWED from the scene's terrain cache -- the renderer and the pick
	 * pass read the same grid.  This used to be a private load, which is why
	 * the collider was whatever was on disk when the level opened: an editor
	 * sculpt changed what you saw and never what you walked on. */
	JceTerrain *t = jce_terrain_cache_acquire(
	    jce_scene_terrain_cache(scene), tc->terrain_path,
	    rt_terrain_cache_load, rt);
	if (!t) {
		LOG_WARN(LOG_TAG, "terrain physics: cannot load '%s' for entity %llu",
		         tc->terrain_path, (unsigned long long)e);
		return false;
	}

	/* Remember the grid for anything that needs to ASK the terrain a question
	 * rather than collide with it -- the water disturbance layer reads it for
	 * bathymetry.
	 *
	 * Recorded HERE, not at the streamed-collider branch below, because that
	 * branch only runs for TILED terrain. Taking it from there made the water's
	 * bed depend on whether the terrain happened to be streamed, so a plain
	 * terrain silently got a flat bed and no wave refraction -- a difference
	 * between two authoring choices that nothing in the water system should be
	 * able to see.
	 *
	 * Borrowed, like the collider's copy: the scene's cache owns it. */
	rt->terrain_stream_src = t;

	bool ok = false;

	/* ── Heightfield first (the industry-standard terrain collider) ──────
	 *
	 * btHeightfieldTerrainShape stores ONE float per sample and finds the
	 * candidate cell by arithmetic; the triangle soup stored three floats per
	 * vertex plus six indices per cell and had to be traversed by a BVH.  For
	 * a 1025x1025 terrain that is ~4 MB against ~50 MB, and no tree walk.
	 *
	 * It cannot represent HOLES, though -- there is no way to tell Bullet a
	 * cell is absent -- so terrain with authored holes keeps the soup, which
	 * simply omits those cells.  Silently filling a cave mouth with collision
	 * would be worse than the memory. */
	const int   W  = jce_terrain_width(t);
	const int   H  = jce_terrain_height(t);
	const float sx = jce_terrain_world_size_x(t);
	const float sz = jce_terrain_world_size_z(t);
	const float mh = jce_terrain_max_height(t);
	const float *norm = jce_terrain_heights(t);

	if (norm && !jce_terrain_has_holes(t) && W >= 2 && H >= 2 &&
	    sx > 0.0f && sz > 0.0f) {
		/* jce_terrain_heights() is NORMALIZED 0..1; the collider wants world
		 * Y, exactly as the renderer's vertex Y = grid * max_height. */
		const size_t n = (size_t)W * (size_t)H;
		float *world_y = (float *)jce_malloc(n * sizeof(float));
		if (world_y) {
			for (size_t i = 0; i < n; i++) world_y[i] = norm[i] * mh;

			JceHeightfieldBodyDesc hd;
			memset(&hd, 0, sizeof hd);
			hd.position    = tf->position;   /* the field's MIN corner */
			hd.rotation    = tf->rotation;
			hd.heights     = world_y;        /* copied by the bridge */
			hd.samples_x   = (uint32_t)W;
			hd.samples_z   = (uint32_t)H;
			hd.cell_size_x = sx / (float)(W - 1);
			hd.cell_size_z = sz / (float)(H - 1);
			hd.min_height  = 0.0f;           /* normalized heights are >= 0 */
			hd.max_height  = mh > 0.0f ? mh : 1.0f;
			/* FIXED == Bullet's unflipped split == the v10-v01 anti-diagonal
			 * the renderer draws.  Measured, not assumed -- see
			 * test_fixed_diagonal_matches_the_triangle_soup. */
			hd.diagonal    = JCE_HEIGHTFIELD_DIAG_FIXED;
			hd.friction    = 0.8f;
			/* Without this a capsule sliding across flat terrain catches on the
			 * shared diagonal of every cell.  A drop-and-settle test cannot see
			 * the difference; only walking can. */
			hd.smooth_internal_edges = true;

			JceBodyHandle body =
			    jce_physics_body_create_heightfield(rt->physics, &hd);
			jce_free(world_y);

			if (jce_body_valid(body)) {
				if (rt->body_count < rt->body_cap || rt_grow_bodies(rt))
					rt_track_body(rt, e, body, tf, (uint8_t)JCE_BODY_STATIC);
				LOG_INFO(LOG_TAG,
				         "terrain physics: %dx%d heightfield static body for "
				         "entity %llu", W, H, (unsigned long long)e);
				/* borrowed from the scene cache -- freed there, not here */
				return true;
			}
		}
	}

	/* ── Tiled / procedural: no resident grid, so PAGE the colliders ─────
	 *
	 * This is the case that previously produced NOTHING.  Both paths above
	 * need `jce_terrain_heights()`, which a tiled terrain does not have -- so a
	 * streamed multi-kilometre world was simply not solid.  The stream samples
	 * the terrain (which is tile-aware) per collision tile and keeps bodies
	 * only near the focus point. */
	if (!norm && W >= 2 && H >= 2 && sx > 0.0f && sz > 0.0f) {
		const float tile_ws = 128.0f;   /* collision tile size, world units */
		const uint32_t tx = (uint32_t)ceilf(sx / tile_ws);
		const uint32_t tz = (uint32_t)ceilf(sz / tile_ws);

		JceTerrainCollisionStreamDesc sd;
		memset(&sd, 0, sizeof sd);
		sd.world           = rt->physics;
		sd.sample_fn       = rt_terrain_collision_sample;
		sd.sample_ctx      = t;          /* borrowed from the scene cache */
		sd.sample_span     = 33u;
		sd.tiles_x         = tx ? tx : 1u;
		sd.tiles_z         = tz ? tz : 1u;
		sd.origin          = tf->position;
		sd.tile_world_size = tile_ws;
		sd.min_height      = 0.0f;
		sd.max_height      = mh > 0.0f ? mh : 1.0f;
		sd.radius          = tile_ws * 1.5f;
		sd.friction        = 0.8f;
		sd.diagonal        = JCE_HEIGHTFIELD_DIAG_FIXED;
		sd.smooth_internal_edges = true;
		sd.max_bodies      = 16u;

		/* One stream per runtime: a scene with two streamed terrains would
		 * need one each, which no content does yet.  Replacing rather than
		 * leaking is the conservative choice. */
		if (rt->terrain_stream) {
			jce_terrain_collision_stream_destroy(rt->terrain_stream);
			rt->terrain_stream = NULL;
		}
		rt->terrain_stream = jce_terrain_collision_stream_create(&sd);
		if (rt->terrain_stream) {
			rt->terrain_stream_src = t;
			/* Seed residency at the terrain origin so the first frame is
			 * already solid where the player starts. */
			jce_terrain_collision_stream_update(rt->terrain_stream,
			                                    tf->position);
			LOG_INFO(LOG_TAG,
			         "terrain physics: streamed collider %ux%u tiles for "
			         "entity %llu", sd.tiles_x, sd.tiles_z,
			         (unsigned long long)e);
			return true;
		}
	}

	/* ── Triangle-soup fallback: holes, or a terrain with no resident grid ── */
	float    *verts = NULL; uint32_t vcount = 0;
	uint32_t *idx   = NULL; uint32_t icount = 0;
	if (jce_terrain_build_collision_mesh(t, &verts, &vcount, &idx, &icount)) {
		JceColliderChild child;
		memset(&child, 0, sizeof child);
		child.shape        = JCE_SHAPE_TRIANGLE_MESH;
		child.rotation     = jce_q_identity();
		child.vertices     = verts;
		child.vertex_count = vcount;
		child.indices      = idx;
		child.index_count  = icount;

		JceCompoundBodyDesc desc;
		memset(&desc, 0, sizeof desc);
		desc.type        = JCE_BODY_STATIC;
		desc.position    = tf->position;
		desc.rotation    = tf->rotation;
		desc.friction    = 0.8f;
		desc.children    = &child;
		desc.child_count = 1;

		JceBodyHandle body = jce_physics_body_create_compound(rt->physics, &desc);
		if (jce_body_valid(body)) {
			if (rt->body_count < rt->body_cap || rt_grow_bodies(rt))
				rt_track_body(rt, e, body, tf, (uint8_t)JCE_BODY_STATIC);
			ok = true;
			LOG_INFO(LOG_TAG, "terrain physics: %u verts / %u tris triangle-soup "
			         "static body for entity %llu (%s)", vcount, icount / 3u,
			         (unsigned long long)e,
			         jce_terrain_has_holes(t) ? "has holes"
			                                  : "no resident height grid");
		}
		jce_free(verts);
		jce_free(idx);
	}
	/* borrowed from the scene cache -- freed there, not here */
	return ok;
}

/*
 * Raycast vehicle (btRaycastVehicle) — VEHICLE last-mile.
 *
 * If the entity carries an ENABLED JceVehicleComponent, build a chassis +
 * wheels via the public engine vehicle API and register a VehicleEntry so the
 * runtime drives input each tick and writes chassis/wheel poses back POST-step.
 *
 * Chassis box: component half-extents if non-zero, else the entity's BoxCollider
 * (0.5 * size), else the engine default.  Wheels: one per CHILD entity carrying
 * a JceWheelColliderComponent (connection point = child LOCAL position, which is
 * the child Transform position under `world = parent_world * local`); if none,
 * four default wheels are synthesized at the chassis corners so a bare vehicle
 * entity still drives.  is_front_wheel = (connection.z > 0) (chassis +Z = fwd).
 *
 * Returns true if a vehicle was spawned — the caller then SHORT-CIRCUITS the
 * normal rigid-body spawn for this entity (the chassis is its body), exactly as
 * rt_try_spawn_terrain does.  Disabled / absent component -> false -> the entity
 * spawns as a normal body and the frame path is byte-identical.
 */
bool rt_try_spawn_vehicle(JceRuntime *rt, JceScene *scene, JceEntity e,
                                 const JceTransform *tf)
{
	JceVehicleComponent *vc = jce_scene_get_vehicle(scene, e);
	if (!vc || !vc->enabled) return false;
	{ int vc_id = jce_component_find("Vehicle");   /* per-component disable (≠ vc->enabled) */
	  if (vc_id >= 0 && !jce_scene_comp_enabled(scene, e, vc_id)) return false; }

	/* Chassis half-extents: component override -> BoxCollider -> default. */
	jce_vec3 half = jce_v3(vc->chassis_half_extents[0],
	                       vc->chassis_half_extents[1],
	                       vc->chassis_half_extents[2]);
	if (half.x <= 0.0f && half.y <= 0.0f && half.z <= 0.0f) {
		JceBoxColliderComponent *box = jce_scene_get_box_collider(scene, e);
		if (box) {
			half = jce_v3(0.5f * box->size[0],
			              0.5f * box->size[1],
			              0.5f * box->size[2]);
		}
	}
	if (half.x <= 0.0f) half.x = 0.9f;
	if (half.y <= 0.0f) half.y = 0.5f;
	if (half.z <= 0.0f) half.z = 2.2f;

	JceVehicleDesc vd;
	memset(&vd, 0, sizeof vd);
	vd.position             = tf->position;
	vd.rotation             = tf->rotation;
	vd.chassis_half_extents = half;
	vd.chassis_mass     = vc->chassis_mass     > 0.0f ? vc->chassis_mass     : 1500.0f;
	vd.max_engine_force = vc->max_engine_force > 0.0f ? vc->max_engine_force : 4000.0f;
	vd.max_brake_force  = vc->max_brake_force  > 0.0f ? vc->max_brake_force  : 100.0f;
	vd.max_steering_rad = (vc->max_steering_deg > 0.0f ? vc->max_steering_deg : 30.0f)
	                      * JCE_DEG2RAD;
	vd.collision_group  = JCE_COLLISION_DEFAULT_GROUP;
	vd.collision_mask   = JCE_COLLISION_ALL_MASK;

	JceVehicleHandle veh = jce_physics_vehicle_create(rt->physics, &vd);
	if (!jce_vehicle_valid(veh)) {
		LOG_WARN(LOG_TAG, "vehicle: chassis create failed for entity %llu",
		         (unsigned long long)e);
		return false;
	}

	/* Reserve a registry slot now so a wheel loop can't realloc mid-build. */
	if (rt->vehicle_count >= rt->vehicle_cap && !rt_grow_vehicles(rt)) {
		jce_physics_vehicle_destroy(rt->physics, veh);
		return false;
	}
	VehicleEntry *ve = &rt->vehicles[rt->vehicle_count];
	memset(ve, 0, sizeof *ve);
	ve->entity      = e;
	ve->veh         = veh;
	ve->wheel_count = 0;
	ve->input_mode  = vc->input_mode;
	ve->drive_mode  = vc->drive_mode;

	/* Wheels from child wheel-collider entities. */
	JceEntity children[JCE_VEHICLE_MAX_WHEELS];
	int nch = jce_scene_get_children(scene, e, children,
	                                 (int)JCE_VEHICLE_MAX_WHEELS);
	for (int i = 0; i < nch && ve->wheel_count < (uint32_t)JCE_VEHICLE_MAX_WHEELS; ++i) {
		JceEntity child = children[i];
		if (!jce_scene_has_wheel_collider(scene, child)) continue;
		JceWheelColliderComponent *wcc = jce_scene_get_wheel_collider(scene, child);
		if (!wcc) continue;

		/* Connection point = child LOCAL position (child Transform position,
		 * which is parent-relative under the world = parent*local hierarchy)
		 * plus the wheel's authored center offset. */
		jce_vec3 conn = jce_v3(wcc->center[0], wcc->center[1], wcc->center[2]);
		JceTransform *ctf = jce_scene_get_transform(scene, child);
		if (ctf) conn = jce_v3_add(conn, ctf->position);

		JceWheelDesc wd;
		memset(&wd, 0, sizeof wd);
		wd.connection_point    = conn;
		wd.wheel_direction     = jce_v3(0.0f, -1.0f, 0.0f);
		wd.wheel_axle          = jce_v3(-1.0f, 0.0f, 0.0f);
		wd.suspension_rest_len = wcc->suspension_distance > 0.0f
		                         ? wcc->suspension_distance : 0.6f;
		wd.wheel_radius        = wcc->radius > 0.0f ? wcc->radius : 0.4f;
		wd.is_front_wheel      = (conn.z > 0.0f);
		if (wcc->suspension_spring > 0.0f)
			wd.suspension_stiffness = wcc->suspension_spring;
		if (wcc->suspension_damper > 0.0f)
			wd.suspension_damping   = wcc->suspension_damper;
		wd.friction_slip  = 1000.0f;
		wd.roll_influence = 0.1f;

		uint32_t widx = jce_physics_vehicle_add_wheel(rt->physics, veh, &wd);
		if (widx == UINT32_MAX) continue;
		ve->wheel_entities[ve->wheel_count] = child;
		ve->wheel_count++;
	}

	/* No authored wheels -> synthesize four at the chassis corners so a bare
	 * vehicle entity still drives.  wheel_entities[] stay 0 (no render target). */
	if (ve->wheel_count == 0) {
		const float cx = half.x;
		const float cz = half.z > 0.5f ? half.z - 0.4f : half.z;
		const float cy = -half.y + 0.1f;
		const float corners[4][3] = {
			{ -cx, cy,  cz },   /* front-left  */
			{  cx, cy,  cz },   /* front-right */
			{ -cx, cy, -cz },   /* rear-left   */
			{  cx, cy, -cz },   /* rear-right  */
		};
		const bool is_front[4] = { true, true, false, false };
		for (int i = 0; i < 4; ++i) {
			JceWheelDesc wd;
			memset(&wd, 0, sizeof wd);
			wd.connection_point    = jce_v3(corners[i][0], corners[i][1],
			                                corners[i][2]);
			wd.wheel_direction     = jce_v3(0.0f, -1.0f, 0.0f);
			wd.wheel_axle          = jce_v3(-1.0f, 0.0f, 0.0f);
			wd.suspension_rest_len = 0.6f;
			wd.wheel_radius        = 0.4f;
			wd.is_front_wheel      = is_front[i];
			wd.friction_slip       = 1000.0f;
			wd.roll_influence      = 0.1f;
			uint32_t widx = jce_physics_vehicle_add_wheel(rt->physics, veh, &wd);
			if (widx == UINT32_MAX) continue;
			ve->wheel_entities[ve->wheel_count] = 0;   /* synthesized */
			ve->wheel_count++;
		}
	}

	rt->vehicle_count++;
	LOG_INFO(LOG_TAG, "vehicle: entity %llu spawned with %u wheels",
	         (unsigned long long)e, ve->wheel_count);
	return true;
}

/*
 * ── Volumetric / pressure soft body (SOFT-BODY last-mile) ───────────────
 *
 * Mirror the scene's STATIC box colliders into the shared soft world so a
 * pressure body can rest / bounce on them.  Called once per scene (gated by
 * rt->soft_statics_mirrored) the first time a soft body spawns.  An entity
 * counts as "static" when it has an enabled BoxCollider and is NOT a dynamic
 * rigid body (no RigidBody, or a static/kinematic/zero-mass one) — the same
 * set that forms the immovable ground for the rigid world.
 */
static void rt_softbody_mirror_static(JceScene *scene, JceEntity e, void *ud)
{
	(void)ud;
	if (!jce_scene_has_box_collider(scene, e)) return;
	if (!jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_BOX_COLLIDER)) return;

	/* Skip dynamic bodies — only immovable colliders form the ground. */
	JceRigidBodyComponent *rb = jce_scene_get_rigidbody(scene, e);
	if (rb && jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_RIGIDBODY)) {
		bool dynamic = (rb->body_type == (uint8_t)JCE_BODY_DYNAMIC) &&
		               !rb->is_kinematic && rb->mass > 0.0f;
		if (dynamic) return;
	}

	JceBoxColliderComponent *box = jce_scene_get_box_collider(scene, e);
	if (!box) return;

	/* World transform: translation column = world position; column lengths =
	 * world scale (uniform-ish; good enough for a static ground proxy). */
	jce_mat4 m = jce_scene_get_world_matrix(scene, e);
	jce_vec3 wpos   = jce_v3(m.col[3].x, m.col[3].y, m.col[3].z);
	jce_vec3 sx     = jce_v3(m.col[0].x, m.col[0].y, m.col[0].z);
	jce_vec3 sy     = jce_v3(m.col[1].x, m.col[1].y, m.col[1].z);
	jce_vec3 sz     = jce_v3(m.col[2].x, m.col[2].y, m.col[2].z);
	float    scl_x  = jce_v3_len(sx);
	float    scl_y  = jce_v3_len(sy);
	float    scl_z  = jce_v3_len(sz);
	if (scl_x <= 0.0f) scl_x = 1.0f;
	if (scl_y <= 0.0f) scl_y = 1.0f;
	if (scl_z <= 0.0f) scl_z = 1.0f;

	/* Box centre offset (authored, scaled) added to the world position. */
	jce_vec3 center = jce_v3(wpos.x + box->center[0] * scl_x,
	                         wpos.y + box->center[1] * scl_y,
	                         wpos.z + box->center[2] * scl_z);
	jce_vec3 half   = jce_v3(0.5f * box->size[0] * scl_x,
	                         0.5f * box->size[1] * scl_y,
	                         0.5f * box->size[2] * scl_z);
	if (half.x <= 0.0f || half.y <= 0.0f || half.z <= 0.0f) return;

	(void)jce_softbody_add_static_box(center, half);
}

/*
 * If the entity carries an ENABLED JceSoftBodyComponent, create a volumetric
 * pressure soft body in the shared soft world via the public soft-body API and
 * register a SoftBodyEntry so the runtime publishes its centroid POST-step.
 *
 * On the FIRST soft body in the scene this enables soft simulation and mirrors
 * the scene's static box colliders into the soft world (so the body rests on
 * the ground).  Terrain trimesh mirroring is a documented follow-up.
 *
 * Returns true if a soft body was spawned — the caller then SHORT-CIRCUITS the
 * normal rigid-body spawn for this entity (the soft world owns its dynamics),
 * exactly as rt_try_spawn_vehicle / rt_try_spawn_terrain do.  Disabled / absent
 * component -> false -> the entity spawns as a normal rigid body.
 */
bool rt_try_spawn_softbody(JceRuntime *rt, JceScene *scene, JceEntity e,
                                  const JceTransform *tf)
{
	JceSoftBodyComponent *sc = jce_scene_get_soft_body(scene, e);
	if (!sc || !sc->enabled) return false;
	{ int sb_id = jce_component_find("SoftBody");   /* per-component disable (≠ sc->enabled) */
	  if (sb_id >= 0 && !jce_scene_comp_enabled(scene, e, sb_id)) return false; }

	/* First soft body this scene: turn the shared soft world on and mirror the
	 * scene's static box colliders in so bodies have a ground to rest on. */
	if (!rt->soft_statics_mirrored) {
		jce_softbody_set_simulation_enabled(true);
		jce_scene_each_entity(scene, rt_softbody_mirror_static, rt);
		rt->soft_statics_mirrored = true;
	}

	/* Reserve a registry slot first so the create can't be orphaned. */
	if (rt->softbody_count >= rt->softbody_cap && !rt_grow_softbodies(rt))
		return false;

	JceSoftBodyDesc d;
	memset(&d, 0, sizeof d);
	d.center           = tf->position;
	d.radius           = jce_v3(sc->radius[0] > 0.0f ? sc->radius[0] : 0.5f,
	                            sc->radius[1] > 0.0f ? sc->radius[1] : 0.5f,
	                            sc->radius[2] > 0.0f ? sc->radius[2] : 0.5f);
	d.resolution       = sc->resolution > 3 ? sc->resolution : 96;
	d.mass             = sc->mass > 0.0f ? sc->mass : 2.0f;
	d.pressure         = sc->pressure;
	d.stiffness_linear = sc->stiffness_linear;
	d.stiffness_volume = sc->stiffness_volume;
	d.damping          = sc->damping;
	d.friction         = sc->friction;
	d.self_collision   = sc->self_collision;

	JceSoftBodyHandle h = jce_softbody_create_ellipsoid(&d);
	if (h == JCE_SOFTBODY_INVALID) {
		LOG_WARN(LOG_TAG, "softbody: create failed for entity %llu",
		         (unsigned long long)e);
		return false;
	}

	SoftBodyEntry *se = &rt->softbodies[rt->softbody_count];
	se->entity = e;
	se->handle = h;
	rt->softbody_count++;
	LOG_INFO(LOG_TAG, "softbody: entity %llu spawned (%u nodes)",
	         (unsigned long long)e, jce_softbody_node_count(h));
	return true;
}

/*
 * Spawn a 2D rigid body for entity `e` from its RigidBody2DComponent
 * (+ optional Collider2DComponent) into the Box2D world.  The simulation
 * runs in the XY plane: the entity's Transform x/y seed the body position,
 * the Z-rotation angle seeds the body angle, and the body's half-extents
 * come from the collider (scaled by the entity's XY scale) — or a unit box
 * when no collider is authored.
 *
 * The Collider2D shape enum (JCE_COLLIDER_2D_*) differs from the wrapper's
 * JceShape2DType: EDGE maps to SEGMENT, and POLYGON has no wrapper shape so
 * it falls back to BOX (noted as a limitation).
 */
void rt_spawn_body2d(JceRuntime *rt, JceScene *scene,
                            JceEntity e, const JceTransform *tf)
{
	JceRigidBody2DComponent *rb = jce_scene_get_rigidbody2d(scene, e);
	if (!rb) return;

	JceBody2DDesc bd;
	memset(&bd, 0, sizeof bd);

	/* XY plane: take x/y from the transform, drop z. */
	bd.position.x = tf->position.x;
	bd.position.y = tf->position.y;

	/* Recover the Z-rotation angle (radians) from the transform quaternion.
	 * For a pure Z rotation q = (0,0,sin(a/2),cos(a/2)) this is exact;
	 * atan2 keeps it well-behaved for small off-axis tilts. */
	{
		jce_quat q = tf->rotation;
		bd.angle = atan2f(2.0f * (q.w * q.z + q.x * q.y),
		                  1.0f - 2.0f * (q.y * q.y + q.z * q.z));
	}

	bd.mass            = rb->mass;
	bd.friction        = rb->friction > 0.0f ? rb->friction : 0.5f;
	bd.restitution     = rb->restitution;
	bd.fixed_rotation  = rb->fixed_rotation;

	/* Body type: kinematic flag / zero-mass static / dynamic. */
	if (rb->body_type == JCE_BODY_KINEMATIC) bd.type = JCE_BODY_KINEMATIC;
	else if (rb->body_type == JCE_BODY_STATIC || rb->mass <= 0.0f)
		bd.type = JCE_BODY_STATIC;
	else                                     bd.type = JCE_BODY_DYNAMIC;

	/* 2D body: x/y projection of the abs-sanitized scale. */
	jce_vec3 s2 = jce_v3_abs_safe_scale(tf->scale);
	float sx = s2.x;
	float sy = s2.y;
	float smax = sx > sy ? sx : sy;

	/* Shape + extents from the optional Collider2D; default to a unit box. */
	JceCollider2DComponent *col = jce_scene_get_collider2d(scene, e);
	if (col && !jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_COLLIDER_2D))
		col = NULL;   /* disabled collider -> default unit box below */
	if (col) {
		bd.position.x += col->offset[0];
		bd.position.y += col->offset[1];
		bd.friction    = col->friction > 0.0f ? col->friction : bd.friction;
		bd.restitution = col->restitution;
		switch (col->shape) {
			case JCE_COLLIDER_2D_CIRCLE: {
				float r = col->radius > 0.0f ? col->radius : 0.5f;
				bd.shape = JCE_SHAPE2D_CIRCLE;
				bd.half_extents.x = r * smax;
				bd.half_extents.y = 0.0f;
				break;
			}
			case JCE_COLLIDER_2D_CAPSULE: {
				float r  = col->radius > 0.0f ? col->radius : 0.25f;
				/* size.y is the full length; wrapper wants half_length. */
				float hl = 0.5f * (col->size[1] > 0.0f ? col->size[1] : 1.0f);
				bd.shape = JCE_SHAPE2D_CAPSULE;
				bd.half_extents.x = r * smax;       /* radius */
				bd.half_extents.y = hl * sy;        /* half length */
				break;
			}
			case JCE_COLLIDER_2D_EDGE: {
				/* No multi-point edge support in the wrapper — approximate
				 * with a single horizontal segment spanning size.x. */
				bd.shape = JCE_SHAPE2D_SEGMENT;
				bd.half_extents.x = 0.5f * (col->size[0] > 0.0f ? col->size[0] : 1.0f) * sx;
				bd.half_extents.y = 0.0f;
				break;
			}
			case JCE_COLLIDER_2D_POLYGON:
				/* Wrapper has no arbitrary-polygon shape — fall back to the
				 * collider's bounding box (limitation, noted). */
				/* fall through */
			case JCE_COLLIDER_2D_BOX:
			default: {
				bd.shape = JCE_SHAPE2D_BOX;
				bd.half_extents.x = 0.5f * (col->size[0] > 0.0f ? col->size[0] : 1.0f) * sx;
				bd.half_extents.y = 0.5f * (col->size[1] > 0.0f ? col->size[1] : 1.0f) * sy;
				break;
			}
		}
	} else {
		/* No collider authored — placeholder unit box from XY scale. */
		bd.shape = JCE_SHAPE2D_BOX;
		bd.half_extents.x = 0.5f * sx;
		bd.half_extents.y = 0.5f * sy;
	}

	if (bd.half_extents.x <= 0.0f) bd.half_extents.x = 0.5f;
	if (bd.shape != JCE_SHAPE2D_CIRCLE && bd.shape != JCE_SHAPE2D_SEGMENT &&
	    bd.half_extents.y <= 0.0f)
		bd.half_extents.y = 0.5f;

	JceBodyHandle body = jce_physics2d_body_create(rt->physics2d, &bd);
	if (!jce_body_valid(body)) return;

	/* Persist the body index back into the component (mirrors 3D contract). */
	rb->body_handle_idx = body.idx;

	if (rt->body2d_count >= rt->body2d_cap && !rt_grow_bodies2d(rt))
		return;
	Body2DEntry *be = &rt->bodies2d[rt->body2d_count];
	be->entity = e;
	be->body   = body;
	be->kind   = (uint8_t)bd.type;
	rt->body2d_count++;
}

/*
 * Spawn ONE static 2D body for entity `e` from its TilemapCollider2D +
 * Tilemap components: the .tilemap.json's solid cells (any non-zero id)
 * are greedy-merged into axis-aligned rectangles and attached as box
 * shapes to a single static Box2D body, so a large map costs a handful
 * of shapes instead of one per cell.
 *
 * Cell convention (mirrors the renderer): cell (col,row) spans entity-
 * local [col,col+1] x [-(row+1),-row], scaled by the entity's XY scale.
 *
 * `used_by_composite` is intentionally ignored: the greedy merge above
 * already IS the composite — there is no separate CompositeCollider2D
 * component to defer shape ownership to.
 */
void rt_spawn_tilemap_collider2d(JceRuntime *rt, JceScene *scene,
                                        JceEntity e, const JceTransform *tf)
{
	JceTilemapCollider2DComponent *col =
		jce_scene_get_tilemap_collider2d(scene, e);
	JceTilemapComponent *tm = jce_scene_get_tilemap(scene, e);
	if (!col || !tm || !tm->tilemap_path[0]) return;

	/* The tilemap stays STATIC even when a dynamic RigidBody2D coexists
	 * on the entity (moving tilemap colliders are out of scope). */
	JceRigidBody2DComponent *rb = jce_scene_get_rigidbody2d(scene, e);
	if (rb && jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_RIGIDBODY_2D) &&
	    rb->body_type != JCE_BODY_STATIC && rb->body_type != JCE_BODY_KINEMATIC &&
	    rb->mass > 0.0f)
		LOG_WARN(LOG_TAG, "tilemap collider on entity %llu coexists with a "
		         "DYNAMIC RigidBody2D; the tilemap body stays static",
		         (unsigned long long)e);

	/* PAK-first (deployed bundles), then the loose file (editor Play). */
	JceTilemapAsset *map = rt->pak
		? jce_tilemap_load_from_pak(rt->pak, tm->tilemap_path) : NULL;
	if (!map) {
		char        mbuf[1024];
		const char *mpath = rt_resolve_host_path(rt, tm->tilemap_path,
		                                         mbuf, sizeof mbuf);
		map = jce_tilemap_load_file(mpath);
	}
	if (!map) {
		LOG_WARN(LOG_TAG, "tilemap collider: load failed '%s' (entity %llu)",
		         tm->tilemap_path, (unsigned long long)e);
		return;
	}

	JceTilemapSolidRect rects[JCE_TILEMAP_COL_MAX_RECTS];
	uint32_t total = jce_tilemap_solid_rects(map, rects,
	                                         JCE_TILEMAP_COL_MAX_RECTS);
	uint32_t n = total;
	if (n > JCE_TILEMAP_COL_MAX_RECTS) {
		LOG_WARN(LOG_TAG, "tilemap collider '%s': %u merged rects exceed "
		         "the %d cap; truncating", tm->tilemap_path, total,
		         JCE_TILEMAP_COL_MAX_RECTS);
		n = JCE_TILEMAP_COL_MAX_RECTS;
	}
	if (n == 0) {
		jce_tilemap_unload(map);
		return;
	}

	/* Same Z-angle quaternion recovery as rt_spawn_body2d. */
	float angle;
	{
		jce_quat q = tf->rotation;
		angle = atan2f(2.0f * (q.w * q.z + q.x * q.y),
		               1.0f - 2.0f * (q.y * q.y + q.z * q.z));
	}
	jce_vec2 pos;
	pos.x = tf->position.x + col->offset[0];
	pos.y = tf->position.y + col->offset[1];

	JceBodyHandle body = jce_physics2d_body_create_empty(rt->physics2d, pos,
	                                                     angle, JCE_BODY_STATIC);
	if (!jce_body_valid(body)) {
		jce_tilemap_unload(map);
		return;
	}

	jce_vec3 s2 = jce_v3_abs_safe_scale(tf->scale);
	float friction    = (float)col->friction_x100 / 100.0f;
	float restitution = (float)col->bounciness_x100 / 100.0f;

	for (uint32_t i = 0; i < n; i++) {
		jce_vec2 center, half;
		center.x =  ((float)rects[i].x + (float)rects[i].w * 0.5f) * s2.x;
		center.y = -((float)rects[i].y + (float)rects[i].h * 0.5f) * s2.y;
		half.x   = (float)rects[i].w * 0.5f * s2.x;
		half.y   = (float)rects[i].h * 0.5f * s2.y;
		jce_physics2d_body_add_box(rt->physics2d, body, center, half,
		                           friction, restitution, col->trigger);
	}

	/* Track like rt_spawn_body2d (static → no transform write-back). */
	if (rt->body2d_count < rt->body2d_cap || rt_grow_bodies2d(rt)) {
		Body2DEntry *be = &rt->bodies2d[rt->body2d_count];
		be->entity = e;
		be->body   = body;
		be->kind   = (uint8_t)JCE_BODY_STATIC;
		rt->body2d_count++;
	}

	LOG_INFO(LOG_TAG, "tilemap collider: %u box shapes for '%s' (entity %llu)",
	         n, tm->tilemap_path, (unsigned long long)e);
	jce_tilemap_unload(map);
}


/* Build the JceBodyDesc + create the rigid body + track it for ONE entity.
 *
 * Extracted verbatim from rt_spawn_entity so the SAME bd-building logic feeds
 * both the scene-load walk and the per-frame draw-distance spawn pass (a far
 * small static is deferred at load and created here later when the player
 * approaches).  Re-resolves tf/rb/colliders from the scene each call so the
 * deferred path needs only the entity id.
 *
 * `allow_defer`: when true (scene-load walk) a SMALL STATIC box/sphere/capsule
 * collider is NOT created — it is recorded in rt->dd[] and the per-frame pass
 * spawns it on approach.  When false (the draw-distance pass itself) the body
 * is always created.  Returns true iff a body was actually created. */
bool rt_spawn_entity_body(JceRuntime *rt, JceScene *scene, JceEntity e,
                                 bool allow_defer)
{
	if (!rt->physics) return false;
	JceTransform *tf = jce_scene_get_transform(scene, e);
	if (!tf) return false;
	JceRigidBodyComponent *rb = jce_scene_get_rigidbody(scene, e);
	if (!rb || !jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_RIGIDBODY))
		return false;

	JceBodyDesc bd;
	memset(&bd, 0, sizeof bd);
	bd.position        = tf->position;
	bd.rotation        = tf->rotation;
	bd.mass            = rb->mass;
	bd.linear_damping  = rb->drag;
	bd.angular_damping = rb->angular_drag;
	bd.friction        = rb->friction    > 0.0f ? rb->friction    : 0.5f;
	bd.restitution     = rb->restitution;

	JceBoxColliderComponent     *box = jce_scene_get_box_collider(scene, e);
	JceSphereColliderComponent  *sph = jce_scene_get_sphere_collider(scene, e);
	JceCapsuleColliderComponent *cap = jce_scene_get_capsule_collider(scene, e);
	if (cap && !jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_CAPSULE_COLLIDER))
		cap = NULL;
	if (box && !jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_BOX_COLLIDER))
		box = NULL;
	if (sph && !jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_SPHERE_COLLIDER))
		sph = NULL;
	/* Collider center offset, LOCAL (scaled, pre-rotation).  Baked into
	 * the body origin below; also stored on the BodyEntry so push/sync
	 * keep the collider centered when the entity is moved at runtime. */
	jce_vec3 collider_center_local = jce_v3(0.0f, 0.0f, 0.0f);
	if (box) {
		bd.shape = JCE_SHAPE_BOX;
		bd.half_extents.x = 0.5f * box->size[0] * tf->scale.x;
		bd.half_extents.y = 0.5f * box->size[1] * tf->scale.y;
		bd.half_extents.z = 0.5f * box->size[2] * tf->scale.z;
		if (bd.half_extents.x <= 0.0f) bd.half_extents.x = 0.5f;
		if (bd.half_extents.y <= 0.0f) bd.half_extents.y = 0.5f;
		if (bd.half_extents.z <= 0.0f) bd.half_extents.z = 0.5f;
		/* Local center: scale by TRS scale, then rotate by TRS rotation
		 * (matches the editor collider overlay; was a raw world add). */
		{
			collider_center_local = jce_v3(box->center[0] * tf->scale.x,
			                               box->center[1] * tf->scale.y,
			                               box->center[2] * tf->scale.z);
			jce_vec3 ofs = jce_q_rotate(tf->rotation, collider_center_local);
			bd.position = jce_v3_add(bd.position, ofs);
		}
		bd.is_trigger = box->is_trigger;
	} else if (sph) {
		bd.shape = JCE_SHAPE_SPHERE;
		float smax = tf->scale.x;
		if (tf->scale.y > smax) smax = tf->scale.y;
		if (tf->scale.z > smax) smax = tf->scale.z;
		float r = sph->radius > 0.0f ? sph->radius : 0.5f;
		bd.half_extents.x = r * smax;
		{
			collider_center_local = jce_v3(sph->center[0] * tf->scale.x,
			                               sph->center[1] * tf->scale.y,
			                               sph->center[2] * tf->scale.z);
			jce_vec3 sofs = jce_q_rotate(tf->rotation, collider_center_local);
			bd.position = jce_v3_add(bd.position, sofs);
		}
		bd.is_trigger = sph->is_trigger;
	} else if (cap) {
		/* Bullet capsules are Y-aligned: half_extents = (radius,
		 * cylinder half-height, 0).  Mirror the editor collider
		 * overlay (jce_scene_render_draw.cpp capsule block) so
		 * draw == physics: radius scales by max(|sx|,|sz|), total
		 * height by |sy|, hemispheres carved out of the authored
		 * total height.  `axis` is intentionally ignored — the
		 * overlay draws Y-aligned too, and JceBodyDesc has no
		 * per-shape axis (only whole-body rotation). */
		bd.shape = JCE_SHAPE_CAPSULE;
		jce_vec3 cs = jce_v3_abs_safe_scale(tf->scale);
		float cr_scale = cs.x > cs.z ? cs.x : cs.z;
		float cr = (cap->radius > 0.0f ? cap->radius : 0.3f) * cr_scale;
		float ch = (cap->height > 0.0f ? cap->height : 1.0f) * cs.y;
		float chh = 0.5f * (ch - 2.0f * cr);
		if (chh < 0.0f) chh = 0.0f;
		bd.half_extents.x = cr;
		bd.half_extents.y = chh;
		/* Local center: scale by TRS scale, then rotate by TRS
		 * rotation (same as the box/sphere branches above). */
		{
			collider_center_local = jce_v3(cap->center[0] * tf->scale.x,
			                               cap->center[1] * tf->scale.y,
			                               cap->center[2] * tf->scale.z);
			jce_vec3 cofs = jce_q_rotate(tf->rotation, collider_center_local);
			bd.position = jce_v3_add(bd.position, cofs);
		}
		bd.is_trigger = cap->is_trigger;
	} else {
		/* No collider authored — placeholder box from transform
		 * scale so dropped objects still collide. */
		bd.shape = JCE_SHAPE_BOX;
		bd.half_extents.x = 0.5f * tf->scale.x;
		bd.half_extents.y = 0.5f * tf->scale.y;
		bd.half_extents.z = 0.5f * tf->scale.z;
		if (bd.half_extents.x <= 0.0f) bd.half_extents.x = 0.5f;
		if (bd.half_extents.y <= 0.0f) bd.half_extents.y = 0.5f;
		if (bd.half_extents.z <= 0.0f) bd.half_extents.z = 0.5f;
	}

	if (rb->is_kinematic)       bd.type = JCE_BODY_KINEMATIC;
	else if (rb->mass <= 0.0f)  bd.type = JCE_BODY_STATIC;
	else                        bd.type = JCE_BODY_DYNAMIC;

	/* ── Draw-distance classification (big-world deferred spawn) ──
	 * On the scene-load walk only: a SMALL STATIC box/sphere/capsule (the
	 * shapes above; the no-collider placeholder is intentionally excluded —
	 * it has no authored collider so it is not a building) is recorded in
	 * rt->dd[] and NOT created now.  The per-frame pass spawns it once the
	 * player is within PHYS_DD_RADIUS and despawns it when far.  Everything
	 * else (large statics like the ground / big boxes, and all dynamic /
	 * kinematic bodies) spawns immediately, exactly as before. */
	if (allow_defer && bd.type == JCE_BODY_STATIC && (box || sph || cap)) {
		float hx = bd.half_extents.x, hy = bd.half_extents.y, hz = bd.half_extents.z;
		float hmax = hx;
		if (hy > hmax) hmax = hy;
		if (hz > hmax) hmax = hz;
		if (hmax < PHYS_DD_SMALL) {
			if (rt->dd_count >= rt->dd_cap && !rt_grow_dd(rt))
				return false;   /* OOM: fall through would create it now */
			DDEntry *de = &rt->dd[rt->dd_count++];
			de->entity  = e;
			de->center  = bd.position;   /* collider world center */
			de->spawned = false;
			return false;                /* deferred, no body yet */
		}
	}

	JceBodyHandle body = jce_physics_body_create(rt->physics, &bd);
	if (!jce_body_valid(body)) return false;
	if (rb->ccd_mode != JCE_CCD_DISCRETE) {
		jce_physics_body_set_ccd_mode(rt->physics, body,
		                              (JceCcdMode)rb->ccd_mode);
		if (rb->ccd_threshold > 0.0f)
			jce_physics_body_set_ccd_motion_threshold(
				rt->physics, body, rb->ccd_threshold);
		if (rb->ccd_sphere_radius > 0.0f)
			jce_physics_body_set_ccd_swept_sphere_radius(
				rt->physics, body, rb->ccd_sphere_radius);
	}
	rt_apply_body_extras(rt, e, body, rb, rb->physmat_path);
	if (rt->body_count >= rt->body_cap && !rt_grow_bodies(rt)) {
		jce_physics_body_destroy(rt->physics, body);
		return false;
	}
	rt_track_body(rt, e, body, tf, (uint8_t)bd.type);
	/* Remember the collider center offset so push/sync keep the body
	 * centered on the collider when the entity is moved at runtime. */
	rt->bodies[rt->body_count - 1].center_local = collider_center_local;
	return true;
}
