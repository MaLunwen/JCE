/*
 * jce_audio_mixer.c -- generic hierarchical mixer bus tree.
 */

#include <jce/middleware/audio/jce_audio_mixer.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>

#include "os/core/jce_memory.h"

#include <math.h>
#include <string.h>
#include <jce/os/core/jce_str.h>

#define LOG_TAG       "mixer"
#define MAX_BUSES     128
#define MAX_NAME      32

/* One aux send originating from a bus (FEATURE 5.2). */
typedef struct {
    JceAudioBusId dest;     /* 0 = empty slot */
    float         amount;   /* 0..1 fraction of post-fader signal */
} BusSend;

/* Per-bus sidechain ducking state (FEATURE 5.2).  Installed on the *target*
 * bus; `key` names the bus whose envelope drives the reduction. */
typedef struct {
    bool          active;
    JceAudioDuckParams p;
    uint32_t      sample_rate;   /* device rate the coefficients assume      */
    float         env;           /* tracked key envelope (linear)           */
    float         gain;          /* current duck multiplier (0..1)          */
} Sidechain;

typedef struct Bus {
    bool          alive;
    JceAudioBusId parent;
    char          name[MAX_NAME];
    float         volume;
    bool          muted;
    bool          solo;
    BusSend       sends[JCE_AUDIO_MAX_SENDS];
    Sidechain     duck;
} Bus;

/* A named mixer snapshot: a target volume per bus id (FEATURE 5.2). */
typedef struct {
    bool  used;
    char  name[JCE_AUDIO_SNAPSHOT_NAME];
    bool  has[MAX_BUSES];          /* whether this snapshot stores a value */
    float volume[MAX_BUSES];       /* target volume per bus id             */
} Snapshot;

/* Open-addressed hash for voice -> bus mapping. */
typedef struct VoiceMap {
    uint64_t      voice_id;   /* 0 = empty */
    JceAudioBusId bus;
} VoiceMap;

struct JceAudioMixer {
    Bus       buses[MAX_BUSES];      /* index 0 unused; bus IDs are 1-based */
    uint32_t  bus_count;
    uint32_t  solo_count;            /* number of solo'd buses (any) */
    VoiceMap *voices;
    uint32_t  voices_cap;            /* power of two */
    uint32_t  voices_size;

    /* FEATURE 5.2 — snapshots + active crossfade. */
    Snapshot  snapshots[JCE_AUDIO_MAX_SNAPSHOTS];
    uint32_t  snapshot_count;
    bool      fade_active;
    float     fade_duration;         /* total fade length (s); 0 => instant   */
    float     fade_elapsed;          /* seconds into the fade                  */
    int       fade_target;           /* index into snapshots[] being applied   */
    float     fade_from[MAX_BUSES];  /* volume per bus at fade start           */
    float     fade_to[MAX_BUSES];    /* target volume per bus                  */
    bool      fade_has[MAX_BUSES];   /* whether this bus participates          */
};

static uint32_t hash_u64(uint64_t x)
{
    x ^= x >> 33; x *= 0xFF51AFD7ED558CCDULL;
    x ^= x >> 33; x *= 0xC4CEB9FE1A85EC53ULL;
    x ^= x >> 33;
    return (uint32_t)x;
}

JceAudioMixer *jce_audio_mixer_create(void)
{
    JceAudioMixer *m = (JceAudioMixer *)JCE_CALLOC(1, sizeof(*m));
    if (!m) return NULL;
    m->voices_cap = 64;
    m->voices = (VoiceMap *)JCE_CALLOC(m->voices_cap, sizeof(VoiceMap));
    if (!m->voices) { JCE_FREE(m); return NULL; }

    /* Master at id=1, parent=INVALID. */
    m->buses[1].alive  = true;
    m->buses[1].parent = JCE_AUDIO_BUS_INVALID;
    jce_strlcpy(m->buses[1].name, "Master", MAX_NAME);
    m->buses[1].volume = 1.0f;
    m->bus_count = 1;
    return m;
}

void jce_audio_mixer_destroy(JceAudioMixer *m)
{
    if (!m) return;
    JCE_FREE(m->voices);
    JCE_FREE(m);
}

static bool bus_valid(const JceAudioMixer *m, JceAudioBusId b)
{
    return m && b > 0 && b < MAX_BUSES && m->buses[b].alive;
}

JceAudioBusId jce_audio_mixer_add_bus(JceAudioMixer *m, JceAudioBusId parent,
                                      const char *name, float volume)
{
    if (!m || !name) return JCE_AUDIO_BUS_INVALID;
    if (parent != JCE_AUDIO_BUS_INVALID && !bus_valid(m, parent))
        return JCE_AUDIO_BUS_INVALID;
    for (uint16_t i = 2; i < MAX_BUSES; ++i) {
        if (!m->buses[i].alive) {
            memset(&m->buses[i], 0, sizeof(m->buses[i]));
            m->buses[i].alive  = true;
            m->buses[i].parent = parent;
            jce_strlcpy(m->buses[i].name, name, MAX_NAME);
            m->buses[i].volume = volume;
            m->buses[i].muted  = false;
            m->buses[i].solo   = false;
            m->bus_count++;
            return (JceAudioBusId)i;
        }
    }
    LOG_ERROR(LOG_TAG, "bus pool exhausted (max %d)", MAX_BUSES);
    return JCE_AUDIO_BUS_INVALID;
}

bool jce_audio_mixer_remove_bus(JceAudioMixer *m, JceAudioBusId bus)
{
    if (!bus_valid(m, bus) || bus == JCE_AUDIO_BUS_MASTER) return false;
    /* Reparent children to grand-parent. */
    JceAudioBusId new_parent = m->buses[bus].parent;
    for (uint16_t i = 2; i < MAX_BUSES; ++i) {
        if (m->buses[i].alive && m->buses[i].parent == bus)
            m->buses[i].parent = new_parent;
    }
    if (m->buses[bus].solo && m->solo_count > 0) m->solo_count--;
    m->buses[bus].alive = false;
    m->bus_count--;
    /* Drop voice assignments to this bus. */
    for (uint32_t i = 0; i < m->voices_cap; ++i)
        if (m->voices[i].bus == bus) { m->voices[i].voice_id = 0; m->voices[i].bus = JCE_AUDIO_BUS_INVALID; m->voices_size--; }
    /* Drop dangling aux sends that point at the removed bus, and any
     * sidechain whose key was the removed bus (FEATURE 5.2). */
    for (uint16_t i = 1; i < MAX_BUSES; ++i) {
        if (!m->buses[i].alive) continue;
        for (int s = 0; s < JCE_AUDIO_MAX_SENDS; ++s)
            if (m->buses[i].sends[s].dest == bus) {
                m->buses[i].sends[s].dest   = JCE_AUDIO_BUS_INVALID;
                m->buses[i].sends[s].amount = 0.0f;
            }
        if (m->buses[i].duck.active && m->buses[i].duck.p.key == bus) {
            memset(&m->buses[i].duck, 0, sizeof(m->buses[i].duck));
            m->buses[i].duck.gain = 1.0f;
        }
    }
    return true;
}

JceAudioBusId jce_audio_mixer_find_bus(const JceAudioMixer *m, const char *name)
{
    if (!m || !name) return JCE_AUDIO_BUS_INVALID;
    for (uint16_t i = 1; i < MAX_BUSES; ++i)
        if (m->buses[i].alive && strncmp(m->buses[i].name, name, MAX_NAME) == 0)
            return (JceAudioBusId)i;
    return JCE_AUDIO_BUS_INVALID;
}

uint32_t jce_audio_mixer_bus_count(const JceAudioMixer *m) { return m ? m->bus_count : 0u; }

void jce_audio_mixer_set_volume(JceAudioMixer *m, JceAudioBusId b, float v)
{ if (bus_valid(m, b)) m->buses[b].volume = v < 0.0f ? 0.0f : v; }

float jce_audio_mixer_get_volume(const JceAudioMixer *m, JceAudioBusId b)
{ return bus_valid(m, b) ? m->buses[b].volume : 0.0f; }

void jce_audio_mixer_set_muted(JceAudioMixer *m, JceAudioBusId b, bool mu)
{ if (bus_valid(m, b)) m->buses[b].muted = mu; }

bool jce_audio_mixer_is_muted(const JceAudioMixer *m, JceAudioBusId b)
{ return bus_valid(m, b) ? m->buses[b].muted : false; }

void jce_audio_mixer_set_solo(JceAudioMixer *m, JceAudioBusId b, bool so)
{
    if (!bus_valid(m, b)) return;
    if (m->buses[b].solo == so) return;
    m->buses[b].solo = so;
    if (so) m->solo_count++;
    else if (m->solo_count > 0) m->solo_count--;
}

bool jce_audio_mixer_is_solo(const JceAudioMixer *m, JceAudioBusId b)
{ return bus_valid(m, b) ? m->buses[b].solo : false; }

/* Walk path; if any bus on the path is solo or any descendant is solo
 * via `m->solo_count > 0` we consider this branch a "solo path" iff
 * any ancestor (incl self) OR any descendant is solo. */

static bool any_descendant_solo(const JceAudioMixer *m, JceAudioBusId root)
{
    if (m->buses[root].solo) return true;
    for (uint16_t i = 2; i < MAX_BUSES; ++i)
        if (m->buses[i].alive && m->buses[i].parent == root)
            if (any_descendant_solo(m, (JceAudioBusId)i)) return true;
    return false;
}

float jce_audio_mixer_resolve_volume(const JceAudioMixer *m, JceAudioBusId b)
{
    if (!bus_valid(m, b)) return 0.0f;
    JCE_PROFILE_ZONE_N("Audio::Mixer::resolve_volume");
    /* Walk up: accumulate volume, check muted, check ancestor solo. */
    float gain = 1.0f;
    bool  on_solo_path = false;
    JceAudioBusId cur = b;
    /* Self chain. */
    while (cur != JCE_AUDIO_BUS_INVALID) {
        if (!bus_valid(m, cur))    { JCE_PROFILE_ZONE_END; return 0.0f; }
        if (m->buses[cur].muted)   { JCE_PROFILE_ZONE_END; return 0.0f; }
        gain *= m->buses[cur].volume;
        if (m->buses[cur].solo) on_solo_path = true;
        cur = m->buses[cur].parent;
    }
    if (m->solo_count > 0 && !on_solo_path) {
        /* Also a solo path if any descendant of `b` is solo. */
        if (!any_descendant_solo(m, b)) { JCE_PROFILE_ZONE_END; return 0.0f; }
    }
    JCE_PROFILE_ZONE_END;
    return gain;
}

/* ---------- voice -> bus map ---------- */

static VoiceMap *vm_find(JceAudioMixer *m, uint64_t id, bool insert)
{
    if (m->voices_size * 10u >= m->voices_cap * 7u && insert) {
        uint32_t new_cap = m->voices_cap * 2u;
        VoiceMap *nv = (VoiceMap *)JCE_CALLOC(new_cap, sizeof(VoiceMap));
        if (!nv) return NULL;
        for (uint32_t i = 0; i < m->voices_cap; ++i) {
            if (m->voices[i].voice_id == 0) continue;
            uint32_t h = hash_u64(m->voices[i].voice_id) & (new_cap - 1u);
            while (nv[h].voice_id != 0) h = (h + 1u) & (new_cap - 1u);
            nv[h] = m->voices[i];
        }
        JCE_FREE(m->voices);
        m->voices = nv;
        m->voices_cap = new_cap;
    }
    uint32_t h = hash_u64(id) & (m->voices_cap - 1u);
    while (m->voices[h].voice_id != 0 && m->voices[h].voice_id != id)
        h = (h + 1u) & (m->voices_cap - 1u);
    if (m->voices[h].voice_id == 0) {
        if (!insert) return NULL;
        m->voices[h].voice_id = id;
        m->voices[h].bus      = JCE_AUDIO_BUS_INVALID;
        m->voices_size++;
    }
    return &m->voices[h];
}

void jce_audio_mixer_assign_voice(JceAudioMixer *m, uint64_t id, JceAudioBusId bus)
{
    if (!m || id == 0 || !bus_valid(m, bus)) return;
    VoiceMap *v = vm_find(m, id, true);
    if (v) v->bus = bus;
}

void jce_audio_mixer_unassign_voice(JceAudioMixer *m, uint64_t id)
{
    if (!m || id == 0) return;
    /* Backward-shift delete. */
    uint32_t h = hash_u64(id) & (m->voices_cap - 1u);
    while (m->voices[h].voice_id != 0 && m->voices[h].voice_id != id)
        h = (h + 1u) & (m->voices_cap - 1u);
    if (m->voices[h].voice_id != id) return;
    m->voices[h].voice_id = 0;
    m->voices[h].bus      = JCE_AUDIO_BUS_INVALID;
    m->voices_size--;
    uint32_t j = (h + 1u) & (m->voices_cap - 1u);
    while (m->voices[j].voice_id != 0) {
        uint32_t home = hash_u64(m->voices[j].voice_id) & (m->voices_cap - 1u);
        if (((j - home) & (m->voices_cap - 1u)) > ((j - h) & (m->voices_cap - 1u))) {
            m->voices[h] = m->voices[j];
            m->voices[j].voice_id = 0;
            m->voices[j].bus      = JCE_AUDIO_BUS_INVALID;
            h = j;
        }
        j = (j + 1u) & (m->voices_cap - 1u);
    }
}

JceAudioBusId jce_audio_mixer_get_voice_bus(const JceAudioMixer *m, uint64_t id)
{
    if (!m || id == 0) return JCE_AUDIO_BUS_INVALID;
    uint32_t h = hash_u64(id) & (m->voices_cap - 1u);
    while (m->voices[h].voice_id != 0 && m->voices[h].voice_id != id)
        h = (h + 1u) & (m->voices_cap - 1u);
    return m->voices[h].voice_id == id ? m->voices[h].bus : JCE_AUDIO_BUS_INVALID;
}

uint32_t jce_audio_mixer_list_buses(const JceAudioMixer *m,
                                    JceAudioBusId *out, uint32_t cap)
{
    if (!m || !out || cap == 0) return 0;
    uint32_t n = 0;
    for (uint16_t i = 1; i < MAX_BUSES && n < cap; ++i)
        if (m->buses[i].alive) out[n++] = (JceAudioBusId)i;
    return n;
}

const char *jce_audio_mixer_get_name(const JceAudioMixer *m, JceAudioBusId b)
{ return bus_valid(m, b) ? m->buses[b].name : NULL; }

JceAudioBusId jce_audio_mixer_get_parent(const JceAudioMixer *m, JceAudioBusId b)
{ return bus_valid(m, b) ? m->buses[b].parent : JCE_AUDIO_BUS_INVALID; }

/* ================================================================== *
 *  FEATURE 5.2 — aux send/return, sidechain ducking, snapshots.       *
 * ================================================================== */

static inline float clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

static inline float db_to_lin(float db)   { return (float)pow(10.0, (double)db / 20.0); }
static inline float lin_to_db(float lin)
{
    if (lin < 1.0e-9f) lin = 1.0e-9f;
    return (float)(20.0 * log10((double)lin));
}

/* ---------------- Aux send / return ---------------- */

static BusSend *send_find(Bus *b, JceAudioBusId dest, bool insert)
{
    BusSend *empty = NULL;
    for (int i = 0; i < JCE_AUDIO_MAX_SENDS; ++i) {
        if (b->sends[i].dest == dest && dest != JCE_AUDIO_BUS_INVALID)
            return &b->sends[i];
        if (!empty && b->sends[i].dest == JCE_AUDIO_BUS_INVALID)
            empty = &b->sends[i];
    }
    return insert ? empty : NULL;
}

bool jce_audio_mixer_set_send(JceAudioMixer *m, JceAudioBusId src,
                              JceAudioBusId dest, float amount)
{
    if (!bus_valid(m, src) || !bus_valid(m, dest) || src == dest) return false;
    BusSend *s = send_find(&m->buses[src], dest, true);
    if (!s) return false;                 /* send slots exhausted */
    s->dest   = dest;
    s->amount = clamp01(amount);
    return true;
}

float jce_audio_mixer_get_send(const JceAudioMixer *m, JceAudioBusId src,
                               JceAudioBusId dest)
{
    if (!bus_valid(m, src) || dest == JCE_AUDIO_BUS_INVALID) return 0.0f;
    const Bus *b = &m->buses[src];
    for (int i = 0; i < JCE_AUDIO_MAX_SENDS; ++i)
        if (b->sends[i].dest == dest) return b->sends[i].amount;
    return 0.0f;
}

bool jce_audio_mixer_remove_send(JceAudioMixer *m, JceAudioBusId src,
                                 JceAudioBusId dest)
{
    if (!bus_valid(m, src) || dest == JCE_AUDIO_BUS_INVALID) return false;
    Bus *b = &m->buses[src];
    for (int i = 0; i < JCE_AUDIO_MAX_SENDS; ++i)
        if (b->sends[i].dest == dest) {
            b->sends[i].dest   = JCE_AUDIO_BUS_INVALID;
            b->sends[i].amount = 0.0f;
            return true;
        }
    return false;
}

uint32_t jce_audio_mixer_send_count(const JceAudioMixer *m, JceAudioBusId src)
{
    if (!bus_valid(m, src)) return 0;
    uint32_t n = 0;
    const Bus *b = &m->buses[src];
    for (int i = 0; i < JCE_AUDIO_MAX_SENDS; ++i)
        if (b->sends[i].dest != JCE_AUDIO_BUS_INVALID) n++;
    return n;
}

float jce_audio_mixer_resolve_send(const JceAudioMixer *m, JceAudioBusId src,
                                   JceAudioBusId dest)
{
    if (!bus_valid(m, src) || !bus_valid(m, dest)) return 0.0f;
    float amount = jce_audio_mixer_get_send(m, src, dest);
    if (amount <= 0.0f) return 0.0f;
    /* The send carries the source's post-fader (mute/solo-folded) signal. */
    float src_gain = jce_audio_mixer_resolve_volume(m, src);
    return src_gain * amount;
}

/* ---------------- Sidechain ducking ---------------- */

JceAudioDuckParams jce_audio_duck_default_params(void)
{
    JceAudioDuckParams p;
    p.key                = JCE_AUDIO_BUS_INVALID;
    p.threshold_db       = -30.0f;
    p.ratio              = 8.0f;
    p.attack_ms          = 10.0f;
    p.release_ms         = 250.0f;
    p.max_attenuation_db = -24.0f;
    return p;
}

/* Coefficient for an attack/release time constant evaluated once per *block*
 * of `frames` samples at `sr` Hz.  (block_seconds = frames / sr.) */
static float duck_coef(float time_ms, uint32_t frames, uint32_t sr)
{
    double fs = sr > 0 ? (double)sr : 48000.0;
    double t  = (double)(time_ms < 0.0f ? 0.0f : time_ms) / 1000.0;
    if (t <= 0.0) return 0.0f;                 /* instantaneous */
    double block_s = (double)(frames ? frames : 1) / fs;
    return (float)exp(-block_s / t);
}

bool jce_audio_mixer_set_sidechain(JceAudioMixer *m, JceAudioBusId target,
                                   const JceAudioDuckParams *params, uint32_t sr)
{
    if (!bus_valid(m, target)) return false;
    Sidechain *sc = &m->buses[target].duck;
    if (!params || params->key == JCE_AUDIO_BUS_INVALID) {
        memset(sc, 0, sizeof(*sc));
        sc->gain = 1.0f;
        return true;
    }
    sc->active      = true;
    sc->p           = *params;
    sc->sample_rate = sr ? sr : 48000u;
    if (sc->p.ratio < 1.0f) sc->p.ratio = 1.0f;
    if (sc->p.max_attenuation_db > 0.0f) sc->p.max_attenuation_db = 0.0f;
    sc->env  = 0.0f;
    sc->gain = 1.0f;
    return true;
}

bool jce_audio_mixer_clear_sidechain(JceAudioMixer *m, JceAudioBusId target)
{
    if (!bus_valid(m, target)) return false;
    if (!m->buses[target].duck.active) return false;
    memset(&m->buses[target].duck, 0, sizeof(m->buses[target].duck));
    m->buses[target].duck.gain = 1.0f;
    return true;
}

bool jce_audio_mixer_has_sidechain(const JceAudioMixer *m, JceAudioBusId target)
{ return bus_valid(m, target) && m->buses[target].duck.active; }

bool jce_audio_mixer_get_sidechain(const JceAudioMixer *m, JceAudioBusId target,
                                   JceAudioDuckParams *out)
{
    if (!bus_valid(m, target) || !out) return false;
    const Sidechain *sc = &m->buses[target].duck;
    if (!sc->active) return false;
    *out = sc->p;
    return true;
}

void jce_audio_mixer_duck_advance(JceAudioMixer *m, uint32_t frames,
                                  JceAudioKeyPeakFn key_peak, void *user)
{
    if (!m || !key_peak || frames == 0) return;
    for (uint16_t i = 1; i < MAX_BUSES; ++i) {
        if (!m->buses[i].alive) continue;
        Sidechain *sc = &m->buses[i].duck;
        if (!sc->active) continue;

        /* Sample the key bus's level for this block.  Fold its resolved
         * (mute/solo) gain in so a muted key cannot duck the target. */
        float key_peak_lin = key_peak(sc->p.key, user);
        if (key_peak_lin < 0.0f) key_peak_lin = 0.0f;
        if (bus_valid(m, sc->p.key))
            key_peak_lin *= jce_audio_mixer_resolve_volume(m, sc->p.key);

        /* Re-derive coefficients for this block length (so a test driving the
         * follower one sample / one block at a time is exact). */
        uint32_t sr = sc->sample_rate ? sc->sample_rate : 48000u;
        float atk = duck_coef(sc->p.attack_ms,  frames, sr);
        float rel = duck_coef(sc->p.release_ms, frames, sr);

        /* Envelope follower on the key level.  The follower attacks when the
         * key gets louder and releases when it falls. */
        float coef = key_peak_lin > sc->env ? atk : rel;
        sc->env = coef * (sc->env - key_peak_lin) + key_peak_lin;

        /* Downward compressor curve: how many dB of reduction the current key
         * envelope demands. */
        float env_db = lin_to_db(sc->env);
        float over   = env_db - sc->p.threshold_db;
        float gr_db  = 0.0f;                  /* gain reduction, <= 0 */
        if (over > 0.0f)
            gr_db = -(over - over / sc->p.ratio);  /* = -over*(1 - 1/ratio) */
        if (gr_db < sc->p.max_attenuation_db)
            gr_db = sc->p.max_attenuation_db;     /* floor the reduction */

        sc->gain = db_to_lin(gr_db);
    }
}

float jce_audio_mixer_duck_gain(const JceAudioMixer *m, JceAudioBusId target)
{
    if (!bus_valid(m, target)) return 1.0f;
    const Sidechain *sc = &m->buses[target].duck;
    return sc->active ? sc->gain : 1.0f;
}

float jce_audio_mixer_resolve_volume_ducked(const JceAudioMixer *m,
                                            JceAudioBusId target)
{
    return jce_audio_mixer_resolve_volume(m, target)
         * jce_audio_mixer_duck_gain(m, target);
}

/* ---------------- Mixer snapshots ---------------- */

static int snapshot_find(const JceAudioMixer *m, const char *name)
{
    if (!name) return -1;
    for (int i = 0; i < JCE_AUDIO_MAX_SNAPSHOTS; ++i)
        if (m->snapshots[i].used &&
            strncmp(m->snapshots[i].name, name, JCE_AUDIO_SNAPSHOT_NAME) == 0)
            return i;
    return -1;
}

static int snapshot_alloc(JceAudioMixer *m, const char *name)
{
    int idx = snapshot_find(m, name);
    if (idx >= 0) return idx;
    for (int i = 0; i < JCE_AUDIO_MAX_SNAPSHOTS; ++i)
        if (!m->snapshots[i].used) {
            memset(&m->snapshots[i], 0, sizeof(m->snapshots[i]));
            m->snapshots[i].used = true;
            jce_strlcpy(m->snapshots[i].name, name, JCE_AUDIO_SNAPSHOT_NAME);
            m->snapshot_count++;
            return i;
        }
    return -1;
}

bool jce_audio_mixer_capture_snapshot(JceAudioMixer *m, const char *name)
{
    if (!m || !name || !name[0]) return false;
    int idx = snapshot_alloc(m, name);
    if (idx < 0) return false;
    Snapshot *s = &m->snapshots[idx];
    for (uint16_t i = 1; i < MAX_BUSES; ++i) {
        if (m->buses[i].alive) {
            s->has[i]    = true;
            s->volume[i] = m->buses[i].volume;
        } else {
            s->has[i] = false;
        }
    }
    return true;
}

bool jce_audio_mixer_snapshot_set_volume(JceAudioMixer *m, const char *name,
                                         JceAudioBusId bus, float volume)
{
    if (!m || !name || !name[0] || bus == JCE_AUDIO_BUS_INVALID || bus >= MAX_BUSES)
        return false;
    int idx = snapshot_alloc(m, name);
    if (idx < 0) return false;
    m->snapshots[idx].has[bus]    = true;
    m->snapshots[idx].volume[bus] = volume < 0.0f ? 0.0f : volume;
    return true;
}

float jce_audio_mixer_snapshot_get_volume(const JceAudioMixer *m,
                                          const char *name, JceAudioBusId bus)
{
    if (!m || bus == JCE_AUDIO_BUS_INVALID || bus >= MAX_BUSES) return -1.0f;
    int idx = snapshot_find(m, name);
    if (idx < 0 || !m->snapshots[idx].has[bus]) return -1.0f;
    return m->snapshots[idx].volume[bus];
}

bool jce_audio_mixer_remove_snapshot(JceAudioMixer *m, const char *name)
{
    if (!m) return false;
    int idx = snapshot_find(m, name);
    if (idx < 0) return false;
    /* Cancel an in-flight fade that targets this snapshot. */
    if (m->fade_active && m->fade_target == idx) m->fade_active = false;
    memset(&m->snapshots[idx], 0, sizeof(m->snapshots[idx]));
    if (m->snapshot_count > 0) m->snapshot_count--;
    return true;
}

uint32_t jce_audio_mixer_snapshot_count(const JceAudioMixer *m)
{ return m ? m->snapshot_count : 0u; }

bool jce_audio_mixer_snapshot_name(const JceAudioMixer *m, uint32_t index,
                                   char *out, uint32_t cap)
{
    if (!m || !out || cap == 0) return false;
    uint32_t seen = 0;
    for (int i = 0; i < JCE_AUDIO_MAX_SNAPSHOTS; ++i) {
        if (!m->snapshots[i].used) continue;
        if (seen == index) {
            jce_strlcpy(out, m->snapshots[i].name, cap);
            return true;
        }
        ++seen;
    }
    return false;
}

bool jce_audio_mixer_apply_snapshot(JceAudioMixer *m, const char *name,
                                    float fade_seconds)
{
    if (!m) return false;
    int idx = snapshot_find(m, name);
    if (idx < 0) return false;
    const Snapshot *s = &m->snapshots[idx];

    /* Snapshot the from/to endpoints for every bus that the snapshot covers
     * and that is currently alive. */
    for (uint16_t i = 1; i < MAX_BUSES; ++i) {
        bool participate = m->buses[i].alive && s->has[i];
        m->fade_has[i] = participate;
        if (participate) {
            m->fade_from[i] = m->buses[i].volume;
            m->fade_to[i]   = s->volume[i];
        }
    }

    if (fade_seconds <= 0.0f) {
        /* Instant: write targets straight away, no active fade. */
        for (uint16_t i = 1; i < MAX_BUSES; ++i)
            if (m->fade_has[i]) m->buses[i].volume = m->fade_to[i];
        m->fade_active = false;
        return true;
    }

    m->fade_target   = idx;
    m->fade_duration = fade_seconds;
    m->fade_elapsed  = 0.0f;
    m->fade_active   = true;
    return true;
}

bool jce_audio_mixer_snapshot_fading(const JceAudioMixer *m)
{ return m && m->fade_active; }

float jce_audio_mixer_snapshot_progress(const JceAudioMixer *m)
{
    if (!m || !m->fade_active || m->fade_duration <= 0.0f) return 1.0f;
    float t = m->fade_elapsed / m->fade_duration;
    return clamp01(t);
}

void jce_audio_mixer_update(JceAudioMixer *m, float dt)
{
    if (!m || !m->fade_active) return;
    if (dt < 0.0f) dt = 0.0f;
    m->fade_elapsed += dt;

    float t = m->fade_duration > 0.0f ? m->fade_elapsed / m->fade_duration : 1.0f;
    bool done = t >= 1.0f;
    if (done) t = 1.0f;

    for (uint16_t i = 1; i < MAX_BUSES; ++i) {
        if (!m->fade_has[i] || !m->buses[i].alive) continue;
        /* Linear crossfade A -> B; lands exactly on B when t == 1. */
        m->buses[i].volume = done
            ? m->fade_to[i]
            : m->fade_from[i] + (m->fade_to[i] - m->fade_from[i]) * t;
    }
    if (done) m->fade_active = false;
}
