/*
 * jce_rt_net.c -- the runtime's networking last mile.
 *
 * WHY A SEPARATE FILE.  jce_runtime.c is long past AGENTS.md's 3000-line cap,
 * and the jce_rt_* family beside it is where per-subsystem runtime work
 * already lives (audio, character, physics, script, streaming).  A sampler
 * that reads two components and calls one net function belongs there, not in
 * the file that is only over the cap because everything used to.
 */

#include "jce_rt_internal.h"

#include <jce/middleware/net/jce_net_animator.h>
#include <jce/middleware/scene/jce_scene.h>

#include <string.h>

/* Sample every locally-authoritative animator into the replication module.
 *
 * WHY THE RUNTIME AND NOT THE NET LAYER.  jce_net_animator.h refuses to read
 * an animation graph and is right to: reaching up into L5 would invert the
 * layering jce_net_transform.h keeps.  The runtime is ABOVE both, which is the
 * same reason it is the thing that consumes the root motion the renderer
 * publishes -- and the playhead reaches it the same way, on
 * JceSkeletalAnimatorComponent.norm_time.
 *
 * WHAT GOES OUT, and why each: the clip NAME hashed (receivers only compare
 * it, and a string costs bytes every tick forever); the playhead as a POSITION
 * so a receiver can take the shorter way round the loop; the speed, so a
 * receiver in slow motion stays in slow motion; and the two blend parameters,
 * because on a blend-tree character they are what decides the pose and a
 * receiver without them plays the right clip at the wrong blend.
 *
 * Authority is NOT decided here.  set_local_state records what this peer's
 * animator is doing; the module's own is_authority_for() decides whether that
 * gets broadcast, which keeps one answer to "am I the authority" rather than
 * two that can disagree. */
static void rt_push_net_animator_state_cb(JceScene *scene, JceEntity e,
                                          void *user)
{
	(void)user;
	JceNetAnimatorComponent *na = jce_scene_get_net_animator(scene, e);
	if (!na) return;
	JceSkeletalAnimatorComponent *sa = jce_scene_get_skeletal_animator(scene, e);
	if (!sa) return;
	/* The id the server assigned, mirrored back onto the authored component
	 * at adopt time -- the same field gameplay reads, so there is one answer
	 * to "which net object is this" rather than two. */
	JceNetworkObjectComponent *no = jce_scene_get_network_object(scene, e);
	if (!no) return;
	const JceNetObjectId id = (JceNetObjectId)no->net_id;
	if (id == JCE_NET_OBJECT_INVALID) return;

	JceNetAnimatorState st;
	memset(&st, 0, sizeof st);
	if (sa->active_clip >= 0 && sa->active_clip < sa->clip_count)
		st.state_hash = jce_net_animator_hash(sa->clip_names[sa->active_clip]);
	st.normalized_time = sa->norm_time;
	st.speed = (sa->speed > 0.0f) ? sa->speed : 1.0f;
	st.param_count = 2;
	st.param_hash[0]  = jce_net_animator_hash("Blend");
	st.param_value[0] = sa->blend_param;
	st.param_hash[1]  = jce_net_animator_hash("BlendY");
	st.param_value[1] = sa->blend_param_y;
	jce_net_animator_set_local_state(id, &st);
}

void rt_push_net_animator_states(JceRuntime *rt)
{
	/* Nothing registered ⇒ nothing to walk.  The counter is maintained at
	 * registration, so a scene with no networked animator pays one compare. */
	if (!rt || !rt->scene || rt->net_animator_count == 0) return;
	jce_scene_each_entity(rt->scene, rt_push_net_animator_state_cb, NULL);
}
