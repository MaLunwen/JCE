/*
 * jce_addressable.c  Addressable-group registry.
 */

#include <jce/resource/jce_addressable.h>

#include <string.h>

static JceAddressableGroup s_groups[JCE_ADDRESSABLE_GROUPS_MAX];

static int find_group_slot(const char *name)
{
    if (!name) return -1;
    int free_slot = -1;
    for (int i = 0; i < JCE_ADDRESSABLE_GROUPS_MAX; ++i) {
        if (s_groups[i].active &&
            strncmp(s_groups[i].name, name, JCE_ADDRESSABLE_GROUP_NAME_LEN) == 0)
            return i;
        if (!s_groups[i].active && free_slot < 0) free_slot = i;
    }
    return free_slot;
}

uint32_t jce_addressable_register_group(const char *name,
                                         const char *const *paths)
{
    if (!name || !name[0]) return UINT32_MAX;
    int slot = find_group_slot(name);
    if (slot < 0) return UINT32_MAX;
    JceAddressableGroup *g = &s_groups[slot];
    if (!g->active) {
        memset(g, 0, sizeof(*g));
        strncpy(g->name, name, JCE_ADDRESSABLE_GROUP_NAME_LEN - 1);
        g->name[JCE_ADDRESSABLE_GROUP_NAME_LEN - 1] = '\0';
        g->active = true;
    }
    if (paths) {
        for (uint32_t i = 0; paths[i] && g->path_count < JCE_ADDRESSABLE_PATHS_PER_GROUP; ++i)
            jce_addressable_add_path((uint32_t)slot, paths[i]);
    }
    return (uint32_t)slot;
}

bool jce_addressable_add_path(uint32_t idx, const char *path)
{
    if (idx >= JCE_ADDRESSABLE_GROUPS_MAX) return false;
    JceAddressableGroup *g = &s_groups[idx];
    if (!g->active || !path || !path[0]) return false;
    for (uint32_t i = 0; i < g->path_count; ++i)
        if (strncmp(g->paths[i], path, JCE_ADDRESSABLE_PATH_LEN) == 0) return true;
    if (g->path_count >= JCE_ADDRESSABLE_PATHS_PER_GROUP) return false;
    strncpy(g->paths[g->path_count], path, JCE_ADDRESSABLE_PATH_LEN - 1);
    g->paths[g->path_count][JCE_ADDRESSABLE_PATH_LEN - 1] = '\0';
    g->path_count++;
    return true;
}

bool jce_addressable_unregister(const char *name)
{
    int slot = find_group_slot(name);
    if (slot < 0 || !s_groups[slot].active) return false;
    memset(&s_groups[slot], 0, sizeof(s_groups[slot]));
    return true;
}

uint32_t jce_addressable_group_count(void)
{
    uint32_t n = 0;
    for (int i = 0; i < JCE_ADDRESSABLE_GROUPS_MAX; ++i)
        if (s_groups[i].active) n++;
    return n;
}

const JceAddressableGroup *jce_addressable_get(uint32_t idx)
{
    if (idx >= JCE_ADDRESSABLE_GROUPS_MAX) return NULL;
    return s_groups[idx].active ? &s_groups[idx] : NULL;
}

const JceAddressableGroup *jce_addressable_find(const char *name)
{
    int slot = find_group_slot(name);
    return (slot >= 0 && s_groups[slot].active) ? &s_groups[slot] : NULL;
}

void jce_addressable_set_loaded(const char *name, bool loaded)
{
    int slot = find_group_slot(name);
    if (slot >= 0 && s_groups[slot].active) s_groups[slot].loaded = loaded;
}
