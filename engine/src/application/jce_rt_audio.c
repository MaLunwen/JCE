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
#include <math.h>

JceSound rt_load_sound(JceRuntime *rt, const char *path)
{
	if (!rt || !rt->audio || !path || !path[0])
		return JCE_SOUND_INVALID;
	if (rt->audio_load_fn)
		return (JceSound)rt->audio_load_fn(rt->user_data, rt->audio, path);
	return jce_audio_load(rt->audio, rt->pak, path);
}

/* Nearest AudioOcclusion probe whose sphere contains the listener.  Nearest
 * rather than first so overlapping probes resolve the same way every frame
 * regardless of ECS iteration order. */
typedef struct {
	JceRuntime                       *rt;
	jce_vec3                          listener;
	const JceAudioOcclusionComponent *best;
	float                             best_d2;
} RtOccProbePick;

static void rt_occ_probe_cb(JceScene *s, JceEntity e, void *ud)
{
	RtOccProbePick *pk = (RtOccProbePick *)ud;
	if (!jce_scene_has_audio_occlusion(s, e)) return;
	const JceAudioOcclusionComponent *o = jce_scene_get_audio_occlusion(s, e);
	if (!o || o->radius <= 0.0f) return;
	if (!jce_scene_component_enabled(s, e, JCE_COMP_FLAG_AUDIO_OCCLUSION)) return;
	jce_vec3 c = rt_world_position(s, e);
	float dx = pk->listener.x - c.x;
	float dy = pk->listener.y - c.y;
	float dz = pk->listener.z - c.z;
	float d2 = dx * dx + dy * dy + dz * dz;
	if (d2 > o->radius * o->radius) return;      /* listener outside */
	if (pk->best && d2 >= pk->best_d2) return;
	pk->best = o;
	pk->best_d2 = d2;
}


/* Occlusion raycast adapter: returns the segment fraction at first physics
 * hit (1.0 = unobstructed). No material DB → mid absorption. */
static float rt_occlusion_raycast(void *ud, jce_vec3 origin, jce_vec3 dir,
                                  float max_distance, float *out_material)
{
	JceRuntime *rt = (JceRuntime *)ud;
	if (out_material) *out_material = 0.5f;
	if (!rt || !rt->physics || max_distance <= 0.0f) return 1.0f;
	/* AudioOcclusion.layer_mask: which physics layers actually block sound.
	 * This used an unfiltered raycast, so the mask -- authored, serialised
	 * and an editable DragInt in the Inspector -- selected nothing and every
	 * collider muffled everything.  rt->occ_layer_mask is refreshed from the
	 * probe each audio tick; 0 (a fresh component) means "no mask authored",
	 * NOT "nothing blocks", because zero-initialised components are what
	 * every scene older than this carries. */
	JceQueryFilter qf;
	qf.layer_mask   = rt->occ_layer_mask ? rt->occ_layer_mask : 0xFFFFFFFFu;
	qf.hit_triggers = false;
	JceRaycastResult r = jce_physics_raycast_filtered(rt->physics, origin, dir,
	                                                  max_distance, qf);
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
static char *rt_mixer_read_config(const JceRuntime *rt, const char *path,
                                  uint64_t *out_size)
{
	if (out_size) *out_size = 0;

	/* A NULL path is the SHIPPED case, not "no config".  jce_default_main
	 * only sets JceRuntimeDesc.mixer_config_path when the file exists ON THE
	 * HOST -- so in a single-file exe, where the config is inside the PAK and
	 * not on disk, the path arrives NULL and an early return here would make
	 * the PAK branch below unreachable in exactly the configuration it exists
	 * for.  Falling back to the cooked location lets the archive be asked.
	 * Fixed HERE rather than at the caller because the runtime owns where its
	 * own config lives, and the caller only knows what it could stat. */
	if (!path || !path[0]) path = "Settings/audio_mixer.json";

	/* Editor / dev: the live file on the host filesystem. */
	uint64_t size = 0;
	char *raw = (char *)jce_fs_host_read_all(path, &size);
	if (raw) {
		if (size > (1u << 20)) { jce_fs_buffer_free(raw); return NULL; }
		if (out_size) *out_size = size;
		return raw;
	}

	/* SHIPPED: the cooked copy inside the mounted PAK.  Without this a
	 * single-file exe read nothing here and fell back to the default bus
	 * tree -- no insert effects, and no aux sends either, both authored and
	 * both silently absent in the only build a player ever runs.  Host first
	 * so an editor edit stays live; PAK second so a packaged game finds it.
	 * Same order and same reason as rt_script_read_file. */
	if (!rt || !rt->pak) return NULL;
	const JcePakAsset *a = jce_pak_find(rt->pak, path);
	if (!a || a->original_size == 0 || a->original_size > (1u << 20))
		return NULL;
	/* The caller frees this with jce_fs_buffer_free, which is JCE_FREE, and
	 * the host branch above allocates with JCE_MALLOC inside
	 * jce_fs_host_read_all -- so both branches must hand back a pointer that
	 * same free accepts.  jce_malloc/jce_free are those macros verbatim
	 * (jce_alloc.c:9-22 is `return JCE_MALLOC(size);` and `JCE_FREE(ptr)`),
	 * so the public pair is exact AND keeps the internal
	 * engine/src/os/core/jce_memory.h out of the application layer. */
	char *pbuf = (char *)jce_malloc((size_t)a->original_size);
	if (!pbuf) return NULL;
	size_t n = jce_pak_decompress(a, pbuf, (size_t)a->original_size);
	if (n == 0) { jce_free(pbuf); return NULL; }
	if (out_size) *out_size = (uint64_t)n;
	return pbuf;
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
	char *cfg = rt_mixer_read_config(rt, config_path, &cfg_size);

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

	/* 2b. Device-gated: route the authored AUX SENDS.  Until this existed,
	 * jce_audio_mixer_resolve_send had no caller anywhere outside its own
	 * header and implementation: the editor authored sends, the config parser
	 * read them back, the mixer modelled them -- and no audio moved, because
	 * the device side had no parallel tap to send into.  Runs after the bus
	 * mirror above so both endpoints exist. */
	for (uint32_t i = 0; i < n; ++i) {
		const char *src_nm = jce_audio_mixer_get_name(rt->mixer, ids[i]);
		if (!src_nm || !src_nm[0]) continue;
		uint32_t sends = jce_audio_mixer_send_count(rt->mixer, ids[i]);
		for (uint32_t k = 0; k < sends; ++k) {
			JceAudioBusId dest = 0;
			if (!jce_audio_mixer_send_at(rt->mixer, ids[i], k, &dest, NULL))
				continue;
			const char *dst_nm = jce_audio_mixer_get_name(rt->mixer, dest);
			if (!dst_nm || !dst_nm[0]) continue;
			/* resolve_send, not the raw amount: it folds in the source bus's
			 * own volume and returns 0 for a muted or solo-suppressed bus, so
			 * a send obeys the same mute/solo the direct path does. */
			float g = jce_audio_mixer_resolve_send(rt->mixer, ids[i], dest);
			jce_audio_bus_set_send(rt->audio, src_nm, dst_nm, g);
		}
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

	/* FEATURE 5.2: advance every authored sidechain from the LIVE key level,
	 * before the push below reads the duck gain back out.
	 *
	 * This is the head of the ducking chain and it was the missing half: the
	 * tail (resolve_volume_ducked -> bus_set_volume) has always been here, so
	 * the duck gain was applied every frame and was permanently 1.0 because
	 * nothing advanced the follower.
	 *
	 * The peak it reads covers the audio blocks rendered since the last frame,
	 * which carried the PREVIOUS frame's duck gain -- one frame of latency,
	 * inherent to advancing a follower on the main thread and well inside the
	 * shortest usable attack time. */
	jce_audio_duck_pump(rt->audio, rt->mixer, dt);

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
 *   room + reverb (mB)        -> wet_mix via the standard mB->linear curve
 *                                (EAX composes the master and late levels)
 *   decay_time (s)            -> decay_seconds (and a room_size hint)
 *   decay_hf_ratio + room_hf  -> damping (less HF persistence = more damping)
 *   diffusion/density (%)     -> diffusion/density (0..1)
 *   reflections/reverb delay  -> pre_delay_ms
 *   hf_reference (Hz)         -> lowpass_hz
 * Everything is clamped so a half-authored zone stays well-formed.
 *
 *   reflections (mB)          -> early_mix, composed with `room` exactly the
 *                                way `reverb` is for the late level
 *   reflections_delay (s)     -> early_delay_ms
 *
 * `reflections` USED TO BE THE ONE FIELD NOT MAPPED, and the reason was
 * true at the time: Freeverb is comb + allpass with no separate
 * early-reflection stage, so there was nothing for that level to set.  There
 * is one now -- one clean tap before the diffuse tail -- so the field arrives
 * and the inspector's unread badge came off with it.  A badge that outlives
 * its defect is the second lie. */
/* Is this voice heard in 3D?
 *
 * TWO SITES have to agree: the per-frame push when the listener's gate
 * changes, and the voice-start path.  They disagreed in the obvious first
 * draft -- start honoured the gate, the push restored everything to 3D --
 * which would have started panning a deliberately flat UI sound the first
 * time someone toggled the listener.  One function so they cannot.
 *
 * The gate does not replace per-source intent: VoiceEntry.spatial still
 * records what the AudioSource authored, and reopening restores it. */
bool rt_voice_should_be_3d(bool listener_flat, bool voice_spatial)
{
	return !listener_flat && voice_spatial;
}

static float rt_clamp01(float v)
{
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

JceReverbPreset rt_reverb_preset_from_user(
    const JceAudioReverbZoneComponent *rz)
{
	JceReverbPreset p;
	memset(&p, 0, sizeof p);

	/* millibels -> linear gain (0 mB = 1.0, -10000 mB ~= 0).  10^(mB/2000).
	 *
	 * ROOM **PLUS REVERB**.  In EAX -- and in Unity's AudioReverbZone, which
	 * these fields are copied from -- Room is the master room-effect level
	 * and Reverb is the LATE level's offset from it; in millibels they add,
	 * i.e. the linear gains multiply.  `reverb` was simply not read here, so
	 * the one slider a sound designer reaches for to make a room's tail
	 * louder moved a number in the scene file and nothing else.  At the
	 * parser's defaults (-1000 and +200 mB) this takes the wet mix from
	 * 0.316 to 0.398: the authored +200 arriving, not a regression. */
	float wet = powf(10.0f, (rz->room + rz->reverb) / 2000.0f);
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

	/* LATE pre-delay: EAX's reverbDelay is measured from the EARLY
	 * reflection, so the tail starts at reflections_delay + reverb_delay.
	 * That sum was already right; what was missing is that the first term is
	 * also a time in its own right -- when the EARLY reflection arrives. */
	float pre = (rz->reflections_delay + rz->reverb_delay) * 1000.0f;
	if (pre < 0.0f)   pre = 0.0f;
	if (pre > 300.0f) pre = 300.0f;
	p.pre_delay_ms  = pre;

	/* EARLY reflection, composed with `room` the same way the late level is
	 * composed with `reverb`: in EAX both are offsets from the master room
	 * level, so in millibels they add and the linear gains multiply. */
	float early = powf(10.0f, (rz->room + rz->reflections) / 2000.0f);
	p.early_mix = rt_clamp01(early);
	float ed = rz->reflections_delay * 1000.0f;
	if (ed < 0.0f)   ed = 0.0f;
	if (ed > 300.0f) ed = 300.0f;
	p.early_delay_ms = ed;

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
/* Velocity from two positions, or zero when there is no honest answer.
 *
 * TWO CASES RETURN ZERO ON PURPOSE, and both of them are the difference
 * between Doppler working and Doppler being a bug report:
 *
 *   no previous position -- the first frame of a sound.  Differentiating
 *       against an implicit origin reads a source 100 m out at 60 fps as
 *       6000 m/s.
 *   faster than sound -- a teleport: a scene load, a camera cut, a
 *       respawn.  Nothing in a game moves at 340 m/s, and the Doppler model
 *       itself divides by (c - v), so past c it does not merely exaggerate,
 *       it inverts.  A cut is not motion and must not be heard as any.
 */
/* Gain from an authored volume-over-distance curve, or 1.0 when there is no
 * curve to read.
 *
 * WHY A SEPARATE FACTOR rather than a fifth attenuation model: the analytic
 * models live inside miniaudio's spatializer, which knows the listener and
 * the source and applies its own distance law.  A curve cannot be expressed
 * to it, so the curve has to be applied from out here -- and that only works
 * because rt_spawn_audio_source sets the engine model to NONE for exactly
 * these voices.  If both ran, a source would be attenuated twice.
 *
 * CHANNEL CHOICE is "volume" when the document has one, else channel 0.  A
 * single-channel curve drawn in the editor has no name worth insisting on;
 * a multi-channel one authored alongside other parameters does.
 *
 * OUT OF RANGE is clamped by jce_curve_eval itself (it holds the end keys),
 * which is the right shape here: past max distance the author's last key is
 * what they drew, and inventing a rolloff beyond it would be this function
 * deciding something the author already answered.
 *
 * A CURVE THAT WILL NOT LOAD returns 1.0 and warns ONCE.  It does not return
 * 0: silence is a legitimate thing to author, so a missing document must not
 * be indistinguishable from a curve drawn at zero. */
static float rt_audio_rolloff_gain(JceRuntime *rt, VoiceEntry *ve,
                                   jce_vec3 listener, jce_vec3 source)
{
	if (!ve->rolloff_curve[0]) return 1.0f;

	bool      owned = false;
	JceCurve *c     = rt_curve_fetch(rt, ve->rolloff_curve, &owned);
	if (!c) {
		if (!ve->rolloff_warned) {
			LOG_WARN("jce_runtime",
			         "audio rolloff curve '%s' will not load; falling back to "
			         "the analytic attenuation model for this voice",
			         ve->rolloff_curve);
			ve->rolloff_warned = true;
		}
		return 1.0f;
	}

	int ch = jce_curve_channel_index(c, "volume");
	if (ch < 0) ch = 0;

	const float dx = source.x - listener.x;
	const float dy = source.y - listener.y;
	const float dz = source.z - listener.z;
	const float d  = sqrtf(dx * dx + dy * dy + dz * dz);

	float g = jce_curve_eval(c, ch, d);
	if (owned) jce_curve_destroy(c);

	if (!(g >= 0.0f)) g = 0.0f;   /* also catches NaN */
	if (g > 4.0f)     g = 4.0f;   /* a curve may boost, but not unboundedly */
	return g;
}

jce_vec3 rt_audio_velocity(jce_vec3 now, jce_vec3 then, bool have_then,
                           float dt)
{
    jce_vec3 v = { 0.0f, 0.0f, 0.0f };
    if (!have_then || dt <= 0.0f) return v;
    const float inv = 1.0f / dt;
    v.x = (now.x - then.x) * inv;
    v.y = (now.y - then.y) * inv;
    v.z = (now.z - then.z) * inv;
    const float sq = v.x * v.x + v.y * v.y + v.z * v.z;
    if (sq > (343.0f * 343.0f)) { v.x = v.y = v.z = 0.0f; }
    return v;
}

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
	{
		jce_vec3 lp; lp.x = L.position[0]; lp.y = L.position[1]; lp.z = L.position[2];
		const jce_vec3 lv = rt_audio_velocity(lp, rt->listener_last_pos,
		                                      rt->listener_has_last_pos, dt);
		L.velocity[0] = lv.x;
		L.velocity[1] = lv.y;
		L.velocity[2] = lv.z;
		rt->listener_last_pos     = lp;
		rt->listener_has_last_pos = true;
	}
	jce_audio_set_listener(rt->audio, &L);

	/* AudioListener component last-mile: when the scene authored an
	 * AudioListener, apply its global fields (the runtime previously consumed
	 * only the listener POSITION above).  Gated on the component existing, so
	 * a scene without one keeps the prior behaviour byte-for-byte.
	 *   volume  -> master gain (paused forces it to 0)
	 *   paused  -> mute the whole mix (master 0) without stopping voices
	 *   doppler -> global Doppler scale
	 *   spatialize -> gate over every voice's own `spatial` flag
	 *
	 * That last one carried a note saying it "has no public engine API yet";
	 * jce_audio_voice_set_3d is public and the voice start path below has
	 * been calling it all along, so the switch had somewhere to go for as
	 * long as the note sat there. */
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
			rt->listener_flat = !als.lc->spatialize;
		} else {
			/* No listener component: 3D as before. */
			rt->listener_flat = false;
		}

		/* Push on CHANGE, restoring each voice to its OWN authored flag when
		 * the gate reopens -- not to 3D, which would make a deliberately flat
		 * UI sound start panning the first time someone toggled this. */
		if (rt->listener_flat != rt->listener_flat_applied) {
			for (int vi = 0; vi < rt->voice_count; ++vi)
				jce_audio_voice_set_3d(rt->audio, rt->voices[vi].voice,
				                       rt_voice_should_be_3d(
				                           rt->listener_flat,
				                           rt->voices[vi].spatial));
			rt->listener_flat_applied = rt->listener_flat;
		}
	}

	/* Position update + gather spatial sources for the occlusion solve. */
	enum { RT_MAX_OCC = 64 };
	JceAudioOcclusionQuery occ_q[RT_MAX_OCC];
	uint64_t               occ_ids[RT_MAX_OCC];
	int                    occ_vidx[RT_MAX_OCC];
	uint32_t               occ_n = 0;

	const jce_vec3 lpos = jce_v3(L.position[0], L.position[1], L.position[2]);

	for (int i = 0; i < rt->voice_count; ++i) {
		if (!rt->voices[i].spatial) continue;
		jce_vec3 wp = rt_world_position(rt->scene, rt->voices[i].entity);
		jce_audio_voice_set_position(rt->audio, rt->voices[i].voice,
		                              wp.x, wp.y, wp.z);
		/* CUSTOM ROLLOFF.  Computed for every spatial voice, not only the
		 * ones the occlusion pass gathers: occlusion is capped at RT_MAX_OCC
		 * and skipped entirely without a physics world, and a curve that
		 * stopped applying past the 65th voice -- or in a scene with no
		 * physics -- would be the kind of gap that only shows up in the one
		 * scene nobody profiles. */
		rt->voices[i].rolloff_gain =
		    rt_audio_rolloff_gain(rt, &rt->voices[i], lpos, wp);
		if (rt->voices[i].rolloff_curve[0])
			jce_audio_set_volume(rt->audio, rt->voices[i].voice,
			                     rt->voices[i].base_volume
			                         * rt->voices[i].rolloff_gain);
		/* The other half of the Doppler pair.  jce_audio_voice_set_velocity
		 * has been public since the feature landed and had NO caller
		 * anywhere, so miniaudio's Doppler ratio was 1.0 no matter what the
		 * authored factor said -- the factor was read, pushed to every live
		 * sound every frame, and multiplied a velocity that was always zero. */
		{
			const jce_vec3 sv = rt_audio_velocity(wp, rt->voices[i].last_pos,
			                                      rt->voices[i].has_last_pos, dt);
			jce_audio_voice_set_velocity(rt->audio, rt->voices[i].voice,
			                              sv.x, sv.y, sv.z);
			rt->voices[i].last_pos     = wp;
			rt->voices[i].has_last_pos = true;
		}
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
	/* AUTHORED OCCLUSION PARAMETERS.
	 *
	 * The solve used jce_audio_occlusion_default_params() and nothing else,
	 * so all five JceAudioOcclusionComponent fields were inert: a designer
	 * could set a cutoff, an attenuation, a radius and a mask, and the audio
	 * behaved identically.  The Inspector drew all five with NO unwired badge,
	 * unlike the reverb zone beside it -- so it was silent about it too.
	 *
	 * The component is a PROBE: `radius` is "the sphere radius the probe
	 * affects", so the probe that covers the LISTENER decides the parameters
	 * for this tick.  A listener inside no probe gets the defaults, which is
	 * every scene authored so far. */
	JceAudioOcclusionParams occ_params = jce_audio_occlusion_default_params();
	rt->occ_layer_mask = 0u;
	/* Reset EVERY tick, before the gated block below.  Leaving it to the
	 * occlusion path would keep the previous tick's value -- and on the first
	 * tick the calloc'd 0, which would multiply the reverb wet mix to silence
	 * in any scene with no spatial sources at all. */
	rt->occ_reverb_scale = 1.0f;
	if (rt->scene && occ_n > 0) {
		RtOccProbePick pick = { rt, jce_v3(L.position[0], L.position[1],
		                                   L.position[2]), NULL, 0.0f };
		jce_scene_each_entity(rt->scene, rt_occ_probe_cb, &pick);
		if (pick.best) {
			const JceAudioOcclusionComponent *o = pick.best;
			if (o->lowpass_cutoff_hz > 0.0f)
				occ_params.min_lowpass_hz = o->lowpass_cutoff_hz;
			/* attenuation_db is authored NEGATIVE (a cut).  dB -> linear. */
			if (o->attenuation_db < 0.0f)
				occ_params.min_direct_volume =
				    powf(10.0f, o->attenuation_db / 20.0f);
			if (o->layer_mask > 0)
				rt->occ_layer_mask = (uint32_t)o->layer_mask;
			rt->occ_affects_reverb = o->affects_reverb;
		} else {
			rt->occ_affects_reverb = false;
		}
	}

	if (rt->physics && occ_n > 0) {
		if (!rt->occ_tracker) {
			rt->occ_tracker = jce_audio_occlusion_tracker_create(RT_MAX_OCC);
			if (rt->occ_tracker) {
				jce_audio_occlusion_tracker_set_params(rt->occ_tracker,
				                                       &occ_params);
			}
		}
		jce_vec3 lp = jce_v3(L.position[0], L.position[1], L.position[2]);
		if (rt->occ_tracker) {
			/* Every tick, not only on create: the covering probe changes as
			 * the listener moves, and a tracker created under one probe must
			 * not keep its parameters after walking into another. */
			jce_audio_occlusion_tracker_set_params(rt->occ_tracker,
			                                       &occ_params);
			jce_audio_occlusion_tracker_solve(rt->occ_tracker, lp, occ_ids,
			                                  occ_q, occ_n,
			                                  rt_occlusion_raycast, rt);
			jce_audio_occlusion_tracker_gc(rt->occ_tracker, occ_ids, occ_n);
		} else {
			/* Allocation failed: fall back to the stateless solve so audio
			 * still reacts to occlusion (just without temporal smoothing). */
			jce_audio_occlusion_solve(&occ_params, lp, occ_q, occ_n,
			                          rt_occlusion_raycast, rt);
		}
		float occ_max = 0.0f;
		for (uint32_t k = 0; k < occ_n; ++k) {
			VoiceEntry *ve = &rt->voices[occ_vidx[k]];
			/* Three factors, one authored level: the curve (1.0 when
			 * there is none) and the occlusion solve both scale
			 * base_volume and neither replaces it. */
			jce_audio_set_volume(rt->audio, ve->voice,
			                     ve->base_volume * ve->rolloff_gain
			                         * occ_q[k].attenuation);
			jce_audio_set_lowpass(rt->audio, ve->voice, occ_q[k].lowpass_hz);
			if (occ_q[k].occlusion > occ_max) occ_max = occ_q[k].occlusion;
		}
		/* AudioOcclusion.affects_reverb: "also dampens reverb send when
		 * occluded".  The reverb node is global, so the strongest occlusion
		 * this tick sets how much of the wet tail survives -- a listener
		 * sealed off from every source should not still hear the room. */
		rt->occ_reverb_scale = rt->occ_affects_reverb
		                     ? (1.0f - rt_clamp01(occ_max)) : 1.0f;
	}

	/* ── Reverb zones: sample the blended preset at the listener and drive
	 * the global reverb DSP send wet/dry/decay.  Only active when the scene
	 * authored at least one AudioReverbZone (rt->reverb_zones non-NULL). */
	if (rt->reverb_zones && jce_reverb_zones_count(rt->reverb_zones) > 0) {
		jce_vec3 lp = jce_v3(L.position[0], L.position[1], L.position[2]);
		JceReverbPreset blend;
		jce_reverb_zones_sample(rt->reverb_zones, lp, &blend);

		JceAudioReverbParams rp;
		rp.wet_mix       = blend.wet_mix * rt->occ_reverb_scale;
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

    /* An authored volume of 0 is a LEVEL, not a missing field.  The JSON
     * parser already substitutes 1.0 when the key is absent
     * (jce_scene_components_audio.c), so a `> 0 ? : 1.0` guard here could only
     * ever fire on a value someone deliberately wrote -- and it turned "start
     * this source silent and let a script fade it in", which is how every
     * ambience bed is authored, into "start it at FULL volume", audible for
     * however many frames pass before the script's first write.  Only negative
     * volumes are rejected now.  Pitch keeps its guard: 0 there is not a quiet
     * sound, it is a stopped one. */
    float vol   = as->volume >= 0.0f ? as->volume : 1.0f;
    float pitch = as->pitch  >  0.0f ? as->pitch  : 1.0f;
    /* Played AT the authored priority, not promoted afterwards: the pool
     * is consulted during ALLOCATION, so a source that plays at 0 and is
     * raised a moment later can be refused -- or steal something more
     * important -- before the setter ever runs.  The set below still
     * happens, because a recycled slot would otherwise keep whatever the
     * previous occupant was stamped with. */
    JceVoice v = jce_audio_play_priority(rt->audio, snd, as->loop, vol,
                                         pitch, as->priority);
    /* CONTINUOUS now.  This read `spatial_blend > 0.5f` and handed the bool
     * to jce_audio_voice_set_3d, so a float the inspector edits in 0.01 steps
     * had exactly two reachable states: 0.0 and 0.49 were bit-identical, as
     * were 0.51 and 1.0.  `spatial` stays as the "is this source spatial at
     * all" question the attenuation and bus choices below still ask -- those
     * are per-source decisions, not per-sample blending. */
    bool spatial = (as->spatial_blend > 0.0f);
    if (spatial) {
        /* ...unless the listener's gate is closed.  VoiceEntry.spatial below
         * still records `spatial`, so reopening the gate restores this voice
         * to 3D; only what miniaudio is told right now is gated. */
        jce_audio_voice_set_3d(rt->audio, v,
                               rt_voice_should_be_3d(rt->listener_flat,
                                                     spatial));
        jce_vec3 wp = rt_world_position(scene, e);
        jce_audio_voice_set_position(rt->audio, v, wp.x, wp.y, wp.z);
        /* 3D attenuation: authorable per-source (large-world audio); a 0 model
         * / 0 distances fall back to the legacy defaults so pre-existing scenes
         * are unchanged. */
        JceAudioAttenuation atten = JCE_AUDIO_ATTEN_INVERSE;
        if (as->attenuation_model > 0)
            atten = (JceAudioAttenuation)(as->attenuation_model - 1);
        /* A CUSTOM ROLLOFF CURVE REPLACES THE ANALYTIC LAW, it does not stack
         * with it.  miniaudio's spatializer applies its own distance
         * attenuation inside the mix, and the curve is applied from
         * rt_update_audio_3d out here -- leaving the model set would
         * attenuate the source twice, and the author would be looking at a
         * curve that describes half of what they hear. */
        if (as->rolloff_curve[0]) atten = JCE_AUDIO_ATTEN_NONE;
        float a_min  = as->min_distance   > 0.0f ? as->min_distance   : 1.0f;
        float a_max  = as->max_distance   > 0.0f ? as->max_distance   : 25.0f;
        float a_roll = as->rolloff_factor > 0.0f ? as->rolloff_factor : 1.0f;
        jce_audio_voice_set_attenuation(rt->audio, v, atten,
                                        a_min, a_max, a_roll);
    } else {
        jce_audio_voice_set_3d(rt->audio, v, false);
    }
    /* The authored blend itself, after the 3D block: set_3d above is a plain
     * on/off and this scales how far distance may attenuate. */
    jce_audio_voice_set_spatial_blend(rt->audio, v, as->spatial_blend);
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
    /* A RECYCLED SLOT MUST NOT INHERIT THE LAST SOUND'S POSITION.  Slots
     * are reused as voices end, and every other field here is assigned,
     * so a missing assignment is silent: the new sound would
     * differentiate against wherever the previous occupant stood.  Three
     * metres apart at 60 fps is 180 m/s -- UNDER the teleport clamp, so
     * that guard would not catch it; it would simply be a wrong pitch on
     * the frame a sound starts, in a scene where sounds start often. */
    rt->voices[rt->voice_count].has_last_pos = false;
    rt->voices[rt->voice_count].last_pos.x   = 0.0f;
    rt->voices[rt->voice_count].last_pos.y   = 0.0f;
    rt->voices[rt->voice_count].last_pos.z   = 0.0f;
    /* CUSTOM ROLLOFF, and all three fields assigned for the reason the
     * comment above gives: a recycled slot must inherit nothing.  A voice
     * that kept the previous occupant's curve path would be attenuated by a
     * curve drawn for a different sound. */
    snprintf(rt->voices[rt->voice_count].rolloff_curve,
             sizeof rt->voices[rt->voice_count].rolloff_curve,
             "%s", as->rolloff_curve);
    rt->voices[rt->voice_count].rolloff_warned = false;
    rt->voices[rt->voice_count].rolloff_gain   = 1.0f;
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
static JceAsyncRunResult rt_audio_decode_run(JceAsyncContext *ctx, void *arg)
{
    RtAudioDecodeArgs *a = (RtAudioDecodeArgs *)arg;
    if (jce_async_context_cancel_requested(ctx))
        return JCE_ASYNC_RUN_CANCELLED;

    a->cpu = jce_audio_decode_cpu(a->pak, a->path);
    if (!a->cpu) {
        jce_async_context_fail(ctx, -1, "audio CPU decode failed");
        return JCE_ASYNC_RUN_FAILED;
    }
    if (jce_async_context_cancel_requested(ctx)) {
        jce_audio_cpu_free(a->cpu);
        a->cpu = NULL;
        return JCE_ASYNC_RUN_CANCELLED;
    }
    return JCE_ASYNC_RUN_SUCCESS;
}

RT_GROW_FN(rt_grow_pending_audio, pending_audio, pending_audio_cap, 8)

/* Concurrency cap for play_on_awake decodes. Unstarted slots have task ==
 * NULL and are promoted in scene order as structured tasks retire. */
enum { RT_AUDIO_DECODE_MAX = 4 };

/* Pending slots that currently own a submitted task. */
static int rt_audio_inflight(const JceRuntime *rt)
{
    int n = 0;
    for (int i = 0; i < rt->pending_audio_count; ++i)
        if (rt->pending_audio[i].task) ++n;
    return n;
}

static bool rt_audio_submit(RtPendingAudio *pending)
{
    if (!pending || !pending->args || pending->task) return false;

    JceAsyncTaskDesc desc;
    jce_async_task_desc_init(&desc);
    desc.work       = rt_audio_decode_run;
    desc.user_data  = pending->args;
    desc.debug_name = "runtime.audio.decode";
    desc.priority   = JCE_ASYNC_PRIORITY_HIGH;
    pending->task = jce_async_submit(jce_async_default_executor(), &desc);
    return pending->task != NULL;
}

/* Kick an async decode of `as->clip_path` for entity `e` (default loader
 * path only). The task uses `args` (stable heap) for its full run; the
 * pending slot only references it, so the slot array may realloc freely. */
void rt_spawn_audio_async(JceRuntime *rt, JceEntity e,
                                 const JceAudioSourceComponent *as)
{
    if (rt->pending_audio_count >= rt->pending_audio_cap &&
        !rt_grow_pending_audio(rt)) {
        jce_log_write(JCE_LOG_LEVEL_WARN, LOG_TAG, __FILE__, __LINE__,
                      "audio decode queue allocation failed for '%s'",
                      as->clip_path);
        return;
    }

    RtAudioDecodeArgs *args = (RtAudioDecodeArgs *)jce_malloc(sizeof(*args));
    if (!args) return;
    args->pak = rt->pak;
    snprintf(args->path, sizeof(args->path), "%s", as->clip_path);
    args->cpu  = NULL;

    RtPendingAudio *p = &rt->pending_audio[rt->pending_audio_count++];
    p->entity = e;
    p->task   = NULL;
    p->args   = args;

    /* Over the cap: leave the slot unstarted for rt_audio_poll to promote. */
    if (rt_audio_inflight(rt) >= RT_AUDIO_DECODE_MAX)
        return;
    (void)rt_audio_submit(p);
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
        if (!p->args || !p->task ||
            !jce_async_task_is_terminal(p->task)) {
            rt->pending_audio[w++] = *p;   /* keep (still running) */
            continue;
        }

        /* Re-fetch the component at play time (the entity may have moved /
         * been disabled in the 1-2 frames since spawn). */
        JceAudioSourceComponent *as =
            jce_scene_get_audio_source(rt->scene, p->entity);
        if (as &&
            jce_scene_component_enabled(rt->scene, p->entity,
                                        JCE_COMP_FLAG_AUDIO_SOURCE) &&
            jce_async_task_state(p->task) == JCE_ASYNC_STATE_SUCCEEDED &&
            p->args->cpu) {
            JceSound snd = jce_audio_upload_cpu(rt->audio, p->args->cpu);
            p->args->cpu = NULL;   /* consumed by upload */
            rt_finish_audio_source(rt, rt->scene, p->entity, snd, as);
        } else {
            jce_audio_cpu_free(p->args->cpu);   /* source gone — drop it */
            p->args->cpu = NULL;
        }
        jce_async_task_release(p->task);
        p->task = NULL;
        jce_free(p->args);
        /* slot dropped (not copied to w) */
    }
    rt->pending_audio_count = w;

    /* Promote slots the cap held back, now that retiring decodes freed
     * workers.  Order is spawn order, so a scene's clips still start in the
     * order the scene walk visited them. */
    int live = rt_audio_inflight(rt);
    for (int i = 0; i < rt->pending_audio_count &&
                    live < RT_AUDIO_DECODE_MAX; ++i) {
        RtPendingAudio *p = &rt->pending_audio[i];
        if (p->task || !p->args) continue;      /* running, or nothing to run */
        if (rt_audio_submit(p)) ++live;
    }
}

/* ── AudioSource control (jce_runtime_audio_play / _stop / _is_playing) ──
 *
 * See the header.  An authored AudioSource sounded once, at spawn, and only
 * with play_on_awake; these are the seam that lets gameplay drive it.
 *
 * rt_audio_source_start is the spawn walk's own body, minus the
 * play_on_awake test -- factored rather than copied, because a second copy of
 * "how to start an authored source" is how the attenuation block, the bus
 * choice and the 0-volume rule would drift apart between the two paths. */
bool rt_audio_source_start(JceRuntime *rt, JceScene *scene, JceEntity e)
{
    if (!rt || !rt->audio || !scene) return false;
    if (!rt->pak && !rt->audio_load_fn) return false;

    JceAudioSourceComponent *as = jce_scene_get_audio_source(scene, e);
    if (!as || !as->clip_path[0]) return false;
    if (!jce_scene_component_enabled(scene, e, JCE_COMP_FLAG_AUDIO_SOURCE))
        return false;

    /* STREAMING is a synchronous load by construction: there is nothing to
     * decode off-thread, only the encoded bytes to fetch, so the async decode
     * path below would be pure overhead.  This is the authored load_type
     * reaching the engine -- without it the field would round-trip through
     * JSON and the inspector and change nothing, which is the shape half this
     * ledger is made of. */
    if (as->load_type == 1) {
        JceSound snd = jce_audio_load_streaming(rt->audio, rt->pak,
                                                as->clip_path);
        if (snd == JCE_SOUND_INVALID) {
            jce_log_write(JCE_LOG_LEVEL_WARN, LOG_TAG, __FILE__, __LINE__,
                          "audio_source: streaming load failed for '%s'",
                          as->clip_path);
            return false;
        }
        rt_finish_audio_source(rt, scene, e, snd, as);
        return true;
    }

    if (rt->audio_load_fn) {
        /* Editor hook: synchronous handoff, same as the spawn walk. */
        JceSound snd = rt->audio_load_fn(rt->user_data, rt->audio,
                                         as->clip_path);
        if (snd == JCE_SOUND_INVALID) {
            jce_log_write(JCE_LOG_LEVEL_WARN, LOG_TAG, __FILE__, __LINE__,
                          "audio_source: failed to load '%s'", as->clip_path);
            return false;
        }
        rt_finish_audio_source(rt, scene, e, snd, as);
        return true;
    }
    rt_spawn_audio_async(rt, e, as);
    return true;
}

/* Stop and forget every voice this entity's AudioSource started.
 *
 * The sound is unloaded with the voice: rt->voices owns the decoded clip (the
 * spawn path uploads one per start), so leaving it would leak a buffer per
 * retrigger -- and a retriggerable source is exactly what this API is for. */
static int rt_audio_stop_entity(JceRuntime *rt, JceEntity e)
{
    if (!rt || !rt->audio) return 0;
    int w = 0, stopped = 0;
    for (int i = 0; i < rt->voice_count; ++i) {
        if (rt->voices[i].entity == e) {
            jce_audio_stop(rt->audio, rt->voices[i].voice);
            jce_audio_unload(rt->audio, rt->voices[i].sound);
            stopped++;
            continue;                       /* dropped (not copied to w) */
        }
        if (w != i) rt->voices[w] = rt->voices[i];
        w++;
    }
    rt->voice_count = w;
    return stopped;
}

JCE_API bool JCE_CALL jce_runtime_audio_play(JceRuntime *rt, uint64_t entity)
{
    if (!rt || !rt->scene) return false;
    /* RESTART, not overlay: Unity's Play() on a sounding source restarts it,
     * and without this a script that fires on a repeating event stacks a new
     * voice per call until the mixer runs out. */
    (void)rt_audio_stop_entity(rt, (JceEntity)entity);
    return rt_audio_source_start(rt, rt->scene, (JceEntity)entity);
}

JCE_API bool JCE_CALL jce_runtime_audio_stop(JceRuntime *rt, uint64_t entity)
{
    return rt_audio_stop_entity(rt, (JceEntity)entity) > 0;
}

JCE_API bool JCE_CALL jce_runtime_audio_is_playing(const JceRuntime *rt,
                                                   uint64_t entity)
{
    if (!rt || !rt->audio) return false;
    for (int i = 0; i < rt->voice_count; ++i)
        if (rt->voices[i].entity == (JceEntity)entity &&
            jce_audio_is_playing(rt->audio, rt->voices[i].voice))
            return true;
    return false;
}
