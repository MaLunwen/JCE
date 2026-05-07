/*
 * jce_editor_presets.cpp
 *
 * File format (little-endian):
 *   [0..15]  magic = "JCEPRESET\0\0\0\0\0\0\0"
 *   [16..23] uint64_t component flag
 *   [24..27] uint32_t struct size
 *   [28..]   raw struct bytes (length = struct size)
 */

#include "core/jce_editor_presets.h"
#include "core/jce_editor_state.h"
#include "ui/jce_editor_panels.h"

#include <jce/middleware/scene/jce_scene.h>
extern "C" {
#include <jce/os/core/jce_filesystem.h>
}

#include <algorithm>
#include <cstdio>
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

static std::string preset_dir(uint64_t flag)
{
    const char *root = jce_editor_assets_get_project();
    if (!root || !*root) return {};
    const char *cname = jce_comp_flag_display_name(flag);
    if (!cname) return {};
    std::string p = root;
    if (!p.empty() && p.back() != '/' && p.back() != '\\') p.push_back('/');
    p += "presets/";
    p += cname;
    return p;
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

static std::string preset_path(uint64_t flag, const char *name)
{
    std::string d = preset_dir(flag);
    if (d.empty() || !name || !*name) return {};
    std::string clean = sanitize_preset_name(name);
    if (clean.empty()) return {};
    d.push_back('/');
    d += clean;
    d += ".preset";
    return d;
}

bool jce_preset_save(uint64_t flag, const char *preset_name,
                     JceScene *scene, JceEntity e)
{
    if (!scene || !preset_name || !*preset_name) return false;

    size_t sz = 0;
    void *blob = jce_inspector_comp_blob(scene, e, flag, &sz);
    if (!blob || sz == 0) return false;

    std::string file = preset_path(flag, preset_name);
    if (file.empty()) return false;
    std::string dir = preset_dir(flag);
    if (dir.empty()) return false;
    if (!jce_fs_host_create_directory(dir.c_str())) return false;

    /* Build header + payload in a single contiguous buffer so we can use
       jce_fs_host_write_all (single truncate-and-replace write). */
    PresetHeader h{};
    memcpy(h.magic, kMagic, sizeof(kMagic));
    h.flag = flag;
    h.size = (uint32_t)sz;

    std::vector<uint8_t> buf(sizeof(h) + sz);
    memcpy(buf.data(), &h, sizeof(h));
    memcpy(buf.data() + sizeof(h), blob, sz);
    return jce_fs_host_write_all(file.c_str(), buf.data(), buf.size());
}

bool jce_preset_apply(uint64_t flag, const char *preset_name,
                      JceScene *scene, JceEntity e)
{
    if (!scene || !preset_name || !*preset_name) return false;

    std::string file = preset_path(flag, preset_name);
    if (file.empty() || !jce_fs_host_exists_file(file.c_str())) return false;

    uint64_t fsz = 0;
    void *raw = jce_fs_host_read_all(file.c_str(), &fsz);
    if (!raw) return false;
    if (fsz < sizeof(PresetHeader)) { jce_fs_buffer_free(raw); return false; }

    PresetHeader h{};
    memcpy(&h, raw, sizeof(h));
    if (memcmp(h.magic, kMagic, sizeof(kMagic)) != 0) { jce_fs_buffer_free(raw); return false; }
    if (h.flag != flag)                                { jce_fs_buffer_free(raw); return false; }
    if (fsz < (uint64_t)sizeof(h) + h.size)            { jce_fs_buffer_free(raw); return false; }

    size_t sz = 0;
    void *blob = jce_inspector_comp_blob(scene, e, flag, &sz);
    if (!blob || sz == 0 || h.size != sz)              { jce_fs_buffer_free(raw); return false; }

    jce_state_begin_batch_edit();
    memcpy(blob, (const uint8_t *)raw + sizeof(h), sz);
    jce_state_end_batch_edit();
    jce_fs_buffer_free(raw);
    return true;
}

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

std::vector<std::string> jce_preset_list(uint64_t flag)
{
    std::vector<std::string> out;
    std::string d = preset_dir(flag);
    if (d.empty() || !jce_fs_host_exists_dir(d.c_str())) return out;
    ListCtx ctx{ &out };
    jce_fs_host_list_dir(d.c_str(), list_cb, &ctx);
    std::sort(out.begin(), out.end());
    return out;
}

bool jce_preset_delete(uint64_t flag, const char *preset_name)
{
    std::string file = preset_path(flag, preset_name);
    if (file.empty()) return false;
    return jce_fs_host_remove_file(file.c_str());
}
