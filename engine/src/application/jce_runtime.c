/*
 * jce_runtime.c  Layer 6 — Application.
 *
 * Authored-scene → live simulation driver shared by the editor's Play
 * button and deployed game executables.  See jce_runtime.h for contract.
 *
 * Walks the JceScene once at create() time, materialising:
 *   - one JcePhysicsWorld with a body for every (RigidBody +
 *     Box/SphereCollider) pair and a JceCharacterHandle for the first
 *     CharacterController found,
 *   - a JceVoice per AudioSource flagged play_on_awake.
 *
 * Per-step:
 *   1. character controller move + jump from the latest input
 *   2. jce_physics_step
 *   3. write back transforms
 *   4. jce_scene_update
 *   5. update audio listener (tracks primary camera) + spatial voices
 *
 * Implementation is intentionally allocation-heavy on create() rather
 * than per-frame: bodies/voices are sized to the actual scene contents,
 * not a fixed cap.
 */

#include "jce_terrain_collision_stream.h"
#include <jce/resource/jce_world_streamer.h>  /* shipped-path world streaming */
#include "jce_rt_internal.h"
#include <jce/os/core/jce_timer.h>
#include <jce/os/core/jce_perf_phase.h>
#include <jce/middleware/scene/jce_ui_canvas.h>
#include <jce/middleware/scene/jce_water_field.h>
#include <jce/middleware/scene/jce_water_ripple.h>
#if defined(JCE_ENABLE_AI_DISPATCH) && JCE_ENABLE_AI_DISPATCH
#include <jce/middleware/ai_dispatch/jce_ai_dispatch.h>
#endif



/* ── Helpers ─────────────────────────────────────────────────────── */

/* Compose the full parent-chain world matrix and return its translation
 * column.  Unlike a plain local-position sum, this honours parent rotation
 * and scale, so a source parented to a rotating/moving rig is placed at its
 * true world location.  Reuses the shared engine world-matrix path so it
 * stays consistent with rendering/picking. */
jce_vec3 rt_world_position(JceScene *scene, JceEntity e)
{
	if (e == 0 || !jce_scene_has_transform(scene, e))
		return jce_v3(0.0f, 0.0f, 0.0f);
	jce_mat4 w = jce_scene_get_world_matrix(scene, e);
	return jce_v3(w.raw[3][0], w.raw[3][1], w.raw[3][2]);
}

/* Array-grower family.  Every dynamic entry array follows the identical
 * "double-or-seed then realloc" pattern; RT_GROW_FN (jce_rt_internal.h)
 * generates one grower per (field, cap-field, seed) triple so the bodies stay
 * in lock-step.  (dedup A6: collapsed ~20 hand-written rt_grow_* clones.)
 * External linkage + prototypes (RT_GROW_DECL) so the carved-out rt_*.c modules
 * can call / define the growers they need. */
RT_GROW_FN(rt_grow_bodies,            bodies,            body_cap,             16)
RT_GROW_FN(rt_grow_dd,                dd,                dd_cap,               64)
RT_GROW_FN(rt_grow_voices,            voices,            voice_cap,             8)
RT_GROW_FN(rt_grow_bodies2d,          bodies2d,          body2d_cap,           16)
RT_GROW_FN(rt_grow_triggers,          triggers,          trigger_cap,           8)
RT_GROW_FN(rt_grow_spawns,            spawns,            spawn_cap,             4)
RT_GROW_FN(rt_grow_weapons,           weapons,           weapon_cap,            4)
RT_GROW_FN(rt_grow_nav_entries,       nav_entries,       nav_entry_cap,         4)
RT_GROW_FN(rt_grow_save_points,       save_points,       save_point_cap,        4)
RT_GROW_FN(rt_grow_bts,               bts,               bt_cap,                4)
RT_GROW_FN(rt_grow_scripts,           scripts,           script_cap,            4)
RT_GROW_FN(rt_grow_gas,               gas_entries,       gas_cap,               4)

/* Find the live GAS for an entity (linear; entry counts are small). NULL if
 * the entity authored no ability system. */
JceGameplayAbilitySystem *rt_gas_for_entity(JceRuntime *rt, JceEntity e)
{
	if (!rt) return NULL;
	for (int i = 0; i < rt->gas_count; ++i)
		if (rt->gas_entries[i].entity == e)
			return &rt->gas_entries[i].gas;
	return NULL;
}

RT_GROW_FN(rt_grow_ragdoll,           ragdoll_entries,   ragdoll_cap,           4)

/* Find the live ragdoll entry for an entity (linear; entry counts are small).
 * NULL if the entity authored no ragdoll. */
static struct RagdollEntry *rt_ragdoll_for_entity(JceRuntime *rt, JceEntity e)
{
	if (!rt) return NULL;
	for (int i = 0; i < rt->ragdoll_count; ++i)
		if (rt->ragdoll_entries[i].entity == e)
			return &rt->ragdoll_entries[i];
	return NULL;
}

RT_GROW_FN(rt_grow_pending_spawns,    pending_spawns,    pending_spawn_cap,     8)
RT_GROW_FN(rt_grow_pending_fractures, pending_fractures, pending_fracture_cap,  8)
RT_GROW_FN(rt_grow_vehicles,          vehicles,          vehicle_cap,           4)
RT_GROW_FN(rt_grow_softbodies,        softbodies,        softbody_cap,          4)
RT_GROW_FN(rt_grow_cfg_joints,        cfg_joints,        cfg_joint_cap,         4)
RT_GROW_FN(rt_grow_joints,            joints,            joint_cap,             4)
RT_GROW_FN(rt_grow_joints2d,          joints2d,          joint2d_cap,           4)

/* Find the live vehicle entry for an entity (linear; entry counts are small).
 * NULL if the entity authored no enabled vehicle. */
VehicleEntry *rt_vehicle_for_entity(JceRuntime *rt, JceEntity e)
{
	if (!rt) return NULL;
	for (int i = 0; i < rt->vehicle_count; ++i)
		if (rt->vehicles[i].entity == e)
			return &rt->vehicles[i];
	return NULL;
}

/* ── Script host bridge (Phase 0 keystone) ────────────────────────────────
 * Supplied to the Lua VM so scripts can read/move entities + log without the
 * script layer depending upward on scene/ECS (mirrors how BT actions reach
 * the runtime).  `user` is always the JceRuntime*. */
/* ── Gameplay-bridge sinks (P0-master-bridge) ────────────────────────
 *
 * The trigger world fires enter/stay/exit through this sink.  We log
 * transitions (ENTER/EXIT) at debug level so designers can verify zones
 * react in Play; STAY is intentionally not logged (it would spam).  Games
 * that need real reactions subscribe to their own sink via the engine
 * trigger API once they hold the JceTriggerWorld (exposed in a follow-up).
 *
 * A SavePoint volume is the one zone the runtime acts on directly: on ENTER
 * it writes a snapshot (P2-save-snapshot), unless the point is one_shot and
 * already fired, or require_interact (left to game input — not auto-fired).
 */
static void rt_trigger_event(const JceTriggerEvent *ev, void *user)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!ev) return;
	if (ev->type == JCE_TRIGGER_EVENT_ENTER) {
		LOG_INFO(LOG_TAG, "trigger ENTER: zone entity=%llu observer=%llu",
		         (unsigned long long)ev->trigger_user,
		         (unsigned long long)ev->observer_user);
		/* SavePoint overlap → snapshot (skip require_interact points). */
		if (rt) {
			for (int i = 0; i < rt->save_point_count; ++i) {
				SavePointEntry *sp = &rt->save_points[i];
				if ((uint64_t)sp->entity != ev->trigger_user) continue;
				/* Tell the game FIRST, whatever happens next.  A
				 * require_interact point was previously left to "game
				 * input" while the engine told the game nothing at all,
				 * so the prompt JceSavePointComponent.display_name
				 * exists for could not be shown.  kind rides along as
				 * the message's number so MANUAL / AUTO / CHECKPOINT
				 * can be presented differently. */
				for (int k = 0; k < rt->script_count; ++k) {
					struct ScriptEntry *se = &rt->scripts[k];
					if (se->active && (uint64_t)se->entity == ev->trigger_user) {
						rt_script_ref_message(
						    se->ref, "on_save_point", (double)sp->kind,
						    sp->display_name[0] ? sp->display_name : NULL);
						break;
					}
				}
				if (sp->require_interact) break;   /* game-driven, not here */
				if (sp->one_shot && sp->fired) break;
				if (rt_perform_save_slot(rt, sp->save_id, sp->slot))
					sp->fired = true;
				break;
			}
		}
	} else if (ev->type == JCE_TRIGGER_EVENT_EXIT) {
		LOG_INFO(LOG_TAG, "trigger EXIT: zone entity=%llu observer=%llu",
		         (unsigned long long)ev->trigger_user,
		         (unsigned long long)ev->observer_user);
	}

	/* Dispatch ENTER/STAY/EXIT to the ZONE entity's Lua script as
	 * on_trigger_enter(self, observer_entity) / on_trigger_exit(self,
	 * observer_entity), so a game reacts to any TriggerVolume in script with no
	 * C wiring — the GTA-style mission-zone primitive (walk into a red zone ->
	 * start a cutscene/mission).  Mirrors rt_script_collision_cb's linear scan;
	 * the observer (usually the player) is passed as the number arg (entity id
	 * fits exactly in a double).  STAY is intentionally not dispatched. */
	if (rt) {
		/* STAY is dispatched now.  It used to be computed by the trigger
		 * world, emitted to this callback, and dropped -- "STAY is
		 * intentionally not dispatched" -- which made
		 * TriggerVolume.fire_stay doubly dead: the flag was unread AND the
		 * event it gates reached nobody.  It only fires for a trigger that
		 * ASKED for it, so a zone that did not tick the box costs nothing.
		 *
		 * The tag rides along as the message's string argument, so a script
		 * can tell one zone from another without hard-coding entity ids --
		 * which is what the component means by "propagated to event
		 * payload". */
		const char *method =
		      (ev->type == JCE_TRIGGER_EVENT_ENTER) ? "on_trigger_enter"
		    : (ev->type == JCE_TRIGGER_EVENT_STAY)  ? "on_trigger_stay"
		    : (ev->type == JCE_TRIGGER_EVENT_EXIT)  ? "on_trigger_exit"
		                                            : NULL;
		if (method) {
			for (int i = 0; i < rt->script_count; ++i) {
				struct ScriptEntry *se = &rt->scripts[i];
				if (se->active && (uint64_t)se->entity == ev->trigger_user) {
					rt_script_ref_message(se->ref, method,
					                      (double)ev->observer_user,
					                      (ev->tag && ev->tag[0]) ? ev->tag
					                                              : NULL);
					break;
				}
			}
		}
	}
}

/* Spawn-manager create/destroy callbacks.  The runtime does not author
 * game entities itself (that is a game/prefab decision), so these issue a
 * monotonic cookie on create and accept the despawn — keeping the spawn
 * state machine (cadence, soft-cap, ring eviction) running so designers
 * can validate pacing in Play.  A game overrides this by driving its own
 * JceSpawnManager. */
static uint64_t rt_spawn_create(const JceSpawnRequest *req, void *user)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene || !req) return 0;
	/* Resolve the spawning manager's ped prefab (cur_spawn_mgr is set
	 * transiently just before this manager's update in rt_tick_gameplay). */
	const JceSpawnManagerComponent *smc =
		jce_scene_get_spawn_manager(rt->scene, rt->cur_spawn_mgr);
	if (!smc || !smc->ped_prefab_path[0]) return 0;   /* nothing authored to spawn */
	/* Global actor budget: refuse the spawn (return 0 ⇒ manager aborts the slot)
	 * once the world-level pool or per-frame quota is reached.  Both 0 = unlimited. */
	if (rt->actor_budget      && rt->actor_count        >= rt->actor_budget)      return 0;
	if (rt->actor_spawn_quota && rt->actor_spawned_frame >= rt->actor_spawn_quota) return 0;
	JceEntity e = rt_spawn_prefab_at(rt, smc->ped_prefab_path,
	                                 req->position.x, req->position.y, req->position.z);
	if (e) { rt->actor_count++; rt->actor_spawned_frame++; }
	return (uint64_t)e;   /* cookie = spawned entity id (0 ⇒ slot aborted) */
}

static void rt_spawn_destroy(uint64_t cookie, void *user)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene || cookie == 0) return;
	jce_scene_destroy_entity(rt->scene, (JceEntity)cookie);
	if (rt->actor_count) rt->actor_count--;   /* mirror the budget counter */
}

/* Default ped sampler: a random point in the [min_r, max_r] ring around the
 * viewer on the XZ plane (no navmesh required).  sqrt(t) keeps it uniform over
 * the annulus; a per-runtime xorshift cursor decorrelates successive frames. */
static bool rt_spawn_ped_sample(jce_vec3 viewer_pos, float min_r, float max_r,
                                jce_vec3 *out_pos, void *user)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !out_pos) return false;
	if (max_r <= 0.0f) max_r = 1.0f;
	if (min_r < 0.0f)  min_r = 0.0f;
	if (min_r > max_r) min_r = max_r;
	uint64_t h = (rt->spawn_cookie_seq += 0x9E3779B97F4A7C15ull);
	h ^= h >> 30; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 27;
	float ang = (float)((h & 0xFFFFu) / 65535.0) * 6.2831853f;
	float t   = (float)(((h >> 16) & 0xFFFFu) / 65535.0);
	float r   = min_r + (max_r - min_r) * sqrtf(t);
	out_pos->x = viewer_pos.x + cosf(ang) * r;
	out_pos->y = viewer_pos.y;
	out_pos->z = viewer_pos.z + sinf(ang) * r;
	return true;
}

/* ── Scene walk: spawn physics + audio for each entity ───────────── */


static void rt_spawn_entity(JceScene *scene, JceEntity e, void *ud)
{
	JceRuntime *rt = (JceRuntime *)ud;
	if (!jce_scene_has_transform(scene, e)) return;
	JceTransform *tf_local = jce_scene_get_transform(scene, e);
	if (!tf_local) return;

	/* PHYSICS PLACES BODIES IN THE WORLD, AND A CHILD'S JceTransform IS NOT A
	 * PLACE.  It is an offset from its parent.  Every spawner below used to
	 * receive the raw local TRS and hand its position straight to Bullet, so a
	 * trigger volume parented to a door at x=4 and authored at local x=0 got
	 * its collider at the WORLD ORIGIN -- measured bit-identical to giving it
	 * no parent at all (same penetration to six digits, same contact count).
	 *
	 * Composed ONCE here, for every spawner at the same time, because they all
	 * take `const JceTransform *` -- so this cannot be a fix that reaches the
	 * rigid-body path and silently misses vehicles, soft bodies, terrain,
	 * compounds, meshes, characters, 2D bodies and tilemap colliders.
	 *
	 * A ROOT ENTITY IS UNCHANGED DOWN TO THE BYTE: jce_scene_get_world_pose
	 * returns a parentless entity's Transform verbatim rather than routing it
	 * through a matrix, so every scene authored before this spawns exactly
	 * where it always did.  The scale comes through too -- a child of a scaled
	 * parent was also getting its collider sized from the local scale. */
	JceTransform tf_world = *tf_local;
	jce_scene_get_world_pose(scene, e, &tf_world.position, &tf_world.rotation,
	                         &tf_world.scale);
	const JceTransform *tf = &tf_world;

	/* Character controller.  Body, counting and the skipped-controller
	 * warning all live in jce_rt_character.c; true means this entity is a
	 * character and must not also become a rigid body. */
	if (rt_character_try_spawn(rt, scene, e, tf)) goto try_audio;

	/* ── Raycast vehicle chassis (VEHICLE last-mile) ── */
	/* The chassis IS this entity's body; short-circuit the normal rigid-body
	 * spawn so we don't also create a redundant plain body (exactly as terrain
	 * does).  Gated on an enabled JceVehicleComponent -> inert otherwise. */
	if (rt->physics && rt_try_spawn_vehicle(rt, scene, e, tf))
		goto try_audio;

	/* ── Volumetric / pressure soft body (SOFT-BODY last-mile) ── */
	/* The soft body IS this entity's body, simulated by the shared soft world
	 * (not a rigid body); short-circuit the normal rigid-body spawn exactly as
	 * vehicle/terrain do.  Gated on an enabled JceSoftBodyComponent -> inert. */
	if (rt->physics && rt_try_spawn_softbody(rt, scene, e, tf))
		goto try_audio;

	/* ── Terrain heightmap → static triangle-mesh collider (P0) ── */
	if (rt->physics && rt_try_spawn_terrain(rt, scene, e, tf))
		goto try_audio;

	/* ── Compound collider (per-object cooked) takes priority ── */
	if (rt->physics && rt_try_spawn_compound(rt, scene, e, tf)) {
		if (jce_scene_has_mesh_collider(scene, e))
			LOG_WARN(LOG_TAG, "entity %llu has both Compound and Mesh "
			         "colliders; compound wins, mesh collider ignored",
			         (unsigned long long)e);
		goto try_audio;
	}

	/* ── Mesh collider (single cooked hull / triangle mesh) ── */
	if (rt->physics && rt_try_spawn_mesh(rt, scene, e, tf))
		goto try_audio;

	/* ── Rigid body ── */
	/* Builds the body desc + creates + tracks the body (shared with the
	 * per-frame draw-distance pass).  allow_defer=true: a SMALL STATIC
	 * box/sphere/capsule far from the player is recorded for lazy spawn
	 * instead of being created now (big-world spawn cost). */
	if (rt->physics)
		rt_spawn_entity_body(rt, scene, e, /*allow_defer=*/true);

	/* ── 2D rigid body (Box2D, independent world) ── */
	if (rt->physics2d &&
	    jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_RIGIDBODY_2D))
		rt_spawn_body2d(rt, scene, e, tf);

	/* ── Tilemap 2D collider (one static body, greedy-merged boxes) ── */
	if (rt->physics2d &&
	    jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_TILEMAP_COLLIDER_2D) &&
	    jce_scene_has_tilemap(scene, e))
		rt_spawn_tilemap_collider2d(rt, scene, e, tf);

try_audio:
	/* ── Audio source ── */
	{
		/* play_on_awake is the only thing decided here; HOW to start an
		 * authored source lives once, in rt_audio_source_start, so the spawn
		 * walk and jce_runtime_audio_play cannot drift apart on the bus
		 * choice, the attenuation block or the 0-volume rule. */
		JceAudioSourceComponent *as = jce_scene_get_audio_source(scene, e);
		if (as && as->play_on_awake)
			(void)rt_audio_source_start(rt, scene, e);
	}

	/* ── Adaptive music track ── one director per runtime, built from the
	 * first entity authoring a play_on_awake MusicTrack (FEATURE 5.3). */
	if (rt->audio && !rt->music && jce_scene_has_music_track(scene, e))
		rt_spawn_music(rt, scene, e);
}

/* ── Gameplay bridge (second walk: triggers / spawners / weapons) ─────
 *
 * Mirror authored gameplay POD components into their runtime subsystems.
 * Runs after the physics/audio walk so it can rely on transforms being
 * present.  Each subsystem is created lazily on first matching component
 * so a scene with none of them allocates nothing. */
static void rt_spawn_gameplay(JceScene *scene, JceEntity e, void *ud)
{
	JceRuntime *rt = (JceRuntime *)ud;
	if (!jce_scene_has_transform(scene, e)) return;

	/* ── Trigger volume ── */
	JceTriggerVolumeComponent *tvc = jce_scene_get_trigger_volume(scene, e);
	if (tvc && jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_TRIGGER_VOLUME)) {
		if (!rt->trigger_world) {
			rt->trigger_world = jce_trigger_world_create();
			if (rt->trigger_world)
				jce_trigger_world_set_event_fn(rt->trigger_world,
				                               rt_trigger_event, rt);
		}
		if (rt->trigger_world) {
			jce_vec3 wp = rt_world_position(scene, e);
			JceTriggerDesc d;
			memset(&d, 0, sizeof d);
			d.shape = (tvc->shape == JCE_TRIGGER_VOL_SPHERE) ? JCE_TRIGGER_SPHERE
			        : (tvc->shape == JCE_TRIGGER_VOL_OBB)    ? JCE_TRIGGER_OBB
			                                                 : JCE_TRIGGER_AABB;
			d.center = jce_v3(wp.x + tvc->center[0],
			                  wp.y + tvc->center[1],
			                  wp.z + tvc->center[2]);
			d.half_extents = jce_v3(tvc->half_extents[0],
			                        tvc->half_extents[1],
			                        tvc->half_extents[2]);
			d.axis_x = jce_v3(tvc->axis_x[0], tvc->axis_x[1], tvc->axis_x[2]);
			d.axis_y = jce_v3(tvc->axis_y[0], tvc->axis_y[1], tvc->axis_y[2]);
			d.axis_z = jce_v3(tvc->axis_z[0], tvc->axis_z[1], tvc->axis_z[2]);
			d.fire_stay = tvc->fire_stay;
			snprintf(d.tag, sizeof d.tag, "%s", tvc->tag);
			JceTriggerHandle h = jce_trigger_add(rt->trigger_world, &d,
			                                     (uint64_t)e);
			jce_trigger_set_enabled(rt->trigger_world, h, tvc->enabled);
			if (jce_trigger_valid(h) &&
			    (rt->trigger_count < rt->trigger_cap || rt_grow_triggers(rt))) {
				rt->triggers[rt->trigger_count].entity = e;
				rt->triggers[rt->trigger_count].handle = h;
				rt->trigger_count++;
			}
		}
	}

	/* ── Spawn manager ── */
	JceSpawnManagerComponent *smc = jce_scene_get_spawn_manager(scene, e);
	if (smc && smc->enabled) {
		if (rt->spawn_count < rt->spawn_cap || rt_grow_spawns(rt)) {
			SpawnEntry *se = &rt->spawns[rt->spawn_count];
			se->entity     = e;
			JceSpawnManagerDesc sd;
			memset(&sd, 0, sizeof sd);
			sd.max_peds         = (uint32_t)(smc->max_peds > 0 ? smc->max_peds : 0);
			sd.max_vehicles     = (uint32_t)(smc->max_vehicles > 0 ? smc->max_vehicles : 0);
			sd.min_spawn_radius = smc->min_spawn_radius;
			sd.max_spawn_radius = smc->max_spawn_radius > 0.0f ? smc->max_spawn_radius : 50.0f;
			sd.despawn_pad      = smc->despawn_pad;
			sd.spawn_interval   = smc->spawn_interval > 0.0f ? smc->spawn_interval : 1.0f;
			sd.road_network     = NULL;   /* vehicles need a road graph — skipped */
			sd.on_create        = rt_spawn_create;
			sd.on_destroy       = rt_spawn_destroy;
			sd.ped_sampler      = rt_spawn_ped_sample;  /* ring sampler (no navmesh needed) */
			sd.user             = rt;     /* callbacks resolve prefab via rt->cur_spawn_mgr */
			sd.rng_seed         = smc->rng_seed;
			if (smc->ped_archetype_count > 0) {
				sd.ped_archetypes      = smc->ped_archetypes;
				sd.ped_archetype_count = (uint32_t)smc->ped_archetype_count;
			}
			if (smc->vehicle_archetype_count > 0) {
				sd.vehicle_archetypes      = smc->vehicle_archetypes;
				sd.vehicle_archetype_count = (uint32_t)smc->vehicle_archetype_count;
			}
			se->mgr = jce_spawn_manager_create(&sd);
			if (se->mgr) rt->spawn_count++;
		}
	}

	/* ── Weapon ── */
	JceWeaponComponent *wc = jce_scene_get_weapon(scene, e);
	if (wc) {
		if (rt->weapon_count < rt->weapon_cap || rt_grow_weapons(rt)) {
			WeaponEntry *we = &rt->weapons[rt->weapon_count];
			we->entity = e;
			we->rng    = (uint64_t)e * 0x9E3779B97F4A7C15ull + 1u;
			JceWeaponArchetype *a = &we->arch;
			memset(a, 0, sizeof *a);
			a->name             = wc->name[0] ? wc->name : "weapon";
			a->kind             = (wc->kind == JCE_WEAPON_COMP_PROJECTILE)
			                      ? JCE_WEAPON_KIND_PROJECTILE
			                      : JCE_WEAPON_KIND_HITSCAN;
			a->damage           = wc->damage;
			a->range            = wc->range;
			a->rpm              = wc->rpm > 0.0f ? wc->rpm : 600.0f;
			a->clip_size        = wc->clip_size > 0 ? wc->clip_size : 1;
			a->reserve_max      = wc->reserve_max;
			a->reload_seconds   = wc->reload_seconds;
			a->spread_deg       = wc->spread_deg;
			a->recoil_per_shot  = wc->recoil_per_shot;
			a->recoil_recovery  = wc->recoil_recovery;
			a->pellets          = wc->pellets > 0 ? wc->pellets : 1;
			a->projectile_speed = wc->projectile_speed;
			a->full_auto        = wc->full_auto;
			jce_weapon_instance_init(&we->inst, a);
			rt->weapon_count++;
		}
	}

	/* ── Save point (P2-save-snapshot) ──
	 * Mirror an authored SavePoint as a sphere trigger in the SAME trigger
	 * world rt_tick_gameplay already drives, so the player observer's overlap
	 * (rt_trigger_event ENTER) writes a snapshot.  Reuses the lazy trigger-
	 * world creation from the trigger-volume block above. */
	JceSavePointComponent *spc = jce_scene_get_save_point(scene, e);
	if (spc) {
		if (!rt->trigger_world) {
			rt->trigger_world = jce_trigger_world_create();
			if (rt->trigger_world)
				jce_trigger_world_set_event_fn(rt->trigger_world,
				                               rt_trigger_event, rt);
		}
		if (rt->trigger_world &&
		    (rt->save_point_count < rt->save_point_cap ||
		     rt_grow_save_points(rt))) {
			jce_vec3 wp = rt_world_position(scene, e);
			float r = spc->radius > 0.0f ? spc->radius : 1.5f;
			JceTriggerDesc d;
			memset(&d, 0, sizeof d);
			d.shape          = JCE_TRIGGER_SPHERE;
			d.center         = wp;
			d.half_extents.x = r;   /* sphere: half_extents.x = radius */
			JceTriggerHandle h = jce_trigger_add(rt->trigger_world, &d,
			                                     (uint64_t)e);
			if (jce_trigger_valid(h)) {
				SavePointEntry *sp = &rt->save_points[rt->save_point_count];
				sp->entity   = e;
				sp->handle   = h;
				sp->one_shot = spc->one_shot;
				sp->require_interact = spc->require_interact;
				sp->slot             = spc->slot;
				sp->kind             = spc->kind;
				snprintf(sp->display_name, sizeof sp->display_name, "%s",
				         spc->display_name);
				sp->fired    = false;
				size_t idn = strlen(spc->save_id);
				if (idn >= sizeof sp->save_id) idn = sizeof sp->save_id - 1;
				memcpy(sp->save_id, spc->save_id, idn);
				sp->save_id[idn] = '\0';
				rt->save_point_count++;
			}
		}
	}

	/* ── Behavior tree (P2-perception-bt-binding) ──
	 * Load the authored tree file into the runtime BT context and record a
	 * BtEntry with a fresh per-agent blackboard.  Perception writes stimuli
	 * into that blackboard each frame and rt_tick_gameplay ticks the tree.
	 * Tree load is host-filesystem (jce_bt_load_tree_file); a pak-resident
	 * tree would need the buffer path — noted as a follow-up. */
	JceBehaviorTree *btc = jce_scene_get_behavior_tree(scene, e);
	if (btc && btc->active && btc->tree_path[0] && rt->bt_ctx &&
	    jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_BEHAVIOR_TREE)) {
		rt_bt_register_default_actions(rt);
		JceBtTreeHandle h = jce_bt_load_tree_file(rt->bt_ctx, btc->tree_path);
		if (jce_bt_tree_valid(h) &&
		    (rt->bt_count < rt->bt_cap || rt_grow_bts(rt))) {
			struct BtEntry *be = &rt->bts[rt->bt_count];
			memset(be, 0, sizeof *be);
			be->entity           = e;
			be->tree             = h;
			be->bb               = jce_blackboard_create();
			/* Per-agent sight/hearing authored on the BehaviorTree component;
			 * a value <=0 falls back to the engine default (also what scenes
			 * authored before these fields existed will have). */
			be->sight_range      = (btc->sight_range      > 0.0f) ? btc->sight_range      : 25.0f;
			be->sight_half_angle = (btc->sight_half_angle > 0.0f) ? btc->sight_half_angle : 1.0472f; /* 60° */
			be->hearing_range    = (btc->hearing_range    > 0.0f) ? btc->hearing_range    : 15.0f;
			be->tick_period      = (btc->tick_hz > 0.0f) ? (1.0f / btc->tick_hz) : 0.0f;
			be->tick_accum       = 0.0f;
			be->simlod_accum     = 0.0f;   /* sim-LOD cadence state */
			be->simlod_prev_tier = -1;     /* -1 = nominal first classification */
			be->active           = true;
			/* Mirror the assigned handle back onto the component so the
			 * inspector / save reflects the loaded tree. */
			btc->tree_handle_idx = h.idx;
			rt->bt_count++;
		} else if (!jce_bt_tree_valid(h)) {
			LOG_WARN(LOG_TAG, "behavior tree: failed to load '%s' for entity %llu",
			         btc->tree_path, (unsigned long long)e);
		}
	}

	/* ── Gameplay script (Phase 0 keystone) ──
	 * Instantiate the authored JceScriptComponent into the runtime VM and call
	 * on_start.  Gated on the component being enabled (same pattern as BT). */
	JceScriptComponent *sc = jce_scene_get_script(scene, e);
	if (sc && sc->script_path[0] && rt->script_enabled &&
	    jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_SCRIPT)) {
		/* The language comes from the PATH: bob.lua and turret.py in one
		 * scene each land in their own VM, and the ref carries the handle
		 * that issued the instance so the two can never drift apart. */
		RtScriptRef ref = rt_script_instantiate(rt, sc->script_path, e);
		if (ref.inst &&
		    (rt->script_count < rt->script_cap || rt_grow_scripts(rt))) {
			struct ScriptEntry *se = &rt->scripts[rt->script_count++];
			se->entity = e;
			se->ref    = ref;
			se->active = true;
			se->simlod_accum     = 0.0f;   /* sim-LOD cadence state */
			se->simlod_prev_tier = -1;
			snprintf(se->script_path, sizeof se->script_path, "%s",
			         sc->script_path);
			/* Watch the source for hot-reload (no-op in shipped: PAK-resident
			 * scripts have no host file, so add() fails silently). */
			if (rt->script_watcher)
				jce_file_watcher_add(rt->script_watcher, sc->script_path,
				                     rt_on_script_changed, rt);
			rt_script_ref_start(ref);
			LOG_INFO(LOG_TAG, "script: loaded '%s' (%s) for entity %llu",
			         sc->script_path,
			         jce_script_vm_language_of(ref.vm),
			         (unsigned long long)e);
		} else if (ref.inst) {
			rt_script_ref_release(ref);   /* grow failed */
		}
	}

	/* ── Gameplay Ability System (GAS consumption last-mile) ──
	 * Init a live JceGameplayAbilitySystem from the entity's authored
	 * attribute + ability tables and record a GasEntry.  Presence-gated like
	 * MorphWeights/NetworkVariable: no component -> this block is skipped and
	 * the gameplay path is byte-identical to before.  rt_tick_gameplay ticks
	 * each system; scripts/game act on it via jce_runtime_entity_gas. */
	JceGameplayAbilitySystemComponent *gc = jce_scene_get_gas(scene, e);
	if (gc && (gc->attribute_count > 0 || gc->ability_count > 0)) {
		if (rt->gas_count < rt->gas_cap || rt_grow_gas(rt)) {
			struct GasEntry *ge = &rt->gas_entries[rt->gas_count];
			ge->entity = e;
			JceGameplayAbilitySystem *gas = &ge->gas;
			jce_gas_init(gas);

			int na = gc->attribute_count;
			if (na > JCE_GAS_AUTHOR_MAX_ATTRIBUTES) na = JCE_GAS_AUTHOR_MAX_ATTRIBUTES;
			for (int i = 0; i < na; ++i) {
				const JceGasAttributeAuthor *a = &gc->attributes[i];
				jce_attribute_set_add(&gas->attributes, a->name,
				                      a->base, a->min, a->max);
			}

			int nb = gc->ability_count;
			if (nb > JCE_GAS_AUTHOR_MAX_ABILITIES) nb = JCE_GAS_AUTHOR_MAX_ABILITIES;
			for (int i = 0; i < nb; ++i) {
				const JceGasAbilityAuthor *b = &gc->abilities[i];
				JceAbilityDef def;
				memset(&def, 0, sizeof def);
				snprintf(def.name, sizeof def.name, "%s", b->name);
				def.id               = b->id;
				def.cost_attr_idx    = b->cost_attr_idx;
				def.cost_magnitude   = b->cost_magnitude;
				def.cooldown_seconds = b->cooldown_seconds;
				def.granted_count    = 0;   /* authoring grants no effects yet */
				jce_gas_register_ability(gas, &def);
			}
			rt->gas_count++;
			LOG_INFO(LOG_TAG,
			         "gas: live system for entity %llu (%d attrs, %d abilities)",
			         (unsigned long long)e, na, nb);
		}
	}

	/* ── Ragdoll (skeleton-driven physics, scene-pass last-mile) ──
	 * Presence-gated like GAS/MorphWeights: build a live JceRagdoll in
	 * rt->physics from the entity's authored JceRagdoll component + its
	 * SkeletalAnimator skeleton.  No component (or disabled / no skeleton)
	 * -> skipped, byte-identical.  On any load/create failure we free what we
	 * allocated and skip gracefully.  The owned JceModel keeps the borrowed
	 * JceSkeleton alive for the ragdoll's whole lifetime. */
	JceRagdollComponent *rc = jce_scene_get_ragdoll(scene, e);
	{ int rc_cid = jce_component_find("Ragdoll");   /* per-component disable (≠ rc->enable) */
	  if (rc && rc_cid >= 0 && !jce_scene_comp_enabled(scene, e, rc_cid)) rc = NULL; }
	if (rc && rc->enable && rt->physics &&
	    jce_scene_has_skeletal_animator(scene, e)) {
		JceSkeletalAnimatorComponent *sa =
			jce_scene_get_skeletal_animator(scene, e);
		if (sa && sa->skeleton_path[0]) {
			JceModel *model = jce_model_load_gltf(rt->pak, sa->skeleton_path);
			JceSkeleton *skel = model ? jce_model_get_skeleton(model) : NULL;
			if (skel) {
				float radius = (rc->radius       > 0.0f) ? rc->radius       : 0.08f;
				float hscale = (rc->height_scale > 0.0f) ? rc->height_scale : 1.0f;
				float bw = rc->blend_weight;
				if (bw < 0.0f) bw = 0.0f;
				if (bw > 1.0f) bw = 1.0f;
				/* 0 => the engine default of 1.0, the same convention
				 * radius and height_scale above use.  A NEGATIVE value is
				 * the explicit opt-out that restores the unlimited joints
				 * this used to build unconditionally. */
				float lscale = rc->joint_limit_scale;
				if (lscale == 0.0f) lscale = 1.0f;
				JceRagdoll *rd = jce_ragdoll_create(skel, rt->physics,
				                                    radius, hscale, lscale);
				if (rd &&
				    (rt->ragdoll_count < rt->ragdoll_cap || rt_grow_ragdoll(rt))) {
					jce_ragdoll_set_blend_weight(rd, bw);
					struct RagdollEntry *re =
						&rt->ragdoll_entries[rt->ragdoll_count];
					re->entity       = e;
					re->model        = model;
					re->rd           = rd;
					re->blend_weight = bw;
					rt->ragdoll_count++;
					LOG_INFO(LOG_TAG,
					         "ragdoll: live ragdoll for entity %llu (%s, %u joints)",
					         (unsigned long long)e, sa->skeleton_path,
					         jce_skeleton_joint_count(skel));
				} else {
					/* create failed, or grow failed after create -> tear down
					 * the body chain before the model so no handles leak. */
					if (rd) jce_ragdoll_destroy(rd);
					jce_model_destroy(model);
				}
			} else {
				/* model loaded but has no skeleton, or load failed. */
				jce_model_destroy(model);
			}
		}
	}

	/* ── Nav agent (P1-navmesh-chain) ──
	 * Mirror an authored NavAgent into the runtime agent set (stood up by
	 * rt_init_navmesh BEFORE this walk).  The component stays the authored
	 * source of truth; the entry caches the engine handle plus the last goal
	 * issued so rt_tick_gameplay can honour live edits and auto_repath. */
	JceNavAgentComponent *nac = jce_scene_get_nav_agent(scene, e);
	{ int na_cid = jce_component_find("NavAgent");   /* per-component disable (≠ nac->enabled) */
	  if (nac && na_cid >= 0 && !jce_scene_comp_enabled(scene, e, na_cid)) nac = NULL; }
	if (nac && nac->enabled && rt->nav_agents) {
		if (rt->nav_entry_count < rt->nav_entry_cap || rt_grow_nav_entries(rt)) {
			jce_vec3 wp = rt_world_position(scene, e);
			JceNavAgentDesc d = {
				.pos_x           = wp.x,
				.pos_z           = wp.z,
				.radius          = nac->radius          > 0.0f ? nac->radius          : 0.4f,
				.max_speed       = nac->max_speed       > 0.0f ? nac->max_speed       : 3.0f,
				.max_accel       = nac->max_accel       > 0.0f ? nac->max_accel       : 12.0f,
				.arrive_radius   = nac->arrive_radius   > 0.0f ? nac->arrive_radius   : 1.5f,
				.waypoint_radius = nac->waypoint_radius > 0.0f ? nac->waypoint_radius : 0.5f,
				.height          = nac->height,
			};
			/* A navmesh belongs to the agent it was carved for; an agent
			 * taller than the clearance is on the WRONG MESH, not slightly
			 * wrong on this one.  Says so once (jce_nav_agent.h). */
			(void)jce_nav_agent_report_fit(
				nac->height, jce_recast_agent_height(rt->nav_recast));
			JceNavAgentHandle h = jce_nav_agent_add(rt->nav_agents, &d);
			if (jce_nav_agent_valid(h)) {
				NavAgentEntry *ne = &rt->nav_entries[rt->nav_entry_count];
				ne->entity = e;
				ne->handle = h;
				ne->simlod_accum     = 0.0f;   /* sim-LOD cadence state */
				ne->simlod_prev_tier = -1;
				/* Initial destination: a referenced target entity's world
				 * position when set, otherwise the authored target point. */
				float gx, gz;
				if (nac->target_entity != 0) {
					jce_vec3 tp = rt_world_position(scene,
					                                (JceEntity)nac->target_entity);
					gx = tp.x; gz = tp.z;
				} else {
					gx = nac->target[0]; gz = nac->target[2];
				}
				ne->has_dest    = jce_nav_agent_set_destination(rt->nav_agents,
				                                                h, gx, gz);
				ne->last_goal_x = gx;
				ne->last_goal_z = gz;
				rt->nav_entry_count++;
			}
		}
	}
}

/* ── Networking bridge (P1-networking-full) ──────────────────────────
 *
 * Walk authored JceNetworkObject entities and wire them into the net
 * runtime: adopt the existing entity into the replication NetObject table
 * (server only — clients learn objects from the spawn stream), then, if a
 * JceNetTransform override is present, register the object's transform
 * with the snapshot-interp module using the authored sync rate / interp /
 * tolerance / authority.  Runs only when a session is live; a no-op for
 * single-player / session-less play. */
static void rt_spawn_net(JceScene *scene, JceEntity e, void *ud)
{
	JceRuntime *rt = (JceRuntime *)ud;
	if (!jce_scene_has_transform(scene, e)) return;

	JceNetworkObjectComponent *no = jce_scene_get_network_object(scene, e);
	if (!no) return;
	if (!jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_NETWORK_OBJECT)) return;

	/* Server adopts the authored entity → assigns a net id + queues a
	 * spawn broadcast.  Clients do not adopt: they receive the spawn from
	 * the server and bind their own local entity. */
	JceNetObjectId id = JCE_NET_OBJECT_INVALID;
	if (jce_session_is_server()) {
		JceClientId owner = (JceClientId)no->owner;
		id = jce_net_object_adopt((uint64_t)e, owner, no->flags, NULL);
		if (id != JCE_NET_OBJECT_INVALID) {
			/* Mirror the assigned id back onto the authored component so
			 * gameplay systems can read net_id without a net round-trip. */
			no->net_id   = id;
			no->is_owner = (owner == jce_net_local_client_id());
			rt->net_obj_count++;
		}
	}

	/* Transform replication config from whichever component authored it.
	 *
	 * NetTransform and NetRigidbody carry the SAME four knobs -- sync_rate_hz,
	 * interp_ms, tolerance, authority_mode -- and only NetTransform's were
	 * read.  There is no rigid-body replication module, and inventing one to
	 * tick a box is not implementing it; but a networked rigid body's
	 * observable state IS its transform, which is what Unity's
	 * NetworkRigidbody replicates too.
	 *
	 * NetTransform wins when both are present: jce_net_transform_register is
	 * idempotent, so registering twice would silently overwrite the config a
	 * designer put on the more specific component. */
	if (id != JCE_NET_OBJECT_INVALID) {
		JceNetTransformComponent *nt = jce_scene_get_net_transform(scene, e);
		JceNetRigidbodyComponent *nrb =
			nt ? NULL : jce_scene_get_net_rigidbody(scene, e);
		uint8_t  hz   = nt ? nt->sync_rate_hz   : (nrb ? nrb->sync_rate_hz   : 0);
		uint16_t ims  = nt ? nt->interp_ms      : (nrb ? nrb->interp_ms      : 0);
		float    tol  = nt ? nt->tolerance      : (nrb ? nrb->tolerance      : 0.0f);
		uint8_t  auth = nt ? nt->authority_mode : (nrb ? nrb->authority_mode : 0);
		if (nt || nrb) {
			JceNetTransformConfig cfg;
			jce_net_transform_get_default_config(&cfg);
			if (hz)  cfg.snapshot_hz      = (uint32_t)hz;
			if (ims) cfg.interp_delay_ms  = (uint32_t)ims;
			if (tol > 0.0f) cfg.divergence_snap_distance = tol;
			cfg.authority = (auth == 1) ? JCE_NET_AUTH_OWNER
			                            : JCE_NET_AUTH_SERVER;
			jce_net_transform_register(id, &cfg);
		}
	}

	/* Animator replication config, from the component that authored it.
	 *
	 * JceNetAnimatorComponent's three knobs -- sync_rate_hz, interp_ms,
	 * authority_mode -- had NO reader at all: they round-tripped through the
	 * scene file and the Inspector showed them, and nothing anywhere consulted
	 * one.  Registering here is what makes them mean something, and it is the
	 * same shape the transform block above uses so the two cannot drift. */
	if (id != JCE_NET_OBJECT_INVALID) {
		JceNetAnimatorComponent *na = jce_scene_get_net_animator(scene, e);
		if (na) {
			JceNetAnimatorConfig acfg;
			jce_net_animator_get_default_config(&acfg);
			if (na->sync_rate_hz) acfg.snapshot_hz     = na->sync_rate_hz;
			if (na->interp_ms)    acfg.interp_delay_ms = na->interp_ms;
			acfg.authority = (na->authority_mode == 1)
			               ? JCE_NET_ANIM_AUTHORITY_OWNER
			               : JCE_NET_ANIM_AUTHORITY_SERVER;
			jce_net_animator_register(id, &acfg);
			rt->net_animator_count++;
		}
	}

	/* ── Network variable (FEATURE 7.2 authoring last-mile) ──
	 * Register the authored typed NetworkVariable for replication: writing
	 * the initial value through the PUBLIC typed-set API attaches the
	 * matching JceNetVarF32/I32 backing component onto this entity, so it
	 * travels the snapshot substrate exactly like a code-registered NetVar.
	 * The typed setters self-gate on jce_net_object_has_authority() — the
	 * server (which is the only seat that adopts here) always has authority,
	 * so the initial value is seeded on the authority and replicated out.
	 * No JceNetworkVariable component -> this block is skipped and the net
	 * path is byte-identical to today.  Bool authors over the i32 NetVar
	 * (wire 0/1) since the substrate exposes only f32/i32 scalar types. */
	JceNetworkVariableComponent *nv = jce_scene_get_network_variable(scene, e);
	{ int nv_cid = jce_component_find("NetworkVariable");
	  if (nv && nv_cid >= 0 && !jce_scene_comp_enabled(scene, e, nv_cid)) nv = NULL; }
	if (nv && id != JCE_NET_OBJECT_INVALID) {
		if (nv->var_type == JCE_NETVAR_AUTHOR_TYPE_F32) {
			jce_net_var_f32_set((uint64_t)e, nv->initial_value);
		} else if (nv->var_type == JCE_NETVAR_AUTHOR_TYPE_BOOL) {
			jce_net_var_i32_set((uint64_t)e,
			                    (nv->initial_value != 0.0f) ? 1 : 0);
		} else { /* JCE_NETVAR_AUTHOR_TYPE_I32 */
			jce_net_var_i32_set((uint64_t)e, (int32_t)(nv->initial_value));
		}
		LOG_INFO(LOG_TAG,
		         "net var: registered '%s' (type=%u auth=%u) on obj %u",
		         nv->var_name[0] ? nv->var_name : "(unnamed)",
		         (unsigned)nv->var_type, (unsigned)nv->authority,
		         (unsigned)id);
	}
}

/* ── Client->server input command channel (F12 slice) ────────────────
 *
 * The upstream half of authoritative networked movement.  A client uploads
 * its sampled JceRuntimeInput each fixed tick as a ServerRpc carrying a
 * fixed-layout JceInputCommand payload; the server's handler decodes it and
 * stores the latest-per-client command (jce_input_command_*).  The store +
 * codec are PURE (jce_net_input_command.c); only the RPC wiring lives here.
 *
 * RPC id is interned once at net-bridge init.  The handler is registered
 * unconditionally (harmless on a client — a ServerRpc is server_authoritative
 * so the receive-side gate in jce_rpc.c never runs it off the server). */
#define RT_INPUT_CMD_RPC_NAME  "jce.input_cmd"

/* Server-side RPC handler: decode the uploaded input command and store it
 * keyed by the originating client id.  `sender` is the JceClientId the RPC
 * dispatcher resolved for the inbound packet (jce_rpc.h contract). */
static void rt_input_cmd_rpc_handler(JceNetObjectId net_id,
                                     JceClientId    sender,
                                     const void    *payload,
                                     uint32_t       payload_size,
                                     void          *user)
{
	(void)net_id;
	(void)user;
	/* Reuse the SAME decode+store seam the headless test exercises. */
	jce_input_command_server_receive((uint32_t)sender, payload, payload_size);
}

/* Register the upstream input-command RPC once.  jce_rpc_register updates in
 * place when re-called with the same name, so this is idempotent and does not
 * disturb any other registered RPC. */
static void rt_register_input_cmd_rpc(void)
{
	JceRpcDesc desc;
	memset(&desc, 0, sizeof desc);
	desc.name                 = RT_INPUT_CMD_RPC_NAME;
	/* Reliable-ordered: simplest correct delivery for the slice.  An
	 * unreliable-sequenced variant is a documented perf follow-up. */
	desc.reliability          = JCE_RPC_RELIABLE;
	desc.server_authoritative = true;   /* client -> server only */
	desc.handler              = rt_input_cmd_rpc_handler;
	desc.user                 = NULL;
	if (!jce_rpc_register(&desc)) {
		/* Client prediction sends every input command through this RPC.
		 * Without it the server receives nothing and the client rubber-bands
		 * against a server that never saw an input — which looks like a
		 * network problem, not a failed registration at startup. */
		LOG_ERROR(LOG_TAG,
		          "input-command RPC '%s' failed to register — client input "
		          "will NOT reach the server", RT_INPUT_CMD_RPC_NAME);
	}
}

/* ── Scripted RPC channel (jce.rpc_send) ──────────────────────────────
 *
 * A single generic RPC carries every script-issued RPC.  The wire payload is
 * [u16 event_len][event bytes][payload bytes]; the receiver unpacks it, resolves
 * the net object back to its entity, and dispatches `event` AS A METHOD on that
 * entity's live script instance (inst:event(0, payload)) — exactly mirroring
 * jce.send_message, but delivered over the replication transport.  Pure no-ops
 * without a session / NetworkObject / live instance. */
#define RT_SCRIPT_RPC_NAME  "jce.script_rpc"

/* Is `name` a method a REMOTE peer is allowed to invoke on a script?
 *
 * The dispatch below takes this string straight off the wire and hands it to
 * jce_script_call_message, which does a plain lua_getfield on the instance —
 * so without a filter a peer can call ANY method the script (or its
 * metatable) exposes: on_update, on_collision, on_destroy, every private
 * helper.  The channel is registered server_authoritative = false, meaning it
 * flows both ways, so that reach belongs to any client as well as the server.
 *
 * The rule is EXPLICIT OPT-IN BY NAME.  Only methods a script author
 * deliberately named "rpc_..." are reachable; everything else is refused.
 * Every mainstream engine requires per-method opt-in for exactly this reason
 * — Unity [Command]/[ClientRpc], Unreal UFUNCTION(Server), Photon [PunRPC] —
 * and none of them make the whole object surface remotely callable.
 *
 * A prefix (rather than a declared table) is used because it needs no script
 * VM cooperation and is legible at the definition site: reading a script, the
 * network entry points are the functions whose names say so.  If per-method
 * direction or authority is ever needed, a declared table is the natural
 * upgrade; the check stays in this one function either way.
 *
 * The charset check is hygiene, not injection defence — lua_getfield treats
 * any string as an opaque key — but it keeps a malformed or padded name from
 * reaching the VM and keeps log output readable. */
#define RT_SCRIPT_RPC_PREFIX     "rpc_"
#define RT_SCRIPT_RPC_PREFIX_LEN 4u

bool rt_script_rpc_name_allowed(const char *name)
{
	if (!name || !name[0]) return false;
	if (strncmp(name, RT_SCRIPT_RPC_PREFIX, RT_SCRIPT_RPC_PREFIX_LEN) != 0)
		return false;
	/* Must have something AFTER the prefix. */
	if (!name[RT_SCRIPT_RPC_PREFIX_LEN]) return false;
	for (const char *c = name; *c; ++c) {
		const bool ok = (*c >= 'a' && *c <= 'z') ||
		                (*c >= 'A' && *c <= 'Z') ||
		                (*c >= '0' && *c <= '9') || *c == '_';
		if (!ok) return false;
	}
	return true;
}

static void rt_script_rpc_handler(JceNetObjectId net_id, JceClientId sender,
                                  const void *payload, uint32_t payload_size,
                                  void *user)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !payload || payload_size < 2u) return;

	const uint8_t *p = (const uint8_t *)payload;
	uint16_t elen = (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
	if ((uint32_t)2u + elen > payload_size) return;          /* malformed */

	char event[256];
	uint16_t ec = elen < 255u ? elen : 255u;
	memcpy(event, p + 2, ec);
	event[ec] = '\0';
	if (!rt_script_rpc_name_allowed(event)) {
		/* Attacker-controlled string; log it as data, never dispatch it. */
		LOG_WARN(LOG_TAG,
		         "script RPC from client %u refused: '%s' is not an "
		         "rpc_-prefixed method (see rt_script_rpc_name_allowed)",
		         (unsigned)sender, event);
		return;
	}

	uint32_t poff = 2u + (uint32_t)elen;
	char pl[512];
	uint32_t pc = payload_size - poff;
	if (pc > 511u) pc = 511u;
	memcpy(pl, p + poff, pc);
	pl[pc] = '\0';

	uint64_t ent = jce_net_object_to_entity(net_id);
	for (int i = 0; i < rt->script_count; ++i) {
		struct ScriptEntry *se = &rt->scripts[i];
		if (se->active && (uint64_t)se->entity == ent) {
			rt_script_ref_message(se->ref, event, 0.0,
			                      pc > 0u ? pl : NULL);
			return;   /* one instance per entity */
		}
	}
}

/* Host cb backing jce.rpc_send: pack (event, payload) + ship on the channel. */
bool rt_script_rpc_send(void *user, JceScriptEntity e, const char *event,
                              int target, const char *payload)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !event || !event[0]) return false;
	/* Refuse at the SENDER too.  The receiver would drop it anyway, but a
	 * silent no-op on a remote machine is the worst possible way for an author
	 * to learn their method is not exposed. */
	if (!rt_script_rpc_name_allowed(event)) {
		LOG_WARN(LOG_TAG,
		         "jce.rpc_send('%s') refused: remote-callable script methods "
		         "must be named rpc_<something>", event);
		return false;
	}
	if (jce_session_mode() == JCE_SESSION_MODE_NONE) return false;
	JceNetObjectId nid = jce_net_object_from_entity((uint64_t)e);
	if (nid == JCE_NET_OBJECT_INVALID) return false;

	uint16_t elen = (uint16_t)strlen(event);
	uint32_t plen = payload ? (uint32_t)strlen(payload) : 0u;
	if (elen > 250u) elen = 250u;
	if (plen > 700u) plen = 700u;

	uint8_t buf[1024];
	uint32_t off = 0u;
	buf[off++] = (uint8_t)(elen & 0xFFu);
	buf[off++] = (uint8_t)((elen >> 8) & 0xFFu);
	memcpy(buf + off, event, elen); off += elen;
	if (plen) { memcpy(buf + off, payload, plen); off += plen; }

	JceRpcTarget tgt = (target >= 0 && target <= 4)
	                       ? (JceRpcTarget)target : JCE_RPC_TO_SERVER;
	return jce_rpc_send(nid, RT_SCRIPT_RPC_NAME, tgt, 0, buf, off);
}

static void rt_register_script_rpc(JceRuntime *rt)
{
	JceRpcDesc desc;
	memset(&desc, 0, sizeof desc);
	desc.name                 = RT_SCRIPT_RPC_NAME;
	desc.reliability          = JCE_RPC_RELIABLE;
	desc.server_authoritative = false;   /* script RPCs flow either direction */
	desc.handler              = rt_script_rpc_handler;
	desc.user                 = rt;
	jce_rpc_register(&desc);
}

/* Bring the networking bridge up at create() time when a session exists.
 * Binds the ECS world + scene into the net subsystems and walks net
 * objects.  Idempotent: no-op without a live session. */
/* Entity lifecycle adapter for the replication substrate (audit C5-03).
 *
 * jce_net is L4 and links only jce_core, so it cannot call jce_scene_* —
 * left to itself it falls back to raw ecs_new/ecs_delete on the bound world.
 * That skips the scene roster epoch (caches keep a stale entity list across
 * a network spawn), the default JceTransform, and JceTagActive — the last of
 * which makes a replicated entity invisible to every active-filtered system,
 * i.e. present but not simulated.
 *
 * The runtime owns both sides, so it installs the bridge here. */
static uint64_t rt_net_entity_create(void *user)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene) return 0u;
	/* Named so a replicated object is identifiable in the hierarchy rather
	 * than showing up as an unnamed entity nobody can attribute. */
	return (uint64_t)jce_scene_create_entity(rt->scene, "NetObject");
}

static void rt_net_entity_destroy(void *user, uint64_t entity)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->scene || !entity) return;
	jce_scene_destroy_entity(rt->scene, (JceEntity)entity);
}

static void rt_init_net_bridge(JceRuntime *rt)
{
	if (jce_session_mode() == JCE_SESSION_MODE_NONE) return;
	if (!rt->scene) return;

	/* Plumb the flecs world into replication + bind the scene into the
	 * transform module so it can read / write entity transforms. */
	jce_net_replication_set_world(jce_scene_get_world(rt->scene));
	/* Route replicated entity create/destroy through the scene layer so
	 * they get the roster bump, default transform and JceTagActive. */
	jce_net_replication_set_entity_hooks(rt_net_entity_create,
	                                     rt_net_entity_destroy, rt);
	jce_net_transform_set_scene(rt->scene);
	jce_net_animator_set_scene(rt->scene);

	/* FEATURE 7.2 — register the built-in NetworkVariable component types
	 * with the replication substrate so a SHIPPED build carries replicated
	 * components (jce_net_replication_component_count() > 0).  Must run
	 * AFTER set_world() — register_all() creates its backing flecs
	 * components on the bound world. */
	if (!jce_net_var_register_all()) {
		/* Every replicated float/int stops moving, AND the wire component
		 * ids are interned in this call, so a peer that skipped it cannot
		 * agree with one that did not. */
		LOG_ERROR(LOG_TAG, "NetworkVariable components failed to register — "
		                   "replicated variables will NOT sync");
	}

	/* GAS attribute replication — register the packed JceGasAttribRepl
	 * replica component AFTER jce_net_var_register_all() so the substrate's
	 * u16 component interning order (f32, i32, then gas-replica) is identical
	 * on every peer and the NetworkVariable registration is undisturbed.
	 * Same bound world as set_world() above. */
	if (!jce_gas_replication_register(jce_scene_get_world(rt->scene))) {
		/* Attributes stop replicating entirely.  On a client that reads as
		 * "my health never changes" — a gameplay bug hunt, not a startup
		 * one, unless it is said out loud here. */
		LOG_ERROR(LOG_TAG, "GAS attribute replication failed to register — "
		                   "attributes will NOT replicate");
	}

	/* Upstream client->server input command channel (F12 slice): intern the
	 * RPC + its server handler once.  Registered on both roles (harmless on a
	 * client: a server_authoritative RPC is gated to the server on receive).
	 * jce_rpc_init() is idempotent and piggy-backs on the live replication
	 * subsystem already brought up by the session. */
	jce_rpc_init();
	rt_register_input_cmd_rpc();
	rt_register_script_rpc(rt);   /* jce.rpc_send channel */

	jce_scene_each_entity(rt->scene, rt_spawn_net, rt);
	rt->net_bridged = true;

	jce_log_write(JCE_LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__,
	              "runtime: net bridge -> role=%s objects=%d xforms=%u interest=%.1fm",
	              jce_session_is_server() ? "server" : "client",
	              rt->net_obj_count,
	              jce_net_transform_registered_count(),
	              (double)jce_net_replication_get_interest_radius());
}

/* ── Joints / constraints (second pass — needs both bodies spawned) ── */

JceBodyHandle rt_body_for_entity(const JceRuntime *rt, JceEntity e)
{
	RT_BODY_FOR_ENTITY(rt->bodies, rt->body_count, e);
}

static void rt_spawn_joint(JceScene *scene, JceEntity e, void *ud)
{
	JceRuntime *rt = (JceRuntime *)ud;
	if (!rt->physics) return;

	JceConstraintComponent *cn = jce_scene_get_constraint(scene, e);
	if (!cn) return;
	if (!jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_CONSTRAINT)) return;

	/* The constrained entity itself must have a body. */
	JceBodyHandle a = rt_body_for_entity(rt, e);
	if (!jce_body_valid(a)) return;

	/* target_entity 0 -> anchor to the world; otherwise resolve its body. */
	JceBodyHandle b = JCE_BODY_INVALID;
	if (cn->target_entity != 0) {
		b = rt_body_for_entity(rt, (JceEntity)cn->target_entity);
		if (!jce_body_valid(b)) {
			LOG_WARN(LOG_TAG, "constraint: target entity %u has no body",
			         cn->target_entity);
			return;
		}
	}

	JceConstraintDesc cd;
	memset(&cd, 0, sizeof cd);
	cd.type              = (JceConstraintType)cn->constraint_type;
	cd.body_a            = a;
	cd.body_b            = b;   /* JCE_BODY_INVALID => world anchor */
	cd.pivot_a           = jce_v3(cn->pivot_a[0], cn->pivot_a[1], cn->pivot_a[2]);
	cd.pivot_b           = jce_v3(cn->pivot_b[0], cn->pivot_b[1], cn->pivot_b[2]);
	cd.axis              = jce_v3(cn->axis[0], cn->axis[1], cn->axis[2]);
	cd.lower_limit       = cn->lower_limit;
	cd.upper_limit       = cn->upper_limit;
	cd.disable_collision = cn->disable_collision;
	/* THE LAST MILE FOR THE MOTOR.  Every field above is copied here and
	 * nowhere else; a motor authored on the component and not copied here is
	 * the exact shape of a feature that is complete in five places and inert
	 * in the sixth. */
	cd.use_motor             = cn->use_motor;
	cd.motor_target_velocity = cn->motor_target_velocity;
	cd.motor_max_force       = cn->motor_max_force;

	/* The Bullet world owns the constraint and frees it on destroy -- but
	 * the MOTOR has to remain reachable, so the handle is tracked.  A joint
	 * that fails to register still exists and still works; it just stops
	 * responding to motor edits, which is the same acceptable degradation
	 * the configurable-joint registry documents. */
	JceConstraintHandle h = jce_physics_constraint_create(rt->physics, &cd);
	if (!jce_constraint_valid(h)) return;
	if (rt->joint_count >= rt->joint_cap && !rt_grow_joints(rt)) return;
	JointEntry *je = &rt->joints[rt->joint_count++];
	je->entity          = e;
	je->handle          = h;
	je->motor_on        = cn->use_motor;
	je->motor_target    = cn->motor_target_velocity;
	je->motor_max_force = cn->motor_max_force;
}

/* CONFIGURABLE-JOINT last-mile: spawn a per-axis 6DOF joint for an entity that
 * authored an ENABLED JceConfigurableJointComponent.  Mirrors rt_spawn_joint
 * (presence-gated via JCE_COMP_FLAG_CONFIGURABLE_JOINT; body_a = the entity's
 * own body; body_b = connected_body's body, or the world when 0) and registers
 * a ConfigJointEntry so the break monitor + teardown can reach the handle.
 * Runs in the SAME post-spawn pass as rt_spawn_joint (both need every body to
 * already exist).  Absent / disabled component -> early return -> no entry ->
 * the break monitor stays a gated no-op. */
static void rt_spawn_configurable_joint(JceScene *scene, JceEntity e, void *ud)
{
	JceRuntime *rt = (JceRuntime *)ud;
	if (!rt->physics) return;

	JceConfigurableJointComponent *cj = jce_scene_get_configurable_joint(scene, e);
	if (!cj) return;
	if (!jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_CONFIGURABLE_JOINT))
		return;

	/* The jointed entity itself must have a body. */
	JceBodyHandle a = rt_body_for_entity(rt, e);
	if (!jce_body_valid(a)) return;

	/* connected_body 0 -> anchor to the world; otherwise resolve its body. */
	JceBodyHandle b = JCE_BODY_INVALID;
	if (cj->connected_body != 0) {
		b = rt_body_for_entity(rt, (JceEntity)cj->connected_body);
		if (!jce_body_valid(b)) {
			LOG_WARN(LOG_TAG, "configurable joint: connected body %llu has no body",
			         (unsigned long long)cj->connected_body);
			return;
		}
	}

	JceConfigurableJointDesc jd;
	memset(&jd, 0, sizeof jd);
	jd.body_a            = a;
	jd.body_b            = b;   /* JCE_BODY_INVALID => world anchor */
	jd.anchor_a          = jce_v3(cj->anchor[0], cj->anchor[1], cj->anchor[2]);
	jd.anchor_b          = jce_v3(cj->connected_anchor[0],
	                              cj->connected_anchor[1],
	                              cj->connected_anchor[2]);
	jd.lin_motion[0]     = cj->x_motion;
	jd.lin_motion[1]     = cj->y_motion;
	jd.lin_motion[2]     = cj->z_motion;
	jd.ang_motion[0]     = cj->x_rotation;
	jd.ang_motion[1]     = cj->y_rotation;
	jd.ang_motion[2]     = cj->z_rotation;
	jd.linear_limit      = cj->linear_limit;
	jd.angular_limit_deg[0] = cj->angular_x_limit_deg;
	jd.angular_limit_deg[1] = cj->angular_y_limit_deg;
	jd.angular_limit_deg[2] = cj->angular_z_limit_deg;
	jd.disable_collision = !cj->enable_collision;
	/* Per-axis drives, index for index -- the component and the desc use the
	 * same 0..2 linear / 3..5 angular order precisely so this stays a loop.
	 * Six separate assignments per knob is thirty chances to transpose one. */
	for (int d = 0; d < 6; ++d) {
		jd.drive_mode[d]      = cj->drive_mode[d];
		jd.drive_target[d]    = cj->drive_target[d];
		jd.drive_spring[d]    = cj->drive_spring[d];
		jd.drive_damper[d]    = cj->drive_damper[d];
		jd.drive_max_force[d] = cj->drive_max_force[d];
	}

	JceConstraintHandle h = jce_physics_configurable_joint_create(rt->physics, &jd);
	if (!jce_constraint_valid(h)) return;

	/* Track for the break monitor + early-destroy.  break_force <= 0 means the
	 * joint never breaks, but we still track it so teardown ordering is uniform
	 * (the constraint is owned by rt->physics either way). */
	if (rt->cfg_joint_count >= rt->cfg_joint_cap && !rt_grow_cfg_joints(rt)) {
		/* Out of memory growing the registry: the constraint is still live in
		 * the world (and freed by jce_physics_destroy on teardown), it just
		 * won't participate in the break monitor.  Acceptable degradation. */
		return;
	}
	ConfigJointEntry *ce = &rt->cfg_joints[rt->cfg_joint_count++];
	ce->entity       = e;
	ce->handle       = h;
	ce->break_force  = cj->break_force;
	ce->break_torque = cj->break_torque;
}


void jce_runtime_iterate_script_costs(const JceRuntime *rt,
                                      JceScriptCostIterFn cb, void *user)
{
	if (!rt || !cb) return;
	for (int i = 0; i < rt->script_count; ++i) {
		const struct ScriptEntry *se = &rt->scripts[i];
		JceScriptCostInfo info;
		info.entity      = (uint64_t)se->entity;
		info.script_path = se->script_path;
		info.last_ms     = se->cost_last_ms;
		info.avg_ms      = (se->cost_calls > 0)
		                 ? (se->cost_total_ms / (double)se->cost_calls)
		                 : 0.0;
		info.calls       = se->cost_calls;
		info.active      = se->active;
		info.measured    = se->cost_measured;
		cb(&info, user);
	}
}

void jce_runtime_reset_script_costs(JceRuntime *rt)
{
	if (!rt) return;
	for (int i = 0; i < rt->script_count; ++i) {
		rt->scripts[i].cost_total_ms = 0.0;
		rt->scripts[i].cost_calls    = 0u;
		/* last_ms and measured survive a reset on purpose: the panel shows
		 * the most recent frame's cost continuously, and clearing `measured`
		 * would make a row that HAS been timed claim it never was until the
		 * next frame arrives. */
	}
}

/* rt_spawn_joint2d lives in jce_rt_physics.c beside rt_spawn_body2d, the
 * body it needs -- and out of this file, which is at its size baseline. */


/* ── Per-frame ────────────────────────────────────────────────────── */

/* rt_clip_by_name + rt_drive_character moved to jce_rt_character.c.
 * See the size-gate note at the top of that file. */

/* Per fixed tick: map player input into each PLAYER-driven vehicle.  SCRIPT-mode
 * vehicles keep whatever the host/script API last set (we never overwrite them).
 * Mapping mirrors the character drive: walk_z -> throttle, walk_x -> steer, jump
 * -> brake.  Gated on vehicle_count -> a scene with no vehicles is a no-op.  The
 * vehicle is auto-stepped by jce_physics_step (btActionInterface), so this only
 * needs to push the latest input before the step. */
static void rt_drive_vehicles(JceRuntime *rt)
{
	if (!rt->physics || rt->vehicle_count == 0) return;
	for (int i = 0; i < rt->vehicle_count; ++i) {
		VehicleEntry *ve = &rt->vehicles[i];
		if (ve->input_mode != JCE_VEHICLE_INPUT_PLAYER) continue;
		float throttle = rt->input.walk_z;
		float steer    = rt->input.walk_x;
		float brake    = (rt->input.jump_pressed || rt->input.jump_held) ? 1.0f : 0.0f;
		throttle = throttle > 1.0f ? 1.0f : (throttle < -1.0f ? -1.0f : throttle);
		steer    = steer    > 1.0f ? 1.0f : (steer    < -1.0f ? -1.0f : steer);
		jce_physics_vehicle_set_input(rt->physics, ve->veh, throttle, brake, steer);
	}
	/* Authored per-wheel trim, AFTER the vehicle-level input it stacks on. */
	rt_apply_wheel_trim(rt);
}

/* POST-step: write each vehicle's chassis pose back to its entity Transform, and
 * each wheel's WORLD pose back to its child wheel entity (converted to the child-
 * local frame so it composes correctly under world = parent_world * local).  The
 * synthesized-wheel entries (wheel_entities[i] == 0) have no render target and
 * are skipped.  Gated on vehicle_count -> no-op for scenes without vehicles. */
static void rt_sync_vehicles(JceRuntime *rt)
{
	if (!rt->physics || !rt->scene || rt->vehicle_count == 0) return;
	for (int i = 0; i < rt->vehicle_count; ++i) {
		VehicleEntry *ve = &rt->vehicles[i];

		jce_vec3 cpos = jce_v3(0.0f, 0.0f, 0.0f);
		jce_quat crot = jce_q_identity();
		jce_physics_vehicle_get_chassis_transform(rt->physics, ve->veh,
		                                          &cpos, &crot);
		JceTransform *ctf = jce_scene_get_transform(rt->scene, ve->entity);
		if (ctf) {
			/* cpos/crot are the chassis's WORLD pose from Bullet.  Written
			 * raw, a vehicle parented to anything (a carrier, a level group,
			 * a spawn anchor) was drawn at double its parent's offset.  The
			 * WHEEL loop below was already the one place in this runtime that
			 * did the world->local conversion properly -- it builds cinv and
			 * takes each wheel into the chassis frame -- which is what made
			 * the three lines above it stand out. */
			jce_scene_solve_local_pose(rt->scene, ve->entity, cpos, crot,
			                           &ctf->position, &ctf->rotation);
			/* Subtree push covers the wheel child entities written below. */
			jce_scene_notify_physics_writeback_entity(rt->scene, ve->entity);
		}

		if (ve->wheel_count == 0) continue;
		/* Inverse of the (unit) chassis rotation = its conjugate; used to take a
		 * wheel's WORLD pose into the chassis-local frame for the child entity. */
		jce_quat cinv = crot;
		cinv.x = -cinv.x; cinv.y = -cinv.y; cinv.z = -cinv.z;
		for (uint32_t w = 0; w < ve->wheel_count; ++w) {
			JceEntity child = ve->wheel_entities[w];
			if (child == 0) continue;   /* synthesized — no render target */
			jce_vec3 wpos = jce_v3(0.0f, 0.0f, 0.0f);
			jce_quat wrot = jce_q_identity();
			jce_physics_vehicle_get_wheel_transform(rt->physics, ve->veh, w,
			                                        &wpos, &wrot);
			JceTransform *wtf = jce_scene_get_transform(rt->scene, child);
			if (!wtf) continue;
			/* world -> chassis-local: local_pos = cinv * (wpos - cpos);
			 * local_rot = cinv * wrot. */
			jce_vec3 rel = jce_v3_sub(wpos, cpos);
			wtf->position = jce_q_rotate(cinv, rel);
			wtf->rotation = jce_q_multiply(cinv, wrot);
		}
	}
}

/* POST-step: configurable-joint break monitor (CONFIGURABLE-JOINT last-mile).
 *
 * Runs once per executed fixed tick, IMMEDIATELY AFTER jce_physics_step, so the
 * applied impulse it reads is from the step just solved.  For each tracked joint
 * with break_force > 0, the constraint's last-step applied IMPULSE (N·s) is
 * compared against the break threshold expressed in the SAME units:
 *
 *     impulse = force * dt   =>   break when applied_impulse > break_force * dt
 *
 * (break_force is authored in NEWTONS, the impulse query is in N·s; multiplying
 * the force by the fixed timestep converts the threshold into impulse units so
 * the comparison is dimensionally correct.)  On break we destroy the constraint
 * and swap-remove the entry so the freed handle is never re-queried.
 *
 * break_torque is monitored too, through the constraint's JOINT FEEDBACK --
 * getAppliedImpulse() has no separable angular part, which is why that field
 * was authored, serialised, copied here and read by nothing.  The thresholds
 * scale differently on purpose and test_jce_joint_break_torque.c measures it.
 *
 * Gated on cfg_joint_count -> a scene with no configurable joints early-outs at
 * zero cost (the hot fixed-step path is byte-identical). */
typedef struct { JceRuntime *rt; float dt; } CfgJointSweep;

static bool rt_cfg_joint_breaks(void *entry, void *user)
{
	ConfigJointEntry *ce = (ConfigJointEntry *)entry;
	CfgJointSweep *cs = (CfgJointSweep *)user;

	/* EITHER limit breaks the joint, and each is independent: <= 0 means
	 * "unlimited on this axis", so a joint that snaps under twisting but
	 * never under pulling is expressible.  Testing both under one `if` would
	 * have made an unset break_force silently disable break_torque. */
	if (ce->break_force > 0.0f) {
		float imp = jce_physics_constraint_applied_impulse(cs->rt->physics,
		                                                   ce->handle);
		if (imp > ce->break_force * cs->dt) return true;
	}
	if (ce->break_torque > 0.0f) {
		float tq = jce_physics_constraint_applied_torque(cs->rt->physics,
		                                                 ce->handle);
		if (tq > ce->break_torque) return true;
	}
	return false;
}

static void rt_cfg_joint_destroy(void *entry, void *user)
{
	ConfigJointEntry *ce = (ConfigJointEntry *)entry;
	CfgJointSweep *cs = (CfgJointSweep *)user;
	jce_physics_constraint_destroy(cs->rt->physics, ce->handle);
}

/* Push a typed constraint's authored motor into the solver WHEN IT CHANGES.
 *
 * The comparison is against what this entry last pushed, not against a
 * default: an unchanged motor must cost nothing, because
 * jce_physics_constraint_set_motor also wakes both bodies and a per-tick call
 * would stop anything jointed from ever sleeping.
 *
 * A component that disappeared (the entity lost it, or was destroyed) leaves
 * the entry alone -- the constraint is still live and still driven by whatever
 * was last pushed.  Destroying it here would be a second owner for a handle
 * the teardown path already owns. */
static void rt_sync_joint_motors(JceRuntime *rt)
{
	if (!rt->physics || !rt->scene || rt->joint_count == 0) return;
	for (int i = 0; i < rt->joint_count; ++i) {
		JointEntry *je = &rt->joints[i];
		if (!jce_constraint_valid(je->handle)) continue;
		const JceConstraintComponent *cn =
			jce_scene_get_constraint(rt->scene, je->entity);
		if (!cn) continue;
		if (cn->use_motor == je->motor_on &&
		    cn->motor_target_velocity == je->motor_target &&
		    cn->motor_max_force == je->motor_max_force)
			continue;
		je->motor_on        = cn->use_motor;
		je->motor_target    = cn->motor_target_velocity;
		je->motor_max_force = cn->motor_max_force;
		jce_physics_constraint_set_motor(rt->physics, je->handle,
		                                 je->motor_on, je->motor_target,
		                                 je->motor_max_force);
	}
}

static void rt_monitor_configurable_joints(JceRuntime *rt, float fixed_dt)
{
	if (!rt->physics || rt->cfg_joint_count == 0) return;
	if (fixed_dt <= 0.0f) fixed_dt = 1.0f / 60.0f;
	CfgJointSweep cs = { rt, fixed_dt };
	rt->cfg_joint_count = rt_break_sweep(rt->cfg_joints,
	                                     sizeof(rt->cfg_joints[0]),
	                                     rt->cfg_joint_count,
	                                     rt_cfg_joint_breaks,
	                                     rt_cfg_joint_destroy, &cs);
}

/* POST-step: write each soft body's CENTROID back to its entity Transform so it
 * visibly settles / bounces.  The soft world is already advanced by this point
 * (jce_physics_step -> jce_cloth_step_ stepped the SHARED world).  Per-node mesh
 * deformation for rendering is a documented follow-up (F12); here we expose only
 * the centroid.  Gated on softbody_count -> no-op for scenes without soft bodies. */
static void rt_sync_softbodies(JceRuntime *rt)
{
	if (!rt->scene || rt->softbody_count == 0) return;
	for (int i = 0; i < rt->softbody_count; ++i) {
		SoftBodyEntry *se = &rt->softbodies[i];
		jce_vec3 c = jce_v3(0.0f, 0.0f, 0.0f);
		if (!jce_softbody_get_center(se->handle, &c)) continue;
		/* jce_softbody_get_center returns a WORLD centre -- the soft solver
		 * simulates in world space and knows nothing about the hierarchy.
		 * rt_try_spawn_softbody now receives the world pose, so writing the
		 * result back into the raw Transform would place a parented soft body
		 * correctly and then DRAW it at double its parent's offset. */
		JceTransform *tf = jce_scene_get_transform(rt->scene, se->entity);
		jce_quat wr;
		if (tf && jce_scene_get_world_pose(rt->scene, se->entity, NULL, &wr,
		                                   NULL)) {
			/* In place + notify, the established write-back shape. */
			jce_scene_solve_local_pose(rt->scene, se->entity, c, wr,
			                           &tf->position, NULL);
			jce_scene_notify_physics_writeback_entity(rt->scene, se->entity);
		}
	}
}

/* ── Client-side prediction hooks (rollback/replay) ───────────────────
 *
 * All gated on rt->predict_buf: with no predicted entity established these are
 * provable no-ops (the fixed loop stays byte-identical to before).  The step
 * function is the PURE deterministic kinematic integrator from
 * jce_predict_locomotion — it does NOT touch Bullet/physics, so it cannot
 * perturb the existing character movement; it only writes the predicted entity
 * transform for client-side prediction. */

/* Copy a JcePredictState's pos+yaw into the predicted entity's transform. */
static void rt_predict_write_state(JceRuntime *rt, const JcePredictState *st)
{
	if (!rt->scene || rt->predict_entity == 0) return;
	JceTransform *cur = jce_scene_get_transform(rt->scene,
	                                            (JceEntity)rt->predict_entity);
	if (!cur) return;
	/* A predicted pose is WORLD -- the integrator and the server both speak
	 * world metres and neither knows this entity has a parent.  Written raw,
	 * a predicted entity parented to anything reconciled against its parent's
	 * offset.  This site goes through set_transform (it runs once per tick for
	 * ONE entity, not per body per frame), so the convenience setter is the
	 * right one here and the in-place solve is not needed. */
	(void)cur;
	jce_scene_set_world_pose(rt->scene, (JceEntity)rt->predict_entity,
	                         jce_v3(st->pos[0], st->pos[1], st->pos[2]),
	                         jce_q_from_axis_angle(jce_v3(0.0f, 1.0f, 0.0f),
	                                               st->yaw));
}

/* Predict one tick forward from the latest input, then write the predicted
 * pose into the predicted entity's transform.  Runs AFTER rt_drive_character
 * (so it never changes the Bullet move) inside the fixed loop. */
static void rt_predict_apply_input(JceRuntime *rt, uint32_t ntick)
{
	if (!rt->predict_buf) return;

	JcePredictInput in;
	memset(&in, 0, sizeof in);
	in.walk_x     = rt->input.walk_x;
	in.walk_z     = rt->input.walk_z;
	in.speed_mult = rt->input.speed_mult > 0.0f ? rt->input.speed_mult : 1.0f;
	in.jump       = (rt->input.jump_pressed || rt->input.jump_held) ? 1u : 0u;
	in.sprint     = rt->input.sprint ? 1u : 0u;

	JcePredictionResult r =
		jce_prediction_apply_input(rt->predict_buf, ntick, &in,
		                           jce_predict_locomotion_step,
		                           &rt->predict_params);
	if (r != JCE_PREDICT_OK_NO_CORRECTION) return;

	JcePredictState st;
	if (jce_prediction_get_current_state(rt->predict_buf, &st))
		rt_predict_write_state(rt, &st);
}

/* Reconcile the prediction ring against any pending authoritative snapshot for
 * the predicted entity: rollback to the authoritative state at its tick and
 * replay buffered inputs, then write the reconciled pose.  Runtime-driven so
 * the net layer stays free of prediction (it only hands us the snapshot via
 * jce_net_transform_get_pending_auth + skips its own snap for this object). */
static void rt_predict_reconcile(JceRuntime *rt)
{
	if (!rt->predict_buf || rt->predict_entity == 0) return;

	uint32_t auth_tick = 0;
	float    pos[3]    = { 0.0f, 0.0f, 0.0f };
	float    rot[4]    = { 0.0f, 0.0f, 0.0f, 1.0f };
	if (!jce_net_transform_get_pending_auth(rt->predict_entity, &auth_tick,
	                                         pos, rot))
		return;

	/* The authority gives us pose, not velocity; seed vel=0 and derive yaw
	 * from the authoritative rotation so the replayed timeline continues from
	 * a consistent state. */
	JcePredictState auth;
	memset(&auth, 0, sizeof auth);
	auth.pos[0] = pos[0];
	auth.pos[1] = pos[1];
	auth.pos[2] = pos[2];
	jce_quat q = jce_v4(rot[0], rot[1], rot[2], rot[3]);
	jce_vec3 fwd = jce_q_rotate(q, jce_v3(0.0f, 0.0f, 1.0f));
	auth.yaw = atan2f(fwd.x, fwd.z);

	JcePredictionResult r =
		jce_prediction_reconcile(rt->predict_buf, auth_tick, &auth,
		                         jce_predict_locomotion_step,
		                         jce_predict_loco_compare,
		                         &rt->predict_params);
	if (r != JCE_PREDICT_CORRECTED) return;   /* matched -> nothing to write */

	JcePredictState st;
	if (jce_prediction_get_current_state(rt->predict_buf, &st))
		rt_predict_write_state(rt, &st);
}

/* ── Client->server input command channel (fixed-tick wiring) ─────────
 *
 * Runs at the TOP of each fixed tick, BEFORE rt_drive_character:
 *
 *   CLIENT — IF we own a predicted entity (rt->predict_entity != 0):
 *     pack the latest rt->input + this tick into a JceInputCommand, encode
 *     it, and upload it to the server as a ServerRpc on the predicted
 *     entity's net object (reliable-ordered).  GATE: not a client, or no
 *     predicted entity -> nothing sent -> byte-identical.
 *
 *   SERVER — IF the driven character entity is owned by a REMOTE client and
 *     that client has uploaded a command, overwrite rt->input from the
 *     stored command so the authoritative sim drives that client's movement
 *     this tick (producing the transform net_transform broadcasts and the
 *     client reconciles against).  GATE: not a server, or no remote
 *     client-owned character with a stored command -> rt->input untouched ->
 *     the existing single-player / local drive is byte-identical.
 *
 * For the SLICE the runtime drives ONE character (rt->character_entity) from
 * a single rt->input, so the server applies exactly that one client-owned
 * entity's command.  Routing each connected client's input to its OWN entity
 * (multi-client per-entity server drive) is the documented consuming
 * follow-up. */
static void rt_net_input_channel(JceRuntime *rt, uint32_t ntick)
{
	if (jce_session_mode() == JCE_SESSION_MODE_NONE) return;

	/* --- CLIENT: upload local input for our predicted (owned) entity. --- */
	if (jce_session_is_client() && rt->predict_entity != 0) {
		JceNetObjectId nid =
			jce_net_object_from_entity((uint64_t)rt->predict_entity);
		if (nid != JCE_NET_OBJECT_INVALID) {
			JceInputCommand cmd;
			memset(&cmd, 0, sizeof cmd);
			cmd.tick       = ntick;
			cmd.walk_x     = rt->input.walk_x;
			cmd.walk_z     = rt->input.walk_z;
			cmd.speed_mult = rt->input.speed_mult > 0.0f
			               ? rt->input.speed_mult : 1.0f;
			cmd.jump       = (rt->input.jump_pressed || rt->input.jump_held)
			               ? 1u : 0u;
			cmd.sprint     = rt->input.sprint ? 1u : 0u;

			uint8_t  buf[JCE_INPUT_COMMAND_WIRE_SIZE];
			uint32_t n = jce_input_command_encode(&cmd, buf, sizeof buf);
			if (n > 0u)
				jce_rpc_send(nid, RT_INPUT_CMD_RPC_NAME,
				             JCE_RPC_TO_SERVER, /*specific_client=*/0,
				             buf, n);
		}
		return;   /* a client never applies stored server-side input */
	}

	/* --- SERVER: drive the client-owned character from its uploaded cmd. - */
	if (jce_session_is_server() && rt->character_entity != 0) {
		JceNetObjectId nid =
			jce_net_object_from_entity((uint64_t)rt->character_entity);
		if (nid == JCE_NET_OBJECT_INVALID) return;
		JceClientId owner = jce_net_object_owner(nid);
		if (owner == JCE_CLIENT_SERVER) return;   /* server-owned -> local drive */

		JceInputCommand cmd;
		if (!jce_input_command_get_latest((uint32_t)owner, &cmd)) return;

		/* Replay the remote client's input into the authoritative drive.
		 * jump_pressed is edge-triggered + buffered inside rt_drive_character;
		 * map the command's level `jump` onto it each tick the client holds
		 * it (the driver refuses mid-ascent repeats, so this is safe). */
		rt->input.walk_x       = cmd.walk_x;
		rt->input.walk_z       = cmd.walk_z;
		rt->input.speed_mult   = cmd.speed_mult > 0.0f ? cmd.speed_mult : 1.0f;
		rt->input.sprint       = (cmd.sprint != 0u);
		rt->input.jump_held    = (cmd.jump != 0u);
		if (cmd.jump != 0u) rt->input.jump_pressed = true;
	}
}

/* Detect entity transforms edited outside physics (editor gizmo, gameplay
 * scripts) since the last write-back and teleport / re-scale the body to
 * match — Unity-style runtime TRS editing. */
static int rt_v3_changed(jce_vec3 a, jce_vec3 b)
{
	const float E = 1e-5f;
	return fabsf(a.x - b.x) > E || fabsf(a.y - b.y) > E || fabsf(a.z - b.z) > E;
}

static int rt_q_changed(jce_quat a, jce_quat b)
{
	const float E = 1e-5f;
	return fabsf(a.x - b.x) > E || fabsf(a.y - b.y) > E ||
	       fabsf(a.z - b.z) > E || fabsf(a.w - b.w) > E;
}

static void rt_push_external_transforms(JceRuntime *rt)
{
	if (!rt->physics || !rt->scene) return;
	for (int i = 0; i < rt->body_count; ++i) {
		BodyEntry *be = &rt->bodies[i];
		/* WORLD, not the raw JceTransform -- see rt_spawn_entity.  This pass
		 * detects that somebody moved the entity, so both ends of the cache
		 * must speak one space: last_* are world, written by
		 * rt_sync_transforms through this same call.
		 *
		 * A CONSEQUENCE WORTH NAMING: a collider parented to something that
		 * MOVES now follows it, because the parent moving changes the child's
		 * world pose and this pass sees exactly that.  Before, it did not
		 * follow its parent at all -- it sat at its local offset from the
		 * world origin, which is why Unity's caution about parenting a
		 * Rigidbody to a moving object had no counterpart here. */
		jce_vec3 wpos, wscale;
		jce_quat wrot;
		if (!jce_scene_get_world_pose(rt->scene, be->entity, &wpos, &wrot,
		                              &wscale))
			continue;

		bool pos_changed = rt_v3_changed(wpos, be->last_pos);
		bool rot_changed = rt_q_changed(wrot, be->last_rot);
		if (pos_changed ||
		    (rot_changed && be->kind != (uint8_t)JCE_BODY_DYNAMIC)) {
			/* Position move (any kind) or rotation of a non-dynamic body =
			 * a real teleport.  Entity Transform is the ORIGIN; the body pose
			 * is the collider CENTER, so re-apply the (rotated) collider offset
			 * — otherwise a moved entity collapses an offset collider onto its
			 * origin. */
			jce_vec3 body_center = jce_v3_add(wpos,
			                       jce_q_rotate(wrot, be->center_local));
			jce_physics_body_set_transform(rt->physics, be->body,
			                               body_center, wrot);
			be->last_pos = wpos;           /* WORLD (edit-detect cache) */
			be->last_rot = wrot;
			/* Teleport: collapse both interpolation endpoints onto the new body
			 * CENTER so the next sync blends to a no-op instead of sliding the
			 * body in from its pre-edit physics position. */
			be->prev_pos = body_center;
			be->prev_rot = wrot;
			be->cur_pos  = body_center;
			be->cur_rot  = wrot;
		} else if (rot_changed) {
			/* DYNAMIC body, rotation-only edit (a gameplay script facing the
			 * body toward a target EVERY frame while physics owns its motion).
			 * Do NOT call jce_physics_body_set_transform here: setting the body
			 * world transform every frame — even to its current position —
			 * disrupts Bullet's velocity integration, which FROZE AI chase
			 * (set_velocity + per-frame set_rotation moved nothing).  Just
			 * acknowledge the edit so it doesn't re-fire; the visual facing
			 * comes from the scene Transform rotation, which rt_sync_transforms
			 * preserves for freeze-rotation bodies (it would otherwise overwrite
			 * it with the body's locked physics rotation). */
			be->last_rot = wrot;
		}
		if (rt_v3_changed(wscale, be->last_scale)) {
			/* World scale, matched to spawn_scale which rt_track_body now
			 * records from the same world pose -- a child of a scaled parent
			 * used to size its collider from the local scale and then compare
			 * against it forever, so a parent being scaled resized the mesh
			 * and left the collider alone. */
			jce_vec3 ratio;
			ratio.x = be->spawn_scale.x != 0.0f ? wscale.x / be->spawn_scale.x : 1.0f;
			ratio.y = be->spawn_scale.y != 0.0f ? wscale.y / be->spawn_scale.y : 1.0f;
			ratio.z = be->spawn_scale.z != 0.0f ? wscale.z / be->spawn_scale.z : 1.0f;
			jce_physics_body_set_scale(rt->physics, be->body, ratio);
			be->last_scale = wscale;
		}
	}

	/* Character (kept out of bodies[]): honor a gizmo TRS edit during Play by
	 * warping the kinematic capsule to the edited feet pose (feet -> center). */
	if (jce_character_valid(rt->character) && rt->character_entity != 0) {
		jce_vec3 cwpos;
		jce_quat cwrot;
		if (jce_scene_get_world_pose(rt->scene, rt->character_entity, &cwpos,
		                             &cwrot, NULL) &&
		    (rt_v3_changed(cwpos, rt->char_last_pos) ||
		     rt_q_changed(cwrot, rt->char_last_rot))) {
			jce_vec3 center = cwpos;                 /* WORLD feet, see above */
			center.y += rt->character_half_height;   /* feet -> capsule center */
			jce_physics_character_set_position(rt->physics, rt->character, center);
			rt->char_prev_pos = center;              /* collapse interpolation */
			rt->char_cur_pos  = center;
			rt->char_last_pos = cwpos;               /* feet, world */
			rt->char_last_rot = cwrot;
		}
	}
}

/* ── Water buoyancy (gap 2.3, slice 3) ───────────────────────────────────
 *
 * Finds the active water surface (the first enabled, visible JceWaterComponent
 * — like foliage finds terrain) and, for every dynamic body whose entity has an
 * ENABLED JceBuoyancyComponent, samples the Gerstner surface height at the
 * body's XZ for the current buoyancy phase time, derives the submersion depth,
 * and applies the upward buoyancy + vertical-drag force from
 * jce_water_buoyancy_force().  Runs once per executed fixed tick, BEFORE
 * jce_physics_step, so the force is integrated by the very next step on the
 * fixed cadence (time_scale already folded into how many ticks run + the phase
 * advance).  Non-buoyant bodies are never touched, so the pass is a strict
 * superset add (zero regression for existing scenes). */

/* Finds the first enabled+visible water component while iterating entities. */
typedef struct {
	JceScene          *scene;
	JceWaterComponent *water;     /* first match (NULL = none active)         */
	JceEntity          entity;    /* key into the scene's water-field set     */
	int                water_comp_id;
	float              surface_y; /* still-water world Y (base + entity Y)    */
	float              center_x;  /* body centre in WORLD XZ                  */
	float              center_z;
} BuoyWaterScan;

static void rt_buoy_find_water(JceScene *s, JceEntity e, void *user)
{
	BuoyWaterScan *ctx = (BuoyWaterScan *)user;
	if (ctx->water) return;                       /* already found the first */
	if (!jce_scene_has_water(s, e)) return;
	if (ctx->water_comp_id >= 0 &&
	    !jce_scene_comp_enabled(s, e, ctx->water_comp_id)) return;
	JceWaterComponent *wc = jce_scene_get_water(s, e);
	if (!wc || !wc->visible) return;

	jce_mat4 m = jce_scene_get_world_matrix(s, e);
	ctx->water     = wc;
	ctx->entity    = e;
	ctx->surface_y = wc->base_height + m.col[3].y;
	ctx->center_x  = m.col[3].x;
	ctx->center_z  = m.col[3].z;
}

/* fixed_dt is taken rather than read from rt, because the disturbance grid
 * stepped at the bottom of this function must advance on the SAME cadence the
 * caller is stepping physics with -- passing it makes that structural instead
 * of a second lookup that could drift. */
static void rt_apply_buoyancy(JceRuntime *rt, float fixed_dt)
{
	if (!rt->physics || !rt->scene || rt->body_count <= 0) return;

	/* Resolve component ids once (cheap; -1 if a build somehow lacks them). */
	static int s_water_id    = -2;   /* -2 = not yet resolved */
	static int s_buoyancy_id = -2;
	if (s_water_id == -2)    s_water_id    = jce_component_find("Water");
	if (s_buoyancy_id == -2) s_buoyancy_id = jce_component_find("Buoyancy");

	/* Find the active water surface (first enabled+visible).  No water ⇒
	 * nothing floats; bail before touching any body.  O(#water) — this was a
	 * FULL-scene walk every fixed tick (the no-water bail itself cost O(E)). */
	BuoyWaterScan scan;
	memset(&scan, 0, sizeof scan);
	scan.scene         = rt->scene;
	scan.water_comp_id = s_water_id;
	jce_scene_each_water(rt->scene, rt_buoy_find_water, &scan);
	if (!scan.water) return;

	/* Sample the SAME field the renderer draws from.  Before this, buoyancy
	 * evaluated its own Gerstner sum on its own clock -- which disagreed with
	 * the screen in phase always, and in MODEL entirely whenever the water was
	 * authored as FFT.  See jce_water_field.h. */
	JceWaterFieldSet *wfs = jce_scene_water_fields(rt->scene);
	/* ONE filler, from the scene -- see jce_scene_water_field_desc. The desc
	 * that used to be written out here was the renderer's twin, and the two
	 * disagreed: this side never applied the weather's wind multiplier, so in
	 * any weather the two acquires of the SAME field saw two spectra and the
	 * whole Tessendorf patch was rebuilt twice a frame. */
	JceWaterFieldDesc wd;
	if (!jce_scene_water_field_desc(rt->scene, scan.entity, &wd)) return;

	JceWaterField *field =
	    jce_water_field_set_acquire(wfs, (uint64_t)scan.entity, &wd);

	/* The DISTURBANCE layer, sized from the same water body this pass already
	 * picked. Created here and nowhere else: this is the only code that knows
	 * which body is the active one, and a grid created by a reader would be
	 * sized by whoever happened to draw first.
	 *
	 * Resolution is fixed rather than authored. It is a cost decision, not a
	 * look decision -- the step is explicit, so the work per simulated second
	 * goes with the SQUARE of it -- and a field on the component would be a
	 * knob whose right value nobody could state. 96 over a typical pond gives
	 * texels of a few tens of centimetres, which is finer than the wake of
	 * anything that floats.
	 *
	 * Depth is left at the uniform default. Real bathymetry from the terrain
	 * is the next step and belongs with the terrain query, not here; until it
	 * exists, a flat bed is honest -- waves simply do not refract toward the
	 * shore yet. */
	JceWaterRipple *ripple;
	{
		JceWaterRippleDesc rd = jce_water_ripple_default_desc();
		rd.resolution      = 96;
		rd.size_m          = (wd.size_x > wd.size_z ? wd.size_x : wd.size_z);
		if (!(rd.size_m > 0.0f)) rd.size_m = 32.0f;
		rd.center_x        = wd.center_x;
		rd.center_z        = wd.center_z;
		rd.default_depth_m = 2.0f;
		ripple = jce_scene_water_ripple(rt->scene, &rd);

		/* BATHYMETRY: the still-water depth at every cell, from the terrain
		 * under it.
		 *
		 * This is the whole reason the solver carries a depth map instead of a
		 * constant. The wave speed is c = sqrt(g H), so shallow water carries
		 * waves more slowly: they bunch up and BEND toward the shore, and cells
		 * where the terrain is above the waterline become land and REFLECT.
		 * Nobody authors any of that -- it falls out of H. With a uniform depth
		 * the pond is a drum skin with a square edge.
		 *
		 * Filled ONCE, on the tick the grid is created. The bed is not static
		 * in principle (a tide, a sculpt, a filling lock all move it), but
		 * re-reading it every tick would sample the terrain resolution^2 times
		 * per frame for an answer that almost never changes; a caller that
		 * moves the bed can call jce_water_ripple_set_depth itself, which is
		 * why that entry point is public.
		 *
		 * Skipped entirely when the runtime has no terrain -- and skipped is
		 * the right word: jce_water_ripple_create already filled the map with
		 * the uniform default, so this is a refinement of a valid state rather
		 * than the only thing standing between it and garbage. */
		if (ripple && rt->terrain_stream_src && !rt->water_bathymetry_done) {
			const int rn = jce_water_ripple_resolution(ripple);
			float cx = 0.0f, cz = 0.0f, sz = 0.0f;
			jce_water_ripple_world_rect(ripple, &cx, &cz, &sz);
			if (rn > 1 && sz > 0.0f) {
				float *dep = (float *)jce_malloc((size_t)rn * (size_t)rn *
				                                 sizeof(float));
				if (dep) {
					/* The still-water plane the depths are measured DOWN from.
					 * wd.base_height already folds the water entity's world Y
					 * (see jce_scene_water_field_desc), so this is the same
					 * datum the ambient field and the renderer use -- taking
					 * the component's local base_height here would put the bed
					 * at the wrong depth for any pond not at the origin. */
					const float surface_y = wd.base_height;
					const float step = sz / (float)(rn - 1);
					const float x0 = cx - sz * 0.5f;
					const float z0 = cz - sz * 0.5f;
					for (int j = 0; j < rn; ++j) {
						const float wz = z0 + step * (float)j;
						for (int i = 0; i < rn; ++i) {
							const float wx = x0 + step * (float)i;
							const float ty =
							    jce_terrain_sample_height(rt->terrain_stream_src,
							                              wx, wz);
							/* Depth <= 0 is LAND to the solver, and that is
							 * exactly what terrain above the waterline is. No
							 * clamp: clamping to a small positive depth would
							 * make the shoreline a very fast, very shallow
							 * channel instead of a bank. */
							dep[(size_t)j * (size_t)rn + (size_t)i] =
							    surface_y - ty;
						}
					}
					jce_water_ripple_set_depth(ripple, dep, rn * rn);
					jce_free(dep);
					rt->water_bathymetry_done = true;
				}
			}
		}
	}
	/* No field (allocation failure, or a set already full of other bodies)
	 * means nothing floats this tick.  Applying a WRONG force would be worse
	 * than applying none. */
	if (!field) return;

	for (int i = 0; i < rt->body_count; ++i) {
		BodyEntry *be = &rt->bodies[i];
		/* Only dynamic bodies float; static/kinematic skip (gravity-less). */
		if (be->kind != (uint8_t)JCE_BODY_DYNAMIC) continue;
		if (!jce_physics_body_is_dynamic(rt->physics, be->body)) continue;

		/* Authoring gate: entity must carry an ENABLED buoyancy component.
		 * Bodies without one are provably untouched. */
		if (!jce_scene_has_buoyancy(rt->scene, be->entity)) continue;
		if (s_buoyancy_id >= 0 &&
		    !jce_scene_comp_enabled(rt->scene, be->entity, s_buoyancy_id))
			continue;
		JceBuoyancyComponent *bc = jce_scene_get_buoyancy(rt->scene, be->entity);
		if (!bc || !bc->enabled) continue;

		jce_vec3 pos;
		jce_quat rot;          /* orientation unused — buoyancy is vertical */
		jce_physics_body_get_transform(rt->physics, be->body, &pos, &rot);

		/* Surface height at the body's WORLD XZ.  vs_water.sc displaces the
		 * surface in WORLD space (phase = k*dot(dir,(world_x,world_z))) after
		 * mul(u_model[0], a_position), so buoyancy must sample the SAME world XZ
		 * to float a body on the visible wave (a water entity offset in XZ would
		 * otherwise read the wrong phase).  surface_y already folds base_height
		 * + the water entity world Y. */
		/* The field inverts the horizontal displacement (Gerstner roll or
		 * FFT chop) before reading the height: the shader moves vertices in X
		 * and Z as well as Y, so the surface point at this world XZ was
		 * authored somewhere else, and reading the vertical sum here floats a
		 * body at visibly the wrong place on a steep wave -- worst at crests.
		 *
		 * Outside the body's extent the field refuses rather than inventing a
		 * surface, so a body far from the pond is simply never floated. */
		JceWaterSample ws;
		if (!jce_water_field_sample(field, pos.x, pos.z, 0.0f, &ws)) continue;
		/* AMBIENT + DISTURBANCE. The wind waves are a pure function of time;
		 * the ripples are what objects did to the water. A body must float on
		 * their sum, or it sits on a surface nobody draws -- which is the exact
		 * disagreement JceWaterField was created to end, reintroduced one layer
		 * up.
		 *
		 * Outside the grid the accessor returns 0, which is correct rather than
		 * a fallback: outside it there is no disturbance. */
		const float water_y = ws.position.y
		                    + jce_water_ripple_height(ripple, pos.x, pos.z);

		const float submersion = water_y - pos.y;   /* >0 only when below */
		const jce_vec3 vel = jce_physics_body_get_velocity(rt->physics, be->body);

		/* WAKE. A body moving through water displaces it, and the impulse is
		 * built entirely from quantities this pass already has -- no new
		 * authored field, because none of them would have a value anyone could
		 * state.
		 *
		 *   radius  the body's own horizontal extent, from the collider bounds
		 *           the physics already keeps. A wake is as wide as the thing
		 *           making it.
		 *   speed   the VERTICAL velocity, which is the component that actually
		 *           moves water up or down. A body settling pushes the surface
		 *           down and a body rising pulls it up, and the sign follows
		 *           from that without a rule.
		 *
		 * Gated on being in the water at all: an airborne body has not touched
		 * it yet, and a body that never enters never disturbs. The gate is the
		 * same `submersion > 0` the force below uses, so the two cannot
		 * disagree about whether this body is in the water. */
		if (submersion > 0.0f && ripple) {
			/* Horizontal half-extent from the AUTHORED collider, not from the
			 * physics world: there is no body-AABB query in the physics API,
			 * and the collider component is the same thing the body was built
			 * from. A default is used when a body has none of the three, so a
			 * compound or mesh collider still makes a wake rather than none. */
			float r = 0.5f;
			if (jce_scene_has_box_collider(rt->scene, be->entity)) {
				const JceBoxColliderComponent *bx =
				    jce_scene_get_box_collider(rt->scene, be->entity);
				if (bx) {
					const float ex = 0.5f * bx->size[0];
					const float ez = 0.5f * bx->size[2];
					r = (ex > ez ? ex : ez);
				}
			} else if (jce_scene_has_sphere_collider(rt->scene, be->entity)) {
				const JceSphereColliderComponent *sp =
				    jce_scene_get_sphere_collider(rt->scene, be->entity);
				if (sp) r = sp->radius;
			} else if (jce_scene_has_capsule_collider(rt->scene, be->entity)) {
				const JceCapsuleColliderComponent *cp =
				    jce_scene_get_capsule_collider(rt->scene, be->entity);
				if (cp) r = cp->radius;
			}
			/* The deadband is not noise rejection: a body resting on the water
			 * has a small residual vertical velocity forever, and without it
			 * every floating object would drive the grid on every tick and the
			 * pond would never go quiet. */
			if (r > 0.05f && (vel.y < -0.05f || vel.y > 0.05f))
				jce_water_ripple_impulse(ripple, pos.x, pos.z, r, -vel.y);
		}

		if (submersion <= 0.0f) continue;           /* airborne -> untouched */
		float fy = jce_water_buoyancy_force(submersion, vel.y,
		                                    bc->buoyancy_strength, bc->drag);
		/* The authored strength/drag are documented as mass-INDEPENDENT (they
		 * express a target acceleration profile, like Unity's "buoyancy" being a
		 * settle depth rather than a raw newton).  applyCentralForce divides by
		 * mass (a = F/m), so scale the force by the authored body mass to cancel
		 * that out: a heavier and a lighter body settle at the same depth/feel.
		 * mass <= 0 (effectively static) is left at 1 so the force is unchanged. */
		if (fy != 0.0f) {
			float mass = 1.0f;
			JceRigidBodyComponent *rb =
			    jce_scene_get_rigidbody(rt->scene, be->entity);
			if (rb && rb->mass > 0.0f) mass = rb->mass;
			fy *= mass;
			jce_vec3 force = { 0.0f, fy, 0.0f };
			jce_physics_body_apply_force(rt->physics, be->body, force);
		}
	}
}

/* ── Constant Force (continuous additive force/torque) ───────────────────
 *
 * Unity-style ConstantForce: for every dynamic body whose entity carries an
 * ENABLED JceConstantForceComponent, accumulate the authored world-space force
 * + torque AND the body-relative force + torque (rotated into world by the
 * body's current orientation) once per executed fixed tick, IMMEDIATELY BEFORE
 * jce_physics_step (right after the buoyancy pass), so the very next step
 * integrates them on the fixed cadence.  Mirrors rt_apply_buoyancy exactly:
 * iterate rt->bodies[], gate on a dynamic body + an enabled component, no-op
 * for static/kinematic bodies and for components at their zeroed defaults.
 *
 * Hot-path zero-cost: a pre-walk counts enabled constant-force components into
 * a cached count; when it is 0 (the overwhelming common case) the per-body loop
 * is never entered, so a scene that authored no ConstantForce keeps the
 * byte-identical step path. */
static void rt_apply_constant_force(JceRuntime *rt)
{
	if (!rt->physics || !rt->scene || rt->body_count <= 0) return;

	/* Resolve the component id once (cheap; -1 if a build somehow lacks it). */
	static int s_cf_id = -2;   /* -2 = not yet resolved */
	if (s_cf_id == -2) s_cf_id = jce_component_find("ConstantForce");

	/* O(1) bail: no ConstantForce component anywhere in the scene (flecs
	 * table-count aggregate).  The previous "cheap pre-walk" was itself an
	 * O(bodies) scan with ~3 hash probes per body EVERY fixed tick just to
	 * discover there was nothing to do.  Enabled-state filtering still
	 * happens in the main loop below (count > 0 only means "some entity
	 * holds the component", which is exactly the bail condition). */
	if (jce_scene_count_constant_force(rt->scene) == 0) return;

	for (int i = 0; i < rt->body_count; ++i) {
		BodyEntry *be = &rt->bodies[i];
		/* Only dynamic bodies accept forces; static/kinematic skip. */
		if (be->kind != (uint8_t)JCE_BODY_DYNAMIC) continue;
		if (!jce_physics_body_is_dynamic(rt->physics, be->body)) continue;

		/* Authoring gate: entity must carry an ENABLED ConstantForce. */
		if (!jce_scene_has_constant_force(rt->scene, be->entity)) continue;
		if (s_cf_id >= 0 &&
		    !jce_scene_comp_enabled(rt->scene, be->entity, s_cf_id))
			continue;
		JceConstantForceComponent *cf =
		    jce_scene_get_constant_force(rt->scene, be->entity);
		if (!cf || !cf->enabled) continue;

		/* World-space force / torque: applied directly. */
		jce_vec3 force = jce_v3(cf->force[0], cf->force[1], cf->force[2]);
		jce_vec3 torque = jce_v3(cf->torque[0], cf->torque[1], cf->torque[2]);

		/* Body-relative force / torque: rotate into world by the body's
		 * current orientation (so a forward thrust always pushes along the
		 * body's own facing as it tumbles). */
		bool has_rel = (cf->relative_force[0] != 0.0f ||
		                cf->relative_force[1] != 0.0f ||
		                cf->relative_force[2] != 0.0f ||
		                cf->relative_torque[0] != 0.0f ||
		                cf->relative_torque[1] != 0.0f ||
		                cf->relative_torque[2] != 0.0f);
		if (has_rel) {
			jce_vec3 pos;
			jce_quat rot;
			jce_physics_body_get_transform(rt->physics, be->body,
			                               &pos, &rot);
			jce_vec3 rf = jce_q_rotate(rot,
			    jce_v3(cf->relative_force[0], cf->relative_force[1],
			           cf->relative_force[2]));
			jce_vec3 rt_q = jce_q_rotate(rot,
			    jce_v3(cf->relative_torque[0], cf->relative_torque[1],
			           cf->relative_torque[2]));
			force.x += rf.x; force.y += rf.y; force.z += rf.z;
			torque.x += rt_q.x; torque.y += rt_q.y; torque.z += rt_q.z;
		}

		if (force.x != 0.0f || force.y != 0.0f || force.z != 0.0f)
			jce_physics_body_apply_force(rt->physics, be->body, force);
		if (torque.x != 0.0f || torque.y != 0.0f || torque.z != 0.0f)
			jce_physics_body_apply_torque(rt->physics, be->body, torque);
	}
}

/* ── Ragdoll pre-step drive (scene-pass last-mile) ───────────────────────
 *
 * Runs once per executed fixed tick, IMMEDIATELY BEFORE jce_physics_step (right
 * after the buoyancy/force-apply pass), gated on rt->ragdoll_count so a scene
 * with no ragdolls keeps the byte-identical frame path.  For each live ragdoll
 * it drives the bodies toward a SOURCE LOCAL pose:
 *   - if the entity has a published relay pose, use it (lets a future followup
 *     feed the renderer's sampled clip pose in with one frame of latency);
 *   - otherwise the skeleton's REST (bind) LOCAL pose.
 * With the default blend_weight=1 the bodies track that source; blend_weight=0
 * leaves them to physics (the death-collapse / F12 demo).
 *
 * MVP NUANCE: the runtime has no clip sampler, so absent a relay pose the
 * source is the bind pose — blend_weight=1 holds the bind pose, blend_weight=0
 * collapses.  Full anim-driven cross-fade needs the renderer to publish its
 * sampled pose into the relay BEFORE the step (1-frame-latency followup). */
static void rt_ragdoll_sync_from(JceRuntime *rt, float dt)
{
	if (!rt || !rt->scene) return;
	for (int i = 0; i < rt->ragdoll_count; ++i) {
		struct RagdollEntry *re = &rt->ragdoll_entries[i];
		if (!re->rd) continue;

		const JceSkeleton *skel = jce_model_get_skeleton(re->model);
		if (!skel) continue;
		uint32_t nj = jce_skeleton_joint_count(skel);
		if (nj == 0) continue;
		if (nj > (uint32_t)JCE_MAX_BONES) nj = (uint32_t)JCE_MAX_BONES;

		jce_mat4 locals[JCE_MAX_BONES];
		uint32_t cnt = 0;
		if (jce_scene_get_ragdoll_pose(rt->scene, re->entity, locals, &cnt) &&
		    cnt >= nj) {
			/* relay pose already filled `locals` */
		} else {
			/* Source the skeleton's REST (bind) LOCAL pose. */
			const jce_mat4 *rest = jce_skeleton_rest_pose(skel);
			if (rest) {
				memcpy(locals, rest, (size_t)nj * sizeof(jce_mat4));
			} else {
				/* Build from rest TRS as a fallback. */
				const jce_vec3 *t = NULL; const jce_quat *r = NULL;
				const jce_vec3 *sc = NULL;
				jce_skeleton_rest_trs(skel, &t, &r, &sc);
				if (!t || !r || !sc) continue;
				for (uint32_t j = 0; j < nj; ++j)
					locals[j] = jce_m4_from_trs(t[j], r[j], sc[j]);
			}
		}

		jce_ragdoll_sync_from_pose(re->rd, locals, re->blend_weight, dt);
	}
}

/* ── Ragdoll post-step publish (scene-pass last-mile) ────────────────────
 *
 * Runs once after the fixed loop, at the END of rt_sync_transforms, gated on
 * rt->ragdoll_count.  Reads each ragdoll's resolved per-bone WORLD->LOCAL pose
 * (jce_ragdoll_sync_to_pose) and publishes it into the SHARED scene relay
 * (jce_scene_set_ragdoll_pose).  The scene renderer reads ONLY that relay and
 * evaluates it into the skin palette — the renderer never touches physics, so
 * the scene layer stays physics-agnostic and the layering (renderer never calls
 * up into the runtime) is respected.  Does NOT write the entity Transform:
 * ragdoll output is per-bone, not a single transform. */
static void rt_ragdoll_sync_to(JceRuntime *rt)
{
	if (!rt || !rt->scene) return;
	for (int i = 0; i < rt->ragdoll_count; ++i) {
		struct RagdollEntry *re = &rt->ragdoll_entries[i];
		if (!re->rd) continue;

		const JceSkeleton *skel = jce_model_get_skeleton(re->model);
		if (!skel) continue;
		uint32_t nj = jce_skeleton_joint_count(skel);
		if (nj == 0) continue;
		if (nj > (uint32_t)JCE_MAX_BONES) nj = (uint32_t)JCE_MAX_BONES;

		jce_mat4 out_locals[JCE_MAX_BONES];
		jce_ragdoll_sync_to_pose(re->rd, out_locals);
		jce_scene_set_ragdoll_pose(rt->scene, re->entity, out_locals, nj);
	}
}

/* Snapshot the post-step physics pose of every dynamic body into the
 * interpolation history (prev <- cur, cur <- live pose).  Called once per
 * executed fixed tick so prev/cur bracket exactly one fixed_dt of motion. */
static void rt_capture_fixed_pose(JceRuntime *rt)
{
	if (!rt->physics) return;
	for (int i = 0; i < rt->body_count; ++i) {
		if (rt->bodies[i].kind == (uint8_t)JCE_BODY_STATIC) continue;
		BodyEntry *be = &rt->bodies[i];
		be->prev_pos = be->cur_pos;
		be->prev_rot = be->cur_rot;
		jce_physics_body_get_transform(rt->physics, be->body,
		                               &be->cur_pos, &be->cur_rot);
	}
	/* Same fixed-tick history for the character so it renders smoothly. */
	if (jce_character_valid(rt->character) && rt->character_entity != 0) {
		rt->char_prev_pos = rt->char_cur_pos;
		jce_physics_character_get_position(rt->physics, rt->character,
		                                   &rt->char_cur_pos);
	}
}

/* Write each dynamic body's render pose into its scene Transform.  When
 * interpolation history is available (have_prev) the pose is the blend of
 * the last two fixed ticks by `alpha` (residual accumulator / fixed_dt),
 * so rendering stays smooth between fixed ticks; otherwise the raw current
 * pose is used.  The character controller is interpolated the same way, then
 * projected from the capsule CENTER down to the FEET (the entity Transform /
 * feet-pivoted character meshes live at the feet). */
static void rt_sync_transforms(JceRuntime *rt, float alpha)
{
	if (!rt->physics || !rt->scene) return;
	if (alpha < 0.0f) alpha = 0.0f;
	if (alpha > 1.0f) alpha = 1.0f;

	bool wrote_any = false;
	for (int i = 0; i < rt->body_count; ++i) {
		/* Static bodies never move during the step — skip the per-frame
		 * write. They are still repositioned through
		 * rt_push_external_transforms when the editor/script moves them. */
		if (rt->bodies[i].kind == (uint8_t)JCE_BODY_STATIC) continue;
		BodyEntry *be = &rt->bodies[i];
		jce_vec3 p; jce_quat q;
		if (rt->have_prev) {
			p = jce_v3_lerp(be->prev_pos, be->cur_pos, alpha);
			q = jce_q_slerp(be->prev_rot, be->cur_rot, alpha);
		} else {
			p = be->cur_pos;
			q = be->cur_rot;
		}
		/* p is the collider CENTER; the entity Transform is the ORIGIN, so
		 * subtract the (rotated) collider offset to recover it. */
		jce_vec3 origin = jce_v3_sub(p, jce_q_rotate(q, be->center_local));
		JceTransform *tc = jce_scene_get_transform(rt->scene, be->entity);
		if (tc) {
			/* A freeze-rotation dynamic body locks its angular axes, so its
			 * physics rotation never changes — a gameplay script owns the
			 * VISUAL facing via the scene Transform rotation (set_rotation).
			 * Writing the body's frozen rotation back here would overwrite that
			 * facing (and AI "lock-on" would stop turning).  Keep tc->rotation
			 * for freeze bodies; only sync position. */
			const JceRigidBodyComponent *rbc =
				jce_scene_get_rigidbody(rt->scene, be->entity);
			bool freeze = rbc && rbc->freeze_rotation;

			/* `origin` IS A WORLD POSE.  Writing it into tc->position was
			 * correct only because every body used to be treated as a root:
			 * for a parented entity the renderer then composes
			 * parent_world * local, so a simulated child was DRAWN AT DOUBLE
			 * THE PARENT OFFSET -- the mirror image of the spawn bug, and the
			 * one nobody reports, because it does not look like physics being
			 * wrong.  set_world_pose solves the local TRS that puts it there;
			 * on a root it assigns verbatim, which is byte-for-byte what this
			 * line did before. */
			jce_quat want_rot = q;
			if (freeze) {
				/* A freeze-rotation body's facing belongs to the script that
				 * set it, and the setter takes a WORLD rotation -- handing it
				 * the LOCAL tc->rotation would re-interpret a child's facing
				 * as a world one and turn it by its parent's orientation
				 * every single frame.  Ask for the rotation the entity
				 * actually has, so writing it back is a no-op. */
				jce_scene_get_world_pose(rt->scene, be->entity, NULL,
				                         &want_rot, NULL);
			}
			/* SOLVED, THEN WRITTEN IN PLACE -- not through
			 * jce_scene_set_world_pose, which goes through
			 * jce_scene_set_transform and bumps the entity subtree's
			 * world-cache generation.  This loop runs for every non-static
			 * body every frame, and not paying that is exactly why these
			 * writes are in place and the entities are named afterwards.
			 * The solve is the shared primitive, so one place still knows
			 * the world->local math.  Every other in-place write-back below
			 * cites this paragraph. */
			jce_scene_solve_local_pose(rt->scene, be->entity, origin, want_rot,
			                           &tc->position, &tc->rotation);

			/* READ THE CACHE BACK rather than storing what we asked for:
			 * under a parent the solve and the recomposition each go through
			 * a decomposition, and that residual is easily larger than
			 * rt_v3_changed's 1e-5 -- which would make every frame look like
			 * an external edit and teleport the body into its own
			 * simulation.  On a root it is a struct copy. */
			jce_scene_get_world_pose(rt->scene, be->entity, &be->last_pos,
			                         &be->last_rot, NULL);
			jce_scene_notify_physics_writeback_entity(rt->scene, be->entity);
			wrote_any = true;
		}
	}

	if (jce_character_valid(rt->character) && rt->character_entity != 0) {
		jce_vec3 cp = rt->have_prev
			? jce_v3_lerp(rt->char_prev_pos, rt->char_cur_pos, alpha)
			: rt->char_cur_pos;
		cp.y -= rt->character_half_height;   /* capsule center -> feet */
		/* World, solved back into whatever frame the character lives in --
		 * see rt_sync_transforms' body loop above.  A character parented to
		 * anything (a vehicle interior, a moving platform, a spawn group) was
		 * drawn at double its parent's offset. */
		JceTransform *tc = jce_scene_get_transform(rt->scene,
		                                            rt->character_entity);
		jce_quat crot;
		if (tc && jce_scene_get_world_pose(rt->scene, rt->character_entity,
		                                   NULL, &crot, NULL)) {
			/* In place, for the same reason as the body loop above. */
			jce_scene_solve_local_pose(rt->scene, rt->character_entity, cp,
			                           crot, &tc->position, NULL);
			/* Read back, not the value we asked for: under a parent the
			 * round trip goes through two decompositions and the residual
			 * would read as an external edit next frame. */
			jce_scene_get_world_pose(rt->scene, rt->character_entity,
			                         &rt->char_last_pos, NULL, NULL);
			jce_scene_notify_physics_writeback_entity(rt->scene,
			                                          rt->character_entity);
			wrote_any = true;
		}
	}

	/* Ragdoll publish (scene-pass last-mile): after the body/character pose
	 * write-back, push each ragdoll's resolved per-bone LOCAL pose into the
	 * shared scene relay so the renderer can evaluate it into the skin palette.
	 * Gated on ragdoll_count -> no-op for scenes without ragdolls. */
	if (rt->ragdoll_count)
		rt_ragdoll_sync_to(rt);

	/* Vehicle publish (VEHICLE last-mile): write each chassis pose to its entity
	 * and each wheel's pose to its child entity so wheel meshes roll + steer.
	 * Vehicles are auto-stepped (not interpolated like bodies), so this writes
	 * the latest sim pose directly.  Gated on vehicle_count -> no-op otherwise. */
	if (rt->vehicle_count)
		rt_sync_vehicles(rt);

	/* Soft-body publish (SOFT-BODY last-mile): write each body's centroid to its
	 * entity Transform so the squishy object visibly settles.  The shared soft
	 * world was already stepped inside jce_physics_step.  Gated on softbody_count
	 * -> no-op otherwise. */
	if (rt->softbody_count)
		rt_sync_softbodies(rt);

	/* All of the above mutate Transforms in place (no set_transform, no per-
	 * entity xgen bump) — signal the scene's frame-invariance counter once per
	 * sync so cross-frame render caches know a world matrix may have changed.
	 * Gated on an ACTUAL write (all-static / body-less scenes stay quiet so a
	 * frozen-frame consumer can still latch; ragdoll/vehicle/softbody publish
	 * writes when their counts are non-zero) — DOTS slice 1. */
	/* Bodies/character/vehicles/softbodies notified per entity above (L2
	 * incremental repair).  Ragdoll bone relay mutates an UNKNOWN set of
	 * skinned poses — keep the conservative blanket for it. */
	if (rt->ragdoll_count > 0)
		jce_scene_notify_physics_writeback(rt->scene);
	(void)wrote_any;
}

/* Write each 2D body's simulated pose into its scene Transform: position x/y
 * land in the XY plane (z preserved), and the body angle becomes a Z-rotation
 * quaternion.  Static bodies don't move during the step, so skip them. */
static void rt_sync_transforms2d(JceRuntime *rt)
{
	if (!rt->physics2d || !rt->scene) return;
	bool wrote_any = false;
	for (int i = 0; i < rt->body2d_count; ++i) {
		if (rt->bodies2d[i].kind == (uint8_t)JCE_BODY_STATIC) continue;
		Body2DEntry *be = &rt->bodies2d[i];
		jce_vec2 p; float angle = 0.0f;
		jce_physics2d_body_get_transform(rt->physics2d, be->body, &p, &angle);
		JceTransform *tc = jce_scene_get_transform(rt->scene, be->entity);
		if (!tc) continue;
		tc->position.x = p.x;
		tc->position.y = p.y;
		/* z preserved (2D bodies live in the XY plane). */
		tc->rotation = jce_q_from_axis_angle(jce_v3(0.0f, 0.0f, 1.0f), angle);
		jce_scene_notify_physics_writeback_entity(rt->scene, be->entity);
		wrote_any = true;
	}
	(void)wrote_any;
}

void rt_pick_primary_cam(JceScene *s, JceEntity e, void *ud)
{
	CamScanCtx *ctx = (CamScanCtx *)ud;
	if (ctx->found) return;
	JceCameraComponent *cam = jce_scene_get_camera(s, e);
	if (!cam || !cam->is_primary) return;
	if (!jce_scene_component_enabled(s, e, JCE_COMP_FLAG_CAMERA)) return;
	jce_mat4 w = jce_scene_get_world_matrix(s, e);
	ctx->pos = jce_v3(w.raw[3][0], w.raw[3][1], w.raw[3][2]);
	/* Columns 0/1/2 are the world X/Y/Z basis (possibly scaled); normalise
	 * to get pure orientation.  Engine convention: look down -Z, up = +Y. */
	jce_vec3 zaxis = jce_v3_normalize(jce_v3(w.raw[2][0], w.raw[2][1], w.raw[2][2]));
	ctx->forward = jce_v3_scale(zaxis, -1.0f);
	ctx->up      = jce_v3_normalize(jce_v3(w.raw[1][0], w.raw[1][1], w.raw[1][2]));
	ctx->found   = true;
}

/* ── Floating-origin large-world rebase (opt-in, default OFF) ─────────────
 *
 * When the scene sets rendering_settings.floating_origin_enabled, this finds
 * the primary camera's position in the current LOCAL frame; if it has wandered
 * past floating_origin_threshold metres from the origin, it rebases the world
 * by a quantized shift (jce_world_origin_update) so the camera returns toward
 * (0,0,0) and float32 transforms stay precise across very large maps.
 *
 * The rebase is applied ATOMICALLY in this one place so entities, the camera,
 * and the physics simulation never desync for a frame:
 *   1. jce_scene_apply_world_shift(scene, shift) — adds `shift` to every ROOT
 *      entity's local position (children, incl. a parented camera, follow).
 *      The primary camera is itself a scene entity, so this moves it too —
 *      the runtime owns no separate JceCamera to shift here.
 *   2. Every tracked physics body's LIVE transform AND its render-interp
 *      history (prev/cur/last) are translated by `shift`, so Bullet's bodies
 *      stay coincident with their (just-shifted) entities and the next
 *      rt_sync_transforms does not slide them across the rebase.
 *   3. The character capsule's live position + its prev/cur/last interp state
 *      are translated identically.
 * The double `origin` inside rt->world_origin absorbs the removed offset, so
 * absolute = origin + local is preserved exactly (see jce_world_origin.h).
 *
 * GATED: when floating_origin_enabled is false (default) this returns before
 * touching anything, so the frame path is byte-identical to before the feature.
 *
 * ORDERING: called once per frame AFTER rt_sync_transforms has written the
 * interpolated body poses into the scene (so camera + entities + bodies are all
 * at their final frame poses) and BEFORE scene_update, so the shifted positions
 * are what the rest of the frame (and rendering) observe.
 *
 * PRECONDITION: a physics-body entity is unparented (a root) — the SAME
 * assumption rt_sync_transforms already makes when it writes the WORLD physics
 * pose straight into the entity's LOCAL Transform.  Bullet has no transform
 * hierarchy, so this holds in practice; under it, shifting the root entity
 * (step 1) and the world-space body+interp state (steps 2/3) by the same delta
 * keeps the entity Transform and its tracked last_pos in lock-step, so the next
 * rt_push_external_transforms sees no spurious "external edit". */
static void rt_apply_floating_origin(JceRuntime *rt)
{
	if (!rt || !rt->scene) return;

	const JceSceneRenderingSettings *rs =
		jce_scene_get_rendering_settings(rt->scene);
	if (!rs || !rs->floating_origin_enabled) return;   /* opt-in gate */

	/* Re-adopt the authored threshold each frame (cheap; lets the scene tune
	 * it live).  Clamp via the constructor's rule by reusing default() only on
	 * a fresh origin would reset the accumulated offset, so set the field
	 * directly and guard non-positive thresholds here. */
	rt->world_origin.rebase_threshold =
		(rs->floating_origin_threshold > 0.0f)
			? rs->floating_origin_threshold : 1.0f;

	/* Camera position in the current local frame (= its world matrix since the
	 * scene's local origin is the float frame the world is expressed in). */
	CamScanCtx ctx = { rt->scene, { 0.0f, 0.0f, 0.0f }, false };
	jce_scene_each_entity(rt->scene, rt_pick_primary_cam, &ctx);
	if (!ctx.found) return;             /* no primary camera → nothing to base */

	float cam_local[3] = { ctx.pos.x, ctx.pos.y, ctx.pos.z };
	float shift[3];
	if (!jce_world_origin_update(&rt->world_origin, cam_local, shift))
		return;                          /* still within threshold → no rebase */

	jce_vec3 sh = jce_v3(shift[0], shift[1], shift[2]);

	/* 1. Entities (roots; children follow). */
	jce_scene_apply_world_shift(rt->scene, shift);

	/* 2. Physics bodies: live transform + interpolation history. */
	if (rt->physics) {
		for (int i = 0; i < rt->body_count; ++i) {
			BodyEntry *be = &rt->bodies[i];
			jce_vec3 p; jce_quat q;
			jce_physics_body_get_transform(rt->physics, be->body, &p, &q);
			p = jce_v3_add(p, sh);
			jce_physics_body_set_transform(rt->physics, be->body, p, q);
			be->prev_pos = jce_v3_add(be->prev_pos, sh);
			be->cur_pos  = jce_v3_add(be->cur_pos,  sh);
			be->last_pos = jce_v3_add(be->last_pos, sh);
		}

		/* 3. Character capsule + its interpolation history. */
		if (jce_character_valid(rt->character) && rt->character_entity != 0) {
			jce_vec3 center;
			jce_physics_character_get_position(rt->physics, rt->character,
			                                   &center);
			center = jce_v3_add(center, sh);
			jce_physics_character_set_position(rt->physics, rt->character,
			                                   center);
			rt->char_prev_pos = jce_v3_add(rt->char_prev_pos, sh);
			rt->char_cur_pos  = jce_v3_add(rt->char_cur_pos,  sh);
			rt->char_last_pos = jce_v3_add(rt->char_last_pos, sh);
		}
	}
}


/* ── Gameplay tick (P0-master-bridge) ────────────────────────────────
 *
 * Resolve the gameplay "viewer/observer" point: the live character
 * controller position when one exists, otherwise the primary camera's
 * world position.  Used to drive trigger overlap + spawn density. */
static jce_vec3 rt_viewer_position(JceRuntime *rt)
{
	if (rt->physics && jce_character_valid(rt->character)) {
		jce_vec3 cp;
		jce_physics_character_get_position(rt->physics, rt->character, &cp);
		return cp;
	}
	/* O(#cameras) — this fallback ran a FULL-scene walk per fixed tick AND
	 * per frame in scenes without a character controller (plain editor Play). */
	CamScanCtx ctx = { rt->scene, { 0.0f, 0.0f, 0.0f }, false };
	if (rt->scene) jce_scene_each_camera(rt->scene, rt_pick_primary_cam, &ctx);
	return ctx.pos;
}

/* ── Simulation LOD (distance-tiered gameplay tick) ──────────────────
 *
 * Per-frame KPI: how many tiered entities resolved into each tier, sampled the
 * last time rt_tick_gameplay ran.  Logged ~1/s when JCE_KPI_SIMLOD is set
 * (A/B + quantify the gameplay-tick CPU reduction; mirrors JCE_KPI_TRAVERSE).
 * Plain process-globals — single sim thread, diagnostic only. */
static int  g_simlod_near = 0, g_simlod_mid = 0, g_simlod_far = 0;
static int  g_simlod_paused = 0;   /* tiered subsystem skipped this frame (pause) */
static int  g_simlod_kpi_on = -1;  /* -1 unread, 0 off, 1 on */

/* Build the three-level distance group for a SimLod component (cached per
 * call; the radii are cheap to recompute).  near=[0,near_radius],
 * mid=(near,mid_radius], far=(mid,inf). */
static void rt_sim_lod_group(const JceSimLodComponent *sl, JceLodGroup *g)
{
	float nr = (sl->near_radius > 0.0f) ? sl->near_radius : 25.0f;
	float mr = (sl->mid_radius  > nr)   ? sl->mid_radius  : (nr + 55.0f);
	jce_lod_init(g);
	g->levels[0].mesh = NULL; g->levels[0].distance = nr;      /* tier 0 NEAR */
	g->levels[1].mesh = NULL; g->levels[1].distance = mr;      /* tier 1 MID  */
	g->levels[2].mesh = NULL; g->levels[2].distance = FLT_MAX; /* tier 2 FAR  */
	g->count = 3;
}

/* Hz -> tick period in seconds, with the sim-LOD sign convention:
 *   hz  > 0 -> 1/hz seconds, hz == 0 -> 0 (every frame), hz < 0 -> -1 (pause). */
static float rt_sim_lod_hz_to_period(float hz)
{
	if (hz < 0.0f) return -1.0f;       /* paused */
	if (hz <= 0.0f) return 0.0f;       /* every frame */
	return 1.0f / hz;
}

float rt_sim_lod_period(JceRuntime *rt, JceEntity e, jce_vec3 viewer,
                        uint32_t gate_bit, int *prev_tier)
{
	/* Read the previous tier (hysteresis state) BEFORE any early-out, then
	 * stamp the -1 "untiered" sentinel on every "full rate" exit so the KPI
	 * census in rt_sim_lod_gate can tell a genuinely tiered entity (tier
	 * 0/1/2) from one that is simply not opted in. */
	int pv = prev_tier ? *prev_tier : -1;
	if (prev_tier) *prev_tier = -1;

	if (!rt || !rt->scene) return 0.0f;
	JceSimLodComponent *sl = jce_scene_get_sim_lod(rt->scene, e);
	if (!sl || !sl->enabled) return 0.0f;          /* untiered -> full rate */

	/* Per-component disable (live editor toggle) acts like !enabled. */
	{ static int sl_cid = -2;
	  if (sl_cid == -2) sl_cid = jce_component_find("SimLod");
	  if (sl_cid >= 0 && !jce_scene_comp_enabled(rt->scene, e, sl_cid))
	      return 0.0f; }

	/* Gate bit clear -> this subsystem is never throttled for this entity. */
	uint32_t mask = sl->gate_mask ? sl->gate_mask : (uint32_t)JCE_SIMLOD_GATE_ALL;
	if (!(mask & gate_bit)) return 0.0f;

	/* Distance (XZ + Y, full 3D) entity -> viewer; classify with hysteresis. */
	jce_vec3 p = rt_world_position(rt->scene, e);
	jce_vec3 d = jce_v3(p.x - viewer.x, p.y - viewer.y, p.z - viewer.z);
	float dist = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);

	JceLodGroup g;
	rt_sim_lod_group(sl, &g);
	int tier = jce_lod_pick(&g, dist, pv);
	if (tier < 0) tier = 2;            /* past last threshold -> FAR */
	if (prev_tier) *prev_tier = tier;

	float hz = (tier == 0) ? sl->near_hz
	         : (tier == 1) ? sl->mid_hz
	                       : sl->far_hz;
	return rt_sim_lod_hz_to_period(hz);
}

/* One-stop gate used by each tiered subsystem.  Advances `*accum` by the real
 * frame dt, then decides whether to fire this frame at the entity's tier rate
 * for `gate_bit`.  On a firing frame returns true and writes the dt to FOLD
 * into the subsystem update (the accumulated time since its last tick, so the
 * logic stays time-correct at any rate) into *out_dt.  Also accumulates the
 * per-tier KPI counters the first time an entity is classified this frame
 * (driven by `count_kpi`, set only on the script pass so each entity counts
 * once).  Untiered entities (period 0) always fire with the plain frame dt. */
static bool rt_sim_lod_gate(JceRuntime *rt, JceEntity e, jce_vec3 viewer,
                            uint32_t gate_bit, float dt,
                            float *accum, int *prev_tier,
                            bool count_kpi, float *out_dt)
{
	float period = rt_sim_lod_period(rt, e, viewer, gate_bit, prev_tier);

	/* Census only genuinely tiered entities (tier 0/1/2); untiered entities
	 * carry the -1 sentinel set by rt_sim_lod_period and are not counted. */
	if (count_kpi && prev_tier) {
		int t = *prev_tier;
		if (t == 0) g_simlod_near++;
		else if (t == 1) g_simlod_mid++;
		else if (t == 2) g_simlod_far++;
	}

	if (period < 0.0f) {                /* tier paused */
		if (count_kpi) g_simlod_paused++;
		return false;
	}
	if (period == 0.0f) {              /* full rate */
		*out_dt = dt;
		return true;
	}
	*accum += dt;
	if (*accum < period) return false;
	*out_dt = *accum;                 /* fold the whole accumulated interval */
	*accum  = 0.0f;
	return true;
}

/* Advance trigger volumes, spawn managers, and weapons once per variable
 * frame.  Triggers re-test overlap against a single observer tracking the
 * viewer; spawn managers run their density/cadence state machine; weapons
 * drain fire/reload/recoil timers.  Each block is a no-op when the scene
 * authored no matching component (the subsystem stays NULL/empty). */
static void rt_tick_gameplay(JceRuntime *rt, float dt)
{
	/* rt_viewer_position ALREADY returns the live character-controller physics
	 * position when a character exists (only falling back to the primary camera
	 * for camera-only scenes), so the trigger observer correctly follows the
	 * walking player.  (Do NOT substitute the entity's raw Transform here: it is
	 * only written on gizmo edits, not by physics movement, so it stays at the
	 * spawn point during Play and the observer never reaches the zone.) */
	jce_vec3 viewer = rt_viewer_position(rt);

	/* Trigger volumes: keep one observer at the viewer, re-test overlap. */
	if (rt->trigger_world) {
		if (!rt->trigger_player_valid) {
			rt->trigger_player = jce_observer_add(rt->trigger_world, viewer, 1u);
			rt->trigger_player_valid = jce_observer_valid(rt->trigger_player);
		} else {
			jce_observer_set_position(rt->trigger_world, rt->trigger_player,
			                          viewer);
		}
		jce_trigger_world_update(rt->trigger_world);
	}

	/* Spawn managers: viewer-relative density pop-in. */
	rt->actor_spawned_frame = 0;   /* reset per-frame spawn quota (actor budget) */
	for (int i = 0; i < rt->spawn_count; ++i) {
		if (!rt->spawns[i].mgr) continue;
		jce_spawn_manager_set_viewer(rt->spawns[i].mgr, viewer);
		/* The on_create/on_destroy callbacks fire synchronously inside update;
		 * publish which manager is active so they resolve its ped prefab. */
		rt->cur_spawn_mgr = rt->spawns[i].entity;
		jce_spawn_manager_update(rt->spawns[i].mgr, dt);
	}

	/* Weapons: advance cooldown / reload / recoil-recovery timers.  Aim is
	 * the viewer looking forward; trigger is not held by the runtime, so no
	 * shots are produced here — games pull the trigger + consume shots via
	 * the scene component / their own JceWeaponInstance. */
	if (rt->weapon_count > 0) {
		jce_vec3 aim_dir = jce_v3(0.0f, 0.0f, -1.0f);
		for (int i = 0; i < rt->weapon_count; ++i) {
			WeaponEntry *we = &rt->weapons[i];
			JceWeaponShot shots[8];
			jce_weapon_update(&we->inst, &we->arch, viewer, aim_dir,
			                  (uint64_t)we->entity, (uint32_t)we->entity,
			                  dt, shots, 8, &we->rng);
		}
	}

	/* Nav-agents: sync authored components into the set, advance every agent
	 * along its path (seek/arrive through the Recast backend set at create()),
	 * then write the steered positions back to the entity transforms.  Empty
	 * set / navmesh-less scenes stay a cheap no-op. */
	if (rt->nav_agents && rt->nav_entry_count > 0 && rt->scene) {
		/* Pre-update sync: re-read each component every tick (flecs table
		 * moves invalidate cached component pointers); honour live enabled
		 * flips and re-issue the destination when auto_repath sees the goal
		 * move by more than the waypoint radius. */
		for (int i = 0; i < rt->nav_entry_count; ++i) {
			NavAgentEntry *ne = &rt->nav_entries[i];
			JceNavAgentComponent *nac = jce_scene_get_nav_agent(rt->scene,
			                                                    ne->entity);
			if (!nac) continue;
			/* Per-component disable (live toggle) acts like !enabled: stop + skip. */
			bool na_on = nac->enabled;
			{ static int na_cid = -2;
			  if (na_cid == -2) na_cid = jce_component_find("NavAgent");
			  if (na_cid >= 0 && !jce_scene_comp_enabled(rt->scene, ne->entity, na_cid))
			      na_on = false; }
			if (!na_on) {
				if (ne->has_dest) {
					jce_nav_agent_stop(rt->nav_agents, ne->handle);
					ne->has_dest = false;
				}
				continue;
			}
			/* Simulation-LOD: throttle the per-agent goal-tracking + repath
			 * decision (the expensive per-entity nav work: target world-pos
			 * lookups, distance math, path re-find) to this agent's tier rate.
			 * The shared crowd integrator (jce_nav_agent_set_update) and the
			 * write-back still run every frame, so a FAR agent keeps gliding
			 * toward its last destination instead of freezing mid-stride — it
			 * just re-targets a moving player less often.  Untiered agents fold
			 * dt==frame and behave exactly as before. */
			float na_dt;
			if (!rt_sim_lod_gate(rt, ne->entity, viewer,
			                     JCE_SIMLOD_GATE_NAV, dt,
			                     &ne->simlod_accum, &ne->simlod_prev_tier,
			                     false, &na_dt))
				continue;   /* not this agent's turn to re-plan this frame */
			float gx, gz;
			if (nac->target_entity != 0) {
				jce_vec3 tp = rt_world_position(rt->scene,
				                                (JceEntity)nac->target_entity);
				gx = tp.x; gz = tp.z;
			} else {
				gx = nac->target[0]; gz = nac->target[2];
			}
			float wr = nac->waypoint_radius > 0.0f ? nac->waypoint_radius : 0.5f;
			float dx = gx - ne->last_goal_x;
			float dz = gz - ne->last_goal_z;
			bool  moved = (dx * dx + dz * dz) > wr * wr;
			if (!ne->has_dest || (nac->auto_repath && moved)) {
				ne->has_dest    = jce_nav_agent_set_destination(rt->nav_agents,
				                                                ne->handle,
				                                                gx, gz);
				ne->last_goal_x = gx;
				ne->last_goal_z = gz;
			}
		}

		jce_nav_agent_set_update(rt->nav_agents, dt);

		/* Write-back: steered XZ into the entity transform; Y snaps to the
		 * navmesh surface when a polygon is near, else it is preserved.
		 * Entities driven by a dynamic rigid body are skipped — the physics
		 * write-back (rt_sync_transforms) owns their transform. */
		for (int i = 0; i < rt->nav_entry_count; ++i) {
			NavAgentEntry *ne = &rt->nav_entries[i];
			JceNavAgentComponent *nac = jce_scene_get_nav_agent(rt->scene,
			                                                    ne->entity);
			if (!nac || !nac->enabled) continue;
			{ static int na_cid2 = -2;
			  if (na_cid2 == -2) na_cid2 = jce_component_find("NavAgent");
			  if (na_cid2 >= 0 && !jce_scene_comp_enabled(rt->scene, ne->entity, na_cid2)) continue; }
			bool body_driven = false;
			for (int j = 0; j < rt->body_count; ++j) {
				if (rt->bodies[j].entity == ne->entity &&
				    rt->bodies[j].kind != (uint8_t)JCE_BODY_STATIC) {
					body_driven = true;
					break;
				}
			}
			if (body_driven) continue;
			/* The agent's position and the navmesh snap are WORLD metres --
			 * the crowd solver and the navmesh have no idea this entity has
			 * a parent.  Writing them into the raw Transform put an agent
			 * parented to anything at its parent's offset PLUS the solver's
			 * answer, so it walked a correct path in the wrong place.  Start
			 * from the entity's current world pose so the untouched axis (y,
			 * when there is no navmesh to snap to) keeps its world value
			 * rather than being read as a local one. */
			JceTransform *tc = jce_scene_get_transform(rt->scene, ne->entity);
			jce_vec3 wp;
			jce_quat wr;
			if (!tc || !jce_scene_get_world_pose(rt->scene, ne->entity, &wp,
			                                     &wr, NULL))
				continue;
			float x, z;
			jce_nav_agent_get_position(rt->nav_agents, ne->handle, &x, &z);
			wp.x = x;
			wp.z = z;
			float sx, sy, sz;
			if (rt->nav_recast &&
			    jce_recast_snap_to_navmesh(rt->nav_recast, x, z, &sx, &sy, &sz))
				wp.y = sy;
			/* In place + notify, the established write-back shape. */
			jce_scene_solve_local_pose(rt->scene, ne->entity, wp, wr,
			                           &tc->position, NULL);
			/* Name the entity for the renderer's incremental repair (was
			 * covered only by the physics blanket overflow). */
			jce_scene_notify_physics_writeback_entity(rt->scene, ne->entity);
		}
	} else if (rt->nav_agents && jce_nav_agent_set_count(rt->nav_agents) > 0) {
		/* Agents added directly through the API (no scene entries). */
		jce_nav_agent_set_update(rt->nav_agents, dt);
	}

	/* Behavior trees (P2-perception-bt-binding): for every agent that loaded
	 * a tree, run a perception pass (sight-cone + LOS raycast + hearing) for
	 * the player/viewer as the sole sensing candidate, writing stimuli into
	 * the agent's blackboard, then tick its tree on its cadence.  The bundled
	 * blackboard-reading actions consult rt->bt_active_bb during the tick.
	 * Empty (no authored BehaviorTree component) → cheap no-op. */
	if (rt->bt_count > 0) {
		/* Build the candidate set once: the player/viewer (loudness rises while
		 * moving so the hearing path is demonstrable) plus every active BT
		 * agent, so agents can perceive one another — not only the player. */
		enum { RT_MAX_PERCEPT = 64 };
		JcePerceptionTarget cand[RT_MAX_PERCEPT];
		int ncand = 0;
		{
			float walk = fabsf(rt->input.walk_x) + fabsf(rt->input.walk_z);
			cand[ncand].entity   = (uint64_t)rt->character_entity;
			cand[ncand].position = viewer;
			cand[ncand].loudness = (walk > 0.01f) ? 12.0f : 0.0f;
			ncand++;
		}
		for (int j = 0; j < rt->bt_count && ncand < RT_MAX_PERCEPT; ++j) {
			struct BtEntry *bj = &rt->bts[j];
			if (!bj->active) continue;
			cand[ncand].entity   = (uint64_t)bj->entity;
			cand[ncand].position = rt_world_position(rt->scene, bj->entity);
			cand[ncand].loudness = 0.0f;   /* agents are silent unless modelled */
			ncand++;
		}

		for (int i = 0; i < rt->bt_count; ++i) {
			struct BtEntry *be = &rt->bts[i];
			if (!be->active || !jce_bt_tree_valid(be->tree)) continue;

			/* Effective tick dt fed to the tree's env (Wait/Cooldown timers).
			 * Defaults to the authored cadence (or dt). */
			float bt_eff_dt = (be->tick_period > 0.0f) ? be->tick_period : dt;

			/* Simulation-LOD gate (takes precedence over the authored tick_hz
			 * when the entity carries a SimLod component gating BehaviorTree):
			 * skip this frame unless the tier rate fires; fold the accumulated
			 * dt so perception/timers stay time-correct.  Returns 0 (full rate)
			 * for untiered agents, leaving the authored cadence gate below in
			 * charge — byte-identical to before for anything without SimLod. */
			float bt_sl_period =
				rt_sim_lod_period(rt, be->entity, viewer,
				                  JCE_SIMLOD_GATE_BT, &be->simlod_prev_tier);
			if (bt_sl_period < 0.0f) continue;          /* tier paused */
			if (bt_sl_period > 0.0f) {
				be->simlod_accum += dt;
				if (be->simlod_accum < bt_sl_period) continue;
				bt_eff_dt = be->simlod_accum;           /* fold whole interval */
				be->simlod_accum = 0.0f;
			} else {
				/* Untiered (or near tier @ every-frame): keep the authored
				 * cadence gate exactly as before. */
				if (be->tick_period > 0.0f) {
					be->tick_accum += dt;
					if (be->tick_accum < be->tick_period) continue;
					be->tick_accum -= be->tick_period;
				}
			}

			/* Agent eye = its entity world position; forward = its -Z basis. */
			JcePerceptionAgent ag;
			memset(&ag, 0, sizeof ag);
			ag.eye_position     = rt_world_position(rt->scene, be->entity);
			if (rt->scene && jce_scene_has_transform(rt->scene, be->entity)) {
				jce_mat4 w = jce_scene_get_world_matrix(rt->scene, be->entity);
				ag.forward = jce_v3_normalize(jce_v3_scale(
					jce_v3(w.raw[2][0], w.raw[2][1], w.raw[2][2]), -1.0f));
			} else {
				ag.forward = jce_v3(0.0f, 0.0f, -1.0f);
			}
			ag.sight_range      = be->sight_range;
			ag.sight_half_angle = be->sight_half_angle;
			ag.hearing_range    = be->hearing_range;

			/* Targets = all candidates except this agent itself. */
			JcePerceptionTarget tlist[RT_MAX_PERCEPT];
			uint32_t tn = 0;
			for (int k = 0; k < ncand; ++k) {
				if (cand[k].entity == (uint64_t)be->entity) continue;
				tlist[tn++] = cand[k];
			}
			jce_perception_update(be->bb, &ag, tlist, tn,
			                      rt_bt_los_blocked, rt, NULL);

			/* Point the bundled actions at this agent's blackboard, supply the
			 * deterministic per-tick env (dt + nav-move hook) the bundled
			 * library reads, then tick. */
			rt->bt_active_bb     = be->bb;
			rt->bt_active_entity = be->entity;
			{
				JceBtTickEnv env;
				env.bb            = be->bb;
				/* Advance library timers by the EFFECTIVE tick dt (folded sim-LOD
				 * interval, authored tick period, or frame dt) so Wait/Cooldown
				 * stay consistent with the cadence the tree is actually ticked at. */
				env.dt            = bt_eff_dt;
				env.move_to       = rt_bt_move_to;
				env.move_userdata = rt;
				jce_bt_set_env(rt->bt_ctx, &env);
			}
			jce_bt_tick(rt->bt_ctx, be->tree);
			jce_bt_set_env(rt->bt_ctx, NULL);
			rt->bt_active_bb     = NULL;
			rt->bt_active_entity = 0;
		}
	}

	/* ── Gameplay Ability Systems (GAS consumption last-mile): advance every
	 * live system by the (already time-scaled) frame dt BEFORE scripts run, so
	 * cooldowns have drained and timed effects/periodic ticks are current when
	 * a script's on_update queries or activates an ability this frame. Empty
	 * (no authored GAS component) -> a cheap no-op. */
	for (int i = 0; i < rt->gas_count; ++i) {
		struct GasEntry *ge = &rt->gas_entries[i];
		jce_gas_tick(&ge->gas, dt);

		/* GAS attribute replication (F12 last-mile): only entities carrying a
		 * NetworkObject ever replicate.  When the net bridge is up and the
		 * entity is a net object, the AUTHORITY (server, or owning client)
		 * fills the packed replica from its live GAS — the substrate then
		 * ships the bytes on the next snapshot; a REMOTE (no-authority) peer
		 * instead pulls the server-authoritative values (already decoded into
		 * the component by the substrate read serializer) back into its local
		 * GAS so it does not fight the server.  No NetworkObject -> net_id is
		 * INVALID -> both branches skipped -> byte-identical to before. */
		if (rt->net_bridged) {
			JceNetObjectId nid =
				jce_net_object_from_entity((uint64_t)ge->entity);
			if (nid != JCE_NET_OBJECT_INVALID) {
				if (jce_net_object_has_authority(nid))
					jce_gas_replication_fill_from_gas((uint64_t)ge->entity,
					                                  &ge->gas);
				else
					jce_gas_replication_apply_to_gas((uint64_t)ge->entity,
					                                 &ge->gas);
			}
		}
	}

	/* ── Ragdoll blend update (scene-pass last-mile): pull the live
	 * JceRagdoll component's blend_weight into each entry + the live ragdoll
	 * each tick so a death trigger (or any script) can collapse / restore the
	 * ragdoll at runtime.  Gated on ragdoll_count -> a cheap no-op. */
	for (int i = 0; i < rt->ragdoll_count; ++i) {
		struct RagdollEntry *re = &rt->ragdoll_entries[i];
		if (!re->rd) continue;
		JceRagdollComponent *rc = jce_scene_get_ragdoll(rt->scene, re->entity);
		if (!rc) continue;
		float bw = rc->blend_weight;
		if (bw < 0.0f) bw = 0.0f;
		if (bw > 1.0f) bw = 1.0f;
		re->blend_weight = bw;
		jce_ragdoll_set_blend_weight(re->rd, bw);
	}

	/* ── Gameplay scripts (Phase 0 keystone): on_update every active
	 * instance with the (already time-scaled by the caller) frame dt. */
	if (rt->script_enabled) {
		/* Hot-reload poll (~every 30 ticks ≈ 0.5 s @60 Hz): cheap mtime stat
		 * per watched script; fires rt_on_script_changed synchronously (which
		 * only rebinds instances in place — it never mutates scripts[], so it
		 * is safe to run right before the update loop). */
		if (rt->script_watcher) {
			if (++rt->script_reload_frame >= 30) {
				rt->script_reload_frame = 0;
				jce_file_watcher_poll(rt->script_watcher);
			}
		}
		/* Read ONCE per frame, not per script: a per-script read of the
		 * profiling gate would be the very overhead the gate exists to
		 * avoid. */
		const bool prof = (jce_perf_phase_enabled() != 0);
		double script_ms_total = 0.0;
		for (int i = 0; i < rt->script_count; ++i) {
			if (!rt->scripts[i].active) continue;
			/* Simulation-LOD gate: a tiered entity's on_update fires only at its
			 * active tier's Hz, folding the accumulated dt so the script sees a
			 * time-correct delta (movement/cooldowns stay right at any rate).
			 * This is also the per-frame entity census: count_kpi=true tallies
			 * each tiered entity into its tier exactly once.  Untiered entities
			 * (no SimLod / disabled / Script gate clear) get period 0 -> fire
			 * every frame with the plain dt — byte-identical to before. */
			float s_dt;
			if (!rt_sim_lod_gate(rt, rt->scripts[i].entity, viewer,
			                     JCE_SIMLOD_GATE_SCRIPT, dt,
			                     &rt->scripts[i].simlod_accum,
			                     &rt->scripts[i].simlod_prev_tier,
			                     true, &s_dt))
				continue;
			if (prof) {
				const uint64_t _t0 = jce_time_perf_counter();
				rt_script_ref_update(rt->scripts[i].ref, s_dt);
				const double ms = jce_time_perf_to_ms(
					_t0, jce_time_perf_counter());
				rt->scripts[i].cost_last_ms   = ms;
				rt->scripts[i].cost_total_ms += ms;
				rt->scripts[i].cost_calls++;
				rt->scripts[i].cost_measured  = true;
				script_ms_total += ms;
			} else {
				rt_script_ref_update(rt->scripts[i].ref, s_dt);
			}
		}
		/* The aggregate, so "is scripting costing me anything" is answerable
		 * from the CPU-phase table the profiler already draws.  Until this
		 * existed on_update was inside the `gameplay` bucket together with
		 * trigger overlap, spawn density, GAS replication, ragdoll blending
		 * and weapon timers -- one number for six things. */
		if (prof)
			jce_perf_phase_add("script", script_ms_total);
		/* Advance cooperative coroutines (jce.start_coroutine / wait_seconds)
		 * with the same time-scaled dt the per-instance updates saw.  These are
		 * shared cooperative timers, not per-entity, so they stay every-frame. */
		/* Every live language advances its own cooperative timers.  A
		 * scene with bob.lua and turret.py has two VMs and both must
		 * tick, or one language's jce.wait_seconds never resumes. */
		for (int i = 0; i < rt->script_lang_count; ++i)
			jce_script_update_coroutines(rt->script_langs[i].vm, dt);
	}

	/* ── Simulation-LOD KPI (diagnostic) ─────────────────────────────
	 * When JCE_KPI_SIMLOD is set, log the per-tier entity census ~1/s so an
	 * A/B traverse can quantify the gameplay-tick reduction (most far entities
	 * at 1 Hz instead of 60).  Counters were accumulated by the script pass
	 * above (each tiered entity counted once); reset for the next frame. */
	if (g_simlod_kpi_on < 0) {
		const char *kv = getenv("JCE_KPI_SIMLOD");
		g_simlod_kpi_on = (kv && kv[0] && kv[0] != '0') ? 1 : 0;
	}
	if (g_simlod_kpi_on) {
		static double accT = 0.0;
		accT += (double)dt;
		if (accT >= 1.0) {
			accT = 0.0;
			int tiered = g_simlod_near + g_simlod_mid + g_simlod_far;
			LOG_INFO(LOG_TAG,
			    "sim-LOD census: tiered=%d near(full)=%d mid=%d far=%d paused=%d "
			    "scripts=%d bts=%d navs=%d",
			    tiered, g_simlod_near, g_simlod_mid, g_simlod_far,
			    g_simlod_paused, rt->script_count, rt->bt_count,
			    rt->nav_entry_count);
		}
	}
	g_simlod_near = g_simlod_mid = g_simlod_far = 0;
	g_simlod_paused = 0;
}


/* Wire the entities queued by rt_script_spawn (physics body + gameplay/scripts).
 * Runs after the script update loop so rt_spawn_gameplay's append to scripts[]
 * is safe.  Processes only the batch present on entry; entities enqueued by a
 * spawned entity's own on_start are shifted down and flushed next frame (bounds
 * per-frame work + prevents a spawn storm from stalling the step). */
static void rt_flush_pending_spawns(JceRuntime *rt)
{
	int n = rt->pending_spawn_count;
	if (n <= 0) return;
	for (int i = 0; i < n; ++i) {
		JceEntity e = rt->pending_spawns[i];   /* re-read: array may realloc */
		rt_spawn_entity(rt->scene, e, rt);
		rt_spawn_gameplay(rt->scene, e, rt);
	}
	/* Entities appended during wiring (nested spawns) sit at [n, count). */
	int remain = rt->pending_spawn_count - n;
	if (remain > 0)
		memmove(rt->pending_spawns, rt->pending_spawns + n,
		        (size_t)remain * sizeof(JceEntity));
	rt->pending_spawn_count = (remain > 0) ? remain : 0;
}

/* ── Streamed-cell gameplay wiring (streaming M3) ─────────────────────
 *
 * Wire / unwire ONE entity's gameplay into / out of the live runtime, so a
 * world-streamer chunk's freshly-spawned entities come alive (scripts, triggers,
 * NPCs, bodies) and an unloaded chunk's entities release every runtime-side
 * reference before the streamer destroys the scene entity.
 *
 * SPAWN reuses the EXACT per-entity wiring rt_spawn_gameplay / the jce.spawn
 * flush path run for a single id (rt_spawn_entity → body/character/audio;
 * rt_spawn_gameplay → trigger/spawner/weapon/save-point/BT/script+on_start/GAS/
 * ragdoll/nav).  No parallel logic — just the two existing walk callbacks fed
 * one id at a time, exactly as rt_flush_pending_spawns does.
 *
 * DESPAWN is the inverse: it RELEASES each tracked per-entity handle, mirroring
 * what rt_teardown_scene_state does in bulk but for ONE id, using swap-remove on
 * every per-scene array.  Order matches teardown's double-free contract: the
 * ragdoll / vehicle / cfg-joint / 2D-joint handles that live inside a physics
 * world are destroyed via their own API; the plain rigid body is destroyed via
 * jce_physics_body_destroy.  Releasing a script instance fires its on_destroy. */

/* Wire ONE streamed entity's gameplay into the live runtime. */
static void rt_wire_entity_gameplay(JceRuntime *rt, JceEntity e)
{
	if (!rt || !rt->scene || e == 0) return;
	/* Bodies / character / voices, then triggers / spawners / scripts+on_start /
	 * BT / nav / GAS / ragdoll — the same two passes the create() walk runs, but
	 * for this single id.  in_scene_walk stays false (set by the caller) so a
	 * script's on_start may legally jce.spawn. */
	rt_spawn_entity(rt->scene, e, rt);
	rt_spawn_gameplay(rt->scene, e, rt);
}

/* Release ONE entity's gameplay from the live runtime (cell unload / pre-destroy).
 * Every block is presence-gated by a linear scan + swap-remove, so an id with no
 * tracked state of a given kind is a no-op.  Must run while `e` is still a valid
 * scene entity (the streamer fires on_despawn BEFORE destroying it). */
static void rt_unwire_entity_gameplay(JceRuntime *rt, JceEntity e)
{
	if (!rt || e == 0) return;

	/* Gameplay-script instance — release fires on_destroy (per-instance self
	 * state freed), same as the bulk teardown.  Removing it from scripts[] stops
	 * on_update / on_collision / broadcast from ever touching the dead id. */
	for (int i = 0; i < rt->script_count; ++i) {
		if (rt->scripts[i].entity != e) continue;
		if (rt->scripts[i].active)
			rt_script_ref_release(rt->scripts[i].ref);
		rt->scripts[i] = rt->scripts[--rt->script_count];
		break;   /* one instance per entity */
	}

	/* Behavior-tree agent — halt the tree on the (reusable) context + free the
	 * per-agent blackboard, mirroring teardown. */
	for (int i = 0; i < rt->bt_count; ++i) {
		if (rt->bts[i].entity != e) continue;
		if (rt->bt_ctx && jce_bt_tree_valid(rt->bts[i].tree))
			jce_bt_halt(rt->bt_ctx, rt->bts[i].tree);
		if (rt->bts[i].bb)
			jce_blackboard_destroy(rt->bts[i].bb);
		rt->bts[i] = rt->bts[--rt->bt_count];
		break;
	}

	/* Nav agent — remove from the agent set. */
	if (rt->nav_agents) {
		for (int i = 0; i < rt->nav_entry_count; ++i) {
			if (rt->nav_entries[i].entity != e) continue;
			if (jce_nav_agent_valid(rt->nav_entries[i].handle))
				jce_nav_agent_remove(rt->nav_agents, rt->nav_entries[i].handle);
			rt->nav_entries[i] = rt->nav_entries[--rt->nav_entry_count];
			break;
		}
	}

	/* Trigger volume observer — remove from the trigger world. */
	if (rt->trigger_world) {
		for (int i = 0; i < rt->trigger_count; ++i) {
			if (rt->triggers[i].entity != e) continue;
			if (jce_trigger_valid(rt->triggers[i].handle))
				jce_trigger_remove(rt->trigger_world, rt->triggers[i].handle);
			rt->triggers[i] = rt->triggers[--rt->trigger_count];
			break;
		}
		/* Save-point trigger (also in trigger_world). */
		for (int i = 0; i < rt->save_point_count; ++i) {
			if (rt->save_points[i].entity != e) continue;
			if (jce_trigger_valid(rt->save_points[i].handle))
				jce_trigger_remove(rt->trigger_world, rt->save_points[i].handle);
			rt->save_points[i] = rt->save_points[--rt->save_point_count];
			break;
		}
	}

	/* Spawn manager — owned, destroy it. */
	for (int i = 0; i < rt->spawn_count; ++i) {
		if (rt->spawns[i].entity != e) continue;
		if (rt->spawns[i].mgr) jce_spawn_manager_destroy(rt->spawns[i].mgr);
		rt->spawns[i] = rt->spawns[--rt->spawn_count];
		break;
	}

	/* Weapon — POD fire-control state, just drop it. */
	for (int i = 0; i < rt->weapon_count; ++i) {
		if (rt->weapons[i].entity != e) continue;
		rt->weapons[i] = rt->weapons[--rt->weapon_count];
		break;
	}

	/* Live ability system — embedded POD, just drop it. */
	for (int i = 0; i < rt->gas_count; ++i) {
		if (rt->gas_entries[i].entity != e) continue;
		rt->gas_entries[i] = rt->gas_entries[--rt->gas_count];
		break;
	}

	/* Ragdoll — destroy the body chain (in rt->physics) BEFORE the owned model,
	 * same ordering contract as teardown.  Must precede the rigid-body destroy
	 * below only in the sense that both happen before jce_physics_destroy; here
	 * we destroy individual handles so order between them is irrelevant. */
	for (int i = 0; i < rt->ragdoll_count; ++i) {
		if (rt->ragdoll_entries[i].entity != e) continue;
		if (rt->ragdoll_entries[i].rd)    jce_ragdoll_destroy(rt->ragdoll_entries[i].rd);
		if (rt->ragdoll_entries[i].model) jce_model_destroy(rt->ragdoll_entries[i].model);
		rt->ragdoll_entries[i] = rt->ragdoll_entries[--rt->ragdoll_count];
		break;
	}

	/* Raycast vehicle — owns a chassis body in rt->physics. */
	if (rt->physics) {
		for (int i = 0; i < rt->vehicle_count; ++i) {
			if (rt->vehicles[i].entity != e) continue;
			if (jce_vehicle_valid(rt->vehicles[i].veh))
				jce_physics_vehicle_destroy(rt->physics, rt->vehicles[i].veh);
			rt->vehicles[i] = rt->vehicles[--rt->vehicle_count];
			break;
		}
		/* Typed constraint — constraint in rt->physics.  Dropped rather
		 * than destroyed for the same reason the configurable one is
		 * destroyed: that one may need to break early, this one never
		 * does, and jce_physics_destroy frees both. */
		for (int i = 0; i < rt->joint_count; ++i) {
			if (rt->joints[i].entity != e) continue;
			if (jce_constraint_valid(rt->joints[i].handle))
				jce_physics_constraint_destroy(rt->physics,
				                               rt->joints[i].handle);
			rt->joints[i] = rt->joints[--rt->joint_count];
			break;
		}
		/* Configurable joint — constraint in rt->physics. */
		for (int i = 0; i < rt->cfg_joint_count; ++i) {
			if (rt->cfg_joints[i].entity != e) continue;
			if (jce_constraint_valid(rt->cfg_joints[i].handle))
				jce_physics_constraint_destroy(rt->physics,
				                               rt->cfg_joints[i].handle);
			rt->cfg_joints[i] = rt->cfg_joints[--rt->cfg_joint_count];
			break;
		}
	}

	/* Soft body — lives in the SHARED soft world (independent of rt->physics). */
	for (int i = 0; i < rt->softbody_count; ++i) {
		if (rt->softbodies[i].entity != e) continue;
		jce_softbody_destroy(rt->softbodies[i].handle);
		rt->softbodies[i] = rt->softbodies[--rt->softbody_count];
		break;
	}

	/* 2D joint — joint in rt->physics2d (destroy before the 2D body below). */
	if (rt->physics2d) {
		for (int i = 0; i < rt->joint2d_count; ++i) {
			if (rt->joints2d[i].entity != e) continue;
			if (jce_constraint_valid(rt->joints2d[i].handle))
				jce_physics2d_joint_destroy(rt->physics2d,
				                            rt->joints2d[i].handle);
			rt->joints2d[i] = rt->joints2d[--rt->joint2d_count];
			break;
		}
		/* 2D rigid body. */
		for (int i = 0; i < rt->body2d_count; ++i) {
			if (rt->bodies2d[i].entity != e) continue;
			if (jce_body_valid(rt->bodies2d[i].body))
				jce_physics2d_body_destroy(rt->physics2d, rt->bodies2d[i].body);
			rt->bodies2d[i] = rt->bodies2d[--rt->body2d_count];
			break;
		}
	}

	/* 3D rigid body / collider (compound / mesh / terrain / plain box). */
	if (rt->physics) {
		for (int i = 0; i < rt->body_count; ++i) {
			if (rt->bodies[i].entity != e) continue;
			if (jce_body_valid(rt->bodies[i].body))
				jce_physics_body_destroy(rt->physics, rt->bodies[i].body);
			rt->bodies[i] = rt->bodies[--rt->body_count];
			break;
		}
		/* Character controller — only one per scene, but a streamed cell could in
		 * principle carry one; release it so it does not dangle on the dead id. */
		if (rt->character_entity == e && jce_character_valid(rt->character)) {
			jce_physics_character_destroy(rt->physics, rt->character);
			rt->character        = JCE_CHARACTER_INVALID;
			rt->character_entity = 0;
			rt->char_yaw_valid   = false;
		}
	}

	/* Draw-distance deferred static collider record (its body, if any, was just
	 * destroyed above as a normal body; drop the bookkeeping entry). */
	for (int i = 0; i < rt->dd_count; ++i) {
		if (rt->dd[i].entity != e) continue;
		rt->dd[i] = rt->dd[--rt->dd_count];
		break;
	}

	/* Audio voices belonging to this entity's AudioSource(s). */
	if (rt->audio) {
		for (int i = 0; i < rt->voice_count; ) {
			if (rt->voices[i].entity != e) { ++i; continue; }
			jce_audio_stop(rt->audio, rt->voices[i].voice);
			jce_audio_unload(rt->audio, rt->voices[i].sound);
			rt->voices[i] = rt->voices[--rt->voice_count];
			/* no ++i: re-test the swapped-in entry (an entity may own >1 voice) */
		}
	}

	/* Drop any not-yet-flushed pending spawn for this id (a cell entity queued by
	 * an in-cell script's jce.spawn but not yet wired) so the flush never wires a
	 * dead id. */
	for (int i = 0; i < rt->pending_spawn_count; ) {
		if (rt->pending_spawns[i] != e) { ++i; continue; }
		rt->pending_spawns[i] =
			rt->pending_spawns[--rt->pending_spawn_count];
	}
	/* Drop any queued fracture for this id. */
	for (int i = 0; i < rt->pending_fracture_count; ) {
		if (rt->pending_fractures[i] != e) { ++i; continue; }
		rt->pending_fractures[i] =
			rt->pending_fractures[--rt->pending_fracture_count];
	}
}

JCE_API void JCE_CALL jce_runtime_spawn_gameplay_for_ids(JceRuntime *rt,
                                                         const uint64_t *ids,
                                                         uint32_t count)
{
	if (!rt || !ids || count == 0) return;
	/* Not the initial create() walk: a wired script's on_start may jce.spawn. */
	bool prev_walk = rt->in_scene_walk;
	rt->in_scene_walk = false;
	for (uint32_t i = 0; i < count; ++i)
		rt_wire_entity_gameplay(rt, (JceEntity)ids[i]);
	rt->in_scene_walk = prev_walk;
}

JCE_API void JCE_CALL jce_runtime_despawn_gameplay_for_ids(JceRuntime *rt,
                                                           const uint64_t *ids,
                                                           uint32_t count)
{
	if (!rt || !ids || count == 0) return;
	for (uint32_t i = 0; i < count; ++i)
		rt_unwire_entity_gameplay(rt, (JceEntity)ids[i]);
}

/* ── Per-scene state lifecycle (FEATURE 9.4) ──────────────────────────
 *
 * The runtime keeps two kinds of state: REUSABLE infrastructure that
 * outlives any one scene (the audio device — not owned — plus the script
 * VM, the BT action context, the snapshot registry, the audio mixer) and
 * PER-SCENE state that is materialised from the authored scene contents
 * (physics worlds + every tracked body, the character controller, audio
 * voices, triggers / spawners / weapons / behavior-tree agents / script
 * instances / nav agents / save points / reverb zones / occlusion tracker).
 *
 * A scene transition releases ONLY the per-scene state below and rebuilds
 * it for the new scene via rt_spawn_scene_state; the reusable infrastructure
 * is kept so a level swap doesn't reinitialise the audio engine or the
 * scripting VM.  jce_runtime_destroy calls rt_teardown_scene_state too (so
 * the teardown lives in one place) and then frees the reusable infra.
 *
 * After this returns, every per-scene array is empty (count==0, capacity
 * retained for reuse) and physics is NULL — ready for rt_spawn_scene_state
 * or final free. */
static void rt_teardown_scene_state(JceRuntime *rt, bool destroying)
{
	/* Cancel in-flight structured audio decodes and drop their results. */
	for (int i = 0; i < rt->pending_audio_count; ++i) {
		RtPendingAudio *p = &rt->pending_audio[i];
		if (p->task) {
			(void)jce_async_task_discard(p->task);
			p->task = NULL;
		}
		if (p->args) {
			jce_audio_cpu_free(p->args->cpu);
			jce_free(p->args);
		}
	}
	rt->pending_audio_count = 0;

	/* Audio voices (sounds belong to this scene's AudioSources). */
	if (rt->audio) {
		for (int i = 0; i < rt->voice_count; ++i)
			jce_audio_stop(rt->audio, rt->voices[i].voice);
		for (int i = 0; i < rt->voice_count; ++i)
			jce_audio_unload(rt->audio, rt->voices[i].sound);
	}
	rt->voice_count = 0;
	if (rt->occ_tracker) {
		jce_audio_occlusion_tracker_destroy(rt->occ_tracker);
		rt->occ_tracker = NULL;
	}
	/* Reverb zones are rebuilt from the new scene; drop the old set. */
	if (rt->reverb_zones) {
		jce_reverb_zones_destroy(rt->reverb_zones);
		rt->reverb_zones = NULL;
	}
	/* Adaptive music director is per-scene; destroy BEFORE the (reusable)
	 * mixer it borrows + drives, which is freed only in jce_runtime_destroy
	 * after this teardown runs. */
	if (rt->music) {
		jce_music_destroy(rt->music);
		rt->music = NULL;
	}

	/* Ragdolls (scene-pass last-mile): destroy each live ragdoll (its bodies +
	 * constraints live in rt->physics) BEFORE jce_physics_destroy, then the
	 * owned model that kept the borrowed skeleton alive.  HIGHEST-SEVERITY
	 * ORDERING: jce_physics_destroy frees ALL bodies including the ragdoll's;
	 * if jce_ragdoll_destroy ran AFTER it, those handles would be double-freed.
	 * So ragdoll teardown MUST precede the physics-world teardown below — the
	 * same contract as the character capsule. */
	for (int i = 0; i < rt->ragdoll_count; ++i) {
		struct RagdollEntry *re = &rt->ragdoll_entries[i];
		if (re->rd)    jce_ragdoll_destroy(re->rd);
		if (re->model) jce_model_destroy(re->model);
		re->rd    = NULL;
		re->model = NULL;
	}
	rt->ragdoll_count = 0;

	/* Vehicles (VEHICLE last-mile): each owns a chassis body inside rt->physics.
	 * Destroy them BEFORE jce_physics_destroy below — same double-free contract
	 * as the character capsule + ragdolls (the world teardown frees ALL bodies,
	 * which would otherwise double-free the chassis). */
	if (rt->physics) {
		for (int i = 0; i < rt->vehicle_count; ++i)
			if (jce_vehicle_valid(rt->vehicles[i].veh))
				jce_physics_vehicle_destroy(rt->physics, rt->vehicles[i].veh);
	}
	rt->vehicle_count = 0;

	/* Configurable joints (CONFIGURABLE-JOINT last-mile): each tracked joint is
	 * a constraint inside rt->physics.  Destroy them BEFORE jce_physics_destroy
	 * below — same ordering contract as vehicles/ragdolls (the world teardown
	 * frees ALL constraints, and explicitly destroying first keeps the registry
	 * consistent across a scene transition).  Unlike chassis bodies a constraint
	 * owns no body, so this is also safe-by-construction; the early-destroy keeps
	 * the next scene's registry empty so the break monitor starts a clean slate. */
	if (rt->physics) {
		for (int i = 0; i < rt->cfg_joint_count; ++i)
			if (jce_constraint_valid(rt->cfg_joints[i].handle))
				jce_physics_constraint_destroy(rt->physics,
				                               rt->cfg_joints[i].handle);
	}
	rt->cfg_joint_count = 0;

	/* Soft bodies (SOFT-BODY last-mile): live in the SHARED soft world (NOT
	 * rt->physics), so they are torn down independently of jce_physics_destroy.
	 * Destroy each body + drop the mirrored static proxies so the next scene
	 * starts clean.  Order-safe / idempotent: jce_softbody_destroy ignores
	 * already-freed handles and jce_softbody_clear_statics is idempotent, so it
	 * does not matter that jce_cloth_shutdown_ (on physics destroy) also clears
	 * any remaining soft bodies + statics. */
	for (int i = 0; i < rt->softbody_count; ++i)
		jce_softbody_destroy(rt->softbodies[i].handle);
	rt->softbody_count        = 0;
	jce_softbody_clear_statics();
	rt->soft_statics_mirrored = false;

	/* Physics worlds + character + bodies.  Destroying the world drops every
	 * body, the character capsule, and the (script on_collision + game)
	 * contact listeners in one shot, so no dangling callback into this scene's
	 * bodies survives.  The listener is re-registered by rt_spawn_scene_state. */
	if (rt->physics) {
		if (rt->script_collision_registered) {
			jce_physics_remove_contact_listener(rt->physics,
			                                    rt_script_collision_cb, rt);
			rt->script_collision_registered = false;
		}
		if (rt->contact_registered && rt->contact_cb) {
			jce_physics_remove_contact_listener(rt->physics,
			                                    rt->contact_cb, rt->contact_ud);
			rt->contact_registered = false;
		}
		if (jce_character_valid(rt->character))
			jce_physics_character_destroy(rt->physics, rt->character);
		jce_physics_destroy(rt->physics);
		rt->physics = NULL;
	}
	rt->character        = JCE_CHARACTER_INVALID;
	rt->character_entity = 0;
	rt->char_yaw_valid   = false;
	rt->body_count       = 0;
	/* Draw-distance deferred entries: the physics world (and every body they
	 * spawned) is destroyed above, so just clear the count.  Capacity is kept
	 * for reuse across a scene transition (freed in jce_runtime_destroy). */
	rt->dd_count         = 0;

	/* 2D joints (JOINT-2D last-mile): each is a joint inside rt->physics2d.
	 * Destroy them BEFORE jce_physics2d_destroy below — same ordering contract
	 * as the 3D cfg joints (the 2D world teardown auto-destroys ALL its joints,
	 * so doing it explicitly first only keeps the registry consistent across a
	 * scene transition; jce_physics2d_joint_destroy is idempotent so a doubled
	 * handle is a safe no-op, and once the world is destroyed we never touch the
	 * stale handles again). */
	if (rt->physics2d) {
		for (int i = 0; i < rt->joint2d_count; ++i)
			if (jce_constraint_valid(rt->joints2d[i].handle))
				jce_physics2d_joint_destroy(rt->physics2d,
				                            rt->joints2d[i].handle);
	}
	rt->joint2d_count = 0;

	if (rt->physics2d) {
		jce_physics2d_destroy(rt->physics2d);
		rt->physics2d = NULL;
	}
	rt->body2d_count = 0;

	/* Gameplay subsystems (P0-master-bridge). */
	for (int i = 0; i < rt->spawn_count; ++i)
		if (rt->spawns[i].mgr) jce_spawn_manager_destroy(rt->spawns[i].mgr);
	rt->spawn_count        = 0;
	rt->weapon_count       = 0;
	rt->trigger_count      = 0;
	rt->save_point_count   = 0;
	if (rt->trigger_world) {
		jce_trigger_world_destroy(rt->trigger_world);
		rt->trigger_world = NULL;
	}
	rt->trigger_player_valid = false;

	/* Behavior-tree agents + per-agent blackboards.  The shared bt_ctx is
	 * REUSABLE (kept), but each agent's loaded tree + blackboard is scene
	 * state: halt + free them here so a new scene's trees load cleanly. */
	for (int i = 0; i < rt->bt_count; ++i) {
		if (rt->bt_ctx && jce_bt_tree_valid(rt->bts[i].tree))
			jce_bt_halt(rt->bt_ctx, rt->bts[i].tree);
		if (rt->bts[i].bb)
			jce_blackboard_destroy(rt->bts[i].bb);
	}
	rt->bt_count      = 0;
	rt->bt_active_bb  = NULL;

	/* Live ability systems (GAS): embedded POD, no external resources — just
	 * drop them so the next scene rebuilds from its own authored components. */
	rt->gas_count = 0;

	/* Gameplay script instances (the VM itself is REUSABLE — kept).  Release
	 * each instance (fires on_destroy) so per-scene self state is freed. */
	for (int i = 0; i < rt->script_count; ++i)
		if (rt->scripts[i].active)
			rt_script_ref_release(rt->scripts[i].ref);
	rt->script_count = 0;
	/* Drop the file watcher's per-scene watched paths by recreating it (the
	 * watcher object is cheap; this avoids stale watches on the old scene's
	 * script files).  When the runtime is being destroyed there is no next
	 * scene to watch, so just destroy it here and let jce_runtime_destroy skip
	 * its (now NULL) watcher — avoids a needless create+immediate-destroy. */
	if (rt->script_watcher) {
		jce_file_watcher_destroy(rt->script_watcher);
		rt->script_watcher = (!destroying && rt->script_enabled)
		                     ? jce_file_watcher_create() : NULL;
	}
	rt->pending_spawn_count = 0;
	/* Queued fracture entities reference this scene's bodies (destroyed above);
	 * drop the batch so a level swap never shatters a stale entity id. */
	rt->pending_fracture_count = 0;

	/* Navigation: nav entries reference this scene; the agent set + recast
	 * navmesh are reloaded per scene from desc_navmesh_path. */
	rt->nav_entry_count = 0;
	if (rt->nav_agents) {
		jce_nav_agent_set_destroy(rt->nav_agents);
		rt->nav_agents = NULL;
	}
	if (rt->nav_recast) {
		jce_recast_destroy(rt->nav_recast);
		rt->nav_recast = NULL;
	}

	/* Net bridge is per-scene wiring (the session itself is process-global). */
	rt->net_bridged   = false;
	rt->net_obj_count = 0;
	rt->net_animator_count = 0;
	/* Drop any per-client uploaded input so a transitioned / re-spawned scene
	 * (whose net ids + ownership are rebuilt) starts with a clean store. */
	jce_input_command_store_reset();

	/* Client prediction is per-scene (its ring references the predicted scene
	 * entity).  Destroy it so a transitioned / re-spawned scene starts with
	 * prediction off until the caller re-establishes it.  The predicted flag
	 * lives in the net-transform layer which is also reset above; clearing the
	 * id here keeps the two in sync. */
	if (rt->predict_buf) {
		jce_prediction_buffer_destroy(rt->predict_buf);
		rt->predict_buf = NULL;
	}
	rt->predict_entity = 0;
}

/* Materialise every per-scene subsystem from rt->scene's authored contents.
 * This is the shared body of jce_runtime_create's spawn sequence, reused by a
 * scene transition's LOAD step.  Assumes rt_teardown_scene_state already ran
 * (or a fresh runtime): physics is NULL and every per-scene count is 0.
 * Mirrors the create() order exactly so a transitioned scene behaves bit-for-
 * bit like one booted directly. */
static void rt_spawn_scene_state(JceRuntime *rt)
{
	if (rt->enable_physics) {
		/* Single fixed cadence (P1-fixed-clock-unify): an explicit
		 * desc fixed_timestep retunes the engine clock; otherwise adopt it. */
		JceFixedClock *gclock = jce_fixed_clock_default();
		float fixed_dt;
		if (rt->desc_fixed_timestep > 0.0f) {
			fixed_dt = rt->desc_fixed_timestep;
			gclock->fixed_dt = (double)fixed_dt;
		} else {
			fixed_dt = (gclock->fixed_dt > 0.0)
			           ? (float)gclock->fixed_dt : 1.0f / 60.0f;
		}

		JcePhysicsWorldDesc wd;
		memset(&wd, 0, sizeof wd);
		wd.gravity.x      = rt->desc_gravity[0];
		wd.gravity.y      = rt->desc_gravity[1];
		wd.gravity.z      = rt->desc_gravity[2];
		wd.fixed_timestep = fixed_dt;
		/* Project Settings > Physics tuning (0 = leave Bullet default). */
		wd.solver_iterations       = rt->desc_solver_iterations;
		wd.linear_sleep_threshold  = rt->desc_sleep_threshold;
		wd.angular_sleep_threshold = rt->desc_sleep_threshold;
		wd.split_impulse  = -1; /* leave Bullet default (ON) */
		/* JCE_PHYSICS_MAX_BODIES: raise the Bullet body pool past the 4096
		 * default for large-scale physics stress benchmarks (#5). */
		{ const char *mb = getenv("JCE_PHYSICS_MAX_BODIES");
		  if (mb && atoi(mb) > 0) wd.max_bodies = (uint32_t)atoi(mb); }
		/* JCE_PHYSICS_MT: opt-in multithreaded Bullet solver (parallel island
		 * solving via btDiscreteDynamicsWorldMt). Compiled in via
		 * JCE_PHYSICS_MT/BT_THREADSAFE; default OFF because the single-threaded
		 * path is deterministic (fixed-timestep reproducibility). On a
		 * single-core host Bullet's scheduler runs one worker, so it degrades
		 * gracefully to the charter baseline. */
		if (getenv("JCE_PHYSICS_MT")) wd.multithreaded = true;
		rt->physics = jce_physics_create(&wd);
		if (!rt->physics)
			jce_log_write(JCE_LOG_LEVEL_ERROR, LOG_TAG, __FILE__, __LINE__,
			              "%s", "failed to create physics world");

		JcePhysics2DDesc wd2;
		memset(&wd2, 0, sizeof wd2);
		wd2.gravity.x  = rt->desc_gravity2d[0];
		wd2.gravity.y  = rt->desc_gravity2d[1];
		wd2.max_bodies = 0; /* wrapper default (4096) */
		rt->physics2d = jce_physics2d_create(&wd2);
		if (!rt->physics2d)
			jce_log_write(JCE_LOG_LEVEL_ERROR, LOG_TAG, __FILE__, __LINE__,
			              "%s", "failed to create 2D physics world");

		jce_fixed_clock_init(&rt->clock, (double)fixed_dt,
		                     (double)fixed_dt * (double)RT_MAX_FIXED_STEPS);
		rt->have_prev = false;
	}

	/* First pass: bodies / character / voices. */
	jce_scene_each_entity(rt->scene, rt_spawn_entity, rt);

	/* Second pass: joints/constraints, now that every body exists. */
	if (rt->physics) {
		jce_scene_each_entity(rt->scene, rt_spawn_joint, rt);
		/* Configurable joints (per-axis 6DOF + break) — same pass, same
		 * "all bodies must already exist" requirement as rt_spawn_joint. */
		jce_scene_each_entity(rt->scene, rt_spawn_configurable_joint, rt);
	}
	/* 2D joints (Box2D distance/hinge/spring) — same post-spawn pass, gated on
	 * the 2D world (independent of the 3D world above) and the same "all 2D
	 * bodies already exist" requirement. */
	if (rt->physics2d)
		jce_scene_each_entity(rt->scene, rt_spawn_joint2d, rt);

	/* Navigation: load the baked navmesh + agent set BEFORE the gameplay walk
	 * so NavAgent components register into it. */
	rt_init_navmesh(rt, rt->desc_navmesh_path);

	/* Re-register the bundled BT perception actions on the (reused) context. */
	rt_bt_register_default_actions(rt);

	/* Physics contact -> script on_collision bridge (one listener slot).  The
	 * old world's registration was dropped in teardown when the world died. */
	if (rt->script_enabled && rt->physics && !rt->script_collision_registered) {
		if (jce_physics_add_contact_listener(rt->physics,
		                                     rt_script_collision_cb, rt))
			rt->script_collision_registered = true;
	}
	/* Re-attach the game contact listener (if one was subscribed) to the new
	 * world so cross-scene subscribers keep receiving events. */
	if (rt->contact_cb && rt->physics && !rt->contact_registered) {
		if (jce_physics_add_contact_listener(rt->physics,
		                                     rt->contact_cb, rt->contact_ud))
			rt->contact_registered = true;
	}

	/* Third pass: gameplay subsystems + authored behavior trees and scripts. */
	rt->in_scene_walk = true;
	jce_scene_each_entity(rt->scene, rt_spawn_gameplay, rt);
	rt->in_scene_walk = false;

	/* Reverb zones from AudioReverbZone components. */
	rt_build_reverb_zones(rt);

	/* Re-point the snapshot scene provider at the (possibly reloaded) scene so
	 * saves capture the live scene.  The registry object is reused. */
	if (rt->save_registry)
		jce_save_register_scene_provider_ex(rt->save_registry, rt->scene,
		                                    rt->save_migrations);
	/* Its live environment too: the hour and the wetness/snow integrators are
	 * earned by a session and cannot be re-derived. */
	if (rt->save_registry)
		jce_save_register_env_provider(rt->save_registry, rt->scene);

	/* Networking bridge for the new scene's Net* components. */
	rt->net_session_driven = (jce_session_mode() != JCE_SESSION_MODE_NONE);
	rt_init_net_bridge(rt);

	/* After the scene spawns: chunks register against a live scene. */
	rt_streaming_begin(rt);
}

/* ── Sequencer event / camera-cut dispatch (FEATURE 8.4) ──────────────
 *
 * The scene-sequencer driver fires EVENT keys through this trampoline; we
 * route the authored handler name to the gameplay script VMs (so a .seq EVENT
 * key calls a global function in whichever live language defines it,
 * mirroring the UIButton on_click path in jce_runtime_dispatch_ui_click).  Camera-cuts are applied inside the driver
 * (it raises the target vcam's priority); the observer here only logs. */
static void rt_seq_event_handler(const char *handler, uint64_t entity,
                                 float time, void *user)
{
	JceRuntime *rt = (JceRuntime *)user;
	(void)time;
	if (!rt || !handler || !handler[0]) return;
	(void)rt_script_call_named(rt, handler, (JceScriptEntity)entity);
}

static void rt_seq_camera_cut_handler(JceScene *s, JceEntity target,
                                      float time, void *user)
{
	(void)s; (void)user;
	LOG_DEBUG(LOG_TAG, "sequence camera-cut -> entity %llu @ %.3fs",
	          (unsigned long long)target, (double)time);
}

/* ── Console cvar bridge (gap 9.1 last-mile) ──────────────────────────────
 *
 * Register the small, high-value set of REAL engine cvars that drive runtime
 * state and cache their handles.  Done once per runtime at create(): the cvar
 * registry is process-global and idempotent (the first registration's
 * default/help/flags win and the existing value is preserved on re-register),
 * so a second Play session re-uses the same cvars but we re-seed each one to
 * THIS runtime's current state — keeping create() authoritative for the
 * initial value (e.g. time_scale seeded from JceProjectTime) while the console
 * drives changes thereafter.  Registering these is purely additive: rt_apply_
 * cvars only writes a value back to the runtime when the cvar CHANGED since the
 * last frame it applied, so an untouched cvar never perturbs the sim.
 *
 * Only cvars with a real existing consumer are registered (no dead cvars):
 *   time_scale         -> jce_runtime_set_time_scale  (sim clock scale)
 *   paused             -> jce_runtime_set_paused      (freeze the sim)
 *   audio.master_volume-> jce_audio_set_master_volume (master gain; gated on
 *                         rt->audio so it is a no-op when audio is disabled). */
static void rt_init_cvars(JceRuntime *rt)
{
	rt->cv_time_scale = jce_cvar_register_float(
		"time_scale", rt->time_scale, JCE_CVAR_FLAG_NONE,
		"Global simulation time scale (0=frozen, 1=normal, >1 fast)");
	rt->cv_paused = jce_cvar_register_bool(
		"paused", rt->paused, JCE_CVAR_FLAG_NONE,
		"Freeze the simulation (audio/UI keep running)");
	rt->cv_master_volume = jce_cvar_register_float(
		"audio.master_volume", 1.0f, JCE_CVAR_FLAG_NONE,
		"Master audio output gain (0..1+)");

	/* Re-seed to this runtime's authoritative initial state (a re-registered
	 * cvar keeps its prior value otherwise — see above). */
	if (rt->cv_time_scale)    jce_cvar_set_float(rt->cv_time_scale, rt->time_scale);
	if (rt->cv_paused)        jce_cvar_set_bool (rt->cv_paused, rt->paused);
	if (rt->cv_master_volume) jce_cvar_set_float(rt->cv_master_volume, 1.0f);

	/* Prime the change-detection baselines so the first jce_runtime_step does
	 * not re-apply the seeds (which would be redundant writes equal to the
	 * current state — keeps "feature unused == byte-identical to before"). */
	rt->cv_last_time_scale    = rt->cv_time_scale    ? jce_cvar_get_float(rt->cv_time_scale)    : rt->time_scale;
	rt->cv_last_paused        = rt->cv_paused        ? jce_cvar_get_bool (rt->cv_paused)        : rt->paused;
	rt->cv_last_master_volume = rt->cv_master_volume ? jce_cvar_get_float(rt->cv_master_volume) : 1.0f;
}

/* Read each bridged cvar and, when it CHANGED since the last frame applied,
 * write the new value to the runtime (one-directional: cvar -> runtime).
 * Called at the top of jce_runtime_step BEFORE sim_dt is computed so a console
 * line `time_scale 0.25` / `paused 1` takes effect on the SAME step.  The
 * change-detection keeps this deterministic and non-clobbering: a cvar nobody
 * touched is never re-applied, so direct callers (jce.set_time_scale, gameplay
 * code) are not overridden by a stale cvar, and the sim is provably unchanged
 * when the console is never used. */
static void rt_apply_cvars(JceRuntime *rt)
{
	if (rt->cv_time_scale) {
		float v = jce_cvar_get_float(rt->cv_time_scale);
		if (v != rt->cv_last_time_scale) {
			rt->cv_last_time_scale = v;
			jce_runtime_set_time_scale(rt, v);   /* clamps to [0,100] */
		}
	}
	if (rt->cv_paused) {
		bool v = jce_cvar_get_bool(rt->cv_paused);
		if (v != rt->cv_last_paused) {
			rt->cv_last_paused = v;
			jce_runtime_set_paused(rt, v);
		}
	}
	if (rt->cv_master_volume && rt->audio) {
		float v = jce_cvar_get_float(rt->cv_master_volume);
		if (v != rt->cv_last_master_volume) {
			rt->cv_last_master_volume = v;
			if (v < 0.0f) v = 0.0f;
			jce_audio_set_master_volume(rt->audio, v);
		}
	}
}

/* ── Public API ──────────────────────────────────────────────────── */

static bool rt_locale_asset_exists(const JceRuntimeDesc *desc,
                                   const char *locale)
{
	if (!desc || !locale || !locale[0]) return false;

	char relative_path[96];
	int n = snprintf(relative_path, sizeof(relative_path), "i18n/%s.json",
	                 locale);
	if (n <= 0 || n >= (int)sizeof(relative_path)) return false;

	if (desc->locales_dir && desc->locales_dir[0]) {
		char host_path[512];
		n = snprintf(host_path, sizeof(host_path), "%s/%s.json",
		             desc->locales_dir, locale);
		if (n > 0 && n < (int)sizeof(host_path) &&
		    jce_fs_host_exists_file(host_path))
			return true;
	}

	return desc->pak && jce_pak_find(desc->pak, relative_path) != NULL;
}

/* Normalize OS locale spelling for JCE's file convention, then select a
 * concrete table. Projects may intentionally ship no i18n assets; in that
 * case the runtime must leave localization uninitialized instead of issuing a
 * misleading missing-file warning. */
static bool rt_select_locale_asset(const JceRuntimeDesc *desc, char out[32])
{
	if (!desc || !out) return false;
	memset(out, 0, 32);
	char preferred[32] = {0};
	const char *source = (desc->locale && desc->locale[0])
	                   ? desc->locale : NULL;
	if (!source && !jce_host_preferred_locale(preferred, sizeof(preferred)))
		source = "en";
	else if (!source)
		source = preferred;

	if (!source || !source[0]) return false;
	for (size_t i = 0; i + 1 < 32 && source[i]; ++i) {
		char c = source[i];
		out[i] = (c == '-') ? '_' : (char)tolower((unsigned char)c);
	}
	out[31] = '\0';
	if (rt_locale_asset_exists(desc, out)) return true;

	char *separator = strchr(out, '_');
	if (separator) {
		*separator = '\0';
		if (rt_locale_asset_exists(desc, out)) return true;
	}
	return false;
}

JCE_API JceRuntime *JCE_CALL jce_runtime_create(const JceRuntimeDesc *desc)
{
	if (!desc || !desc->scene) return NULL;

	JceRuntime *rt = (JceRuntime *)jce_malloc(sizeof *rt);
	if (!rt) return NULL;
	memset(rt, 0, sizeof *rt);

	rt->scene             = desc->scene;
	rt->pak               = desc->pak;
	rt->audio             = desc->audio;
	rt->audio_load_fn     = desc->audio_load_fn;
	rt->user_data         = desc->user_data;
	rt->resolve_path_fn   = desc->resolve_path_fn;
	rt->character         = JCE_CHARACTER_INVALID;
	rt->character_entity  = 0;
	rt->input.speed_mult  = 1.0f;
	rt->time_scale        = 1.0f;   /* 0 from memset would freeze the sim */
	rt->paused            = false;

	/* Floating-origin world origin starts at absolute (0,0,0).  The per-frame
	 * rebase pass re-adopts the scene's authored threshold each step and only
	 * runs at all when the scene opts in (rendering_settings.floating_origin_
	 * enabled) — so for every existing scene this is inert. */
	rt->world_origin      = jce_world_origin_default(4096.0f);

	/* Client-prediction tuning defaults (no predicted entity yet -> inert).
	 * jce_runtime_set_predicted_entity refreshes move/sprint speed from the
	 * authored CharacterController feel when prediction is established. */
	jce_predict_loco_params_default(&rt->predict_params);
	rt->predict_buf    = NULL;
	rt->predict_entity = 0;

	/* Register the REAL engine cvars that drive the sim (time_scale / paused /
	 * audio.master_volume) and cache their handles, so the in-game console can
	 * actually control the runtime (gap 9.1 last-mile).  Done after the
	 * time-control + audio fields are seeded above so each cvar starts at this
	 * runtime's authoritative initial value; applied each step by rt_apply_cvars
	 * (purely additive — an untouched cvar never perturbs the sim). */
	rt_init_cvars(rt);

	/* Capture the desc fields a scene transition needs so it can re-init each
	 * loaded scene's subsystems with the same wiring (FEATURE 9.4). */
	rt->enable_physics          = desc->enable_physics;
	/* Resolve the effective gravity vector once: an explicit gravity[] wins;
	 * else the scalar gravity_y (source-compat); else the -9.81 Y default.
	 * The old scalar-only path silently dropped X and Z. */
	if (desc->gravity[0] != 0.0f || desc->gravity[1] != 0.0f ||
	    desc->gravity[2] != 0.0f) {
		rt->desc_gravity[0] = desc->gravity[0];
		rt->desc_gravity[1] = desc->gravity[1];
		rt->desc_gravity[2] = desc->gravity[2];
	} else {
		rt->desc_gravity[0] = 0.0f;
		rt->desc_gravity[1] = (desc->gravity_y != 0.0f) ? desc->gravity_y : -9.81f;
		rt->desc_gravity[2] = 0.0f;
	}
	rt->desc_fixed_timestep     = desc->fixed_timestep;
	rt->desc_solver_iterations  = desc->solver_iterations;
	rt->desc_sleep_threshold    = desc->sleep_threshold;
	rt->desc_disable_auto_physics = desc->disable_auto_physics;
	rt->desc_max_frame_dt       = desc->max_frame_dt;
	/* 2D gravity: explicit gravity2d wins; else default (0, -9.81). */
	if (desc->gravity2d[0] != 0.0f || desc->gravity2d[1] != 0.0f) {
		rt->desc_gravity2d[0] = desc->gravity2d[0];
		rt->desc_gravity2d[1] = desc->gravity2d[1];
	} else {
		rt->desc_gravity2d[0] = 0.0f;
		rt->desc_gravity2d[1] = -9.81f;
	}
	rt->desc_mixer_config_path  = desc->mixer_config_path;
	if (desc->navmesh_path && desc->navmesh_path[0]) {
		size_t nn = strlen(desc->navmesh_path);
		if (nn >= sizeof rt->desc_navmesh_path) nn = sizeof rt->desc_navmesh_path - 1;
		memcpy(rt->desc_navmesh_path, desc->navmesh_path, nn);
		rt->desc_navmesh_path[nn] = '\0';
	}
	rt->trans_state = JCE_RT_TRANSITION_IDLE;

	/* Game-content localization (L10n): initialise only after finding a real
	 * locale table. An application PAK is not by itself proof that the project
	 * ships translations; empty/no-i18n projects keep key passthrough quietly. */
	{
		char locale[32] = {0};
		if (rt_select_locale_asset(desc, locale)) {
			bool has_loc_dir = desc->locales_dir && desc->locales_dir[0];
			jce_loc_init(has_loc_dir ? desc->locales_dir : NULL);
			if (desc->pak)
				jce_loc_set_source_pak(desc->pak, "i18n");
			jce_loc_set_locale(locale);
		}
	}

	/* ── Reusable infrastructure (outlives any one scene) ────────────────
	 * The audio mixer, BT action context, gameplay script VM, hot-reload
	 * watcher, and snapshot registry are stood up ONCE here and survive a
	 * scene transition (FEATURE 9.4) — a level swap must not reinitialise the
	 * audio engine or the scripting VM.  The per-scene state (physics worlds,
	 * bodies, voices, triggers, behavior trees, script instances, nav agents,
	 * reverb zones) is materialised by rt_spawn_scene_state below and rebuilt
	 * on every transition. */

	/* Audio mixer: stand up the bus tree (from audio_mixer.json or default)
	 * and mirror it onto the audio device BEFORE the spawn walk so each
	 * AudioSource voice can be routed to its bus as it is created. */
	rt_init_mixer(rt, desc->mixer_config_path);

	/* Behavior-tree action context + gameplay scripting VM, created BEFORE the
	 * gameplay walk in rt_spawn_scene_state so rt_spawn_gameplay can load each
	 * entity's authored behavior tree / Lua script into them as it is visited.
	 * (These were once created AFTER that walk, so the `rt->bt_ctx` /
	 * `rt->script_enabled` guards inside rt_spawn_gameplay were always false and
	 * authored trees and scripts silently never loaded — Phase 0 keystone
	 * ordering fix, regression-guarded by
	 * tests/application/test_jce_runtime_script_load.c.) */
	rt->bt_ctx = jce_bt_create();
		rt_script_install_vm(rt);

	/* Sequencer EVENT keys → Lua handlers; CAMERA-CUT keys → active-camera
	 * seam (the driver applies the cut; this observer just logs).  Process-
	 * global sinks, registered once with this runtime as userdata. */
	jce_scene_sequencer_set_event_handler(rt_seq_event_handler, rt);
	jce_scene_sequencer_set_camera_cut_handler(rt_seq_camera_cut_handler, rt);
	/* Resolve SequencePlayer .seq.json paths through the same host resolver as
	 * scripts/terrain/tilemap (editor CWD != project root).  NULL in a shipped
	 * build → the integrator uses the raw path (CWD already the asset root). */
	jce_scene_sequencer_set_resolve_fn(rt->resolve_path_fn, rt->user_data);

	/* Save / snapshot (P2-save-snapshot): stand up a registry with the
	 * scene/ECS provider so SavePoint overlaps + game code can persist and
	 * restore the session.  saves_dir (optional) is the SavePoint write base. */
	rt->asset_fs = desc->asset_fs;   /* borrowed; the host owns it */
	rt->save_registry = jce_snapshot_registry_create();
	/* Migration registry: future schema bumps register "scene_ecs" upgrade
	 * steps here so older saves load instead of being refused.  Empty today
	 * (current-version loads are a no-op), but the seam is wired. */
	rt->save_migrations = jce_save_migration_registry_create();
	if (desc->saves_dir && desc->saves_dir[0]) {
		size_t dn = strlen(desc->saves_dir);
		if (dn >= sizeof rt->saves_dir) dn = sizeof rt->saves_dir - 1;
		memcpy(rt->saves_dir, desc->saves_dir, dn);
		rt->saves_dir[dn] = '\0';
	}

	/* ── Per-scene state: physics worlds, bodies, voices, gameplay
	 * subsystems, navmesh, behavior trees, scripts, reverb, net bridge.
	 * Shared with the scene-transition LOAD step. */
	rt_spawn_scene_state(rt);

	jce_log_write(JCE_LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__,
	              "runtime: physics=%s bodies=%d bodies2d=%d character=%s/%u "
	              "voices=%d",
	              rt->physics ? "on" : "off",
	              rt->body_count,
	              rt->body2d_count,
	              jce_character_valid(rt->character) ? "1" : "0",
	              rt->character_authored,
	              rt->voice_count);
	/* Every field below is COUNTED.  This line used to end with a literal
	 * "(deferred: ai_steering, anim-events, ik, sprite-anim)" and three of the
	 * four had shipped: jce_anim_events_advance, the jce_anim_ik_*_solve family
	 * and jce_sprite_player_update all run in jce_sr_anim.c, which the editor
	 * and a shipped game both drive.  So the runtime's own boot log answered
	 * "why does my animation event not fire" with "deferred" while it worked.
	 * The fourth is not pending wiring either -- jce_ai_ecs.h is a
	 * caller-driven adapter, ticked from game code over a flecs world the
	 * runtime does not own.  Deleted rather than corrected: a status line that
	 * reports a STRING LITERAL can only be right about the day it was typed. */
	jce_log_write(JCE_LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__,
	              "runtime: gameplay bridge -> triggers=%d spawners=%d weapons=%d "
	              "save_points=%d bt_ctx=%s behavior_trees=%d net_session=%s",
	              rt->trigger_count,
	              rt->spawn_count,
	              rt->weapon_count,
	              rt->save_point_count,
	              rt->bt_ctx ? "on" : "off",
	              rt->bt_count,
	              rt->net_session_driven ? "driven" : "none");
	jce_log_write(JCE_LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__,
	              "runtime: save snapshot -> registry=%s scene_provider=on saves_dir=%s",
	              rt->save_registry ? "on" : "off",
	              rt->saves_dir[0] ? rt->saves_dir : "(none)");
	return rt;
}

JCE_API void JCE_CALL jce_runtime_destroy(JceRuntime *rt)
{
	if (rt && rt->world_streamer) {
		jce_world_streamer_destroy(rt->world_streamer);
		rt->world_streamer = NULL;
	}
	if (!rt) return;

	/* Paged terrain colliders own physics bodies; drop them before the
	 * physics world goes away. */
	if (rt->terrain_stream) {
		jce_terrain_collision_stream_destroy(rt->terrain_stream);
		rt->terrain_stream = NULL;
		rt->terrain_stream_src = NULL;
	}

	/* Release the water clock.  The scene outlives this runtime (the editor
	 * keeps it after Play ends), and a set still claimed by a freed pointer
	 * would never advance again -- the water would silently freeze, and the
	 * cause would look like a rendering bug rather than a lifetime one. */
	if (rt->scene)
		jce_water_field_set_release(jce_scene_water_fields(rt->scene), rt);

	/* Drop the process-global sequencer event/camera-cut sinks before this
	 * runtime (their userdata) is freed, so no dangling callback remains. */
	jce_scene_sequencer_set_event_handler(NULL, NULL);
	jce_scene_sequencer_set_camera_cut_handler(NULL, NULL);
	jce_scene_sequencer_set_resolve_fn(NULL, NULL);

	/* NOTE: jce_loc_shutdown is deliberately NOT called here.  The
	 * localization table is process-global and outlives any one runtime
	 * (the editor's preview locale must survive Play sessions; final
	 * cleanup happens at engine teardown). */

	/* Release ALL per-scene state (bodies, physics worlds, voices, triggers,
	 * spawners, weapons, behavior-tree agents, script instances, nav agents,
	 * reverb zones, pending audio decodes) through the shared teardown helper.
	 * This drops the script on_collision + game contact listeners before the
	 * physics world dies, so no dangling callback into this (freed) runtime
	 * can remain — exactly as a scene transition does (FEATURE 9.4). */
	rt_teardown_scene_state(rt, /*destroying=*/true);

	/* Free the per-scene array backing stores (the helper only zeroes counts
	 * so capacity can be reused across a transition; the runtime is going away
	 * now so release the memory). */
	jce_free(rt->pending_audio);
	jce_free(rt->voices);
	jce_free(rt->bodies);
	jce_free(rt->dd);
	jce_free(rt->bodies2d);
	jce_free(rt->spawns);
	jce_free(rt->weapons);
	jce_free(rt->triggers);
	jce_free(rt->save_points);
	jce_free(rt->bts);
	jce_free(rt->scripts);
	jce_free(rt->gas_entries);
	jce_free(rt->ragdoll_entries);
	jce_free(rt->pending_spawns);
	jce_free(rt->pending_fractures);
	jce_free(rt->vehicles);
	jce_free(rt->softbodies);
	jce_free(rt->cfg_joints);
	jce_free(rt->joints);   /* the one RT_GROW_FN registry that had no free */
	jce_free(rt->joints2d);
	jce_free(rt->nav_entries);
	rt_curve_cache_clear(rt);   /* parsed jce.curve_eval assets */

	/* ── Reusable infrastructure (survives a transition; freed only here) ── */
	if (rt->mixer)
		jce_audio_mixer_destroy(rt->mixer);
	if (rt->bt_ctx)
		jce_bt_destroy(rt->bt_ctx);
	/* The script watcher was already destroyed (and NOT recreated) by the
	 * destroying-pass rt_teardown_scene_state above; this guarded call is just
	 * a defensive no-op for that NULL. */
	if (rt->script_watcher)
		jce_file_watcher_destroy(rt->script_watcher);
	rt_script_destroy_vms(rt);
	if (rt->save_registry) {
		jce_save_unregister_scene_provider(rt->save_registry);
		jce_snapshot_registry_destroy(rt->save_registry);
	}
	if (rt->save_migrations)
		jce_save_migration_registry_destroy(rt->save_migrations);

	jce_free(rt);
}

/* ── Root motion application (FEATURE 3.2) ─────────────────────────────
 *
 * The scene renderer's pose evaluator owns the animation playhead + clip +
 * skeleton, so it extracts the root joint's per-frame local delta (loop-wrap
 * aware) and re-centers the rendered pose, stashing the delta on the skeletal
 * animator component's transient rm_* fields.  The RUNTIME owns entity
 * transform write-back, so it consumes that delta here: rotate it by the
 * entity's current world orientation and add it to the position.  Gated on the
 * entity's JceAvatarComponent.apply_root_motion — entities without it (the
 * default, including the existing physics-driven player) are never touched, so
 * there is no regression.
 *
 * NOTE (followup): a CharacterController-driven entity should NOT poke the
 * transform directly like this — instead it should feed (rm delta / dt) into
 * jce_physics_character_move as a DESIRED VELOCITY so the move still resolves
 * collisions/slopes.  That bridge belongs in rt_drive_character once a
 * root-motion locomotion clip set ships; this direct-transform path covers the
 * generic "animation drives a non-physics entity" case. */
static void rt_apply_root_motion(JceScene *scene, JceEntity e, void *ud)
{
	(void)ud;
	if (!jce_scene_has_transform(scene, e)) return;
	if (!jce_scene_has_avatar(scene, e)) return;
	JceAvatarComponent *av = jce_scene_get_avatar(scene, e);
	if (!av || !av->apply_root_motion) return;
	if (!jce_scene_has_skeletal_animator(scene, e)) return;
	JceSkeletalAnimatorComponent *sa = jce_scene_get_skeletal_animator(scene, e);
	if (!sa || !sa->rm_valid) return;

	JceTransform *cur = jce_scene_get_transform(scene, e);
	if (!cur) { sa->rm_valid = false; return; }

	/* Rotate the root-local delta into world space by the entity's current
	 * orientation so a turned character moves along its own facing. */
	jce_vec3 local_d = jce_v3(sa->rm_dx, sa->rm_dy, sa->rm_dz);
	jce_vec3 world_d = jce_q_rotate(cur->rotation, local_d);

	JceTransform t = *cur;             /* copy to avoid src==dst aliasing */
	t.position = jce_v3_add(t.position, world_d);
	if (sa->rm_dyaw != 0.0f) {
		jce_quat dq = jce_q_from_axis_angle(jce_v3(0.0f, 1.0f, 0.0f),
		                                    sa->rm_dyaw);
		t.rotation = jce_q_normalize(jce_q_multiply(t.rotation, dq));
	}
	jce_scene_set_transform(scene, e, &t);

	/* One-shot: the delta is now baked into the transform. */
	sa->rm_valid = false;
	sa->rm_dx = sa->rm_dy = sa->rm_dz = 0.0f;
	sa->rm_dyaw = 0.0f;
}

/* ── Scene / level transition (FEATURE 9.4) ───────────────────────────
 *
 * Perform the LOAD step of an in-flight transition: release this scene's
 * tracked runtime state, load rt->trans_path INTO rt->scene (the SAME object
 * the caller renders, so no renderer rebinding is needed), and re-run the
 * spawn walks so physics / scripts / terrain / gameplay re-init for the new
 * scene.  Returns true on a successful swap; on a read/parse failure the old
 * scene is already cleared (jce_scene_serial_load clears first), so we leave
 * an empty scene and log — the transition still completes (FADE_IN) rather
 * than wedging.  Synchronous by design (async load is a documented follow-up). */
static bool rt_transition_do_load(JceRuntime *rt)
{
	/* Tear the old scene's runtime subsystems down FIRST so no body / script /
	 * trigger still references entities about to be cleared by the loader. */
	rt_teardown_scene_state(rt, /*destroying=*/false);

	uint64_t size = 0;
	void *json = rt_read_asset_with_fallback(rt, rt->trans_path, &size);

	/* Set the scene serializer base-dir to the loaded scene's directory so
	 * relative material backfill resolves against THIS scene's folder (not a
	 * stale process-global path left by a prior load — editor Play loading a
	 * scene from a different directory).  jce_scene_serial_load_file does this
	 * implicitly from its path; the in-memory jce_scene_serial_load used below
	 * does not, so derive it from trans_path here (mirrors that helper). */
	{
		const char *sep = strrchr(rt->trans_path, '/');
		const char *bs  = strrchr(rt->trans_path, '\\');
		if (bs > sep) sep = bs;
		if (sep) {
			char dir[1024];
			size_t dl = (size_t)(sep - rt->trans_path);
			if (dl >= sizeof dir) dl = sizeof dir - 1;
			memcpy(dir, rt->trans_path, dl);
			dir[dl] = '\0';
			jce_scene_serial_set_base_dir(dir);
		} else {
			jce_scene_serial_set_base_dir(NULL);
		}
	}

	/* Clear the outgoing scene's entities + settings FIRST.  jce_scene_serial_load
	 * APPENDS (it does not clear), so without this the new scene's entities would
	 * stack on top of the old ones.  This mirrors how the editor opens a scene
	 * (clear then load) and makes the swap a true replacement. */
	jce_scene_clear(rt->scene);
	bool loaded = false;
	if (json && size > 0) {
		loaded = jce_scene_serial_load(rt->scene, (const char *)json,
		                               (size_t)size);
	}
	if (json) jce_free(json);

	if (!loaded) {
		/* Read or parse failed; the scene was already cleared above (and a
		 * partial load may have added some entities), so clear again to leave a
		 * consistent, body-free empty scene for the respawn walk. */
		jce_scene_clear(rt->scene);
		jce_log_write(JCE_LOG_LEVEL_ERROR, LOG_TAG, __FILE__, __LINE__,
		              "scene transition: failed to load '%s' (scene now empty)",
		              rt->trans_path);
	} else {
		jce_log_write(JCE_LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__,
		              "scene transition: loaded '%s'", rt->trans_path);
	}

	/* Rebuild every per-scene subsystem for the freshly loaded scene. */
	rt_spawn_scene_state(rt);
	return loaded;
}

/* Advance the fade-out -> load -> fade-in state machine on the REAL (unscaled,
 * unpaused) frame dt, so a transition completes even while the game is paused
 * or in bullet-time.  trans_alpha is the fade-quad opacity (0 clear, 1 black);
 * the LOAD swap is hidden at full black.  No-op when idle. */
static void rt_transition_advance(JceRuntime *rt, float real_dt)
{
	if (rt->trans_state == JCE_RT_TRANSITION_IDLE) return;
	if (real_dt <= 0.0f) real_dt = 1.0f / 60.0f;
	float fade = rt->trans_fade_secs > 0.0f ? rt->trans_fade_secs
	                                         : RT_TRANSITION_FADE_SECS;

	switch (rt->trans_state) {
	case JCE_RT_TRANSITION_FADE_OUT:
		rt->trans_timer += real_dt;
		rt->trans_alpha = (fade > 0.0f) ? (rt->trans_timer / fade) : 1.0f;
		if (rt->trans_alpha >= 1.0f) {
			rt->trans_alpha = 1.0f;
			rt->trans_state = JCE_RT_TRANSITION_LOAD;   /* swap next, at black */
		}
		break;
	case JCE_RT_TRANSITION_LOAD:
		rt->trans_alpha = 1.0f;                          /* hide the swap */
		rt_transition_do_load(rt);
#if defined(JCE_ENABLE_AI_DISPATCH) && JCE_ENABLE_AI_DISPATCH
		/* ai_dispatch generation token (spec H): the scene swap is the
		 * staleness boundary — async constraint results requested for the
		 * OLD scene must be discarded at pump time, never applied to the
		 * new one. */
		if (jce_aid_initialised())
			jce_aid_bump_generation();
#endif
		rt->trans_timer = 0.0f;
		rt->trans_state = JCE_RT_TRANSITION_FADE_IN;
		break;
	case JCE_RT_TRANSITION_FADE_IN:
		rt->trans_timer += real_dt;
		rt->trans_alpha = (fade > 0.0f) ? (1.0f - rt->trans_timer / fade) : 0.0f;
		if (rt->trans_alpha <= 0.0f) {
			rt->trans_alpha = 0.0f;
			rt->trans_state = JCE_RT_TRANSITION_IDLE;
			rt->trans_path[0] = '\0';
		}
		break;
	default:
		break;
	}
}

JCE_API bool JCE_CALL jce_runtime_request_scene(JceRuntime *rt,
                                                const char *scene_path)
{
	if (!rt || !scene_path || !scene_path[0]) return false;
	/* One transition at a time: the in-flight one wins (a game gates with
	 * jce_runtime_is_transitioning before requesting another). */
	if (rt->trans_state != JCE_RT_TRANSITION_IDLE) return false;

	size_t n = strlen(scene_path);
	if (n >= sizeof rt->trans_path) n = sizeof rt->trans_path - 1;
	memcpy(rt->trans_path, scene_path, n);
	rt->trans_path[n] = '\0';

	rt->trans_state     = JCE_RT_TRANSITION_FADE_OUT;
	rt->trans_timer     = 0.0f;
	rt->trans_alpha     = 0.0f;
	rt->trans_fade_secs = RT_TRANSITION_FADE_SECS;
	jce_log_write(JCE_LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__,
	              "scene transition: requested '%s'", rt->trans_path);
	return true;
}

JCE_API float JCE_CALL jce_runtime_transition_alpha(const JceRuntime *rt)
{
	return rt ? rt->trans_alpha : 0.0f;
}

JCE_API bool JCE_CALL jce_runtime_is_transitioning(const JceRuntime *rt)
{
	return rt && rt->trans_state != JCE_RT_TRANSITION_IDLE;
}

/* The Trail Renderer pass lives in jce_rt_trail.c -- this file is at its
 * size baseline, and the pass grew a per-point ageing step. */

JCE_API void JCE_CALL jce_runtime_step(JceRuntime *rt, float dt)
{
	if (!rt) return;
	if (dt <= 0.0f) dt = 1.0f / 60.0f;

	uint64_t _t0_tick = jce_time_perf_counter();

	/* Console cvar bridge (gap 9.1): pull any console-changed cvars
	 * (time_scale / paused / audio.master_volume) into runtime state BEFORE
	 * sim_dt is computed below, so a console line `time_scale 0.25` /
	 * `paused 1` takes effect on THIS step.  No-op (provably byte-identical to
	 * before this bridge) when no cvar changed since the last frame. */
	rt_apply_cvars(rt);

	/* Scene / level transition (FEATURE 9.4): advance the fade -> load -> fade
	 * state machine on the REAL frame dt FIRST so the LOAD swap (which rebuilds
	 * physics, scripts, gameplay) happens before this frame's sim runs — the
	 * rest of step() then drives the freshly loaded scene.  No-op when idle. */
	rt_transition_advance(rt, dt);

	/* Time control (Phase 0.2): the sim advances on a scaled clock — slow
	 * (bullet-time), fast, or frozen (pause == scale 0).  Physics banks
	 * sim_dt into its fixed accumulator (0 → no steps → frozen); scene /
	 * sequencer / particles / gameplay+scripts all step on sim_dt.  Audio
	 * (rt_audio_poll/_3d, no dt) stays real-time. */
	const float sim_dt = rt->paused ? 0.0f : dt * rt->time_scale;

	uint64_t _t0_physics = jce_time_perf_counter();
	/* Project Settings > Physics > auto simulation == false: the world still
	 * exists (for manual stepping via the physics API) but the runtime does not
	 * auto-step it. */
	if (rt->physics && !rt->desc_disable_auto_physics) {
		/* Keep the physics cadence locked to the engine-wide clock so a
		 * jce_engine_set_fixed_hz() (or the FIXED_UPDATE phase rate) and the
		 * physics step can never desync (P1-fixed-clock-unify).  We copy the
		 * cadence into our own accumulator-bearing clock rather than stepping
		 * the global one directly, because jce_engine_iterate already banks
		 * the frame dt into the global clock for the FIXED_UPDATE phase —
		 * sharing it would double-count.  max_frame_dt tracks the cadence so
		 * the RT_MAX_FIXED_STEPS catch-up ceiling stays correct. */
		const double gdt = jce_fixed_clock_default()->fixed_dt;
		if (gdt > 0.0 && gdt != rt->clock.fixed_dt) {
			rt->clock.fixed_dt     = gdt;
			rt->clock.max_frame_dt = gdt * (double)RT_MAX_FIXED_STEPS;
			/* Project Settings > Time > max_allowed_timestep: a tighter
			 * user-authored spiral-of-death clamp wins over the tick ceiling. */
			if (rt->desc_max_frame_dt > 0.0f &&
			    (double)rt->desc_max_frame_dt < rt->clock.max_frame_dt)
				rt->clock.max_frame_dt = (double)rt->desc_max_frame_dt;
			/* Don't let a now-oversized residual replay as a burst. */
			if (rt->clock.accumulator > gdt)
				rt->clock.accumulator = gdt;
		}

		/* Bank the frame time and run the simulation in whole fixed_dt
		 * chunks.  The fixed clock clamps frame_dt (spiral guard) so the
		 * tick count this frame is bounded by RT_MAX_FIXED_STEPS. */
		const float fixed_dt = (float)rt->clock.fixed_dt;
		uint32_t    steps    = jce_fixed_clock_advance(&rt->clock, (double)sim_dt);

		/* Apply external (editor/script) TRS edits once before stepping so
		 * teleports land on the upcoming fixed ticks. */
		rt_push_external_transforms(rt);

		for (uint32_t s = 0; s < steps; ++s) {
			/* Client->server input command channel (F12 slice): a CLIENT
			 * uploads its sampled input for the predicted (owned) entity; a
			 * SERVER overwrites rt->input from the remote client's uploaded
			 * command so the authoritative sim drives that client's movement
			 * this tick.  Runs BEFORE rt_drive_character so the applied input
			 * feeds the drive.  No-op (byte-identical) for single-player /
			 * server-owned / no-session play. */
			rt_net_input_channel(rt, (uint32_t)rt->clock.tick_count);
			rt_drive_character(rt, fixed_dt);
			/* Map player input into PLAYER-mode vehicles before the step
			 * integrates them.  SCRIPT-mode vehicles are left to the public
			 * API.  No-op when vehicle_count == 0 (byte-identical). */
			rt_drive_vehicles(rt);
			/* Client-side prediction (rollback/replay): predict this tick
			 * forward from the latest input and write the predicted pose into
			 * the predicted entity's transform.  Runs AFTER rt_drive_character
			 * (uses the PURE kinematic step fn, never touches Bullet) so it
			 * cannot perturb the existing physics movement.  No-op (byte-
			 * identical) when no predicted entity is established. */
			if (rt->predict_buf)
				rt_predict_apply_input(rt, (uint32_t)rt->clock.tick_count);
			/* Water buoyancy (gap 2.3): apply per-tick upward force on
			 * buoyant dynamic bodies BEFORE the step integrates it.  Advance
			 * the surface phase first so it tracks the fixed cadence; no-op
			 * when no scene has a buoyant body + active water. */
			rt->buoyancy_time += (double)fixed_dt;
			/* Claim the scene's shared water clock and advance it on the
			 * fixed cadence.  The renderer's own advance becomes a no-op for
			 * as long as we hold the claim, so the drawn surface and the
			 * floated body are the same evaluated tick by construction. */
			jce_water_field_set_advance(jce_scene_water_fields(rt->scene),
			                            rt, (double)fixed_dt);
			rt_apply_buoyancy(rt, fixed_dt);
			/* Advance the DISTURBANCE layer, beside the ambient clock it adds
			 * to, and OUTSIDE rt_apply_buoyancy.
			 *
			 * It was inside, at the bottom, which reads as the natural place:
			 * that pass already runs exactly once per executed tick, and
			 * stepping after the impulse loop integrates an impulse on the
			 * tick it was applied. Both true -- and both irrelevant to the
			 * defect, because that pass has FOUR early returns above the step:
			 * no rigid bodies, no enabled+visible water, no field desc, and no
			 * free field slot. The last of those fires AFTER the grid has been
			 * created, so a grid could exist and never advance: a pond with a
			 * ring on it, frozen, in a scene whose seventeenth water body took
			 * the last slot. A scene with a pond and no rigid bodies froze it
			 * outright.
			 *
			 * Here it is unconditional per executed tick, which is what a
			 * stateful solver needs. It is still stepped from exactly one
			 * place -- the property that matters, for the reason
			 * JceWaterFieldSet takes a driver token: a grid stepped twice
			 * advances at twice the rate its caller believes, and the symptom
			 * is a pond that damps too fast rather than anything shaped like a
			 * bug.
			 *
			 * The cost of moving it is one tick of latency between an impulse
			 * and its integration, which is invisible, against a class of
			 * silent freeze that is not.
			 *
			 * NULL until the buoyancy pass has created it, and stepping NULL
			 * is a no-op -- so a scene with no water never pays for this. */
			jce_water_ripple_step(jce_scene_water_ripple(rt->scene, NULL),
			                      fixed_dt);
			/* Page terrain colliders around the player.  Seeded at spawn,
			 * but a streamed world only stays solid if residency follows
			 * whoever is walking on it. */
			if (rt->terrain_stream) {
				jce_vec3 focus;
				if (jce_runtime_get_player_position(rt, &focus))
					jce_terrain_collision_stream_update(rt->terrain_stream,
					                                    focus);
			}
			/* Constant Force (Unity ConstantForce last-mile): accumulate
			 * authored world + body-relative force/torque on enabled dynamic
			 * bodies BEFORE the step integrates them.  No-op (byte-identical)
			 * when no scene authored a ConstantForce component. */
			rt_apply_constant_force(rt);
			/* Ragdoll drive (scene-pass last-mile): nudge each ragdoll's
			 * bodies toward its source LOCAL pose BEFORE the step integrates
			 * them.  Gated on ragdoll_count -> a scene with no ragdolls keeps
			 * the byte-identical step path. */
			if (rt->ragdoll_count)
				rt_ragdoll_sync_from(rt, fixed_dt);
			/* Sim-LOD physics gating (large-world #3): sleep far-tier dynamic
			 * bodies that opt in (SimLod gate_mask & PHYSICS) so Bullet's solver
			 * skips them, and wake near/mid ones.  Bodies without the opt-in
			 * return tier -1 and are never touched — byte-identical otherwise. */
			{
				jce_vec3 sv = rt_viewer_position(rt);
				for (int bi = 0; bi < rt->body_count; ++bi) {
					BodyEntry *be = &rt->bodies[bi];
					if (be->kind == (uint8_t)JCE_BODY_STATIC) continue;
					int tier = -1;
					(void)rt_sim_lod_period(rt, be->entity, sv,
					                        JCE_SIMLOD_GATE_PHYSICS, &tier);
					if (tier < 0) continue;          /* not opted in -> untouched */
					jce_physics_body_set_active(rt->physics, be->body, tier != 2);
				}
			}
			/* ── Script on_fixed_update, BEFORE the step ──────────────
			 * Unity's FixedUpdate, Godot's _physics_process.  A force
			 * applied here is integrated by the very next line, which is
			 * the ordering that makes a jump clear the same gap at 30 Hz
			 * and at 144 Hz.  on_update cannot do this: it runs on a
			 * render frame whose dt varies with the frame rate.
			 *
			 * HERE, NOT JCE_PHASE_FIXED_UPDATE, though that phase exists
			 * and coroutines already use it.  jce_player_loop.h says in
			 * its own header that the phases are an AUXILIARY surface
			 * firing ALONGSIDE the inline pipeline, and this accumulator
			 * is inline.  Two accumulators unified by clock are not
			 * thereby guaranteed to agree on step count in a given frame,
			 * and a script force applied a different number of times than
			 * physics steps is the exact bug this callback exists to
			 * prevent.  One dispatch per step, same dt, no inference.
			 *
			 * NOT SIM-LOD GATED, unlike on_update above.  A fixed-step
			 * callback that fires at a distance-dependent rate is a
			 * contradiction: the whole contract is "always the same dt".
			 * A script that wants to be cheap far away uses on_update. */
			for (int si = 0; si < rt->script_count; ++si) {
				if (!rt->scripts[si].active) continue;
				rt_script_ref_fixed_update(rt->scripts[si].ref, fixed_dt);
			}

			jce_physics_step(rt->physics, fixed_dt);
			/* Configurable-joint break monitor: compare each tracked joint's
			 * last-step applied impulse against break_force * fixed_dt (impulse =
			 * force * dt) and snap on overrun.  Gated on cfg_joint_count -> no-op
			 * when no scene authored a breakable configurable joint. */
			rt_monitor_configurable_joints(rt, fixed_dt);
			/* Motor edits (a script or the Inspector changing
			 * JceConstraintComponent) reach the solver here.  Gated on
			 * joint_count and on an actual change -> a scene with no
			 * motorised joint pays one comparison per joint. */
			rt_sync_joint_motors(rt);
			if (rt->physics2d) {
				jce_physics2d_step(rt->physics2d, fixed_dt);
				rt_monitor_joints2d(rt);   /* break_force / break_torque */
			}
			/* Networking on the fixed cadence (only when a session is live —
			 * checked per tick so a session started AFTER runtime create is
			 * still driven): poll the host, advance handshake/timeout,
			 * dispatch packets + replication.  No-op otherwise. */
			if (jce_session_mode() != JCE_SESSION_MODE_NONE) {
				jce_session_tick();
				/* Sample + broadcast transform snapshots, then encode the
				 * acked-baseline delta / late-joiner burst.  The tick id is
				 * the engine-wide fixed clock so client interpolation shares
				 * one timeline with the sim. */
				/* Drive net off rt->clock — the clock this loop actually
				 * advances — so editor Play (which never runs the engine-wide
				 * fixed clock) matches the shipped binary instead of a frozen
				 * global tick (audit F82). */
				uint32_t ntick = (uint32_t)rt->clock.tick_count;
				jce_net_transform_fixed_step(ntick);
				/* SAMPLE BEFORE STEPPING.  fixed_step builds and sends the
				 * snapshot from whatever the authority last pushed, so a push
				 * that arrives after it is a tick late -- forever, not once. */
				rt_push_net_animator_states(rt);
				jce_net_animator_fixed_step(ntick);
				jce_net_replication_tick((JceNetTick)ntick);
			}
			jce_fixed_clock_tick(&rt->clock);
			/* Capture post-tick poses for render interpolation. */
			rt_capture_fixed_pose(rt);
			rt->have_prev = true;
		}

		/* Blend the last two fixed poses by the residual accumulator so the
		 * rendered transform is smooth even when steps==0 this frame. */
		rt_sync_transforms(rt, (float)rt->clock.alpha);
		/* 2D bodies aren't interpolated — write the latest Box2D pose. */
		if (rt->physics2d && steps > 0)
			rt_sync_transforms2d(rt);
	} else if (jce_session_mode() != JCE_SESSION_MODE_NONE) {
		/* No physics world → no fixed loop above advances rt->clock, so tick
		 * it once per frame (the existing "one net tick per frame" cadence)
		 * and drive net off it.  Reading the engine-wide clock here froze the
		 * tick under editor Play, which never advances it (audit F82). */
		jce_session_tick();
		jce_fixed_clock_tick(&rt->clock);
		uint32_t ntick = (uint32_t)rt->clock.tick_count;
		jce_net_transform_fixed_step(ntick);
		jce_net_replication_tick((JceNetTick)ntick);
	}
	jce_perf_phase_add("physics", jce_time_perf_to_ms(_t0_physics, jce_time_perf_counter()));

	/* Floating-origin large-world rebase (opt-in, default OFF).  Runs after the
	 * physics sync above (so camera + entities + bodies are at their final
	 * frame poses) and BEFORE scene_update / rendering, so the whole frame
	 * downstream observes the rebased coordinates atomically.  A scene without
	 * floating_origin_enabled returns immediately → byte-identical frame path. */
	rt_apply_floating_origin(rt);
	{
		uint64_t _t0_su = jce_time_perf_counter();
		if (rt->scene)
			jce_scene_update(rt->scene, sim_dt);
		jce_perf_phase_add("scene_update", jce_time_perf_to_ms(_t0_su, jce_time_perf_counter()));
	}
	rt_streaming_tick(rt);   /* after scene_update: uses THIS frame's player pos */
	/* Root motion (FEATURE 3.2): apply the root-joint delta the renderer
	 * extracted last frame to entities whose avatar has apply_root_motion set.
	 * Runs after scene_update so it adds on top of the freshest gameplay
	 * transforms, and before the net/sequencer steps so downstream consumers
	 * see the root-motion-advanced pose.  No-op while paused (no new delta is
	 * produced) and for every entity without apply_root_motion. */
	if (rt->scene && !rt->paused)
		jce_scene_each_entity(rt->scene, rt_apply_root_motion, rt);
	/* Trail Renderer point capture (large-world #E): append each emitting
	 * trail's entity world position to its point buffer once it has moved past
	 * min_vertex_distance (FIFO when the buffer is full).  Runs after the final
	 * frame transforms, only during Play (jce_runtime_step) and never paused, so
	 * trails grow as entities move; sr_draw_trail_renderer draws the buffer. */
	if (rt->scene && !rt->paused)
		rt_trail_step(rt, dt);
	/* Networking render-step: write interpolated poses for non-owned
	 * networked objects + apply owned-object snap corrections.  Runs after
	 * scene_update (so gameplay-owned transforms are final) and before the
	 * gameplay/audio bridge below.  No-op without a live session. */
	if (rt->net_bridged && jce_session_mode() != JCE_SESSION_MODE_NONE)
		jce_net_transform_render_step((double)rt->clock.alpha);
		jce_net_animator_render_step((double)rt->clock.alpha);
	/* Client-side prediction reconcile (runtime-driven so the net layer stays
	 * prediction-free): pop any authoritative snapshot the net layer stashed
	 * for the predicted entity and rollback+replay against it, writing the
	 * reconciled pose.  Runs after render_step (which has consumed inbound
	 * packets + skipped its own snap for this object).  No-op when prediction
	 * is not established. */
	if (rt->predict_buf)
		rt_predict_reconcile(rt);
	/* SequencePlayer: advance every playing .seq.json and apply evaluated
	 * track values to the bound entities' components.  Runs after
	 * scene_update (freshest transforms) and before video/particles so
	 * downstream systems see sequenced values this frame.  Runtime-only:
	 * the editor previews through the Sequencer panel instead. */
	if (rt->scene)
		jce_scene_sequencer_update(rt->scene, sim_dt);
	/* VideoPlayer-as-texture: advance every playing clip one frame and
	 * upload it into its component texture (the scene renderer binds it as
	 * mesh albedo).  Runs once per runtime step; the editor drives the same
	 * call when NOT in play mode so Scene View previews video too. */
	if (rt->scene)
		jce_scene_video_update(rt->scene, (double)sim_dt, NULL, NULL);
	/* Particle emitters: load authored *.particles.json into the scene's
	 * shared JceParticleSystem, sync emitter origins to entity world
	 * positions, step the sim, and debug-draw alive particles.  Runs after
	 * scene_update for the freshest transforms (mirrors video-as-texture). */
	{
		uint64_t _t0_par = jce_time_perf_counter();
		if (rt->scene)
			jce_scene_particles_update(rt->scene, sim_dt);
		jce_perf_phase_add("particles", jce_time_perf_to_ms(_t0_par, jce_time_perf_counter()));
	}
	/* Variable-rate gameplay bridge: trigger overlap, spawn density, weapon
	 * timers.  Runs after scene_update so it reads the freshest transforms. */
	{
		uint64_t _t0_game = jce_time_perf_counter();
		rt_tick_gameplay(rt, sim_dt);
		jce_perf_phase_add("gameplay", jce_time_perf_to_ms(_t0_game, jce_time_perf_counter()));
	}
	/* Physics draw-distance (big-world spawn): spawn deferred small static
	 * colliders within radius of the player, despawn far ones.  Runs after
	 * the physics sync above so it reads the player's final frame pose; a
	 * no-op when nothing was deferred (dd_count == 0). */
	{
		uint64_t _t0_dd = jce_time_perf_counter();
		rt_drive_draw_distance(rt);
		jce_perf_phase_add("draw_dist", jce_time_perf_to_ms(_t0_dd, jce_time_perf_counter()));
	}
	/* Wire any entities spawned this frame (jce.spawn) now that the script
	 * update loop has finished iterating scripts[]. */
	rt_flush_pending_spawns(rt);
	/* Perform any queued fracture body-swaps POST-step: the intact body is
	 * destroyed and replaced by Voronoi fragment bodies here, never inside a
	 * contact callback / mid-solve.  No-op when nothing broke this frame. */
	rt_flush_pending_fractures(rt);
	/* Upload + play any play_on_awake clips whose async decode finished. */
	rt_audio_poll(rt);
	rt_update_audio_3d(rt, sim_dt);
	/* Advance the adaptive music director on the REAL frame dt so the beat/
	 * bar playhead tracks wall-clock like the rest of audio (no-op when the
	 * scene authored no MusicTrack). */
	rt_tick_music(rt, dt);

	jce_perf_phase_add("runtime_tick", jce_time_perf_to_ms(_t0_tick, jce_time_perf_counter()));

	rt_input_expire_frame_sample(rt);   /* jce_rt_input.c */
}

/* ── Time control (Phase 0.2) ─────────────────────────────────────── */

JCE_API void JCE_CALL jce_runtime_set_time_scale(JceRuntime *rt, float scale)
{
	if (!rt) return;
	if (scale < 0.0f)   scale = 0.0f;
	if (scale > 100.0f) scale = 100.0f;
	rt->time_scale = scale;
}

/* Camera trauma shake (gap 6.5): forward to the live VCam resolver's shake
 * generator.  Trauma is process-global state inside jce_vcam_system (the editor
 * / shipped game only ever drives one game camera), so this is a thin pass-
 * through that does not need a per-runtime field; the next
 * jce_vcam_system_evaluate (driven by the Game View / default loop) folds the
 * decaying offset into the active VCam's pose.  Safe with NULL rt (the seam is
 * also surfaced to gameplay scripts as jce.shake_camera(amount)). */
JCE_API void JCE_CALL jce_runtime_shake_camera(JceRuntime *rt, float amount)
{
	(void)rt;
	jce_vcam_system_add_trauma(amount);
}

/* DESTRUCTION/FRACTURE: queue a fracturable entity to shatter.  The actual
 * body-swap (destroy intact body, spawn Voronoi convex-hull fragments) runs
 * deferred POST-step in rt_flush_pending_fractures, so this is safe to call
 * from a contact callback / on_collision script / mid-step gameplay code.  A
 * no-op when the entity has no ENABLED JceFracture component (gate), keeping
 * the path inert for non-fracturable entities. */
JCE_API void JCE_CALL jce_runtime_fracture_entity(JceRuntime *rt, uint64_t entity)
{
	if (!rt || !rt->scene) return;
	JceEntity e = (JceEntity)entity;
	JceFractureComponent *fc = jce_scene_get_fracture(rt->scene, e);
	if (!fc || !fc->enabled) return;   /* gate: only enabled fracturables */

	/* De-dup: an entity already queued this frame is not enqueued twice. */
	for (int i = 0; i < rt->pending_fracture_count; ++i)
		if (rt->pending_fractures[i] == e) return;

	if (rt->pending_fracture_count >= rt->pending_fracture_cap &&
	    !rt_grow_pending_fractures(rt))
		return;
	rt->pending_fractures[rt->pending_fracture_count++] = e;
}

/* ── Vehicle control (VEHICLE last-mile) ──────────────────────────── */

JCE_API void JCE_CALL jce_runtime_vehicle_set_input(JceRuntime *rt,
                                                    uint64_t entity,
                                                    float throttle,
                                                    float brake,
                                                    float steer)
{
	if (!rt || !rt->physics) return;
	VehicleEntry *ve = rt_vehicle_for_entity(rt, (JceEntity)entity);
	if (!ve) return;
	jce_physics_vehicle_set_input(rt->physics, ve->veh, throttle, brake, steer);
}

JCE_API float JCE_CALL jce_runtime_vehicle_get_speed(JceRuntime *rt,
                                                     uint64_t entity)
{
	if (!rt || !rt->physics) return 0.0f;
	VehicleEntry *ve = rt_vehicle_for_entity(rt, (JceEntity)entity);
	if (!ve) return 0.0f;
	return jce_physics_vehicle_get_speed(rt->physics, ve->veh);
}

JCE_API bool JCE_CALL jce_runtime_reload_script(JceRuntime *rt, const char *path)
{
	/* The VM to recompile into is the one that LOADED this path's language,
	 * and only if it is already live: a language with no VM has no instance
	 * to rebind, and standing an interpreter up to discover that would be
	 * absurd.  NULL therefore also covers "nothing in this scene is written
	 * in that language". */
	JceScript *vm = rt_script_vm_for_path_existing(rt, path);
	if (!rt || !vm || !path || !path[0]) {
		/* Previously a bare `return` with no log at all — a file watcher
		 * firing before the VM exists looked exactly like a successful
		 * reload that changed nothing. */
		LOG_WARN(LOG_TAG,
		         "hot-reload ignored: no live script VM for '%s' or empty path",
		         path ? path : "(null)");
		return false;
	}

	uint64_t size = 0;
	void *src = rt_read_asset_with_fallback(rt, path, &size);
	if (!src || size == 0) {
		if (src) jce_free(src);
		LOG_WARN(LOG_TAG, "hot-reload: cannot read '%s'", path);
		return false;
	}
	char chunkname[256];
	snprintf(chunkname, sizeof chunkname, "@%s", path);
	JceScriptModule mod = jce_script_compile_module(vm, chunkname,
	                                                (const char *)src, (size_t)size);
	jce_free(src);
	if (mod == 0) {
		LOG_WARN(LOG_TAG, "hot-reload: '%s' failed to compile; keeping previous", path);
		return false;
	}
	int rebound = 0;
	for (int i = 0; i < rt->script_count; ++i) {
		if (rt->scripts[i].active &&
		    strcmp(rt->scripts[i].script_path, path) == 0) {
			/* Same path => same language => the module we just compiled
			 * belongs to exactly the VM this ref carries. */
			rt_script_ref_rebind(rt->scripts[i].ref, mod);
			rebound++;
		}
	}
	/* Rebound instances now reference `mod` via their metatable, so releasing
	 * this temp handle is safe (the module stays alive while in use). */
	jce_script_release_module(vm, mod);
	LOG_INFO(LOG_TAG, "hot-reload: '%s' -> rebound %d instance(s)", path, rebound);
	/* rebound == 0 is success: the file compiled, nothing live uses it yet. */
	return true;
}

JCE_API bool JCE_CALL jce_runtime_dispatch_ui_click(JceRuntime *rt,
                                                    uint64_t button_entity,
                                                    const char *handler)
{
	/* Clean no-op when there is no VM or no authored handler — a UIButton with
	 * an empty on_click_handler is just a visual button (hover/press tint only),
	 * not an error. */
	if (!rt || !handler || !handler[0]) return false;
	return rt_script_call_named(rt, handler, (JceScriptEntity)button_entity);
}

JCE_API bool JCE_CALL jce_runtime_dispatch_ui_value_changed(JceRuntime *rt,
                                                            uint64_t entity)
{
	/* Resolve the widget on the runtime's own scene + fire its authored
	 * on_value_changed as fn(entity, value).  Slider/Toggle/Dropdown only;
	 * a non-widget entity or empty handler is a clean no-op. */
	if (!rt || !rt->scene) return false;
	JceEntity e = (JceEntity)entity;
	JceUISliderComponent *sl = jce_scene_get_ui_slider(rt->scene, e);
	if (sl)
		return rt_script_call_named_num(rt, sl->on_value_changed,
		                                (JceScriptEntity)entity, (double)sl->value);
	JceUIToggleComponent *tg = jce_scene_get_ui_toggle(rt->scene, e);
	if (tg)
		return rt_script_call_named_num(rt, tg->on_value_changed,
		                                (JceScriptEntity)entity, tg->is_on ? 1.0 : 0.0);
	JceUIDropdownComponent *dd = jce_scene_get_ui_dropdown(rt->scene, e);
	if (dd)
		return rt_script_call_named_num(rt, dd->on_value_changed,
		                                (JceScriptEntity)entity, (double)dd->selected_index);
	return false;
}

JCE_API bool JCE_CALL jce_runtime_dispatch_ui_text_changed(JceRuntime *rt,
                                                           uint64_t entity)
{
	if (!rt || !rt->scene) return false;
	JceUIInputFieldComponent *f = jce_scene_get_ui_input_field(rt->scene, (JceEntity)entity);
	if (!f) return false;
	return rt_script_call_named_str(rt, f->on_value_changed,
	                                (JceScriptEntity)entity, f->text);
}

JCE_API bool JCE_CALL jce_runtime_dispatch_ui_submit(JceRuntime *rt,
                                                     uint64_t entity)
{
	if (!rt || !rt->scene) return false;
	JceUIInputFieldComponent *f = jce_scene_get_ui_input_field(rt->scene, (JceEntity)entity);
	if (!f) return false;
	return rt_script_call_named_str(rt, f->on_submit,
	                                (JceScriptEntity)entity, f->text);
}

JCE_API void JCE_CALL jce_runtime_dispatch_ui_events(JceRuntime *rt,
                                                     JceUICanvas *uc,
                                                     JceScene *scene)
{
	/* Every channel is read exactly once, unconditionally, because two of the
	 * four (text_changed, submitted) are CLEARED ON READ -- skipping a read is
	 * not "no event this frame", it is "this event stays latched and fires on
	 * a later frame".  That is the bug this function exists to make
	 * unspellable, so the reads are not guarded by rt/scene: the drain must
	 * happen even when there is nothing to dispatch to. */
	if (!uc) return;

	uint64_t clicked = jce_ui_canvas_last_clicked(uc);
	uint64_t vc      = jce_ui_canvas_last_value_changed(uc);
	uint64_t tc      = jce_ui_canvas_last_text_changed(uc);
	uint64_t sub     = jce_ui_canvas_last_submitted(uc);

	if (!rt || !scene) return;

	if (clicked) {
		JceUIButtonComponent *bt =
			jce_scene_get_ui_button(scene, (JceEntity)clicked);
		if (bt)
			jce_runtime_dispatch_ui_click(rt, clicked, bt->on_click_handler);
	}
	if (vc)  jce_runtime_dispatch_ui_value_changed(rt, vc);
	if (tc)  jce_runtime_dispatch_ui_text_changed(rt, tc);
	if (sub) jce_runtime_dispatch_ui_submit(rt, sub);
}

JCE_API void JCE_CALL jce_runtime_dispatch_anim_event(JceRuntime *rt,
                                                      uint64_t entity,
                                                      const JceAnimEvent *ev)
{
	/* Clean no-op when there is no VM or no event — an animator whose entity
	 * authored no gameplay script (or no on_anim_event handler) simply fires
	 * nothing here; the scene renderer still logs the event. */
	if (!rt || !ev) return;
	for (int i = 0; i < rt->script_count; ++i) {
		struct ScriptEntry *se = &rt->scripts[i];
		if (!se->active) continue;
		if ((uint64_t)se->entity == entity) {
			rt_script_ref_anim_event(se->ref, ev->id, ev->name,
			                         ev->f0, ev->f1, ev->i0);
			return;   /* one instance per entity; first match wins */
		}
	}
}

JCE_API void JCE_CALL jce_runtime_dispatch_anim_state(JceRuntime *rt,
                                                      uint64_t entity,
                                                      const char *from_state,
                                                      const char *to_state)
{
	/* Clean no-op when there is no VM — an animator whose entity authored no
	 * gameplay script (or neither state handler) simply fires nothing here. */
	if (!rt) return;
	for (int i = 0; i < rt->script_count; ++i) {
		struct ScriptEntry *se = &rt->scripts[i];
		if (!se->active) continue;
		if ((uint64_t)se->entity == entity) {
			/* on_state_exit only when there is a real prior state (the
			 * initial enter has none); on_state_enter for the new state.
			 * jce_script_call_message tolerates a missing method, so a script
			 * defining only one (or neither) handler is a clean no-op. */
			if (from_state && from_state[0])
				rt_script_ref_message(se->ref, "on_state_exit",
				                      0.0, from_state);
			rt_script_ref_message(se->ref, "on_state_enter", 0.0, to_state);
			return;   /* one instance per entity; first match wins */
		}
	}
}

JCE_API float JCE_CALL jce_runtime_get_time_scale(const JceRuntime *rt)
{
	return rt ? rt->time_scale : 1.0f;
}

JCE_API void JCE_CALL jce_runtime_set_paused(JceRuntime *rt, bool paused)
{
	if (rt) rt->paused = paused;
}

JCE_API bool JCE_CALL jce_runtime_is_paused(const JceRuntime *rt)
{
	return rt ? rt->paused : false;
}

JCE_API void JCE_CALL jce_runtime_set_actor_budget(JceRuntime *rt,
                                                   uint32_t max_actors,
                                                   uint32_t per_frame_quota)
{
	if (!rt) return;
	rt->actor_budget      = max_actors;
	rt->actor_spawn_quota = per_frame_quota;
}

JCE_API void JCE_CALL jce_runtime_get_actor_stats(const JceRuntime *rt,
                                                  uint32_t *out_count,
                                                  uint32_t *out_budget)
{
	if (out_count)  *out_count  = rt ? rt->actor_count  : 0u;
	if (out_budget) *out_budget = rt ? rt->actor_budget : 0u;
}

JCE_API JceWorldOrigin *JCE_CALL jce_runtime_world_origin(JceRuntime *rt)
{
	return rt ? &rt->world_origin : NULL;
}

JCE_API float JCE_CALL jce_runtime_interpolation_alpha(const JceRuntime *rt)
{
	if (!rt || !rt->physics) return 0.0f;
	return (float)jce_fixed_clock_alpha(&rt->clock);
}

/* Internal trampoline: forwards engine contact events to the game callback. */
static void rt_contact_trampoline(const JceContactEvent *ev, void *ud)
{
	JceRuntime *rt = (JceRuntime *)ud;
	if (rt && rt->contact_cb) rt->contact_cb(ev, rt->contact_ud);
}

JCE_API void JCE_CALL jce_runtime_set_contact_listener(JceRuntime *rt,
                                                       jce_contact_listener_fn fn,
                                                       void *userdata)
{
	if (!rt) return;
	rt->contact_cb = fn;
	rt->contact_ud = userdata;
	/* Register the engine-side listener once, only when a game subscribes. */
	if (fn && rt->physics && !rt->contact_registered) {
		if (jce_physics_add_contact_listener(rt->physics,
		                                     rt_contact_trampoline, rt))
			rt->contact_registered = true;
	}
}

JCE_API void JCE_CALL jce_runtime_set_input(JceRuntime *rt,
                                             const JceRuntimeInput *in)
{
	if (!rt || !in) return;
	/* Whole-struct copy so a newly-added JceRuntimeInput field is NEVER silently
	 * dropped (the field-by-field copy was a recurring footgun — e.g. attack_pressed).
	 * Preserve the two explicit semantics afterwards: sticky jump (don't clear a
	 * jump that hasn't been consumed yet) and the speed_mult default. */
	bool sticky_jump = rt->input.jump_pressed;
	rt->input = *in;
	if (sticky_jump) rt->input.jump_pressed = true;
	if (rt->input.speed_mult <= 0.0f) rt->input.speed_mult = 1.0f;
}

JCE_API void JCE_CALL jce_runtime_set_pointer_input(JceRuntime *rt,
                                                     float dx, float dy,
                                                     float wheel,
                                                     uint32_t buttons)
{
	if (!rt) return;
	rt->input.pointer_dx      = dx;
	rt->input.pointer_dy      = dy;
	rt->input.pointer_wheel   = wheel;
	rt->input.pointer_buttons = buttons;
}

JCE_API void JCE_CALL jce_runtime_set_touch_input(
	JceRuntime *rt, const JceRuntimeTouch *touches, int count)
{
	int kept = 0;

	if (!rt) return;
	rt->input.touch_count = 0;
	memset(rt->input.touches, 0, sizeof(rt->input.touches));
	if (!touches || count <= 0) return;

	for (int i = 0; i < count && kept < JCE_RUNTIME_MAX_TOUCHES; ++i) {
		const JceRuntimeTouch *touch = &touches[i];
		if (!isfinite(touch->x) || !isfinite(touch->y) ||
		    !isfinite(touch->pressure))
			continue;
		rt->input.touches[kept++] = *touch;
	}
	rt->input.touch_count = kept;
}

JCE_API void JCE_CALL jce_runtime_set_actions(JceRuntime *rt,
                                              const JceInputActions *actions)
{
	if (!rt) return;
	rt->actions = actions;   /* borrowed; valid until the next call / frame */
}

/* ── Client-side prediction (rollback/replay) ─────────────────────────── */

/* Ring depth: ~0.5s of buffered inputs at 60Hz is plenty of rollback runway
 * for typical RTT; the ring stores fixed-size blobs so this is cheap. */
#define RT_PREDICT_RING_CAPACITY  64u

JCE_API void JCE_CALL jce_runtime_set_predicted_entity(JceRuntime *rt,
                                                       uint64_t entity)
{
	if (!rt) return;

	/* Tear down any existing prediction first (also handles the entity==0
	 * "disable" case and re-establishing on a different entity). */
	if (rt->predict_buf) {
		if (rt->predict_entity != 0)
			jce_net_transform_set_predicted(rt->predict_entity, false);
		jce_prediction_buffer_destroy(rt->predict_buf);
		rt->predict_buf    = NULL;
		rt->predict_entity = 0;
	}
	if (entity == 0) return;

	/* Refresh the kinematic tuning from the engine fixed cadence + (when the
	 * predicted entity is the character) its authored movement feel, so the
	 * pure step fn tracks the same speeds the Bullet driver uses. */
	jce_predict_loco_params_default(&rt->predict_params);
	{
		double gdt = jce_fixed_clock_default()->fixed_dt;
		if (gdt > 0.0) rt->predict_params.dt = (float)gdt;
	}
	if (entity == (uint64_t)rt->character_entity) {
		if (rt->char_move_speed  > 0.0f) rt->predict_params.move_speed  = rt->char_move_speed;
		if (rt->char_sprint_mult > 0.0f) rt->predict_params.sprint_mult = rt->char_sprint_mult;
	}

	JcePredictionBuffer *buf =
		jce_prediction_buffer_create((uint32_t)sizeof(JcePredictInput),
		                             (uint32_t)sizeof(JcePredictState),
		                             RT_PREDICT_RING_CAPACITY);
	if (!buf) {
		LOG_WARN(LOG_TAG, "prediction: buffer alloc failed for entity %llu",
		         (unsigned long long)entity);
		return;
	}

	/* Seed the baseline from the entity's current transform (pos + yaw). */
	JcePredictState init;
	memset(&init, 0, sizeof init);
	if (rt->scene) {
		JceTransform *tc = jce_scene_get_transform(rt->scene, (JceEntity)entity);
		if (tc) {
			init.pos[0] = tc->position.x;
			init.pos[1] = tc->position.y;
			init.pos[2] = tc->position.z;
			jce_vec3 fwd = jce_q_rotate(tc->rotation, jce_v3(0.0f, 0.0f, 1.0f));
			init.yaw = atan2f(fwd.x, fwd.z);
		}
	}
	jce_prediction_set_initial_state(buf, &init);

	rt->predict_buf    = buf;
	rt->predict_entity = entity;

	/* Tell the net layer this owned object is runtime-predicted so it stops
	 * applying its own snap-correction (we own the transform now).  Silent
	 * no-op if the entity has no registered net transform — the prediction
	 * ring still works for a local/offline predicted entity. */
	jce_net_transform_set_predicted(entity, true);

	LOG_INFO(LOG_TAG, "prediction: established for entity %llu "
	         "(dt=%.4f move=%.2f sprint=%.2f cap=%u)",
	         (unsigned long long)entity,
	         (double)rt->predict_params.dt,
	         (double)rt->predict_params.move_speed,
	         (double)rt->predict_params.sprint_mult,
	         (unsigned)RT_PREDICT_RING_CAPACITY);
}

JCE_API uint64_t JCE_CALL jce_runtime_predicted_entity(const JceRuntime *rt)
{
	return rt ? rt->predict_entity : 0;
}

JCE_API bool JCE_CALL jce_runtime_get_player_position(const JceRuntime *rt,
                                                       jce_vec3 *out_pos)
{
	if (!rt || !rt->physics || !jce_character_valid(rt->character)) return false;
	/* Return the SMOOTHED entity transform that rt_sync_transforms wrote
	 * (interpolated between fixed ticks + projected to the feet), so a follow
	 * camera tracks the same smooth pose as the rendered mesh — not the raw
	 * per-tick physics center, which jitters between ticks at render rate. */
	if (rt->scene && rt->character_entity != 0) {
		JceTransform *tc = jce_scene_get_transform(rt->scene, rt->character_entity);
		if (tc) { if (out_pos) *out_pos = tc->position; return true; }
	}
	jce_vec3 p;
	jce_physics_character_get_position(rt->physics, rt->character, &p);
	p.y -= rt->character_half_height;   /* fallback: raw feet */
	if (out_pos) *out_pos = p;
	return true;
}

JCE_API bool JCE_CALL jce_runtime_get_player_forward(const JceRuntime *rt,
                                                      jce_vec3 *out_fwd)
{
	if (!rt || !rt->physics || !jce_character_valid(rt->character)) return false;
	jce_vec3 fwd;
	if (rt->char_yaw_valid) {
		/* Live locomotion yaw (matches the renderer's facing). */
		fwd = jce_v3(sinf(rt->char_yaw), 0.0f, cosf(rt->char_yaw));
	} else if (rt->scene && rt->character_entity != 0) {
		/* Frame 0 of Play: char_yaw not yet computed — derive the spawn
		 * forward from the entity transform exactly as the locomotion
		 * lazy-init does (jce_q_rotate(rotation,+Z) -> horizontal). */
		JceTransform *tc = jce_scene_get_transform(rt->scene, rt->character_entity);
		if (!tc) return false;
		jce_vec3 f = jce_q_rotate(tc->rotation, jce_v3(0.0f, 0.0f, 1.0f));
		float len = sqrtf(f.x * f.x + f.z * f.z);
		fwd = (len > 1e-4f) ? jce_v3(f.x / len, 0.0f, f.z / len)
		                    : jce_v3(0.0f, 0.0f, 1.0f);
	} else {
		fwd = jce_v3(0.0f, 0.0f, 1.0f);
	}
	if (out_fwd) *out_fwd = fwd;
	return true;
}

JCE_API void JCE_CALL jce_runtime_pause_audio(JceRuntime *rt)
{
	if (!rt || !rt->audio) return;
	for (int i = 0; i < rt->voice_count; ++i)
		jce_audio_pause(rt->audio, rt->voices[i].voice);
}

JCE_API void JCE_CALL jce_runtime_resume_audio(JceRuntime *rt)
{
	if (!rt || !rt->audio) return;
	for (int i = 0; i < rt->voice_count; ++i)
		jce_audio_resume(rt->audio, rt->voices[i].voice);
}

JCE_API JcePhysicsWorld *JCE_CALL jce_runtime_physics(const JceRuntime *rt)
{
	return rt ? rt->physics : NULL;
}

JCE_API JceAudio *JCE_CALL jce_runtime_audio(const JceRuntime *rt)
{
	return rt ? rt->audio : NULL;
}

JCE_API JceScene *JCE_CALL jce_runtime_scene(const JceRuntime *rt)
{
	return rt ? rt->scene : NULL;
}

JCE_API JceSnapshotRegistry *JCE_CALL jce_runtime_save_registry(const JceRuntime *rt)
{
	return rt ? rt->save_registry : NULL;
}

JCE_API JceBtContext *JCE_CALL jce_runtime_bt_context(const JceRuntime *rt)
{
	return rt ? rt->bt_ctx : NULL;
}

JCE_API int JCE_CALL jce_runtime_bt_count(const JceRuntime *rt)
{
	return rt ? rt->bt_count : 0;
}

JCE_API JceBlackboard *JCE_CALL jce_runtime_bt_blackboard(const JceRuntime *rt,
                                                          uint64_t entity)
{
	if (!rt) return NULL;
	for (int i = 0; i < rt->bt_count; ++i)
		if ((uint64_t)rt->bts[i].entity == entity)
			return rt->bts[i].bb;
	return NULL;
}

JCE_API bool JCE_CALL jce_runtime_bt_tree(const JceRuntime *rt, uint64_t entity,
                                          uint32_t *out_tree_idx)
{
	if (!rt || !out_tree_idx) return false;
	for (int i = 0; i < rt->bt_count; ++i) {
		if ((uint64_t)rt->bts[i].entity == entity &&
		    jce_bt_tree_valid(rt->bts[i].tree)) {
			*out_tree_idx = rt->bts[i].tree.idx;
			return true;
		}
	}
	return false;
}

JCE_API JceGameplayAbilitySystem *JCE_CALL jce_runtime_entity_gas(JceRuntime *rt,
                                                                  uint64_t entity)
{
	return rt_gas_for_entity(rt, (JceEntity)entity);
}

JCE_API JceRagdoll *JCE_CALL jce_runtime_entity_ragdoll(JceRuntime *rt,
                                                        uint64_t entity)
{
	struct RagdollEntry *re = rt_ragdoll_for_entity(rt, (JceEntity)entity);
	return re ? re->rd : NULL;
}

JCE_API bool JCE_CALL jce_runtime_save_to_file(JceRuntime *rt, const char *path)
{
	if (!rt || !rt->save_registry || !path || !path[0]) return false;

	/* mkdir -p the parent directory so a fresh save slot just works. */
	const char *slash = strrchr(path, '/');
	const char *bslash = strrchr(path, '\\');
	if (bslash && (!slash || bslash > slash)) slash = bslash;
	if (slash && slash != path) {
		char dir[640];
		size_t dl = (size_t)(slash - path);
		if (dl < sizeof dir) {
			memcpy(dir, path, dl);
			dir[dl] = '\0';
			if (!jce_fs_host_exists_dir(dir))
				jce_fs_host_create_directory(dir);
		}
	}
	return jce_snapshot_save_to_file(rt->save_registry, path);
}

JCE_API bool JCE_CALL jce_runtime_load_from_file(JceRuntime *rt, const char *path)
{
	if (!rt || !rt->save_registry || !path || !path[0]) return false;

	/* READ THE BYTES BEFORE TOUCHING THE LIVE SESSION.
	 *
	 * A missing or unreadable slot is the COMMON failure -- a player picks an
	 * empty slot, a file is gone, a path is wrong -- and it must not cost them
	 * the session they are in.  Loading the file first means that case returns
	 * false having changed nothing at all. */
	uint64_t size = 0;
	void *buf = jce_fs_host_read_all(path, &size);
	if (!buf || size == 0) {
		if (buf) jce_free(buf);
		jce_log_write(JCE_LOG_LEVEL_WARN, LOG_TAG, __FILE__, __LINE__,
		              "save load: cannot read '%s' (session unchanged)", path);
		return false;
	}

	/* TEAR DOWN FIRST, exactly as the transition path does.  The snapshot's
	 * read callback destroys and recreates every entity, so the bodies,
	 * controllers, script instances and voices keyed to the OLD ids must be
	 * released BEFORE the swap -- releasing them afterwards would be releasing
	 * them against ids that no longer mean anything, and skipping it would leak
	 * a whole scene's worth of physics bodies per load. */
	rt_teardown_scene_state(rt, /*destroying=*/false);

	const bool ok = jce_snapshot_load_from_buffer(rt->save_registry, buf,
	                                              (size_t)size);
	jce_free(buf);
	if (!ok) {
		/* Readable but not restorable: a section may have applied before the
		 * failure, so the scene is now indeterminate.  Leave a consistent,
		 * body-free empty scene and respawn against it rather than a
		 * half-restored one the caller cannot reason about. */
		jce_scene_clear(rt->scene);
		rt_spawn_scene_state(rt);
		jce_log_write(JCE_LOG_LEVEL_ERROR, LOG_TAG, __FILE__, __LINE__,
		              "save load: '%s' is unreadable as a snapshot (scene now "
		              "empty)", path);
		return false;
	}

	/* THE HALF THAT WAS MISSING.  The scene provider's read callback clears
	 * the scene and loads fresh entities, so every runtime object keyed to the
	 * OLD entity ids -- physics bodies, character controllers, script
	 * instances, audio voices, triggers, spawners, behaviour trees -- now
	 * refers to entities that no longer exist.  Without this walk the restored
	 * scene renders and does nothing, which is why driving
	 * jce_snapshot_load_from_file directly (as the header used to suggest) was
	 * never a working save path.
	 *
	 * jce_runtime_request_scene has always had to do exactly this after its
	 * own jce_scene_clear + load; this is the same requirement, reached from
	 * the other direction. */
	rt_spawn_scene_state(rt);
	jce_log_write(JCE_LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__,
	              "save load: restored '%s'", path);
	return true;
}

JCE_API void JCE_CALL jce_runtime_set_locale(JceRuntime *rt, const char *locale)
{
	(void)rt; /* jce_loc is process-global; rt kept for API symmetry */
	if (!locale || !locale[0]) return;
	jce_loc_set_locale(locale);
}
