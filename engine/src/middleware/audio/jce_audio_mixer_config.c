/*
 * jce_audio_mixer_config.c -- consume the editor's audio_mixer.json routing.
 *
 * Parses the exact schema jce_panel_audio_mixer.cpp's mixer_save() writes and
 * applies it onto a JceAudioMixer (device-free) + enumerates per-bus insert
 * effects to a callback (the caller attaches them to a live device by name).
 *
 * The JSON scanners here are a C99 port of the editor loader's bounded scanners
 * (scan_uint/scan_float/scan_str + match_brace/match_brace_arr_end), so the read
 * side mirrors the write side field-for-field — including the JSON-id -> live-id
 * remap (jce_audio_mixer_add_bus reassigns dense ids, so deferred send/sidechain/
 * snapshot refs that name JSON ids resolve through a remap table in a 2nd pass).
 */

#include <jce/middleware/audio/jce_audio_mixer_config.h>

#include <stdio.h>
#include <string.h>

/* Upper bound on buses we track for the id-remap (matches the mixer's own
 * MAX_BUSES). 256-deep id_map is keyed by JSON id so we can resolve refs. */
#define CFG_MAX_BUSES 128

/* ── Bounded JSON scanners (C99 port of the editor loader) ───────────────── */

/* Scan a "key":number within a bounded region [beg,end). Tolerates both
 * "key" : v and "key":v spacing.  Returns true if found. */
static bool cfg_scan_uint(const char *beg, const char *end, const char *key,
                          unsigned *out)
{
	const char *k = strstr(beg, key);
	if (!k || k >= end) return false;
	const char *c = strchr(k, ':');
	if (!c || c >= end) return false;
	return sscanf(c + 1, " %u", out) == 1;
}

static bool cfg_scan_float(const char *beg, const char *end, const char *key,
                           float *out)
{
	const char *k = strstr(beg, key);
	if (!k || k >= end) return false;
	const char *c = strchr(k, ':');
	if (!c || c >= end) return false;
	return sscanf(c + 1, " %f", out) == 1;
}

static bool cfg_scan_str(const char *beg, const char *end, const char *key,
                         char *out, size_t cap)
{
	const char *k = strstr(beg, key);
	if (!k || k >= end) return false;
	const char *c = strchr(k, ':');
	if (!c || c >= end) return false;
	const char *q1 = strchr(c, '"');
	const char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
	if (!q1 || !q2 || q1 >= end || q2 >= end) return false;
	size_t nl = (size_t)(q2 - q1 - 1);
	if (nl >= cap) nl = cap - 1;
	memcpy(out, q1 + 1, nl);
	out[nl] = 0;
	return true;
}

/* Find the matching closing brace for the '{' at `open` (handles nesting and
 * skips braces inside strings).  Returns pointer to the '}' or `end`. */
static const char *cfg_match_brace(const char *open, const char *end)
{
	int depth = 0;
	bool in_str = false;
	for (const char *p = open; p < end; ++p) {
		if (in_str) { if (*p == '"') in_str = false; continue; }
		if (*p == '"') in_str = true;
		else if (*p == '{') ++depth;
		else if (*p == '}') { if (--depth == 0) return p; }
	}
	return end;
}

/* Find the matching closing ']' for the '[' at `open` (handles nesting, skips
 * brackets inside strings).  Returns pointer to ']' or NULL. */
static const char *cfg_match_arr_end(const char *open, const char *end)
{
	int depth = 0;
	bool in_str = false;
	for (const char *p = open; p < end; ++p) {
		if (in_str) { if (*p == '"') in_str = false; continue; }
		if (*p == '"') in_str = true;
		else if (*p == '[') ++depth;
		else if (*p == ']') { if (--depth == 0) return p; }
	}
	return NULL;
}

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

/* ── Effect-desc parsing (shared by apply skip + each_effect) ────────────── */

/* Parse one effect object span [o,c) into `*d`; returns true if a known type
 * was parsed (d->type != NONE). */
static bool cfg_parse_effect(const char *o, const char *c, JceAudioEffectDesc *d)
{
	char type[16] = {0};
	if (!cfg_scan_str(o, c, "\"type\"", type, sizeof(type))) return false;
	if (strcmp(type, "eq") == 0) {
		*d = jce_audio_effect_default(JCE_AUDIO_EFFECT_EQ);
		unsigned shape = (unsigned)d->u.eq.shape;
		cfg_scan_uint (o, c, "\"shape\"",   &shape);
		d->u.eq.shape = (JceAudioEqShape)shape;
		cfg_scan_float(o, c, "\"freq\"",    &d->u.eq.frequency_hz);
		cfg_scan_float(o, c, "\"gain_db\"", &d->u.eq.gain_db);
		cfg_scan_float(o, c, "\"q\"",       &d->u.eq.q);
	} else if (strcmp(type, "comp") == 0) {
		*d = jce_audio_effect_default(JCE_AUDIO_EFFECT_COMPRESSOR);
		cfg_scan_float(o, c, "\"threshold_db\"", &d->u.comp.threshold_db);
		cfg_scan_float(o, c, "\"ratio\"",        &d->u.comp.ratio);
		cfg_scan_float(o, c, "\"attack_ms\"",    &d->u.comp.attack_ms);
		cfg_scan_float(o, c, "\"release_ms\"",   &d->u.comp.release_ms);
		cfg_scan_float(o, c, "\"makeup_db\"",    &d->u.comp.makeup_db);
		cfg_scan_float(o, c, "\"knee_db\"",      &d->u.comp.knee_db);
	} else if (strcmp(type, "limiter") == 0) {
		*d = jce_audio_effect_default(JCE_AUDIO_EFFECT_LIMITER);
		cfg_scan_float(o, c, "\"ceiling_db\"",  &d->u.limiter.ceiling_db);
		cfg_scan_float(o, c, "\"release_ms\"",  &d->u.limiter.release_ms);
	} else if (strcmp(type, "delay") == 0) {
		*d = jce_audio_effect_default(JCE_AUDIO_EFFECT_DELAY);
		cfg_scan_float(o, c, "\"delay_ms\"",  &d->u.delay.delay_ms);
		cfg_scan_float(o, c, "\"feedback\"",  &d->u.delay.feedback);
		cfg_scan_float(o, c, "\"wet\"",       &d->u.delay.wet);
		cfg_scan_float(o, c, "\"dry\"",       &d->u.delay.dry);
	} else {
		return false;
	}
	return d->type != JCE_AUDIO_EFFECT_NONE;
}

/* Locate the "buses" array span [out_beg+1, out_end). Returns false if no
 * buses array is present. out_beg points at '[', out_end at the matching ']'. */
static bool cfg_find_buses(const char *json, const char *file_end,
                           const char **out_beg, const char **out_end)
{
	const char *buses_key = strstr(json, "\"buses\"");
	const char *buses_beg = buses_key ? strchr(buses_key, '[') : NULL;
	if (!buses_beg || buses_beg >= file_end) return false;
	/* Bound to the matching ']' so the snapshots array (which follows) is not
	 * scanned for bus rows. */
	int bdepth = 0; bool bstr = false; const char *buses_end = file_end;
	for (const char *q = buses_beg; q < file_end; ++q) {
		if (bstr) { if (*q == '"') bstr = false; continue; }
		if (*q == '"') bstr = true;
		else if (*q == '[') ++bdepth;
		else if (*q == ']') { if (--bdepth == 0) { buses_end = q; break; } }
	}
	*out_beg = buses_beg;
	*out_end = buses_end;
	return true;
}

/* ── Device-free apply: bus tree + sends + sidechain + snapshots ─────────── */

bool JCE_CALL jce_audio_mixer_apply_config(JceAudioMixer *m,
                                           const char *json, size_t len)
{
	if (!m || !json || len == 0) return false;
	const char *file_end = json + len;

	/* An optional top-level "version" key (written 1 by current editors) may
	 * precede "buses".  The scanners key on names and skip everything else,
	 * so both versioned and pre-version files parse identically; the key
	 * exists so a future breaking schema change has something to gate on. */
	const char *buses_beg = NULL, *buses_end = NULL;
	if (!cfg_find_buses(json, file_end, &buses_beg, &buses_end))
		return false;

	CfgIdMap map; map.n = 0;
	cfg_idmap_put(&map, JCE_AUDIO_BUS_MASTER, JCE_AUDIO_BUS_MASTER);

	/* Record each bus object's byte span for pass 2 (sends/sidechain need the
	 * full id_map resolved first). */
	struct { JceAudioBusId live; const char *beg; const char *end; }
		objs[CFG_MAX_BUSES];
	int obj_n = 0;
	bool any = false;

	/* Pass 1: create every bus, capture its object span. */
	const char *p = buses_beg + 1;
	while (p < buses_end) {
		const char *o = strchr(p, '{');
		if (!o || o >= buses_end) break;
		const char *c = cfg_match_brace(o, buses_end);
		unsigned id = 0, parent = 0, mutedv = 0, solov = 0;
		float    vol = 1.0f;
		char     name[32] = {0};
		cfg_scan_uint (o, c, "\"id\"",     &id);
		cfg_scan_uint (o, c, "\"parent\"", &parent);
		cfg_scan_str  (o, c, "\"name\"",   name, sizeof(name));
		cfg_scan_float(o, c, "\"volume\"", &vol);
		cfg_scan_uint (o, c, "\"muted\"",  &mutedv);
		cfg_scan_uint (o, c, "\"solo\"",   &solov);

		JceAudioBusId live = JCE_AUDIO_BUS_INVALID;
		if (id == JCE_AUDIO_BUS_MASTER) {
			jce_audio_mixer_set_volume(m, JCE_AUDIO_BUS_MASTER, vol);
			jce_audio_mixer_set_muted (m, JCE_AUDIO_BUS_MASTER, mutedv != 0);
			jce_audio_mixer_set_solo  (m, JCE_AUDIO_BUS_MASTER, solov  != 0);
			live = JCE_AUDIO_BUS_MASTER;
			any  = true;
		} else if (id != 0 && name[0]) {
			unsigned par = parent ? parent : JCE_AUDIO_BUS_MASTER;
			JceAudioBusId par_id = cfg_idmap_get(&map, par);
			if (par_id == JCE_AUDIO_BUS_INVALID) par_id = JCE_AUDIO_BUS_MASTER;
			live = jce_audio_mixer_add_bus(m, par_id, name, vol);
			if (live != JCE_AUDIO_BUS_INVALID) {
				jce_audio_mixer_set_muted(m, live, mutedv != 0);
				jce_audio_mixer_set_solo (m, live, solov  != 0);
				any = true;
			}
		}
		if (live != JCE_AUDIO_BUS_INVALID) {
			cfg_idmap_put(&map, id, live);
			if (obj_n < CFG_MAX_BUSES) {
				objs[obj_n].live = live;
				objs[obj_n].beg  = o;
				objs[obj_n].end  = c;
				++obj_n;
			}
		}
		p = c + 1;
	}

	/* Pass 2: sends + sidechain (full id_map resolved). */
	for (int bi = 0; bi < obj_n; ++bi) {
		const char *o = objs[bi].beg, *c = objs[bi].end;
		JceAudioBusId src = objs[bi].live;

		/* Sends: iterate { dest, amount } objects inside "sends":[...]. */
		const char *sk = strstr(o, "\"sends\"");
		if (sk && sk < c) {
			const char *sb = strchr(sk, '[');
			const char *se = sb ? cfg_match_arr_end(sb, c) : NULL;
			if (sb && se) {
				const char *q = sb + 1;
				while (q < se) {
					const char *so = strchr(q, '{');
					if (!so || so >= se) break;
					const char *sc = cfg_match_brace(so, se);
					unsigned dest = 0; float amt = 0.0f;
					cfg_scan_uint (so, sc, "\"dest\"",   &dest);
					cfg_scan_float(so, sc, "\"amount\"", &amt);
					JceAudioBusId dst = cfg_idmap_get(&map, dest);
					/* amount 0 is a valid authored state (registered but
					 * silent) — set_send registers it; reject only
					 * negative garbage. */
					if (dst != JCE_AUDIO_BUS_INVALID && amt >= 0.0f)
						jce_audio_mixer_set_send(m, src, dst, amt);
					q = sc + 1;
				}
			}
		}

		/* Sidechain object. */
		const char *xk = strstr(o, "\"sidechain\"");
		if (xk && xk < c) {
			const char *xo = strchr(xk, '{');
			const char *xc = xo ? cfg_match_brace(xo, c) : NULL;
			if (xo && xc) {
				unsigned key = 0;
				JceAudioDuckParams dp = jce_audio_duck_default_params();
				cfg_scan_uint (xo, xc, "\"key\"",          &key);
				cfg_scan_float(xo, xc, "\"threshold_db\"", &dp.threshold_db);
				cfg_scan_float(xo, xc, "\"ratio\"",        &dp.ratio);
				cfg_scan_float(xo, xc, "\"attack_ms\"",    &dp.attack_ms);
				cfg_scan_float(xo, xc, "\"release_ms\"",   &dp.release_ms);
				cfg_scan_float(xo, xc, "\"floor_db\"",     &dp.max_attenuation_db);
				JceAudioBusId kid = cfg_idmap_get(&map, key);
				if (kid != JCE_AUDIO_BUS_INVALID) {
					dp.key = kid;
					jce_audio_mixer_set_sidechain(m, src, &dp, 48000u);
				}
			}
		}
	}

	/* Snapshots array (top-level, after the buses array). */
	const char *snap_key = strstr(buses_end, "\"snapshots\"");
	const char *snap_beg = snap_key ? strchr(snap_key, '[') : NULL;
	if (snap_beg) {
		const char *snap_end = cfg_match_arr_end(snap_beg, file_end);
		const char *q = snap_beg + 1;
		while (snap_end && q < snap_end) {
			const char *so = strchr(q, '{');
			if (!so || so >= snap_end) break;
			const char *sc = cfg_match_brace(so, snap_end);
			char sname[JCE_AUDIO_SNAPSHOT_NAME] = {0};
			if (cfg_scan_str(so, sc, "\"name\"", sname, sizeof(sname)) &&
			    sname[0]) {
				/* volumes:[ { id, volume } ] */
				const char *vk = strstr(so, "\"volumes\"");
				const char *vb = vk ? strchr(vk, '[') : NULL;
				if (vb && vb < sc) {
					const char *ve = cfg_match_arr_end(vb, sc);
					const char *r = vb + 1;
					while (ve && r < ve) {
						const char *vo = strchr(r, '{');
						if (!vo || vo >= ve) break;
						const char *vc = cfg_match_brace(vo, ve);
						unsigned vid = 0; float vv = 0.0f;
						cfg_scan_uint (vo, vc, "\"id\"",     &vid);
						cfg_scan_float(vo, vc, "\"volume\"", &vv);
						JceAudioBusId live = cfg_idmap_get(&map, vid);
						if (live != JCE_AUDIO_BUS_INVALID)
							jce_audio_mixer_snapshot_set_volume(m, sname,
							                                    live, vv);
						r = vc + 1;
					}
				}
			}
			q = sc + 1;
		}
	}

	return any;
}

/* ── Per-bus insert-effect enumeration (device coupling left to caller) ──── */

uint32_t JCE_CALL jce_audio_mixer_config_each_effect(const char *json,
                                                     size_t len,
                                                     JceAudioMixerEffectFn cb,
                                                     void *user)
{
	if (!json || len == 0 || !cb) return 0;
	const char *file_end = json + len;

	const char *buses_beg = NULL, *buses_end = NULL;
	if (!cfg_find_buses(json, file_end, &buses_beg, &buses_end))
		return 0;

	uint32_t total = 0;
	const char *p = buses_beg + 1;
	while (p < buses_end) {
		const char *o = strchr(p, '{');
		if (!o || o >= buses_end) break;
		const char *c = cfg_match_brace(o, buses_end);

		char name[32] = {0};
		cfg_scan_str(o, c, "\"name\"", name, sizeof(name));

		const char *fk = strstr(o, "\"effects\"");
		if (name[0] && fk && fk < c) {
			const char *fb = strchr(fk, '[');
			const char *fe = (fb && fb < c) ? cfg_match_arr_end(fb, c) : NULL;
			if (fb && fe) {
				uint32_t idx = 0;
				const char *q = fb + 1;
				while (q < fe) {
					const char *eo = strchr(q, '{');
					if (!eo || eo >= fe) break;
					const char *ec = cfg_match_brace(eo, fe);
					JceAudioEffectDesc d;
					memset(&d, 0, sizeof d);
					if (cfg_parse_effect(eo, ec, &d)) {
						cb(name, &d, idx, user);
						++idx;
						++total;
					}
					q = ec + 1;
				}
			}
		}
		p = c + 1;
	}
	return total;
}
