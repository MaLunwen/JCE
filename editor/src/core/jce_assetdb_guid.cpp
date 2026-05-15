/*
 * jce_assetdb_guid.cpp  Stable asset identifier + dependency graph.
 *
 * Storage layout
 *   Entries:   fixed-size open-addressing hash on path → (guid, index)
 *   Reverse:   array indexed by entry, holding outgoing dep guid list
 * Both grow lazily up to JCE_ASSETDB_MAX_ENTRIES; collisions resolve
 * by linear probe.  Bumped well above realistic project sizes — the
 * editor is single-user so memory is fine.
 */

#include "jce_assetdb_guid.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr uint32_t MAX_ENTRIES = 8192;
constexpr uint32_t MAX_DEPS_PER_ENTRY = 64;
constexpr uint32_t PATH_LEN    = 256;

struct Entry {
    char     path[PATH_LEN];
    JceGuid  guid;
    /* Outgoing dependencies. */
    JceGuid  deps[MAX_DEPS_PER_ENTRY];
    uint16_t dep_count;
    bool     used;
};

Entry    s_entries[MAX_ENTRIES];
uint32_t s_entry_count = 0;
uint64_t s_guid_counter = 0;

uint32_t path_hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

int find_entry_index(const char *path)
{
    if (!path || !path[0]) return -1;
    uint32_t h = path_hash(path) % MAX_ENTRIES;
    for (uint32_t i = 0; i < MAX_ENTRIES; ++i) {
        uint32_t idx = (h + i) % MAX_ENTRIES;
        const Entry &e = s_entries[idx];
        if (!e.used) return -1;
        if (strncmp(e.path, path, PATH_LEN) == 0) return (int)idx;
    }
    return -1;
}

int find_or_create_index(const char *path)
{
    int existing = find_entry_index(path);
    if (existing >= 0) return existing;
    if (!path || !path[0]) return -1;
    uint32_t h = path_hash(path) % MAX_ENTRIES;
    for (uint32_t i = 0; i < MAX_ENTRIES; ++i) {
        uint32_t idx = (h + i) % MAX_ENTRIES;
        Entry &e = s_entries[idx];
        if (!e.used) {
            memset(&e, 0, sizeof(e));
            strncpy(e.path, path, PATH_LEN - 1);
            e.path[PATH_LEN - 1] = '\0';
            e.used = true;
            s_entry_count++;
            return (int)idx;
        }
    }
    return -1;
}

int find_entry_index_by_guid(JceGuid g)
{
    for (uint32_t i = 0; i < MAX_ENTRIES; ++i) {
        if (!s_entries[i].used) continue;
        if (jce_guid_equal(s_entries[i].guid, g)) return (int)i;
    }
    return -1;
}

uint64_t mix64(uint64_t v)
{
    v ^= v >> 33;
    v *= 0xff51afd7ed558ccdULL;
    v ^= v >> 33;
    v *= 0xc4ceb9fe1a85ec53ULL;
    v ^= v >> 33;
    return v;
}

} /* namespace */

JceGuid jce_guid_new(void)
{
    JceGuid g;
    s_guid_counter++;
    g.lo = mix64(s_guid_counter * 0x9E3779B97F4A7C15ULL);
    g.hi = mix64((uint64_t)(uintptr_t)&g ^ (s_guid_counter << 1));
    if (g.lo == 0 && g.hi == 0) g.lo = 1; /* never return the sentinel */
    return g;
}

bool jce_guid_is_zero(JceGuid g) { return g.lo == 0 && g.hi == 0; }

bool jce_guid_equal(JceGuid a, JceGuid b)
{
    return a.lo == b.lo && a.hi == b.hi;
}

void jce_guid_to_string(JceGuid g, char out[JCE_GUID_STR_LEN])
{
    std::snprintf(out, JCE_GUID_STR_LEN, "%016llx%016llx",
                  (unsigned long long)g.hi, (unsigned long long)g.lo);
}

bool jce_guid_from_string(const char *s, JceGuid *out)
{
    if (!s || !out) return false;
    if (strlen(s) < 32) return false;
    char hi_buf[17], lo_buf[17];
    memcpy(hi_buf, s,      16); hi_buf[16] = '\0';
    memcpy(lo_buf, s + 16, 16); lo_buf[16] = '\0';
    char *endp = nullptr;
    out->hi = (uint64_t)strtoull(hi_buf, &endp, 16);
    if (endp != hi_buf + 16) return false;
    out->lo = (uint64_t)strtoull(lo_buf, &endp, 16);
    return endp == lo_buf + 16;
}

/* ── Registry ────────────────────────────────────────────────── */

JceGuid jce_assetdb_register(const char *path, JceGuid guid)
{
    int idx = find_or_create_index(path);
    if (idx < 0) {
        JceGuid z = { 0, 0 };
        return z;
    }
    Entry &e = s_entries[idx];
    if (jce_guid_is_zero(e.guid)) {
        if (jce_guid_is_zero(guid)) guid = jce_guid_new();
        e.guid = guid;
    }
    return e.guid;
}

JceGuid jce_assetdb_guid_for_path(const char *path)
{
    int idx = find_entry_index(path);
    if (idx < 0) {
        JceGuid z = { 0, 0 };
        return z;
    }
    return s_entries[idx].guid;
}

bool jce_assetdb_path_for_guid(JceGuid g, char *out_path, size_t cap)
{
    int idx = find_entry_index_by_guid(g);
    if (idx < 0) return false;
    strncpy(out_path, s_entries[idx].path, cap - 1);
    out_path[cap - 1] = '\0';
    return true;
}

bool jce_assetdb_forget(const char *path)
{
    int idx = find_entry_index(path);
    if (idx < 0) return false;
    JceGuid removed_guid = s_entries[idx].guid;
    s_entries[idx].used = false;
    s_entries[idx].dep_count = 0;
    s_entry_count--;
    /* Strip incoming edges from other entries. */
    for (uint32_t i = 0; i < MAX_ENTRIES; ++i) {
        if (!s_entries[i].used) continue;
        Entry &e = s_entries[i];
        for (uint16_t d = 0; d < e.dep_count;) {
            if (jce_guid_equal(e.deps[d], removed_guid)) {
                e.deps[d] = e.deps[e.dep_count - 1];
                e.dep_count--;
            } else {
                d++;
            }
        }
    }
    return true;
}

/* ── Dependency edges ────────────────────────────────────────── */

void jce_assetdb_add_dependency(JceGuid from, JceGuid to)
{
    if (jce_guid_is_zero(from) || jce_guid_is_zero(to)) return;
    int idx = find_entry_index_by_guid(from);
    if (idx < 0) return;
    Entry &e = s_entries[idx];
    for (uint16_t i = 0; i < e.dep_count; ++i)
        if (jce_guid_equal(e.deps[i], to)) return;
    if (e.dep_count >= MAX_DEPS_PER_ENTRY) return;
    e.deps[e.dep_count++] = to;
}

void jce_assetdb_remove_dependency(JceGuid from, JceGuid to)
{
    int idx = find_entry_index_by_guid(from);
    if (idx < 0) return;
    Entry &e = s_entries[idx];
    for (uint16_t i = 0; i < e.dep_count; ++i) {
        if (jce_guid_equal(e.deps[i], to)) {
            e.deps[i] = e.deps[e.dep_count - 1];
            e.dep_count--;
            return;
        }
    }
}

void jce_assetdb_clear_dependencies(JceGuid from)
{
    int idx = find_entry_index_by_guid(from);
    if (idx >= 0) s_entries[idx].dep_count = 0;
}

uint32_t jce_assetdb_dependants(JceGuid to, JceGuid *out, uint32_t cap)
{
    if (!out || cap == 0) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < MAX_ENTRIES && n < cap; ++i) {
        if (!s_entries[i].used) continue;
        const Entry &e = s_entries[i];
        for (uint16_t d = 0; d < e.dep_count; ++d) {
            if (jce_guid_equal(e.deps[d], to)) {
                out[n++] = e.guid;
                break;
            }
        }
    }
    return n;
}

uint32_t jce_assetdb_dependencies(JceGuid from, JceGuid *out, uint32_t cap)
{
    if (!out || cap == 0) return 0;
    int idx = find_entry_index_by_guid(from);
    if (idx < 0) return 0;
    const Entry &e = s_entries[idx];
    uint32_t n = e.dep_count < cap ? e.dep_count : cap;
    for (uint32_t i = 0; i < n; ++i) out[i] = e.deps[i];
    return n;
}

uint32_t jce_assetdb_entry_count(void) { return s_entry_count; }

uint32_t jce_assetdb_edge_count(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < MAX_ENTRIES; ++i)
        if (s_entries[i].used) n += s_entries[i].dep_count;
    return n;
}
