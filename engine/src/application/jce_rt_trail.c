/*
 * jce_rt_trail.c  Trail Renderer capture + ageing.
 *
 * Split out of jce_runtime.c, which is one of the size-frozen files and sat
 * AT its baseline.  The pass grew a per-point ageing step when
 * JceTrailRendererComponent.time stopped being ignored.
 */

#include "jce_rt_internal.h"

#include <string.h>

/* Trail Renderer capture: append the entity's world position to its trail point
 * buffer when it has moved past min_vertex_distance, FIFO-dropping the oldest
 * sample when the buffer is full.  (Length is bounded by JCE_TRAIL_MAX_POINTS
 * rather than the `time` field — a per-point age buffer is a follow-up.) */
typedef struct { JceRuntime *rt; float dt; } RtTrailCtx;

static void rt_trail_capture_cb(JceScene *scene, JceEntity e, void *ud)
{
	if (!jce_scene_has_trail_renderer(scene, e)) return;
	JceTrailRendererComponent *tr = jce_scene_get_trail_renderer(scene, e);
	if (!tr) return;

	/* JceTrailRendererComponent.time -- "seconds points persist" -- had
	 * nothing to measure against: a trail was a fixed 64-point ring that
	 * dropped its oldest sample only when full, so a trail on a slow object
	 * lasted forever and one on a fast object lasted a fraction of a second.
	 *
	 * <= 0 keeps the ring behaviour, which is what a zeroed component carries
	 * and therefore what every existing scene means.
	 *
	 * Ageing runs even when NOT emitting: that is the whole point of a fade,
	 * and it is what autodestruct waits for. */
	RtTrailCtx *tctx = (RtTrailCtx *)ud;
	float tdt = tctx ? tctx->dt : 0.0f;
	if (tr->time > 0.0f && tr->point_count > 0 && tdt > 0.0f) {
		int keep = 0;
		for (int i = 0; i < tr->point_count; ++i) {
			float age = tr->point_age[i] + tdt;
			if (age >= tr->time) continue;      /* expired -- drop */
			tr->points[keep][0] = tr->points[i][0];
			tr->points[keep][1] = tr->points[i][1];
			tr->points[keep][2] = tr->points[i][2];
			tr->point_age[keep] = age;
			++keep;
		}
		tr->point_count = keep;

		/* .autodestruct: Unity destroys the object once the trail has faded
		 * out.  It needs `time` to fade at all, which is why both were dead
		 * together.  Only once the trail has actually emitted and drained. */
		if (keep == 0 && tr->autodestruct && !tr->emitting &&
		    tctx && tctx->rt && tctx->rt->scene)
			jce_scene_destroy_entity(tctx->rt->scene, e);
	}

	if (!tr->emitting) return;

	jce_vec3 wp = rt_world_position(scene, e);
	if (tr->point_count <= 0) {
		tr->points[0][0] = wp.x; tr->points[0][1] = wp.y; tr->points[0][2] = wp.z;
		tr->point_age[0] = 0.0f;
		tr->point_count = 1;
		return;
	}
	int   last = tr->point_count - 1;
	float dx = wp.x - tr->points[last][0];
	float dy = wp.y - tr->points[last][1];
	float dz = wp.z - tr->points[last][2];
	float mind = tr->min_vertex_distance > 0.0f ? tr->min_vertex_distance : 0.1f;
	if (dx*dx + dy*dy + dz*dz < mind*mind) return;   /* not moved enough */

	if (tr->point_count < JCE_TRAIL_MAX_POINTS) {
		int n = tr->point_count++;
		tr->points[n][0] = wp.x; tr->points[n][1] = wp.y; tr->points[n][2] = wp.z;
		tr->point_age[n] = 0.0f;
	} else {
		memmove(&tr->points[0][0], &tr->points[1][0],
		        (size_t)(JCE_TRAIL_MAX_POINTS - 1) * 3u * sizeof(float));
		/* The ages ride with the points; shifting one and not the other
		 * would age each sample by whatever the sample ahead of it was. */
		memmove(&tr->point_age[0], &tr->point_age[1],
		        (size_t)(JCE_TRAIL_MAX_POINTS - 1) * sizeof(float));
		tr->points[JCE_TRAIL_MAX_POINTS - 1][0] = wp.x;
		tr->points[JCE_TRAIL_MAX_POINTS - 1][1] = wp.y;
		tr->points[JCE_TRAIL_MAX_POINTS - 1][2] = wp.z;
		tr->point_age[JCE_TRAIL_MAX_POINTS - 1] = 0.0f;
	}
}

/* Per fixed step: age the trails, expire what the authored `time` says is
 * gone, and append this frame's sample.  Gated by the caller on a live,
 * unpaused scene. */
void rt_trail_step(JceRuntime *rt, float dt)
{
	if (!rt || !rt->scene) return;
	RtTrailCtx tctx = { rt, dt };
	jce_scene_each_entity(rt->scene, rt_trail_capture_cb, &tctx);
}
