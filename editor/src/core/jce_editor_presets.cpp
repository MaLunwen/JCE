/*
 * jce_editor_presets.cpp
 *
 * File format (little-endian):
 *   [0..15]  magic = "JCEPRESET\0\0\0\0\0\0\0"
 *   [16..23] uint64_t legacy component flag (0 for post-64-bit components)
 *   [24..27] uint32_t struct size
 *   [28..]   raw struct bytes (length = struct size)
 *
 * Identity is the directory name (the canonical registry component name);
 * the header flag is a belt-and-braces check kept for back-compat with
 * files written by the pre-registry editor (which stored either the
 * component's JCE_COMP_FLAG_* bit or a synthetic editor slot constant).
 */

#include "core/jce_editor_presets.h"
#include "core/jce_editor_component_registry.h"
#include "core/jce_editor_state.h"

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_component_registry.h>
extern "C" {
#include <jce/os/core/jce_filesystem.h>
}

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

extern "C" const char *jce_editor_assets_get_project(void);

static const char kMagic[16] = { 'J','C','E','P','R','E','S','E','T',0,0,0,0,0,0,0 };

struct PresetHeader {
    char     magic[16];
    uint64_t flag;
    uint32_t size;
};

/* ── Legacy-read migration table ──────────────────────────────────────
 * Pre-registry preset headers stored the editor's synthetic slot value
 * (the UINT64_MAX-n workaround for the exhausted 64-bit flag space) for
 * components that had no JCE_COMP_FLAG_* bit.  These raw values are the
 * ONLY remaining trace of the retired JCE_EDITOR_COMP_SLOT_* constants —
 * kept solely so those old files keep validating. */
static const struct { uint64_t slot; const char *engine_name; } kLegacySlotNames[] = {
    { UINT64_MAX - UINT64_C(1), "CompoundCollider" },
    { UINT64_MAX - UINT64_C(2), "VideoPlayer" },
    { UINT64_MAX - UINT64_C(3), "NavAgent" },
    { UINT64_MAX - UINT64_C(4), "IkConstraints" },
    { UINT64_MAX - UINT64_C(5), "SequencePlayer" },
};

/* Accept a header flag when it matches the component's identity in any of
 * the formats ever written: 0 (post-64 rows, current format), the legacy
 * JCE_COMP_FLAG_* bit (old files for flag-backed rows), or a synthetic
 * editor slot constant (old files for former synthetic rows). */
static bool header_flag_matches(uint64_t h_flag, int comp_id)
{
    if (h_flag == 0) return true;
    if (h_flag == jce_component_legacy_flag(comp_id)) return true;
    for (const auto &m : kLegacySlotNames) {
        if (m.slot == h_flag)
            return jce_component_find(m.engine_name) == comp_id;
    }
    return false;
}

/* ── Directory / path resolution ──────────────────────────────────── */

static std::string preset_dir_for_name(const char *cname)
{
    const char *root = jce_editor_assets_get_project();
    if (!root || !*root || !cname || !*cname) return {};
    std::string p = root;
    if (!p.empty() && p.back() != '/' && p.back() != '\\') p.push_back('/');
    p += "presets/";
    p += cname;
    return p;
}

/* Canonical directory: the engine registry name ("MeshRenderer"). */
static std::string preset_dir(int comp_id)
{
    return preset_dir_for_name(jce_component_name(comp_id));
}

/* Pre-registry directory: presets used to be filed under the editor
 * display name ("Mesh Renderer", jce_comp_flag_display_name).  Read-only
 * fallback so existing user presets keep loading. */
static std::string preset_dir_legacy(int comp_id)
{
    const JceEditorComponentDescriptor *d =
        jce_editor_component_find_by_id(comp_id);
    return d ? preset_dir_for_name(d->display_name) : std::string();
}

/* Sanitize a user-supplied preset name into a portable filename. Replaces
 * any character not in [A-Za-z0-9_- .] with '_'; collapses leading/trailing
 * whitespace and dots so we cannot produce "." / ".." / hidden files. */
static std::string sanitize_preset_name(const char *raw)
{
    if (!raw) return {};
    std::string s;
    s.reserve(strlen(raw));
    for (const char *p = raw; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                  || (c >= '0' && c <= '9')
                  || c == '_' || c == '-' || c == ' ' || c == '.';
        s.push_back(ok ? (char)c : '_');
    }
    while (!s.empty() && (s.front() == ' ' || s.front() == '.')) s.erase(s.begin());
    while (!s.empty() && (s.back()  == ' ' || s.back()  == '.')) s.pop_back();
    return s;
}

static std::string preset_path_in(const std::string &dir, const char *name)
{
    if (dir.empty() || !name || !*name) return {};
    std::string clean = sanitize_preset_name(name);
    if (clean.empty()) return {};
    std::string p = dir;
    p.push_back('/');
    p += clean;
    p += ".preset";
    return p;
}

/* Resolve a preset for reading: canonical dir first, then the legacy
 * display-name dir. Empty when neither file exists. */
static std::string preset_read_path(int comp_id, const char *name)
{
    std::string p = preset_path_in(preset_dir(comp_id), name);
    if (!p.empty() && jce_fs_host_exists_file(p.c_str())) return p;
    std::string l = preset_path_in(preset_dir_legacy(comp_id), name);
    if (!l.empty() && jce_fs_host_exists_file(l.c_str())) return l;
    return {};
}

/* ── Save / apply ─────────────────────────────────────────────────── */

bool jce_preset_save(int comp_id, const char *preset_name,
                     JceScene *scene, JceEntity e)
{
    if (!scene || comp_id == JCE_COMP_ID_INVALID
        || !preset_name || !*preset_name) return false;

    uint32_t sz = 0;
    void *blob = jce_scene_get_comp(scene, e, comp_id, &sz);
    if (!blob || sz == 0) return false;

    std::string dir = preset_dir(comp_id);
    std::string file = preset_path_in(dir, preset_name);
    if (dir.empty() || file.empty()) return false;
    if (!jce_fs_host_create_directory(dir.c_str())) return false;

    /* Build header + payload in a single contiguous buffer so we can use
       jce_fs_host_write_all (single truncate-and-replace write). */
    PresetHeader h{};
    memcpy(h.magic, kMagic, sizeof(kMagic));
    h.flag = jce_component_legacy_flag(comp_id); /* 0 for post-64 rows */
    h.size = sz;

    std::vector<uint8_t> buf(sizeof(h) + sz);
    memcpy(buf.data(), &h, sizeof(h));
    memcpy(buf.data() + sizeof(h), blob, sz);
    return jce_fs_host_write_all(file.c_str(), buf.data(), buf.size());
}

bool jce_preset_apply(int comp_id, const char *preset_name,
                      JceScene *scene, JceEntity e)
{
    if (!scene || comp_id == JCE_COMP_ID_INVALID
        || !preset_name || !*preset_name) return false;

    std::string file = preset_read_path(comp_id, preset_name);
    if (file.empty()) return false;

    uint64_t fsz = 0;
    void *raw = jce_fs_host_read_all(file.c_str(), &fsz);
    if (!raw) return false;
    if (fsz < sizeof(PresetHeader)) { jce_fs_buffer_free(raw); return false; }

    PresetHeader h{};
    memcpy(&h, raw, sizeof(h));
    if (memcmp(h.magic, kMagic, sizeof(kMagic)) != 0) { jce_fs_buffer_free(raw); return false; }
    if (!header_flag_matches(h.flag, comp_id))         { jce_fs_buffer_free(raw); return false; }
    if (fsz < (uint64_t)sizeof(h) + h.size)            { jce_fs_buffer_free(raw); return false; }

    /* Only apply onto an existing component of the exact same size — the
       registry struct_size is the authoritative byte count. */
    uint32_t sz = 0;
    void *blob = jce_scene_get_comp(scene, e, comp_id, &sz);
    if (!blob || sz == 0 || h.size != sz)              { jce_fs_buffer_free(raw); return false; }

    jce_state_begin_batch_edit();
    bool ok = jce_scene_set_comp(scene, e, comp_id,
                                 (const uint8_t *)raw + sizeof(h));
    jce_state_end_batch_edit();
    jce_fs_buffer_free(raw);
    return ok;
}

/* ── List / delete ────────────────────────────────────────────────── */

namespace {
struct ListCtx {
    std::vector<std::string> *out;
};
bool list_cb(const char *name, bool is_dir, void *user)
{
    if (is_dir || !name) return true;
    size_t n = strlen(name);
    const char ext[] = ".preset";
    const size_t elen = sizeof(ext) - 1;
    if (n <= elen) return true;
    if (memcmp(name + n - elen, ext, elen) != 0) return true;
    auto *ctx = static_cast<ListCtx *>(user);
    ctx->out->emplace_back(name, n - elen);
    return true;
}
} /* anonymous namespace */

std::vector<std::string> jce_preset_list(int comp_id)
{
    std::vector<std::string> out;
    if (comp_id == JCE_COMP_ID_INVALID) return out;
    ListCtx ctx{ &out };
    /* Union of the canonical dir and the pre-registry display-name dir
       (deduped) so presets saved before the migration stay visible. */
    std::string d = preset_dir(comp_id);
    if (!d.empty() && jce_fs_host_exists_dir(d.c_str()))
        jce_fs_host_list_dir(d.c_str(), list_cb, &ctx);
    std::string l = preset_dir_legacy(comp_id);
    if (!l.empty() && l != d && jce_fs_host_exists_dir(l.c_str()))
        jce_fs_host_list_dir(l.c_str(), list_cb, &ctx);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

bool jce_preset_delete(int comp_id, const char *preset_name)
{
    if (comp_id == JCE_COMP_ID_INVALID) return false;
    bool removed = false;
    std::string f = preset_path_in(preset_dir(comp_id), preset_name);
    if (!f.empty() && jce_fs_host_exists_file(f.c_str()))
        removed = jce_fs_host_remove_file(f.c_str()) || removed;
    std::string l = preset_path_in(preset_dir_legacy(comp_id), preset_name);
    if (!l.empty() && l != f && jce_fs_host_exists_file(l.c_str()))
        removed = jce_fs_host_remove_file(l.c_str()) || removed;
    return removed;
}
