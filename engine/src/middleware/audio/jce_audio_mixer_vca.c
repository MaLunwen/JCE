/*
 * jce_audio_mixer_vca.c  VCA (Volume Control Automation) side-table.
 *
 * Kept separate from jce_audio_mixer.c so the existing mixer struct
 * + linker symbol set is untouched.  The VCA registry is process-
 * global, keyed by mixer pointer + VCA name; bus subscriptions are
 * indexed by (mixer, bus_id) pairs.
 *
 * Effect: jce_audio_mixer_resolve_volume should call into the
 * companion helper jce_audio_mixer_vca_apply_to_volume() once the
 * mixer adopts it — currently optional; the side-table is read by
 * game code that explicitly queries VCA values via the public API.
 */

#include <jce/middleware/audio/jce_audio_mixer.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define VCA_NAMES_PER_MIXER 16
#define MIXER_SLOTS         8

typedef struct {
    char  name[JCE_MIXER_VCA_NAME_LEN];
    float value;
    bool  active;
} VcaSlot;

typedef struct {
    JceAudioBusId bus;
    char          vca_names[JCE_MIXER_BUS_VCA_SLOTS][JCE_MIXER_VCA_NAME_LEN];
    uint32_t      vca_count;
    bool          active;
} BusVcaSub;

#define BUS_SUB_MAX 256

typedef struct {
    const JceAudioMixer *mixer;
    VcaSlot              vcas[VCA_NAMES_PER_MIXER];
    BusVcaSub            subs[BUS_SUB_MAX];
    bool                 active;
} MixerVcaTable;

static MixerVcaTable s_tables[MIXER_SLOTS];

static MixerVcaTable *find_table(const JceAudioMixer *m, bool create)
{
    if (!m) return NULL;
    MixerVcaTable *free_slot = NULL;
    for (int i = 0; i < MIXER_SLOTS; ++i) {
        if (s_tables[i].active && s_tables[i].mixer == m) return &s_tables[i];
        if (!s_tables[i].active && !free_slot) free_slot = &s_tables[i];
    }
    if (!create || !free_slot) return NULL;
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->mixer = m;
    free_slot->active = true;
    return free_slot;
}

static VcaSlot *find_vca(MixerVcaTable *t, const char *name, bool create)
{
    if (!t || !name) return NULL;
    VcaSlot *free_slot = NULL;
    for (int i = 0; i < VCA_NAMES_PER_MIXER; ++i) {
        if (t->vcas[i].active &&
            strncmp(t->vcas[i].name, name, JCE_MIXER_VCA_NAME_LEN) == 0)
            return &t->vcas[i];
        if (!t->vcas[i].active && !free_slot) free_slot = &t->vcas[i];
    }
    if (!create || !free_slot) return NULL;
    memset(free_slot, 0, sizeof(*free_slot));
    strncpy(free_slot->name, name, JCE_MIXER_VCA_NAME_LEN - 1);
    free_slot->value  = 1.0f;
    free_slot->active = true;
    return free_slot;
}

static BusVcaSub *find_sub(MixerVcaTable *t, JceAudioBusId bus, bool create)
{
    BusVcaSub *free_slot = NULL;
    for (int i = 0; i < BUS_SUB_MAX; ++i) {
        if (t->subs[i].active && t->subs[i].bus == bus) return &t->subs[i];
        if (!t->subs[i].active && !free_slot) free_slot = &t->subs[i];
    }
    if (!create || !free_slot) return NULL;
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->bus    = bus;
    free_slot->active = true;
    return free_slot;
}

bool jce_audio_mixer_bus_attach_vca(JceAudioMixer *m, JceAudioBusId bus,
                                      const char *vca_name)
{
    MixerVcaTable *t = find_table(m, true);
    if (!t) return false;
    BusVcaSub *sub = find_sub(t, bus, true);
    if (!sub) return false;
    /* Idempotent on duplicates. */
    for (uint32_t i = 0; i < sub->vca_count; ++i)
        if (strncmp(sub->vca_names[i], vca_name,
                     JCE_MIXER_VCA_NAME_LEN) == 0) return true;
    if (sub->vca_count >= JCE_MIXER_BUS_VCA_SLOTS) return false;
    strncpy(sub->vca_names[sub->vca_count], vca_name,
             JCE_MIXER_VCA_NAME_LEN - 1);
    sub->vca_count++;
    find_vca(t, vca_name, /*create=*/true);  /* ensure entry */
    return true;
}

bool jce_audio_mixer_bus_detach_vca(JceAudioMixer *m, JceAudioBusId bus,
                                      const char *vca_name)
{
    MixerVcaTable *t = find_table(m, false);
    if (!t) return false;
    BusVcaSub *sub = find_sub(t, bus, false);
    if (!sub) return false;
    for (uint32_t i = 0; i < sub->vca_count; ++i) {
        if (strncmp(sub->vca_names[i], vca_name,
                     JCE_MIXER_VCA_NAME_LEN) != 0) continue;
        /* Swap-and-pop. */
        memcpy(sub->vca_names[i], sub->vca_names[sub->vca_count - 1],
                JCE_MIXER_VCA_NAME_LEN);
        sub->vca_count--;
        return true;
    }
    return false;
}

void jce_audio_mixer_set_vca_value(JceAudioMixer *m, const char *vca_name,
                                     float value)
{
    MixerVcaTable *t = find_table(m, true);
    if (!t) return;
    VcaSlot *v = find_vca(t, vca_name, true);
    if (v) v->value = value;
}

float jce_audio_mixer_get_vca_value(const JceAudioMixer *m,
                                      const char *vca_name)
{
    MixerVcaTable *t = find_table(m, false);
    if (!t) return 1.0f;
    VcaSlot *v = find_vca(t, vca_name, false);
    return v ? v->value : 1.0f;
}

uint32_t jce_audio_mixer_list_vcas(const JceAudioMixer *m,
                                     const char **out_names, uint32_t cap)
{
    MixerVcaTable *t = find_table(m, false);
    if (!t || !out_names || cap == 0) return 0;
    uint32_t n = 0;
    for (int i = 0; i < VCA_NAMES_PER_MIXER && n < cap; ++i)
        if (t->vcas[i].active) out_names[n++] = t->vcas[i].name;
    return n;
}

/* Helper consumed by the mixer's resolve_volume once integrated.
 * Pure read-only; returns the product of every VCA subscribed by
 * `bus`.  Default = 1.0 when no subscriptions. */
float jce_audio_mixer_vca_apply_to_volume(const JceAudioMixer *m,
                                            JceAudioBusId bus)
{
    MixerVcaTable *t = find_table(m, false);
    if (!t) return 1.0f;
    BusVcaSub *sub = find_sub((MixerVcaTable *)t, bus, false);
    if (!sub || sub->vca_count == 0) return 1.0f;
    float prod = 1.0f;
    for (uint32_t i = 0; i < sub->vca_count; ++i) {
        VcaSlot *v = find_vca((MixerVcaTable *)t, sub->vca_names[i], false);
        if (v) prod *= v->value;
    }
    return prod;
}
