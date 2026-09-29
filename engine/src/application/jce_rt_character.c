/*
 * jce_rt_character.c  The runtime's character controller.
 *
 * Its own translation unit for the reason jce_rt_physics.c / jce_rt_script.c /
 * jce_rt_audio.c / jce_rt_streaming.c / jce_rt_input.c are: jce_runtime.c sits
 * at the size gate's frozen baseline, and this code had to make room for a
 * change rather than the other way round.
 *
 * Everything here reached only public API plus one file-static helper
 * (rt_clip_by_name, used by nothing else), which is what made the move
 * mechanical.
 */

#include "jce_rt_internal.h"

#include <jce/middleware/scene/jce_scene.h>

#include <string.h>

#define LOG_TAG "runtime"

/* First clip whose name contains `want` (case-insensitive), or -1. */
static int rt_clip_by_name(const JceSkeletalAnimatorComponent *sa, const char *want)
{
	if (!sa) return -1;
	for (int i = 0; i < sa->clip_count && i < 8; i++) {
		for (const char *p = sa->clip_names[i]; *p; p++) {
			int k = 0;
			while (want[k] &&
			       tolower((unsigned char)p[k]) == tolower((unsigned char)want[k]))
				k++;
			if (!want[k]) return i;
		}
	}
	return -1;
}

void rt_drive_character(JceRuntime *rt, float dt)
{
	if (!rt->physics || !jce_character_valid(rt->character)) return;
	if (dt <= 0.0f) dt = 1.0f / 60.0f;

	/* Direction (clamped to unit length) × authored speed: movement feel
	 * comes from the CharacterController component, not the caller.
	 * speed_mult stays an extra gameplay multiplier (crouch, slow zones). */
	float dx = rt->input.walk_x, dz = rt->input.walk_z;
	float dl = sqrtf(dx * dx + dz * dz);
	if (dl > 1.0f) { dx /= dl; dz /= dl; dl = 1.0f; }
	float m = rt->input.speed_mult > 0.0f ? rt->input.speed_mult : 1.0f;
	if (rt->input.sprint) m *= rt->char_sprint_mult;
	float    speed = rt->char_move_speed * m;
	jce_vec3 walk  = jce_v3(dx * speed, 0.0f, dz * speed);
	jce_physics_character_move(rt->physics, rt->character, walk, dt);

	/* Jump with buffering + coyote time: a press up to JUMP_BUFFER s early
	 * still fires on landing, and stepping off a ledge keeps a JUMP_COYOTE
	 * grace window — both standard platformer forgiveness mechanics. */
	const float JUMP_BUFFER = 0.12f, JUMP_COYOTE = 0.12f;
	bool grounded = jce_physics_character_is_grounded(rt->physics, rt->character);
	rt->char_coyote_t = grounded ? JUMP_COYOTE : rt->char_coyote_t - dt;
	if (rt->input.jump_pressed) {
		rt->char_jump_buf_t    = JUMP_BUFFER;
		rt->input.jump_pressed = false;
	} else if (rt->char_jump_buf_t > 0.0f) {
		rt->char_jump_buf_t -= dt;
	}
	bool jumped = false;
	if (rt->char_jump_buf_t > 0.0f && (grounded || rt->char_coyote_t > 0.0f)) {
		/* Only consume the buffer / pulse the anim trigger when the jump
		 * actually fired (the physics layer refuses mid-ascent repeats). */
		if (jce_physics_character_jump(rt->physics, rt->character)) {
			rt->char_jump_buf_t = 0.0f;
			rt->char_coyote_t   = 0.0f;
			jumped = true;
		}
	}
	/* Variable jump height: releasing jump while ascending cuts the rest
	 * of the rise, so taps hop and holds clear the full arc. */
	if (rt->char_jump_was_held && !rt->input.jump_held && !grounded)
		jce_physics_character_cut_jump(rt->physics, rt->character, 0.45f);
	rt->char_jump_was_held = rt->input.jump_held;

	/* Generic locomotion (engine-level, so it works in Play AND shipped):
	 * turn the character toward its move direction and feed the live
	 * physics state to whatever animation driver the entity carries. */
	if (rt->scene && rt->character_entity != 0) {
		JceTransform *tf = jce_scene_get_transform(rt->scene, rt->character_entity);
		jce_vec3 vel = jce_v3(0.0f, 0.0f, 0.0f);
		jce_physics_character_get_velocity(rt->physics, rt->character, &vel);
		float plan_speed = sqrtf(vel.x * vel.x + vel.z * vel.z);

		/* Smooth shortest-arc turn at the authored turn speed (the old
		 * instant snap reads as robotic with a real model). */
		if (tf && dl > 0.0001f) {
			/* dx/dz are a WORLD move direction (physics velocity / camera-
			 * relative input), so the yaw derived from them is a world yaw.
			 * Seeding it from the raw tf->rotation -- a LOCAL rotation for a
			 * parented character -- mixed the two spaces, and writing the
			 * result back into tf->rotation turned a character riding a
			 * rotating parent by its parent's orientation on top of its own.
			 * rt_push_external_transforms caches char_last_rot in world too,
			 * so a local write here would read as a gizmo edit every frame. */
			jce_vec3 cwp;
			jce_quat cwr;
			if (!jce_scene_get_world_pose(rt->scene, rt->character_entity,
			                              &cwp, &cwr, NULL))
				goto skip_turn;
			if (!rt->char_yaw_valid) {
				jce_vec3 fwd = jce_q_rotate(cwr, jce_v3(0.0f, 0.0f, 1.0f));
				rt->char_yaw       = atan2f(fwd.x, fwd.z);
				rt->char_yaw_valid = true;
			}
			float target = atan2f(dx, dz);
			float diff   = target - rt->char_yaw;
			while (diff >  JCE_PI) diff -= 2.0f * JCE_PI;
			while (diff < -JCE_PI) diff += 2.0f * JCE_PI;
			float max_turn = rt->char_turn_speed * dt;
			if (diff >  max_turn) diff =  max_turn;
			if (diff < -max_turn) diff = -max_turn;
			rt->char_yaw += diff;
			{
				const jce_quat yawq = jce_q_from_axis_angle(
					jce_v3(0.0f, 1.0f, 0.0f), rt->char_yaw);
				/* In place: this runs every frame the character turns, and
				 * jce_scene_set_world_pose would bump the subtree's
				 * world-cache generation each time. */
				jce_scene_solve_local_pose(rt->scene, rt->character_entity,
				                           cwp, yawq, NULL, &tf->rotation);
				/* World, matching the cache push reads.  Our own yaw, not a
				 * gizmo edit. */
				rt->char_last_rot = yawq;
			}
		}
skip_turn:
		if (jce_scene_has_skeletal_animator(rt->scene, rt->character_entity)) {
			JceSkeletalAnimatorComponent *sa =
				jce_scene_get_skeletal_animator(rt->scene, rt->character_entity);
			/* Live locomotion state for the renderer's SM / blend-tree
			 * driver (consumed when auto_speed is set): physics velocity is
			 * steadier than the transform-derived estimate, and grounded /
			 * vertical-velocity / jump unlock Jump+Fall+Land SM states. */
			sa->loco_valid    = true;
			sa->loco_grounded = grounded;
			sa->loco_speed    = plan_speed;
			sa->loco_vert_vel = vel.y;
			if (jumped) sa->loco_jump = true;   /* one-shot, consumer clears */
			/* If an animation state machine is bound it OWNS active_clip
			 * (driven by the engine "Speed" param in the renderer). Setting
			 * the clip by name here too would fight the SM every frame and
			 * make it flicker — only name-drive when there is no SM. */
			if (!sa->sm_path[0]) {
				int idx = -1;
				if (!grounded && rt->char_coyote_t <= 0.0f) {
					/* Airborne: prefer a jump/fall clip when the model has
					 * one; otherwise keep the current ground clip. */
					idx = rt_clip_by_name(sa, vel.y > 0.5f ? "jump" : "fall");
					if (idx < 0) idx = rt_clip_by_name(sa, "jump");
				} else {
					const char *want =
					    (plan_speed <= 0.15f) ? "idle"
					  : (plan_speed > rt->char_move_speed * 1.15f) ? "run"
					                                               : "walk";
					idx = rt_clip_by_name(sa, want);
					if (idx < 0 && plan_speed > 0.15f)
						idx = rt_clip_by_name(sa, "walk");
				}
				if (idx >= 0) sa->active_clip = idx;
			}
			sa->playing = true;
		}
	}
}

/* Spawn the scene's character controller, if this entity carries one.
 *
 * Returns true when the entity IS a character and must therefore not also
 * become a rigid body -- the caller's `goto try_audio`, preserved exactly:
 * true only when the component is present and enabled, and true even when
 * jce_physics_character_create fails, which is what the inline version did.
 *
 * ONE CHARACTER PER SCENE, AND IT USED TO BE SILENT ABOUT IT.  The runtime
 * holds a single JceCharacterHandle and sixteen scalar fields beside it
 * (char_cur_pos, char_move_speed, char_yaw, the coyote and jump-buffer
 * timers), so the first entity with an enabled CharacterController wins and
 * every later one is skipped.  Nothing said so: not a log line, not the
 * inspector, not the boot summary -- a designer who placed two of them got
 * one, and the second entity fell through to the RIGID BODY path instead,
 * which looks like a character that ignores its controller rather than one
 * that was refused.
 *
 * Lifting the limit is a different change with a semantic decision inside it
 * (which character does JceRuntimeInput drive?), and a half-done multi-
 * character refactor would be worse than a limit that is stated.  So this
 * states it: every skipped controller is named in a WARN, and the boot
 * summary reports how many were authored next to how many exist.
 */
bool rt_character_try_spawn(JceRuntime *rt, JceScene *scene, JceEntity e,
                            const JceTransform *tf)
{
	JceCharacterControllerComponent *cc;

	if (!rt || !rt->physics) return false;

	cc = jce_scene_get_character_controller(scene, e);
	if (!cc || !jce_scene_component_enabled(scene, e,
	                                        JCE_COMP_FLAG_CHARACTER_CONTROLLER))
		return false;

	rt->character_authored++;
	if (jce_character_valid(rt->character)) {
		LOG_WARN(LOG_TAG,
		         "entity %llu has an enabled CharacterController but the scene "
		         "already has one (entity %llu) -- the runtime supports ONE per "
		         "scene, so this entity gets no character and falls through to "
		         "the rigid-body path",
		         (unsigned long long)e,
		         (unsigned long long)rt->character_entity);
		return false;
	}

	JceCharacterDesc cd;
	memset(&cd, 0, sizeof cd);
	cd.position      = tf->position;
	cd.radius        = cc->radius      > 0.0f ? cc->radius      : 0.35f;
	cd.height        = cc->height      > 0.0f ? cc->height      : 1.8f;
	cd.step_height   = cc->step_offset > 0.0f ? cc->step_offset : 0.35f;
	cd.max_slope_deg = cc->slope_limit > 0.0f ? cc->slope_limit : 50.0f;
	cd.gravity       = 9.81f;
	cd.jump_speed    = cc->jump_speed  > 0.0f ? cc->jump_speed  : 5.0f;
	cd.accel         = cc->accel;        /* 0 → physics default */
	cd.air_control   = cc->air_control;
	cd.layer         = cc->physics_layer;  /* 0 = Default, as for bodies */
	/* The scene Transform is the FEET (feet-pivoted meshes), but the
	 * Bullet capsule is centered on its origin — spawn the CENTER half
	 * a height above the feet so it rests instead of sinking on frame 0. */
	cd.position.y   += 0.5f * cd.height;
	rt->character = jce_physics_character_create(rt->physics, &cd);
	if (jce_character_valid(rt->character)) {
		rt->character_entity      = e;
		rt->character_half_height = 0.5f * cd.height;
		rt->char_cur_pos          = cd.position;   /* capsule center */
		rt->char_prev_pos         = cd.position;
		rt->char_last_pos         = tf->position;  /* feet */
		rt->char_last_rot         = tf->rotation;
		rt->char_move_speed  = cc->move_speed  > 0.0f ? cc->move_speed  : 4.0f;
		rt->char_sprint_mult = cc->sprint_mult > 0.0f ? cc->sprint_mult : 1.8f;
		rt->char_turn_speed  = (cc->turn_speed_deg > 0.0f
		                        ? cc->turn_speed_deg : 720.0f) * JCE_DEG2RAD;
		rt->char_yaw_valid   = false;
		rt->char_coyote_t    = 0.0f;
		rt->char_jump_buf_t  = 0.0f;
		rt->char_jump_was_held = false;
	}
	return true;
}
