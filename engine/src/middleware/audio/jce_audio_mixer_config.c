/*
 * jce_audio_mixer_config.c -- consume the editor's audio_mixer.json routing.
 *
 * Parses the exact schema jce_panel_audio_mixer.cpp's mixer_save() writes and
 * applies it onto a JceAudioMixer (device-free) + enumerates per-bus insert
 * effects to a callback (the caller attaches them to a live device by name).
 *
 * Parsing runs through the jce_json facade (one real parser, correct string
 * unescaping) instead of hand-rolled key scanners: the scanners keyed on raw
 * `"name":` byte patterns, so a bus named with a quote or a backslash derailed
 * them.  The read side still mirrors the write side field-for-field — including
 * the JSON-id -> live-id remap (jce_audio_mixer_add_bus reassigns dense ids, so
 * deferred send/sidechain/snapshot refs that name JSON ids resolve through a
 * remap table in a 2nd pass).
 */

#include <jce/middleware/audio/jce_audio_mixer_config.h>

#include <jce/os/core/jce_json.h>

#include <stdio.h>
#include <string.h>

/* Upper bound on buses we track for the id-remap (matches the mixer's own
 * MAX_BUSES). 256-deep id_map is keyed by JSON id so we can resolve refs. */
#define CFG_MAX_BUSES 128

/* Bus names are truncated exactly the way jce_audio_mixer_add_bus truncates
 * them (its own name field is 32 bytes), so the name handed to the effect
 * callback still matches the live bus it must attach to. */
#define CFG_NAME_CAP 32

/* ── id-remap table (JSON id -> live bus id) ─────────────────────────────── */

typedef struct {
	unsigned      jid[CFG_MAX_BUSES];
	JceAudioBusId lid[CFG_MAX_BUSES];
	int           n;
} CfgIdMap;

static void cfg_idmap_put(CfgIdMap *map, unsigned jid, JceAudioBusId lid)
{
	if (map->n >= CFG_MAX_BUSES) return;
	map->jid[map->n] = jid;
	map->lid[map->n] = lid;
	++map->n;
}

/* Resolve a JSON id to the live bus id (INVALID if unknown). Master maps to
 * itself even before it is recorded. */
static JceAudioBusId cfg_idmap_get(const CfgIdMap *map, unsigned jid)
{
	if (jid == JCE_AUDIO_BUS_MASTER) return JCE_AUDIO_BUS_MASTER;
	for (int i = 0; i < map->n; ++i)
		if (map->jid[i] == jid) return map->lid[i];
	return JCE_AUDIO_BUS_INVALID;
}

/* Copy a string member into a fixed local buffer, truncating like the mixer. */
static void cfg_copy_name(char *dst, size_t cap, const JceJson *obj,
                          const char *key)
{
	snprintf(dst, cap, "%s", jce_json_get_string(obj, key, ""));
}

/* ── Effect-desc parsing (shared by apply skip + each_effect) ────────────── */

/* Parse one effect object into `*d`; returns true if a known type was parsed
 * (d->type != NONE).  Missing fields keep the type's engine defaults. */
static bool cfg_parse_effect(const JceJson *e, JceAudioEffectDesc *d)
{
	const char *type = jce_json_get_string(e, "type", "");
	if (strcmp(type, "eq") == 0) {
		*d = jce_audio_effect_default(JCE_AUDIO_EFFECT_EQ);
		d->u.eq.shape = (JceAudioEqShape)
			jce_json_get_int(e, "shape", (int)d->u.eq.shape);
		d->u.eq.frequency_hz =
			(float)jce_json_get_number(e, "freq",    d->u.eq.frequency_hz);
		d->u.eq.gain_db =
			(float)jce_json_get_number(e, "gain_db", d->u.eq.gain_db);
		d->u.eq.q =
			(float)jce_json_get_number(e, "q",       d->u.eq.q);
	} else if (strcmp(type, "comp") == 0) {
		*d = jce_audio_effect_default(JCE_AUDIO_EFFECT_COMPRESSOR);
		d->u.comp.threshold_db =
			(float)jce_json_get_number(e, "threshold_db", d->u.comp.threshold_db);
		d->u.comp.ratio =
			(float)jce_json_get_number(e, "ratio",        d->u.comp.ratio);
		d->u.comp.attack_ms =
			(float)jce_json_get_number(e, "attack_ms",    d->u.comp.attack_ms);
		d->u.comp.release_ms =
			(float)jce_json_get_number(e, "release_ms",   d->u.comp.release_ms);
		d->u.comp.makeup_db =
			(float)jce_json_get_number(e, "makeup_db",    d->u.comp.makeup_db);
		d->u.comp.knee_db =
			(float)jce_json_get_number(e, "knee_db",      d->u.comp.knee_db);
	} else if (strcmp(type, "limiter") == 0) {
		*d = jce_audio_effect_default(JCE_AUDIO_EFFECT_LIMITER);
		d->u.limiter.ceiling_db =
			(float)jce_json_get_number(e, "ceiling_db", d->u.limiter.ceiling_db);
		d->u.limiter.release_ms =
			(float)jce_json_get_number(e, "release_ms", d->u.limiter.release_ms);
	} else if (strcmp(type, "delay") == 0) {
		*d = jce_audio_effect_default(JCE_AUDIO_EFFECT_DELAY);
		d->u.delay.delay_ms =
			(float)jce_json_get_number(e, "delay_ms", d->u.delay.delay_ms);
		d->u.delay.feedback =
			(float)jce_json_get_number(e, "feedback", d->u.delay.feedback);
		d->u.delay.wet =
			(float)jce_json_get_number(e, "wet",      d->u.delay.wet);
		d->u.delay.dry =
			(float)jce_json_get_number(e, "dry",      d->u.delay.dry);
	} else {
		return false;
	}
	return d->type != JCE_AUDIO_EFFECT_NONE;
}

/* Parse `json` and hand back its "buses" array.  Returns NULL (and frees
 * nothing) when the text is unparseable or carries no buses array; on success
 * the caller owns *out_root and must jce_json_free it. */
static const JceJson *cfg_open(const char *json, size_t len, JceJson **out_root)
{
	JceJson *root = jce_json_parse(json, len);
	if (!root) return NULL;
	const JceJson *buses = jce_json_get(root, "buses");
	if (!jce_json_is_array(buses)) { jce_json_free(root); return NULL; }
	*out_root = root;
	return buses;
}

/* ── Device-free apply: bus tree + sends + sidechain + snapshots ─────────── */

bool JCE_CALL jce_audio_mixer_apply_config(JceAudioMixer *m,
                                           const char *json, size_t len)
{
	if (!m || !json || len == 0) return false;

	/* An optional top-level "version" key (written 1 by current editors) may
	 * accompany "buses".  Nothing gates on it yet; the key exists so a future
	 * breaking schema change has something to gate on, and pre-version files
	 * parse identically. */
	JceJson       *root  = NULL;
	const JceJson *buses = cfg_open(json, len, &root);
	if (!buses) return false;

	CfgIdMap map; map.n = 0;
	cfg_idmap_put(&map, JCE_AUDIO_BUS_MASTER, JCE_AUDIO_BUS_MASTER);

	/* Record each bus node against its live id for pass 2 (sends/sidechain
	 * need the full id_map resolved first). */
	struct { JceAudioBusId live; const JceJson *node; } objs[CFG_MAX_BUSES];
	int  obj_n = 0;
	bool any   = false;

	/* Pass 1: create every bus. */
	const int bus_n = jce_json_array_size(buses);
	for (int i = 0; i < bus_n; ++i) {
		const JceJson *b = jce_json_array_at(buses, i);
		if (!jce_json_is_object(b)) continue;
		unsigned id     = (unsigned)jce_json_get_int(b, "id",     0);
		unsigned parent = (unsigned)jce_json_get_int(b, "parent", 0);
		float    vol    = (float)jce_json_get_number(b, "volume", 1.0);
		/* muted/solo were written as 0/1 numbers; get_bool takes numbers and
		 * real booleans alike, so old and future files both read back. */
		bool     muted  = jce_json_get_bool(b, "muted", false);
		bool     solo   = jce_json_get_bool(b, "solo",  false);
		char     name[CFG_NAME_CAP];
		cfg_copy_name(name, sizeof(name), b, "name");

		JceAudioBusId live = JCE_AUDIO_BUS_INVALID;
		if (id == JCE_AUDIO_BUS_MASTER) {
			jce_audio_mixer_set_volume(m, JCE_AUDIO_BUS_MASTER, vol);
			jce_audio_mixer_set_muted (m, JCE_AUDIO_BUS_MASTER, muted);
			jce_audio_mixer_set_solo  (m, JCE_AUDIO_BUS_MASTER, solo);
			live = JCE_AUDIO_BUS_MASTER;
			any  = true;
		} else if (id != 0 && name[0]) {
			unsigned par = parent ? parent : JCE_AUDIO_BUS_MASTER;
			JceAudioBusId par_id = cfg_idmap_get(&map, par);
			if (par_id == JCE_AUDIO_BUS_INVALID) par_id = JCE_AUDIO_BUS_MASTER;
			live = jce_audio_mixer_add_bus(m, par_id, name, vol);
			if (live != JCE_AUDIO_BUS_INVALID) {
				jce_audio_mixer_set_muted(m, live, muted);
				jce_audio_mixer_set_solo (m, live, solo);
				any = true;
			}
		}
		if (live != JCE_AUDIO_BUS_INVALID) {
			cfg_idmap_put(&map, id, live);
			if (obj_n < CFG_MAX_BUSES) {
				objs[obj_n].live = live;
				objs[obj_n].node = b;
				++obj_n;
			}
		}
	}

	/* Pass 2: sends + sidechain (full id_map resolved). */
	for (int bi = 0; bi < obj_n; ++bi) {
		const JceJson *b   = objs[bi].node;
		JceAudioBusId  src = objs[bi].live;

		/* Sends: iterate { dest, amount } objects inside "sends":[...]. */
		const JceJson *sends  = jce_json_get(b, "sends");
		const int      send_n = jce_json_array_size(sends);
		for (int si = 0; si < send_n; ++si) {
			const JceJson *s = jce_json_array_at(sends, si);
			if (!jce_json_is_object(s)) continue;
			unsigned dest = (unsigned)jce_json_get_int(s, "dest",   0);
			float    amt  = (float)jce_json_get_number(s, "amount", 0.0);
			JceAudioBusId dst = cfg_idmap_get(&map, dest);
			/* amount 0 is a valid authored state (registered but silent) —
			 * set_send registers it; reject only negative garbage. */
			if (dst != JCE_AUDIO_BUS_INVALID && amt >= 0.0f)
				jce_audio_mixer_set_send(m, src, dst, amt);
		}

		/* Sidechain object. */
		const JceJson *sc = jce_json_get(b, "sidechain");
		if (jce_json_is_object(sc)) {
			JceAudioDuckParams dp = jce_audio_duck_default_params();
			unsigned key = (unsigned)jce_json_get_int(sc, "key", 0);
			dp.threshold_db =
				(float)jce_json_get_number(sc, "threshold_db", dp.threshold_db);
			dp.ratio =
				(float)jce_json_get_number(sc, "ratio",        dp.ratio);
			dp.attack_ms =
				(float)jce_json_get_number(sc, "attack_ms",    dp.attack_ms);
			dp.release_ms =
				(float)jce_json_get_number(sc, "release_ms",   dp.release_ms);
			dp.max_attenuation_db =
				(float)jce_json_get_number(sc, "floor_db", dp.max_attenuation_db);
			JceAudioBusId kid = cfg_idmap_get(&map, key);
			if (kid != JCE_AUDIO_BUS_INVALID) {
				dp.key = kid;
				jce_audio_mixer_set_sidechain(m, src, &dp, 48000u);
			}
		}
	}

	/* Snapshots array (top-level, beside the buses array). */
	const JceJson *snaps  = jce_json_get(root, "snapshots");
	const int      snap_n = jce_json_array_size(snaps);
	for (int si = 0; si < snap_n; ++si) {
		const JceJson *s = jce_json_array_at(snaps, si);
		if (!jce_json_is_object(s)) continue;
		char sname[JCE_AUDIO_SNAPSHOT_NAME];
		cfg_copy_name(sname, sizeof(sname), s, "name");
		if (!sname[0]) continue;

		/* volumes:[ { id, volume } ] */
		const JceJson *vols = jce_json_get(s, "volumes");
		const int      vol_n = jce_json_array_size(vols);
		for (int vi = 0; vi < vol_n; ++vi) {
			const JceJson *v = jce_json_array_at(vols, vi);
			if (!jce_json_is_object(v)) continue;
			unsigned vid = (unsigned)jce_json_get_int(v, "id",     0);
			float    vv  = (float)jce_json_get_number(v, "volume", 0.0);
			JceAudioBusId live = cfg_idmap_get(&map, vid);
			if (live != JCE_AUDIO_BUS_INVALID)
				jce_audio_mixer_snapshot_set_volume(m, sname, live, vv);
		}
	}

	jce_json_free(root);
	return any;
}

/* ── Per-bus insert-effect enumeration (device coupling left to caller) ──── */

uint32_t JCE_CALL jce_audio_mixer_config_each_effect(const char *json,
                                                     size_t len,
                                                     JceAudioMixerEffectFn cb,
                                                     void *user)
{
	if (!json || len == 0 || !cb) return 0;

	JceJson       *root  = NULL;
	const JceJson *buses = cfg_open(json, len, &root);
	if (!buses) return 0;

	uint32_t  total = 0;
	const int bus_n = jce_json_array_size(buses);
	for (int i = 0; i < bus_n; ++i) {
		const JceJson *b = jce_json_array_at(buses, i);
		if (!jce_json_is_object(b)) continue;
		char name[CFG_NAME_CAP];
		cfg_copy_name(name, sizeof(name), b, "name");
		if (!name[0]) continue;

		const JceJson *fx   = jce_json_get(b, "effects");
		const int      fx_n = jce_json_array_size(fx);
		uint32_t       idx  = 0;
		for (int fi = 0; fi < fx_n; ++fi) {
			const JceJson *e = jce_json_array_at(fx, fi);
			if (!jce_json_is_object(e)) continue;
			JceAudioEffectDesc d;
			memset(&d, 0, sizeof d);
			if (cfg_parse_effect(e, &d)) {
				cb(name, &d, idx, user);
				++idx;
				++total;
			}
		}
	}

	jce_json_free(root);
	return total;
}
