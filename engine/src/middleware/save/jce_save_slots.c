/*
 * jce_save_slots.c  Slot-based save game persistence.
 *
 * File layout:
 *   <root>/slot_<NN>.jsnp        — raw bytes the caller supplied
 *   <root>/slot_<NN>.meta.json   — tiny metadata sidecar
 *
 * The .jsnp content is opaque to this module; jce_snapshot.* owns the
 * actual binary section format.  Multi-slot management is just file +
 * sidecar bookkeeping plus a stable filename convention.
 */

#include <jce/middleware/save/jce_save_slots.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_path.h>

#include "os/core/jce_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LOG_TAG "save-slots"

static char s_root[512] = "saves";

bool jce_save_slots_set_root(const char *root)
{
    if (!root || !root[0]) return false;
    strncpy(s_root, root, sizeof(s_root) - 1);
    s_root[sizeof(s_root) - 1] = '\0';
    if (!jce_fs_host_exists_dir(s_root))
        return jce_fs_host_create_directory(s_root);
    return true;
}

static void slot_blob_path(int slot, char *out, size_t cap)
{
    char name[32];
    snprintf(name, sizeof(name), "slot_%02d.jsnp", slot);
    jce_path_join(out, cap, s_root, name);
}

static void slot_meta_path(int slot, char *out, size_t cap)
{
    char name[32];
    snprintf(name, sizeof(name), "slot_%02d.meta.json", slot);
    jce_path_join(out, cap, s_root, name);
}

static bool meta_write(int slot, const JceSaveSlotInfo *info)
{
    JceJson *root = jce_json_object();
    if (!root) return false;
    jce_json_set_string(root, "display_name",   info->display_name);
    jce_json_set_number(root, "timestamp_unix", (double)info->timestamp_unix);
    jce_json_set_number(root, "playtime_seconds", info->playtime_seconds);
    jce_json_set_string(root, "thumbnail_path", info->thumbnail_path);
    jce_json_set_number(root, "format_version", info->format_version);
    char path[1024];
    slot_meta_path(slot, path, sizeof(path));
    return jce_json_write_file(path, root, true, /*take_ownership=*/true);
}

static bool meta_read(int slot, JceSaveSlotInfo *out)
{
    char path[1024];
    slot_meta_path(slot, path, sizeof(path));
    JceJson *root = jce_json_parse_file(path);
    if (!root) return false;

    const char *dn = jce_json_get_string(root, "display_name", "");
    strncpy(out->display_name, dn, sizeof(out->display_name) - 1);
    out->display_name[sizeof(out->display_name) - 1] = '\0';

    out->timestamp_unix   = (uint64_t)jce_json_get_number(root, "timestamp_unix",   0.0);
    out->playtime_seconds = (float)   jce_json_get_number(root, "playtime_seconds", 0.0);
    const char *tn = jce_json_get_string(root, "thumbnail_path", "");
    strncpy(out->thumbnail_path, tn, sizeof(out->thumbnail_path) - 1);
    out->thumbnail_path[sizeof(out->thumbnail_path) - 1] = '\0';
    out->format_version = (uint32_t)jce_json_get_number(root, "format_version", 0.0);
    jce_json_free(root);
    return true;
}

bool jce_save_slots_get_info(int slot, JceSaveSlotInfo *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->slot_index = slot;
    if (slot < 0 || slot >= JCE_SAVE_SLOT_MAX) return false;

    char blob[1024];
    slot_blob_path(slot, blob, sizeof(blob));
    if (!jce_fs_host_exists_file(blob)) return false;

    out->exists = true;
    jce_fs_host_get_size(blob, &out->blob_size_bytes);
    meta_read(slot, out);  /* meta may be missing — fields default */
    return true;
}

uint32_t jce_save_slots_list(JceSaveSlotInfo *out, uint32_t max)
{
    if (!out || max == 0) return 0;
    uint32_t n = max < JCE_SAVE_SLOT_MAX ? max : JCE_SAVE_SLOT_MAX;
    for (uint32_t i = 0; i < n; ++i) {
        if (!jce_save_slots_get_info((int)i, &out[i])) {
            /* Slot doesn't exist — mark as empty but valid index. */
            memset(&out[i], 0, sizeof(out[i]));
            out[i].slot_index = (int)i;
        }
    }
    return n;
}

bool jce_save_slots_write(int slot, const void *blob, uint64_t size,
                           const char *display_name, float playtime,
                           const char *thumb, uint32_t format_version)
{
    if (slot < 0 || slot >= JCE_SAVE_SLOT_MAX) return false;
    if (!blob || size == 0) return false;
    if (!jce_fs_host_exists_dir(s_root)) jce_fs_host_create_directory(s_root);

    char blob_path[1024];
    slot_blob_path(slot, blob_path, sizeof(blob_path));
    if (!jce_fs_host_write_all(blob_path, blob, size)) return false;

    JceSaveSlotInfo info;
    memset(&info, 0, sizeof(info));
    info.slot_index = slot;
    info.exists = true;
    info.blob_size_bytes = size;
    if (display_name && display_name[0]) {
        strncpy(info.display_name, display_name, sizeof(info.display_name) - 1);
    } else {
        snprintf(info.display_name, sizeof(info.display_name),
                 "Save Slot %d", slot);
    }
    info.timestamp_unix   = (uint64_t)time(NULL);
    info.playtime_seconds = playtime;
    if (thumb) strncpy(info.thumbnail_path, thumb,
                       sizeof(info.thumbnail_path) - 1);
    info.format_version = format_version;

    return meta_write(slot, &info);
}

void *jce_save_slots_read(int slot, uint64_t *out_size,
                           JceSaveSlotInfo *out_info)
{
    if (slot < 0 || slot >= JCE_SAVE_SLOT_MAX) return NULL;
    char blob[1024];
    slot_blob_path(slot, blob, sizeof(blob));
    if (!jce_fs_host_exists_file(blob)) return NULL;
    uint64_t size = 0;
    void *buf = jce_fs_host_read_all(blob, &size);
    if (!buf || size == 0) {
        if (buf) jce_fs_buffer_free(buf);
        return NULL;
    }
    /* Copy to a malloc-style buffer the caller can free() directly. */
    void *out = malloc((size_t)size);
    if (!out) {
        jce_fs_buffer_free(buf);
        return NULL;
    }
    memcpy(out, buf, (size_t)size);
    jce_fs_buffer_free(buf);

    if (out_size) *out_size = size;
    if (out_info) {
        memset(out_info, 0, sizeof(*out_info));
        out_info->slot_index = slot;
        out_info->exists = true;
        out_info->blob_size_bytes = size;
        meta_read(slot, out_info);
    }
    return out;
}

bool jce_save_slots_delete(int slot)
{
    if (slot < 0 || slot >= JCE_SAVE_SLOT_MAX) return false;
    char blob[1024], meta[1024];
    slot_blob_path(slot, blob, sizeof(blob));
    slot_meta_path(slot, meta, sizeof(meta));
    bool ok = true;
    if (jce_fs_host_exists_file(blob))
        ok = jce_fs_host_remove_file(blob) && ok;
    if (jce_fs_host_exists_file(meta))
        ok = jce_fs_host_remove_file(meta) && ok;
    return ok;
}

int jce_save_slots_most_recent(void)
{
    int best = -1;
    uint64_t best_t = 0;
    for (int i = 0; i < JCE_SAVE_SLOT_MAX; ++i) {
        JceSaveSlotInfo info;
        if (!jce_save_slots_get_info(i, &info)) continue;
        if (!info.exists) continue;
        if (info.timestamp_unix >= best_t) {
            best_t = info.timestamp_unix;
            best   = i;
        }
    }
    return best;
}
