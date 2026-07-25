/*
 * jce_mem_profile.c  Tag-categorised memory accounting (P3-A.5).
 *
 * Implementation notes:
 *   - 64-entry table guarded by a single SDL mutex. Coarse and correct;
 *     the call sites we instrument are not per-byte, they are per-asset
 *     / per-packet, so a global lock is fine.
 *   - mi_process_info() supplies the process / mimalloc snapshot — no
 *     direct PSAPI / sysctl / proc-fs calls (banned by §0).
 *   - Tracy categorised allocs: the vendored Tracy C ABI used by
 *     jce_profiler.h only exposes anonymous alloc/free hooks, so we
 *     plot the per-tag totals instead. Deep-trace correlation still
 *     works via the named plot.
 */

#include <jce/os/core/jce_mem_profile.h>

#include <jce/os/core/jce_profiler.h>

#include <mimalloc.h>
#include <SDL3/SDL.h>
#include <string.h>

/* ================================================================== */
/* State                                                                */
/* ================================================================== */

typedef struct TagSlot {
    uint64_t current_bytes;
    uint64_t peak_bytes;
    uint64_t total_bytes_allocated;
    uint64_t total_bytes_freed;
    uint32_t live_alloc_count;
    uint32_t total_alloc_count;
    const char *name;       /* either default literal or user-supplied */
} TagSlot;

static TagSlot     g_tags[JCE_MEM_TAG_COUNT_MAX];
static SDL_Mutex  *g_lock;
static SDL_InitState g_init;

static const char *default_name_for(JceMemTag tag)
{
    switch (tag) {
    case JCE_MEM_TAG_GENERIC:           return "generic";
    case JCE_MEM_TAG_RESOURCE_TEXTURES: return "resource/textures";
    case JCE_MEM_TAG_RESOURCE_MESHES:   return "resource/meshes";
    case JCE_MEM_TAG_RESOURCE_AUDIO:    return "resource/audio";
    case JCE_MEM_TAG_RESOURCE_OTHER:    return "resource/other";
    case JCE_MEM_TAG_RENDERER:          return "renderer";
    case JCE_MEM_TAG_PHYSICS:           return "physics";
    case JCE_MEM_TAG_SCENE_ECS:         return "scene/ecs";
    case JCE_MEM_TAG_SCRIPTING:         return "scripting";
    case JCE_MEM_TAG_NETWORK:           return "network";
    case JCE_MEM_TAG_UI:                return "ui";
    case JCE_MEM_TAG_AUDIO:             return "audio";
    case JCE_MEM_TAG_EDITOR:            return "editor";
    case JCE_MEM_TAG_TEMP:              return "temp";
    default:                            return NULL;
    }
}

static void ensure_init(void)
{
    if (SDL_ShouldInit(&g_init)) {
        g_lock = SDL_CreateMutex();
        for (int i = 0; i < JCE_MEM_TAG_COUNT_MAX; ++i) {
            g_tags[i].name = default_name_for((JceMemTag)i);
        }
        SDL_SetInitialized(&g_init, true);
    }
}

static bool tag_in_range(JceMemTag tag)
{
    return (unsigned)tag < (unsigned)JCE_MEM_TAG_COUNT_MAX;
}

/* ================================================================== */
/* Public API                                                           */
/* ================================================================== */

void jce_mem_profile_register_tag_name(JceMemTag tag, const char *name)
{
    if (!tag_in_range(tag) || !name) return;
    ensure_init();
    SDL_LockMutex(g_lock);
    g_tags[tag].name = name;
    SDL_UnlockMutex(g_lock);
}

const char *jce_mem_profile_tag_name(JceMemTag tag)
{
    if (!tag_in_range(tag)) return "?";
    ensure_init();
    SDL_LockMutex(g_lock);
    const char *n = g_tags[tag].name;
    SDL_UnlockMutex(g_lock);
    return n ? n : "user";
}

void jce_mem_profile_record_alloc(JceMemTag tag, size_t bytes)
{
    if (!tag_in_range(tag) || bytes == 0) return;
    ensure_init();
    SDL_LockMutex(g_lock);
    TagSlot *s = &g_tags[tag];
    s->current_bytes          += bytes;
    s->total_bytes_allocated  += bytes;
    s->live_alloc_count       += 1u;
    s->total_alloc_count      += 1u;
    if (s->current_bytes > s->peak_bytes) s->peak_bytes = s->current_bytes;
    uint64_t snapshot = s->current_bytes;
    const char *name = s->name;
    SDL_UnlockMutex(g_lock);

    if (name) JCE_PROFILE_PLOT_I(name, (int64_t)snapshot);
    (void)name;
}

void jce_mem_profile_record_free(JceMemTag tag, size_t bytes)
{
    if (!tag_in_range(tag) || bytes == 0) return;
    ensure_init();
    SDL_LockMutex(g_lock);
    TagSlot *s = &g_tags[tag];
    /* Clamp to avoid underflow on instrumentation mistakes. */
    if (bytes > s->current_bytes) bytes = (size_t)s->current_bytes;
    s->current_bytes      -= bytes;
    s->total_bytes_freed  += bytes;
    if (s->live_alloc_count > 0) s->live_alloc_count -= 1u;
    uint64_t snapshot = s->current_bytes;
    const char *name = s->name;
    SDL_UnlockMutex(g_lock);

    if (name) JCE_PROFILE_PLOT_I(name, (int64_t)snapshot);
    (void)name;
}

bool jce_mem_profile_get_stats(JceMemTag tag, JceMemTagStats *out)
{
    if (!tag_in_range(tag) || !out) return false;
    ensure_init();
    SDL_LockMutex(g_lock);
    const TagSlot *s = &g_tags[tag];
    out->current_bytes         = s->current_bytes;
    out->peak_bytes            = s->peak_bytes;
    out->total_bytes_allocated = s->total_bytes_allocated;
    out->total_bytes_freed     = s->total_bytes_freed;
    out->live_alloc_count      = s->live_alloc_count;
    out->total_alloc_count     = s->total_alloc_count;
    SDL_UnlockMutex(g_lock);
    return true;
}

void jce_mem_profile_reset_peaks(void)
{
    ensure_init();
    SDL_LockMutex(g_lock);
    for (int i = 0; i < JCE_MEM_TAG_COUNT_MAX; ++i) {
        g_tags[i].peak_bytes = g_tags[i].current_bytes;
    }
    SDL_UnlockMutex(g_lock);
}

void jce_mem_profile_get_total(uint64_t *current, uint64_t *peak)
{
    ensure_init();
    uint64_t cur = 0, pk = 0;
    SDL_LockMutex(g_lock);
    for (int i = 0; i < JCE_MEM_TAG_COUNT_MAX; ++i) {
        cur += g_tags[i].current_bytes;
        pk  += g_tags[i].peak_bytes;
    }
    SDL_UnlockMutex(g_lock);
    if (current) *current = cur;
    if (peak)    *peak    = pk;
}

bool jce_mem_profile_get_process_stats(JceProcessMemStats *out)
{
    if (!out) return false;

    size_t elapsed = 0, user_ms = 0, sys_ms = 0;
    size_t cur_rss = 0, peak_rss = 0;
    size_t cur_commit = 0, peak_commit = 0;
    size_t page_faults = 0;

    mi_process_info(&elapsed, &user_ms, &sys_ms,
                    &cur_rss, &peak_rss,
                    &cur_commit, &peak_commit,
                    &page_faults);

    /* mimalloc 3.x's release build compiles out the stats subsystem (MI_STAT=0),
     * so mi_stats_merge() has no linkable definition — and mi_process_info()
     * above already provides the process RSS/commit snapshot without it. */

    out->process_rss_bytes        = (uint64_t)cur_rss;
    out->process_committed_bytes  = (uint64_t)cur_commit;
    /* mimalloc does not expose a separate "reserved-but-not-committed"
     * counter through the public C API, so we report peak committed as
     * the high-water mark and current committed for the live value.
     * These mirror the process counters today; left as separate fields
     * so a future mimalloc version (or a swap to another allocator
     * exposing finer counters) can populate them independently. */
    out->mimalloc_reserved_bytes  = (uint64_t)peak_commit;
    out->mimalloc_committed_bytes = (uint64_t)cur_commit;
    return true;
}
