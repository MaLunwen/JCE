/*
 * jce_editor_asset_label_map.cpp  Editor-side asset-label map.
 *
 * Stores (asset_path, label_bits) entries in a flat array.  Linear
 * scan is fine — projects rarely have more than a few hundred labelled
 * assets.  Persistence is a small JSON sidecar so version-control
 * diffs stay readable.
 */

#include "jce_editor_asset_label_map.h"
#include "ui/jce_editor_panels.h"

extern "C" {
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>
#include <jce/os/core/jce_log.h>
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>

namespace {

#define LOG_TAG "asset-labels"

struct Entry {
    std::string path;
    uint64_t    bits;
};

static std::vector<Entry> &entries(void)
{
    static std::vector<Entry> e;
    return e;
}

static int find_idx(const char *path)
{
    auto &es = entries();
    for (size_t i = 0; i < es.size(); ++i)
        if (es[i].path == path) return (int)i;
    return -1;
}

static bool sidecar_path(char *out, size_t cap)
{
    const char *root = jce_editor_assets_get_project();
    if (!root || !*root) return false;
    char dir[1024];
    if (!jce_path_join(dir, sizeof(dir), root, ".jce")) return false;
    if (!jce_fs_host_exists_dir(dir)) {
        if (!jce_fs_host_create_directory(dir)) return false;
    }
    return jce_path_join(out, cap, dir, "asset_labels.json");
}

} /* namespace */

extern "C" uint64_t jce_editor_asset_labels_get(const char *asset_path)
{
    if (!asset_path) return 0;
    int i = find_idx(asset_path);
    return (i >= 0) ? entries()[i].bits : 0u;
}

extern "C" bool jce_editor_asset_labels_set(const char *asset_path, uint64_t bits)
{
    if (!asset_path) return false;
    int i = find_idx(asset_path);
    if (bits == 0) {
        if (i >= 0) entries().erase(entries().begin() + i);
        return true;
    }
    if (i >= 0) {
        entries()[i].bits = bits;
    } else {
        Entry e;
        e.path = asset_path;
        e.bits = bits;
        entries().push_back(std::move(e));
    }
    return true;
}

extern "C" uint32_t jce_editor_asset_labels_entry_count(void)
{
    return (uint32_t)entries().size();
}

/* Tiny hand-rolled JSON writer/reader to avoid pulling cJSON for what
 * is a flat list.  Format:
 *
 *   { "entries": [
 *       { "path": "...", "bits": 12 },
 *       ...
 *   ] }
 *
 * `bits` is stored as a decimal number (uint64 in JSON range). */

extern "C" bool jce_editor_asset_labels_save(void)
{
    char path[1280];
    if (!sidecar_path(path, sizeof(path))) return false;

    std::string s;
    s.reserve(64 + entries().size() * 96);
    s += "{\n  \"entries\": [\n";
    bool first = true;
    char buf[512];
    for (auto &e : entries()) {
        snprintf(buf, sizeof(buf),
                 "%s    { \"path\": \"%s\", \"bits\": %llu }",
                 first ? "" : ",\n",
                 e.path.c_str(), (unsigned long long)e.bits);
        s += buf;
        first = false;
    }
    s += "\n  ]\n}\n";
    return jce_fs_host_write_all(path, s.data(), s.size());
}

extern "C" bool jce_editor_asset_labels_load(void)
{
    char path[1280];
    if (!sidecar_path(path, sizeof(path))) return false;
    if (!jce_fs_host_exists_file(path)) return false;

    uint64_t size = 0;
    void *raw = jce_fs_host_read_all(path, &size);
    if (!raw || size == 0) {
        if (raw) jce_fs_buffer_free(raw);
        return false;
    }
    /* Tiny parser: scan for `"path":` and `"bits":` pairs in order.
     * Robust enough for our writer's output; not a general JSON
     * parser. */
    entries().clear();
    const char *p = (const char *)raw;
    const char *end = p + size;
    while (p < end) {
        const char *path_key = strstr(p, "\"path\"");
        if (!path_key || path_key >= end) break;
        const char *q = strchr(path_key, ':');
        if (!q) break;
        q = strchr(q, '"');
        if (!q) break;
        const char *q_end = strchr(q + 1, '"');
        if (!q_end) break;
        std::string asset(q + 1, q_end - q - 1);

        const char *bits_key = strstr(q_end, "\"bits\"");
        if (!bits_key) break;
        const char *colon = strchr(bits_key, ':');
        if (!colon) break;
        uint64_t bits = strtoull(colon + 1, NULL, 10);

        Entry e;
        e.path = std::move(asset);
        e.bits = bits;
        entries().push_back(std::move(e));
        p = bits_key + 6;
    }
    jce_fs_buffer_free(raw);
    return true;
}
