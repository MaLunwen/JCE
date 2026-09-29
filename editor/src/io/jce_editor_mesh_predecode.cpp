/*
 * jce_editor_mesh_predecode.cpp — see the header for why this exists and what
 * it measured.
 */

#include "io/jce_editor_mesh_predecode.h"

#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_thread.h>

#include <string>
#include <unordered_map>

#define LOG_TAG "editor.predecode"

namespace {

/* One scene's distinct glTF models.  hidden_cove, the scene this was measured
 * on, has 40; the cap is a backstop against a pathological scene holding CPU
 * mesh data for thousands of models, not a tuning knob. */
constexpr size_t kMaxEntries = 512;

std::unordered_map<std::string, JceModelCpu *> g_store;
int g_hits = 0, g_misses = 0, g_stored = 0;
JceMutex *g_mu = nullptr;

/* Created on first use rather than at static-init: the engine's thread layer
 * is not up when this TU's statics are constructed. */
JceMutex *mu()
{
    if (!g_mu) g_mu = jce_mutex_create();
    return g_mu;
}

}  // namespace

extern "C" void jce_editor_mesh_predecode_reset(void)
{
    JceMutex *m = mu();
    if (m) jce_mutex_lock(m);
    size_t n = g_store.size();
    for (auto &kv : g_store) {
        if (kv.second) jce_model_gltf_cpu_free(kv.second);
    }
    g_store.clear();
    if (m) jce_mutex_unlock(m);
    if (n)
        LOG_INFO(LOG_TAG, "dropped %zu unclaimed pre-decoded mesh(es)", n);
}

extern "C" bool jce_editor_mesh_predecode_put(const char *key, JceModelCpu *cpu)
{
    if (!key || !key[0] || !cpu) return false;
    JceMutex *m = mu();
    if (m) jce_mutex_lock(m);
    bool taken = false;
    if (g_store.size() < kMaxEntries &&
        g_store.find(key) == g_store.end()) {
        g_store.emplace(key, cpu);
        taken = true;
        g_stored++;
    }
    if (m) jce_mutex_unlock(m);
    return taken;
}

extern "C" JceModelCpu *jce_editor_mesh_predecode_take(const char *key)
{
    if (!key || !key[0]) return nullptr;
    JceMutex *m = mu();
    if (m) jce_mutex_lock(m);
    JceModelCpu *out = nullptr;
    auto it = g_store.find(key);
    if (it != g_store.end()) {
        out = it->second;
        g_store.erase(it);
    }
    if (out) g_hits++; else g_misses++;
    if (m) jce_mutex_unlock(m);
    return out;
}

extern "C" void jce_editor_mesh_predecode_report(void)
{
    double decode_ms = 0.0, blocked_ms = 0.0;
    jce_editor_mesh_predecode_timings(&decode_ms, &blocked_ms);
    LOG_INFO(LOG_TAG, "pre-decoded mesh store: %d stored, %d claimed, "
             "%d missed (decoded on the main thread instead), %d still "
             "pending; decode %.0f ms on workers, main thread blocked %.0f ms",
             g_stored, g_hits, g_misses, jce_editor_mesh_predecode_pending(),
             decode_ms, blocked_ms);
}

extern "C" int jce_editor_mesh_predecode_pending(void)
{
    JceMutex *m = mu();
    if (m) jce_mutex_lock(m);
    int n = (int)g_store.size();
    if (m) jce_mutex_unlock(m);
    return n;
}
