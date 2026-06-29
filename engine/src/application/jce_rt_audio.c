/*
 * jce_rt_audio.c  Runtime audio module (split from jce_runtime.c).
 *
 * The audio-occlusion raycast adapter, the audio-mixer bus tree setup +
 * per-frame apply, reverb-zone collection + blend, navmesh load (placed
 * here with the listener/3D-audio update it feeds), and the spatial-voice /
 * listener 3D-audio update.  Pure move from the monolithic runtime:
 * cross-module entry points are declared in jce_rt_internal.h, everything
 * else stays file-static here.  No behaviour change.
 */

#include "jce_rt_internal.h"

/* Occlusion raycast adapter: returns the segment fraction at first physics
 * hit (1.0 = unobstructed). No material DB → mid absorption. */
static float rt_occlusion_raycast(void *ud, jce_vec3 origin, jce_vec3 dir,
                                  float max_distance, float *out_material)
{
	JceRuntime *rt = (JceRuntime *)ud;
	if (out_material) *out_material = 0.5f;
	if (!rt || !rt->physics || max_distance <= 0.0f) return 1.0f;
	JceRaycastResult r = jce_physics_raycast(rt->physics, origin, dir, max_distance);
	if (!r.hit) return 1.0f;
	float frac = r.distance / max_distance;
	return frac < 0.0f ? 0.0f : (frac > 1.0f ? 1.0f : frac);
}

/* ── Audio mixer buses (P1-audio-mixer-reverb) ──────────────────────── */

/* Seed the default bus layout matching the editor's mixer panel. */
static void rt_mixer_seed_default(JceAudioMixer *m)
{
	jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Music", 0.8f);
	jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "SFX",   1.0f);
	jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Voice", 1.0f);
	jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "UI",    1.0f);
}

/* Read audio_mixer.json into a heap buffer (caller frees with
 * jce_fs_buffer_free).  Returns NULL on miss / over-size; *out_size set to the
 * byte length.  Shared by the device-free bus/sends/sidechain/snapshot apply
 * and the device-gated effect-attach pass so the file is read once. */
static char *rt_mixer_read_config(const char *path, uint64_t *out_size)
{
	if (out_size) *out_size = 0;
	if (!path || !path[0]) return NULL;
	uint64_t size = 0;
	char *raw = (char *)jce_fs_host_read_all(path, &size);
	if (!raw) return NULL;
	if (size > (1u << 20)) { jce_fs_buffer_free(raw); return NULL; }
	if (out_size) *out_size = size;
	return raw;
}

/* Device-gated effect-attach callback: each authored insert effect for a bus is
 * attached to the LIVE audio device by bus name (jce_audio_bus_add_effect), in
 * authored chain order.  `user` is the JceRuntime* (rt->audio is non-NULL — the
 * enumeration only runs when the device exists). */
static void rt_mixer_attach_effect_cb(const char *bus_name,
                                      const JceAudioEffectDesc *desc,
                                      uint32_t index, void *user)
{
	(void)index;
	JceRuntime *rt = (JceRuntime *)user;
	if (!rt || !rt->audio || !bus_name || !bus_name[0] || !desc) return;
	jce_audio_bus_add_effect(rt->audio, bus_name, desc);
}

/* Stand up the runtime mixer + mirror its buses onto the audio device as
 * ma_sound_group buses.  Called once at create() when audio exists.
 *
 * Consumes the FULL authored routing from audio_mixer.json so it goes live at
 * Play start (was: bus tree only, leaving authored sends/sidechain/snapshots/
 * effects INERT):
 *   1. DEVICE-FREE: jce_audio_mixer_apply_config builds the bus tree AND the
 *      aux sends, per-bus sidechain ducking, and named snapshots onto the
 *      JceAudioMixer (pure CPU bookkeeping).
 *   2. mirror each non-Master bus onto the audio device.
 *   3. DEVICE-GATED: attach each bus's authored insert-effect chain to the live
 *      device by name (jce_audio_bus_add_effect) — only here, after the device
 *      buses exist; headless/no-device Play is a safe no-op (this whole function
 *      early-returns when rt->audio is NULL).
 * Ordering is deterministic: tree+routing -> device buses -> effects. */
void rt_init_mixer(JceRuntime *rt, const char *config_path)
{
	if (!rt->audio) return;
	rt->mixer = jce_audio_mixer_create();
	if (!rt->mixer) return;

	uint64_t cfg_size = 0;
	char *cfg = rt_mixer_read_config(config_path, &cfg_size);

	/* 1. Device-free: bus tree + sends + sidechain + snapshots. */
	bool applied = cfg && jce_audio_mixer_apply_config(rt->mixer, cfg,
	                                                   (size_t)cfg_size);
	if (!applied)
		rt_mixer_seed_default(rt->mixer);

	/* 2. Mirror every non-Master bus onto the audio device. */
	JceAudioBusId ids[64];
	uint32_t n = jce_audio_mixer_list_buses(rt->mixer, ids, 64);
	for (uint32_t i = 0; i < n; ++i) {
		if (ids[i] == JCE_AUDIO_BUS_MASTER) continue;
		const char *nm = jce_audio_mixer_get_name(rt->mixer, ids[i]);
		if (nm && nm[0]) jce_audio_bus_create(rt->audio, nm);
	}

	/* 3. Device-gated: attach authored per-bus insert-effect chains to the
	 * live device by bus name (after the device buses exist). */
	if (applied && cfg)
		jce_audio_mixer_config_each_effect(cfg, (size_t)cfg_size,
		                                   rt_mixer_attach_effect_cb, rt);

	if (cfg) jce_fs_buffer_free(cfg);
}

/* Pick the mixer bus for an AudioSource.  No bus field exists on the
 * component, so derive one by role: looping non-spatial = Music (BGM),
 * everything else = SFX, falling back to whatever buses the project
 * actually defined. */
const char *rt_bus_for_source(const JceRuntime *rt,
                                     const JceAudioSourceComponent *as,
                                     bool spatial)
{
	/* Explicit per-source bus override (large-world audio) wins when it names
	 * a bus the project actually defined; else fall back to the role heuristic. */
	if (as->mixer_bus[0] && rt->mixer &&
	    jce_audio_mixer_find_bus(rt->mixer, as->mixer_bus) != JCE_AUDIO_BUS_INVALID)
		return as->mixer_bus;
	const char *want = (!spatial && as->loop) ? "Music" : "SFX";
	if (rt->mixer && jce_audio_mixer_find_bus(rt->mixer, want)
	        != JCE_AUDIO_BUS_INVALID)
		return want;
	if (rt->mixer && jce_audio_mixer_find_bus(rt->mixer, "SFX")
	        != JCE_AUDIO_BUS_INVALID)
		return "SFX";
	return NULL;   /* route direct to Master */
}

/* Push the resolved (solo/mute/volume) gain of every bus onto its matching
 * audio-device group each frame, so live mixer edits drive playback.  Master
 * maps to the engine master volume. */
static void rt_apply_mixer(JceRuntime *rt, float dt)
{
	if (!rt->audio || !rt->mixer) return;
	/* FEATURE 5.2: advance any in-flight snapshot crossfade so authored bus
	 * volumes interpolate before we push them to the device this frame. */
	jce_audio_mixer_update(rt->mixer, dt);
	JceAudioBusId ids[64];
	uint32_t n = jce_audio_mixer_list_buses(rt->mixer, ids, 64);
	for (uint32_t i = 0; i < n; ++i) {
		const char *nm = jce_audio_mixer_get_name(rt->mixer, ids[i]);
		if (!nm || !nm[0]) continue;
		/* Resolve gain *with* the sidechain duck folded in (duck gain is 1.0
		 * for buses without a sidechain, so this is a no-op for them). */
		float gain = jce_audio_mixer_resolve_volume_ducked(rt->mixer, ids[i]);
		jce_audio_bus_set_volume(rt->audio, nm, gain);
	}
}

/* ── Reverb zones (P1-audio-mixer-reverb) ───────────────────────────── */

/* Map the Unity-style component preset selector onto a generic DSP preset. */
static JceReverbPreset rt_reverb_preset_for(int preset)
{
	switch (preset) {
	case JCE_REVERB_ZONE_PRESET_OFF: {
		JceReverbPreset p = jce_reverb_preset_outdoor();
		p.wet_mix = 0.0f;
		return p;
	}
	case JCE_REVERB_ZONE_PRESET_ROOM:
	case JCE_REVERB_ZONE_PRESET_LIVING_ROOM:
	case JCE_REVERB_ZONE_PRESET_BATHROOM:
	case JCE_REVERB_ZONE_PRESET_PADDED_CELL:
		return jce_reverb_preset_room();
	case JCE_REVERB_ZONE_PRESET_AUDITORIUM:
	case JCE_REVERB_ZONE_PRESET_CONCERT_HALL:
	case JCE_REVERB_ZONE_PRESET_ARENA:
	case JCE_REVERB_ZONE_PRESET_HANGAR:
	case JCE_REVERB_ZONE_PRESET_STONE_ROOM:
		return jce_reverb_preset_hall();
	case JCE_REVERB_ZONE_PRESET_CAVE:
	case JCE_REVERB_ZONE_PRESET_SEWER_PIPE:
	case JCE_REVERB_ZONE_PRESET_QUARRY:
		return jce_reverb_preset_cave();
	case JCE_REVERB_ZONE_PRESET_UNDERWATER:
		return jce_reverb_preset_underwater();
	case JCE_REVERB_ZONE_PRESET_GENERIC:
	case JCE_REVERB_ZONE_PRESET_FOREST:
	case JCE_REVERB_ZONE_PRESET_CITY:
	case JCE_REVERB_ZONE_PRESET_MOUNTAINS:
	case JCE_REVERB_ZONE_PRESET_PLAIN:
	case JCE_REVERB_ZONE_PRESET_PARKINGLOT:
	default:
		return jce_reverb_preset_outdoor();
	}
}

/* Map a USER (custom) AudioReverbZone's EAX-style detail params onto the
 * engine-agnostic JceReverbPreset.  Without this the runtime fell back to a
 * built-in preset and dropped every authored field when preset == USER.
 *
 * The component fields are Unity/EAX units (millibels, seconds, Hz, %); the
 * preset is normalised floats.  Conversions:
 *   room (mB, -10000..0)      -> wet_mix via the standard mB->linear curve
 *   decay_time (s)            -> decay_seconds (and a room_size hint)
 *   decay_hf_ratio + room_hf  -> damping (less HF persistence = more damping)
 *   diffusion/density (%)     -> diffusion/density (0..1)
 *   reflections/reverb delay  -> pre_delay_ms
 *   hf_reference (Hz)         -> lowpass_hz
 * Everything is clamped so a half-authored zone stays well-formed. */
static float rt_clamp01(float v)
{
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static JceReverbPreset rt_reverb_preset_from_user(
    const JceAudioReverbZoneComponent *rz)
{
	JceReverbPreset p;
	memset(&p, 0, sizeof p);

	/* millibels -> linear gain (0 mB = 1.0, -10000 mB ~= 0).  10^(mB/2000). */
	float wet = powf(10.0f, rz->room / 2000.0f);
	p.wet_mix       = rt_clamp01(wet);
	p.dry_mix       = 1.0f;

	float decay = rz->decay_time > 0.0f ? rz->decay_time : 1.0f;
	p.decay_seconds = decay;
	/* room_size hint scales with the tail (1..1000 m); ~30 m per second. */
	float rs = 30.0f * decay;
	p.room_size     = rs < 1.0f ? 1.0f : (rs > 1000.0f ? 1000.0f : rs);

	/* HF persistence: ratio<1 absorbs highs faster -> more damping; also fold
	 * the room_hf attenuation in (more negative = darker). */
	float hf_keep = rz->decay_hf_ratio > 0.0f ? rz->decay_hf_ratio : 1.0f;
	float damp    = 1.0f - rt_clamp01(hf_keep * 0.5f);
	float room_hf_lin = powf(10.0f, rz->room_hf / 2000.0f);
	damp += (1.0f - rt_clamp01(room_hf_lin)) * 0.5f;
	p.damping       = rt_clamp01(damp);

	p.diffusion     = rt_clamp01(rz->diffusion / 100.0f);
	p.density       = rt_clamp01(rz->density   / 100.0f);

	float pre = (rz->reflections_delay + rz->reverb_delay) * 1000.0f;
	if (pre < 0.0f)   pre = 0.0f;
	if (pre > 300.0f) pre = 300.0f;
	p.pre_delay_ms  = pre;

	float lp = rz->hf_reference > 0.0f ? rz->hf_reference : 22050.0f;
	if (lp < 100.0f)   lp = 100.0f;
	if (lp > 22050.0f) lp = 22050.0f;
	p.lowpass_hz    = lp;

	return p;
}

/* Walk the scene for AudioReverbZone components and build the runtime zone
 * set.  Each zone becomes a sphere at the entity's world position: full
 * strength within min_distance, blending out to max_distance. */
static void rt_reverb_zone_collect(JceScene *scene, JceEntity e, void *ud)
{
	JceRuntime *rt = (JceRuntime *)ud;
	JceAudioReverbZoneComponent *rz = jce_scene_get_audio_reverb_zone(scene, e);
	if (!rz) return;
	if (!rt->reverb_zones) {
		rt->reverb_zones = jce_reverb_zones_create(16);
		if (!rt->reverb_zones) return;
	}
	jce_vec3 wp = rt_world_position(scene, e);

	JceReverbZoneDesc d;
	memset(&d, 0, sizeof d);
	d.shape          = JCE_REVERB_SHAPE_SPHERE;
	d.center         = wp;
	float maxd       = rz->max_distance > 0.0f ? rz->max_distance : 10.0f;
	float mind       = rz->min_distance > 0.0f ? rz->min_distance : 0.0f;
	if (mind > maxd) mind = maxd;
	d.extents        = jce_v3(mind, 0.0f, 0.0f);     /* full-strength radius */
	d.falloff_radius = (maxd - mind) > 0.0f ? (maxd - mind) : 1.0f;
	d.priority       = 0;
	/* USER preset: honour the authored EAX-style detail params instead of a
	 * built-in fallback (previously the custom fields were silently dropped). */
	if (rz->preset == JCE_REVERB_ZONE_PRESET_USER)
		d.preset     = rt_reverb_preset_from_user(rz);
	else
		d.preset     = rt_reverb_preset_for(rz->preset);
	jce_reverb_zones_add(rt->reverb_zones, &d);
}

void rt_build_reverb_zones(JceRuntime *rt)
{
	if (!rt->audio || !rt->scene) return;
	jce_scene_each_entity(rt->scene, rt_reverb_zone_collect, rt);
}

/* ── Navigation (P1-navmesh-chain) ───────────────────────────────────
 *
 * Load the editor-baked Detour navmesh and stand up a nav-agent set
 * bound to it via jce_recast_path_fn, so JceNavAgent destinations
 * resolve through jce_recast_find_path.  No-op when no path is supplied
 * or it fails to load.
 *
 * Runs BEFORE the rt_spawn_gameplay walk so authored NavAgent components
 * can register into the set as they are visited; rt_tick_gameplay then
 * syncs/steps the agents and writes positions back to the transforms.
 * The load -> query chain is also proven by a snap+find_path self-test
 * logged at create. */
void rt_init_navmesh(JceRuntime *rt, const char *navmesh_path)
{
	if (!navmesh_path || !navmesh_path[0]) return;

	rt->nav_recast = jce_recast_load_file(navmesh_path);
	if (!rt->nav_recast) {
		jce_log_write(JCE_LOG_LEVEL_WARN, LOG_TAG, __FILE__, __LINE__,
		              "navmesh: failed to load '%s' (navigation disabled)",
		              navmesh_path);
		return;
	}

	/* Agent set with no bound grid navmesh — paths come from the Recast
	 * backend via the path-fn below. */
	rt->nav_agents = jce_nav_agent_set_create(NULL, 256u);
	if (rt->nav_agents)
		jce_nav_agent_set_path_fn(rt->nav_agents, jce_recast_path_fn,
		                          rt->nav_recast);

	/* Self-test: snap two points onto the mesh and prove a query path,
	 * so a broken load surfaces immediately in the log rather than as a
	 * silent no-path at runtime. */
	JceRecastStats st;
	jce_recast_get_stats(rt->nav_recast, &st);
	float sx, sy, sz;
	int   probe = 0;
	if (jce_recast_snap_to_navmesh(rt->nav_recast, 0.0f, 0.0f,
	                               &sx, &sy, &sz)) {
		float wp[2 * 8];
		probe = jce_recast_find_path(rt->nav_recast, sx, sz, sx, sz, wp, 8);
	}
	jce_log_write(JCE_LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__,
	              "navmesh: loaded '%s' (%d polys, %d verts); self-test path=%d",
	              navmesh_path, st.polygon_count, st.vertex_count, probe);
}

/* Find the first AudioListener component in the scene (last-mile: the runtime
 * tracks the listener POSITION from the primary camera, but the authored
 * JceAudioListenerComponent's volume/paused/doppler were never consumed). */
typedef struct {
	JceAudioListenerComponent *lc;   /* first match (NULL = none authored) */
} AudioListenerScan;

static void rt_pick_audio_listener(JceScene *s, JceEntity e, void *ud)
{
	AudioListenerScan *ctx = (AudioListenerScan *)ud;
	if (ctx->lc) return;                         /* already found the first */
	if (!jce_scene_has_audio_listener(s, e)) return;
	if (!jce_scene_component_enabled(s, e, JCE_COMP_FLAG_AUDIO_LISTENER)) return;
	ctx->lc = jce_scene_get_audio_listener(s, e);
}

/* Attach the listener to the primary camera's world position; push live
 * world positions to every spatial voice so distance attenuation tracks
 * scene movement; and attenuate each spatial voice by physics occlusion
 * (sound is quieter when a collider blocks the listener→source path). */
void rt_update_audio_3d(JceRuntime *rt, float dt)
{
	if (!rt->audio || !rt->scene) return;

	/* Push live bus gains every frame so mixer edits affect playback. */
	rt_apply_mixer(rt, dt);

	JceAudioListener L;
	memset(&L, 0, sizeof L);
	/* Engine convention: look down -Z, up = +Y.  Overwritten below by the
	 * primary camera's true world orientation when one exists. */
	L.forward[2] = -1.0f;
	L.up[1]      =  1.0f;

	CamScanCtx ctx = { rt->scene, { 0.0f, 0.0f, 0.0f }, false };
	jce_scene_each_entity(rt->scene, rt_pick_primary_cam, &ctx);
	if (ctx.found) {
		L.position[0] = ctx.pos.x;
		L.position[1] = ctx.pos.y;
		L.position[2] = ctx.pos.z;
		/* Drive panning/Doppler from where the camera actually looks, so
		 * turning the view re-spatialises the field (was hardwired -Z/+Y). */
		L.forward[0] = ctx.forward.x;
		L.forward[1] = ctx.forward.y;
		L.forward[2] = ctx.forward.z;
		L.up[0]      = ctx.up.x;
		L.up[1]      = ctx.up.y;
		L.up[2]      = ctx.up.z;
	}
	jce_audio_set_listener(rt->audio, &L);

	/* AudioListener component last-mile: when the scene authored an
	 * AudioListener, apply its global fields (the runtime previously consumed
	 * only the listener POSITION above).  Gated on the component existing, so
	 * a scene without one keeps the prior behaviour byte-for-byte.
	 *   volume  -> master gain (paused forces it to 0)
	 *   paused  -> mute the whole mix (master 0) without stopping voices
	 *   doppler -> global Doppler scale
	 * `spatialize` (HRTF toggle) has no public engine API yet -> followup. */
	{
		AudioListenerScan als = { NULL };
		jce_scene_each_entity(rt->scene, rt_pick_audio_listener, &als);
		if (als.lc) {
			float vol = als.lc->volume;
			if (vol < 0.0f) vol = 0.0f;
			if (als.lc->paused) vol = 0.0f;
			jce_audio_set_master_volume(rt->audio, vol);
			float dop = als.lc->doppler_factor;
			if (dop < 0.0f) dop = 0.0f;
			jce_audio_set_doppler_factor(rt->audio, dop);
		}
	}

	/* Position update + gather spatial sources for the occlusion solve. */
	enum { RT_MAX_OCC = 64 };
	JceAudioOcclusionQuery occ_q[RT_MAX_OCC];
	uint64_t               occ_ids[RT_MAX_OCC];
	int                    occ_vidx[RT_MAX_OCC];
	uint32_t               occ_n = 0;

	for (int i = 0; i < rt->voice_count; ++i) {
		if (!rt->voices[i].spatial) continue;
		jce_vec3 wp = rt_world_position(rt->scene, rt->voices[i].entity);
		jce_audio_voice_set_position(rt->audio, rt->voices[i].voice,
		                              wp.x, wp.y, wp.z);
		if (occ_n < RT_MAX_OCC) {
			occ_q[occ_n].source_position = wp;
			occ_ids[occ_n]  = (uint64_t)rt->voices[i].voice;
			occ_vidx[occ_n] = i;
			occ_n++;
		}
	}

	/* Occlusion: raycast listener->source against physics colliders, then
	   apply the result as a SEPARATE multiplicative gain over each voice's
	   authored base volume (so we never clobber the authored level), plus a
	   matching low-pass muffle.  The stateful tracker smooths per-source
	   attenuation across frames to avoid the pops the stateless solver gave
	   when a collider edge flickered in/out of the path. */
	if (rt->physics && occ_n > 0) {
		if (!rt->occ_tracker) {
			rt->occ_tracker = jce_audio_occlusion_tracker_create(RT_MAX_OCC);
			if (rt->occ_tracker) {
				JceAudioOcclusionParams op =
				    jce_audio_occlusion_default_params();
				jce_audio_occlusion_tracker_set_params(rt->occ_tracker, &op);
			}
		}
		jce_vec3 lp = jce_v3(L.position[0], L.position[1], L.position[2]);
		if (rt->occ_tracker) {
			jce_audio_occlusion_tracker_solve(rt->occ_tracker, lp, occ_ids,
			                                  occ_q, occ_n,
			                                  rt_occlusion_raycast, rt);
			jce_audio_occlusion_tracker_gc(rt->occ_tracker, occ_ids, occ_n);
		} else {
			/* Allocation failed: fall back to the stateless solve so audio
			 * still reacts to occlusion (just without temporal smoothing). */
			JceAudioOcclusionParams op =
			    jce_audio_occlusion_default_params();
			jce_audio_occlusion_solve(&op, lp, occ_q, occ_n,
			                          rt_occlusion_raycast, rt);
		}
		for (uint32_t k = 0; k < occ_n; ++k) {
			VoiceEntry *ve = &rt->voices[occ_vidx[k]];
			jce_audio_set_volume(rt->audio, ve->voice,
			                     ve->base_volume * occ_q[k].attenuation);
			jce_audio_set_lowpass(rt->audio, ve->voice, occ_q[k].lowpass_hz);
		}
	}

	/* ── Reverb zones: sample the blended preset at the listener and drive
	 * the global reverb DSP send wet/dry/decay.  Only active when the scene
	 * authored at least one AudioReverbZone (rt->reverb_zones non-NULL). */
	if (rt->reverb_zones && jce_reverb_zones_count(rt->reverb_zones) > 0) {
		jce_vec3 lp = jce_v3(L.position[0], L.position[1], L.position[2]);
		JceReverbPreset blend;
		jce_reverb_zones_sample(rt->reverb_zones, lp, &blend);

		JceAudioReverbParams rp;
		rp.wet_mix       = blend.wet_mix;
		rp.dry_mix       = blend.dry_mix;
		rp.decay_seconds = blend.decay_seconds;
		rp.room_size     = blend.room_size;
		rp.damping       = blend.damping;
		rp.diffusion     = blend.diffusion;
		rp.density       = blend.density;
		rp.pre_delay_ms  = blend.pre_delay_ms;
		rp.lowpass_hz    = blend.lowpass_hz;
		jce_audio_set_reverb(rt->audio, &rp);
	}
}

/* ── Pending async audio-source decode (play_on_awake) ──────── */

/* Play a resolved sound for an audio source + track its voice.  Shared by
 * the synchronous (callback) path and the async upload path. */
void rt_finish_audio_source(JceRuntime *rt, JceScene *scene,
                                   JceEntity e, JceSound snd,
                                   const JceAudioSourceComponent *as)
{
    if (snd == JCE_SOUND_INVALID || !as) return;

    float vol   = as->volume > 0.0f ? as->volume : 1.0f;
    float pitch = as->pitch  > 0.0f ? as->pitch  : 1.0f;
    JceVoice v = jce_audio_play(rt->audio, snd, as->loop, vol, pitch);
    bool spatial = (as->spatial_blend > 0.5f);
    if (spatial) {
        jce_audio_voice_set_3d(rt->audio, v, true);
        jce_vec3 wp = rt_world_position(scene, e);
        jce_audio_voice_set_position(rt->audio, v, wp.x, wp.y, wp.z);
        /* 3D attenuation: authorable per-source (large-world audio); a 0 model
         * / 0 distances fall back to the legacy defaults so pre-existing scenes
         * are unchanged. */
        JceAudioAttenuation atten = JCE_AUDIO_ATTEN_INVERSE;
        if (as->attenuation_model > 0)
            atten = (JceAudioAttenuation)(as->attenuation_model - 1);
        float a_min  = as->min_distance   > 0.0f ? as->min_distance   : 1.0f;
        float a_max  = as->max_distance   > 0.0f ? as->max_distance   : 25.0f;
        float a_roll = as->rolloff_factor > 0.0f ? as->rolloff_factor : 1.0f;
        jce_audio_voice_set_attenuation(rt->audio, v, atten,
                                        a_min, a_max, a_roll);
    } else {
        jce_audio_voice_set_3d(rt->audio, v, false);
    }
    const char *bus = rt_bus_for_source(rt, as, spatial);
    if (bus) jce_audio_voice_set_bus(rt->audio, v, bus);

    if (rt->voice_count >= rt->voice_cap && !rt_grow_voices(rt))
        return;
    rt->voices[rt->voice_count].entity      = e;
    rt->voices[rt->voice_count].sound       = snd;
    rt->voices[rt->voice_count].voice       = v;
    rt->voices[rt->voice_count].spatial     = spatial;
    rt->voices[rt->voice_count].base_volume = vol;
    rt->voices[rt->voice_count].bus[0]      = '\0';
    if (bus) {
        size_t bl = strlen(bus);
        if (bl >= sizeof(rt->voices[rt->voice_count].bus))
            bl = sizeof(rt->voices[rt->voice_count].bus) - 1;
        memcpy(rt->voices[rt->voice_count].bus, bus, bl);
        rt->voices[rt->voice_count].bus[bl] = '\0';
    }
    rt->voice_count++;
}

/* WORKER: decode a play_on_awake clip to CPU PCM (PAK + miniaudio). */
static void rt_audio_decode_run(void *arg)
{
    RtAudioDecodeArgs *a = (RtAudioDecodeArgs *)arg;
    a->cpu = jce_audio_decode_cpu(a->pak, a->path);
    jce_atomic_i32_store(a->done, 1);
}

RT_GROW_FN(rt_grow_pending_audio, pending_audio, pending_audio_cap, 8)

/* Kick an async decode of `as->clip_path` for entity `e` (default loader
 * path only).  The worker owns `args` (stable heap) for its full run; the
 * pending slot only references it, so the slot array may realloc freely. */
void rt_spawn_audio_async(JceRuntime *rt, JceEntity e,
                                 const JceAudioSourceComponent *as)
{
    if (rt->pending_audio_count >= rt->pending_audio_cap &&
        !rt_grow_pending_audio(rt)) {
        /* Out of queue memory — fall back to a synchronous load. */
        JceSound snd = jce_audio_load(rt->audio, rt->pak, as->clip_path);
        rt_finish_audio_source(rt, rt->scene, e, snd, as);
        return;
    }

    RtAudioDecodeArgs *args = (RtAudioDecodeArgs *)jce_malloc(sizeof(*args));
    if (!args) return;
    args->pak = rt->pak;
    snprintf(args->path, sizeof(args->path), "%s", as->clip_path);
    args->cpu  = NULL;
    args->done = jce_atomic_i32_create(0);

    JceThread *thr = jce_thread_create(rt_audio_decode_run, args, "jce_rt_audio");
    if (!thr) {
        /* No worker thread: decode + play inline, then drop the job. */
        rt_audio_decode_run(args);
        JceSound snd = jce_audio_upload_cpu(rt->audio, args->cpu);
        rt_finish_audio_source(rt, rt->scene, e, snd, as);
        if (args->done) jce_atomic_i32_destroy(args->done);
        jce_free(args);
        return;
    }

    RtPendingAudio *p = &rt->pending_audio[rt->pending_audio_count++];
    p->entity = e;
    p->thr    = thr;
    p->args   = args;
}

/* ── Adaptive music director (FEATURE 5.3) ───────────────────────────
 *
 * Build rt->music from the first entity that authored an (enabled)
 * MusicTrack component with play_on_awake.  Only one director is created
 * per runtime; subsequent MusicTrack entities are ignored.  The track is
 * expanded from jce_music_track_desc_default and seeded with the authored
 * bpm; the director is bound to rt->mixer so any layers it grows can drive
 * mixer buses.  Initial intensity is applied right after creation. */
void rt_spawn_music(JceRuntime *rt, JceScene *scene, JceEntity e)
{
    if (!rt || rt->music) return;   /* one director per runtime */

    JceMusicTrackComponent *mt = jce_scene_get_music_track(scene, e);
    if (!mt || !mt->play_on_awake) return;

    static int s_music_cid = -2;
    if (s_music_cid == -2) s_music_cid = jce_component_find("MusicTrack");
    if (s_music_cid >= 0 && !jce_scene_comp_enabled(scene, e, s_music_cid))
        return;

    JceMusicTrackDesc desc;
    if (!jce_music_track_desc_default(&desc)) return;
    if (mt->bpm > 0) desc.tempo_bpm = (float)mt->bpm;

    rt->music = jce_music_create(&desc, rt->mixer);
    if (!rt->music) {
        jce_log_write(JCE_LOG_LEVEL_WARN, LOG_TAG, __FILE__, __LINE__,
                      "music: failed to create director for track '%s'",
                      mt->track_path);
        return;
    }
    jce_music_set_intensity(rt->music, mt->initial_intensity);
    jce_log_write(JCE_LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__,
                  "music: director up (track='%s', %.0f bpm, intensity %.2f)",
                  mt->track_path, (double)desc.tempo_bpm,
                  (double)mt->initial_intensity);
}

/* MAIN thread, per-frame: advance the music director (playhead + layer
 * ramps + quantized transition firing).  Resolved layer gains are pushed
 * onto any bound mixer buses inside jce_music_update.  No-op when the scene
 * authored no music track. */
void rt_tick_music(JceRuntime *rt, float dt)
{
    if (!rt || !rt->music) return;
    jce_music_update(rt->music, dt);
}

/* MAIN thread, per-frame: upload + play any finished async audio decodes. */
void rt_audio_poll(JceRuntime *rt)
{
    if (!rt || rt->pending_audio_count == 0) return;
    int w = 0;
    for (int i = 0; i < rt->pending_audio_count; ++i) {
        RtPendingAudio *p = &rt->pending_audio[i];
        if (!p->args || jce_atomic_i32_load(p->args->done) == 0) {
            rt->pending_audio[w++] = *p;   /* keep (still running) */
            continue;
        }
        if (p->thr) { jce_thread_join(p->thr); p->thr = NULL; }

        /* Re-fetch the component at play time (the entity may have moved /
         * been disabled in the 1-2 frames since spawn). */
        JceAudioSourceComponent *as =
            jce_scene_get_audio_source(rt->scene, p->entity);
        if (as &&
            jce_scene_component_enabled(rt->scene, p->entity,
                                        JCE_COMP_FLAG_AUDIO_SOURCE)) {
            JceSound snd = jce_audio_upload_cpu(rt->audio, p->args->cpu);
            p->args->cpu = NULL;   /* consumed by upload */
            rt_finish_audio_source(rt, rt->scene, p->entity, snd, as);
        } else {
            jce_audio_cpu_free(p->args->cpu);   /* source gone — drop it */
            p->args->cpu = NULL;
        }
        jce_atomic_i32_destroy(p->args->done);
        jce_free(p->args);
        /* slot dropped (not copied to w) */
    }
    rt->pending_audio_count = w;
}
