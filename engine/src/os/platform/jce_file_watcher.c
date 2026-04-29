/*
 * jce_file_watcher.c  Poll-based file watcher.
 *
 * Backed by SDL_GetPathInfo() for portable mtime queries.
 */

#include <jce/os/platform/jce_file_watcher.h>

#include "os/core/jce_memory.h"

#include <SDL3/SDL.h>
#include <string.h>

typedef struct {
    char            *path;            /* heap-owned */
    int64_t          last_mtime_ns;   /* SDL_PathInfo.modify_time */
    JceFileChangedFn cb;
    void            *user_data;
    bool             active;
} WatchEntry;

struct JceFileWatcher {
    WatchEntry *entries;
    uint32_t    count;
    uint32_t    cap;
};

JceFileWatcher *jce_file_watcher_create(void)
{
    JceFileWatcher *fw = (JceFileWatcher *)JCE_CALLOC(1, sizeof(*fw));
    return fw;
}

void jce_file_watcher_destroy(JceFileWatcher *fw)
{
    if (!fw) return;
    for (uint32_t i = 0; i < fw->count; ++i)
        JCE_FREE(fw->entries[i].path);
    JCE_FREE(fw->entries);
    JCE_FREE(fw);
}

static WatchEntry *find_entry(JceFileWatcher *fw, const char *path)
{
    for (uint32_t i = 0; i < fw->count; ++i) {
        if (fw->entries[i].active && fw->entries[i].path &&
            strcmp(fw->entries[i].path, path) == 0)
            return &fw->entries[i];
    }
    return NULL;
}

static WatchEntry *alloc_entry(JceFileWatcher *fw)
{
    /* Recycle. */
    for (uint32_t i = 0; i < fw->count; ++i) {
        if (!fw->entries[i].active) {
            memset(&fw->entries[i], 0, sizeof(WatchEntry));
            return &fw->entries[i];
        }
    }
    if (fw->count == fw->cap) {
        uint32_t newcap = fw->cap ? fw->cap * 2 : 8;
        WatchEntry *ne = (WatchEntry *)JCE_REALLOC(fw->entries,
                                                    sizeof(WatchEntry) * newcap);
        if (!ne) return NULL;
        memset(ne + fw->cap, 0, sizeof(WatchEntry) * (newcap - fw->cap));
        fw->entries = ne;
        fw->cap     = newcap;
    }
    return &fw->entries[fw->count++];
}

bool jce_file_watcher_add(JceFileWatcher *fw, const char *path,
                          JceFileChangedFn cb, void *user_data)
{
    if (!fw || !path || !cb) return false;
    if (find_entry(fw, path)) return true;  /* already watching */

    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info)) return false;

    WatchEntry *e = alloc_entry(fw);
    if (!e) return false;
    size_t n = strlen(path) + 1;
    e->path = (char *)JCE_MALLOC(n);
    if (!e->path) {
        e->active = false;
        return false;
    }
    memcpy(e->path, path, n);
    e->last_mtime_ns = info.modify_time;
    e->cb            = cb;
    e->user_data     = user_data;
    e->active        = true;
    return true;
}

void jce_file_watcher_remove(JceFileWatcher *fw, const char *path)
{
    if (!fw || !path) return;
    WatchEntry *e = find_entry(fw, path);
    if (!e) return;
    JCE_FREE(e->path);
    e->path   = NULL;
    e->active = false;
}

uint32_t jce_file_watcher_poll(JceFileWatcher *fw)
{
    if (!fw) return 0;
    uint32_t fired = 0;
    for (uint32_t i = 0; i < fw->count; ++i) {
        WatchEntry *e = &fw->entries[i];
        if (!e->active || !e->path) continue;

        SDL_PathInfo info;
        if (!SDL_GetPathInfo(e->path, &info)) continue;
        if (info.modify_time == e->last_mtime_ns) continue;

        e->last_mtime_ns = info.modify_time;
        e->cb(e->path, e->user_data);
        fired++;
    }
    return fired;
}

uint32_t jce_file_watcher_count(const JceFileWatcher *fw)
{
    if (!fw) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < fw->count; ++i)
        if (fw->entries[i].active) ++n;
    return n;
}
