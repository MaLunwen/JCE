/*
 * jce_audio_mixer.c -- generic hierarchical mixer bus tree.
 */

#include <jce/middleware/audio/jce_audio_mixer.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>

#include "os/core/jce_memory.h"

#include <string.h>
#include <jce/os/core/jce_str.h>

#define LOG_TAG       "mixer"
#define MAX_BUSES     128
#define MAX_NAME      32

typedef struct Bus {
    bool          alive;
    JceAudioBusId parent;
    char          name[MAX_NAME];
    float         volume;
    bool          muted;
    bool          solo;
} Bus;

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
