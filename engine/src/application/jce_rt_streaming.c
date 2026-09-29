/*
 * jce_rt_streaming.c  World streaming for the SHIPPED runtime.
 *
 * Its own translation unit for the reason jce_rt_physics.c / jce_rt_script.c /
 * jce_rt_audio.c are: jce_runtime.c is a 5000-line file the size gate freezes,
 * and a per-scene subsystem belongs beside its siblings rather than inside it.
 *
 * Guarded by tools/lint/check_world_streaming_shipped.py.
 */

#include "jce_rt_internal.h"

#include <jce/middleware/scene/jce_scene.h>
#include <jce/resource/jce_world_streamer.h>

#include <stdlib.h>   /* getenv */

#define LOG_TAG "runtime"

/* ── World streaming ────────────────────────────────────────────────────
 *
 * jce_world_streamer_create had exactly TWO callers in the whole tree before
 * this, both under editor/src/ -- the Play session and the scene-view preview.
 * The shipped runtime created none, so a scene that authored streaming loaded
 * none of it: the chunks were authored, serialized into the scene, and never
 * read.  Editor-only again, and invisible again for the same reason as every
 * other instance -- the configuration where it is broken is the one nobody
 * opens while authoring.
 *
 * A streamed entity is not just geometry: a script, a trigger or an NPC
 * authored in a streamed cell has to actually RUN when its cell arrives.  That
 * is what the spawn/despawn callbacks are for, and they are the same public
 * entry points the editor's Play callbacks use.
 */
static void rt_streamer_spawn_cb(const uint64_t *ids, uint32_t count, void *user)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (rt) jce_runtime_spawn_gameplay_for_ids(rt, ids, count);
}

static void rt_streamer_despawn_cb(const uint64_t *ids, uint32_t count, void *user)
{
	JceRuntime *rt = (JceRuntime *)user;
	if (rt) jce_runtime_despawn_gameplay_for_ids(rt, ids, count);
}

void rt_streaming_begin(JceRuntime *rt)
{
	if (!rt) return;
	/* Scene transitions come through here too: the old streamer holds chunk
	 * state for a scene that is gone, so it must die before the new one is
	 * built rather than leak pointing at freed entities. */
	if (rt->world_streamer) {
		jce_world_streamer_destroy(rt->world_streamer);
		rt->world_streamer = NULL;
	}
	if (!rt->asset_fs || !rt->scene) return;

	const JceSceneStreamingSettings *st =
		jce_scene_get_streaming_settings(rt->scene);
	if (!st || !st->enabled || st->chunk_count == 0) return;

	JceWorldStreamConfig wsc = jce_world_stream_config_default();
	wsc.mode = (st->mode == 1) ? JCE_STREAM_RECTANGULAR : JCE_STREAM_RADIAL;
	wsc.load_radius     = st->load_radius;
	wsc.unload_radius   = st->unload_radius;
	wsc.max_pending     = st->max_pending;
	wsc.budget_mb       = st->budget_mb;
	wsc.frame_budget_ms = st->frame_budget_ms;

	/* Same safety hatch and same bench toggle the editor path honours, read
	 * from the same variable, so an A/B measured in one is measurable in the
	 * other.  A second spelling here would make the two paths differ under
	 * exactly the flag used to compare them. */
	{
		const char *sync = getenv("JCE_STREAM_SYNC");
		wsc.single_thread = (sync && sync[0] && sync[0] != '0');
	}

	rt->world_streamer = jce_world_streamer_create(&wsc, rt->scene,
	                                               rt->asset_fs, NULL);
	if (!rt->world_streamer) {
		LOG_WARN(LOG_TAG, "%s",
		         "world streamer creation failed -- streaming disabled");
		return;
	}
	jce_world_streamer_register_from_scene_settings(rt->world_streamer, st);
	jce_world_streamer_set_entity_callbacks(rt->world_streamer,
	                                        rt_streamer_spawn_cb,
	                                        rt_streamer_despawn_cb, rt);
	LOG_INFO(LOG_TAG, "world streaming active (%u chunks, r=%.0f/%.0f)",
	         jce_world_streamer_chunk_count(rt->world_streamer),
	         (double)wsc.load_radius, (double)wsc.unload_radius);
}

void rt_streaming_tick(JceRuntime *rt)
{
	if (!rt || !rt->world_streamer) return;
	jce_vec3 pos;
	if (!jce_runtime_get_player_position(rt, &pos)) {
		/* No character yet: hold the origin so the inner ring around spawn
		 * still streams in on frame 0.  A streamer that waited for a player
		 * would show an empty world for exactly as long as the player took
		 * to exist. */
		pos = jce_v3(0.0f, 0.0f, 0.0f);
	}
	jce_world_streamer_update(rt->world_streamer, pos);
}
