/* Phase observation through public SDK loaders; not a second asset loader. */
#include "load_witness.h"
#include <jce/api_core.h>
#include <jce/api_resource.h>
#include <jce/api_graphics.h>
#include <stdlib.h>
#include <string.h>

bool bench_load_prepare(bool *requested)
{
    const char *project = getenv("JCE_BENCH_LOAD_PREPARE_ROOT");
    const char *output = getenv("JCE_BENCH_LOAD_PREPARE_OUT");
    *requested = project && project[0];
    if (!*requested) return true;
    if (!output || !output[0]) return false;
    JceBundlePackOptions options = {0};
    options.project_root = project;
    options.out_dir = output;
    options.zstd_level = 3;
    const int result = jce_bundle_pack_run(&options, NULL, NULL);
    LOG_INFO("benchmark_load", "benchmark-load-prepare: ok=%u result=%d", (unsigned)(result == 0), result);
    return result == 0;
}

void bench_load_destroy(BenchLoadWitness *state)
{
    if (!state->active) return;
    if (state->model) jce_model_destroy(state->model);
    if (jce_texture_valid(state->texture)) jce_texture_destroy(state->texture);
    if (state->sound != JCE_SOUND_INVALID) jce_audio_unload(state->audio, state->sound);
    memset(state, 0, sizeof(*state));
}

bool bench_load_kind_valid(const char *kind)
{
    return kind && (!strcmp(kind, "glb") || !strcmp(kind, "texture") ||
                   !strcmp(kind, "audio") || !strcmp(kind, "bundle"));
}

void bench_load_cpu_destroy(BenchLoadCpuPhase *phase)
{
    if (phase->model) jce_model_gltf_cpu_free(phase->model);
    if (phase->texture) jce_texture_cpu_free(phase->texture);
    if (phase->audio) jce_audio_cpu_free(phase->audio);
    jce_free(phase->unpacked);
    if (phase->bundle) jce_bundle_file_close(phase->bundle);
    if (phase->fs) jce_fs_destroy(phase->fs);
    jce_free(phase->encoded);
    memset(phase, 0, sizeof(*phase));
}

bool bench_load_cpu_decode(BenchLoadCpuPhase *phase, const char *kind,
                           const char *path, const char *entry_path)
{
    if (!bench_load_kind_valid(kind) || !path || !path[0]) return false;
    uint64_t started = jce_time_ticks_ns();
    phase->encoded = jce_fs_host_read_all(path, &phase->read_bytes);
    uint64_t read_done = jce_time_ticks_ns();
    uint64_t opened = read_done, decompressed = read_done;
    phase->payload_bytes = phase->read_bytes;
    bool ok = phase->encoded && phase->read_bytes > 0 && phase->read_bytes <= SIZE_MAX;
    if (ok && !strcmp(kind, "bundle")) {
        phase->fs = jce_fs_create();
        if (phase->fs) phase->bundle = jce_bundle_file_open_memory(phase->fs,
            phase->encoded, (size_t)phase->read_bytes, "load-witness");
        opened = jce_time_ticks_ns();
        JcePakArchive *pak = phase->bundle ? jce_bundle_file_pak(phase->bundle) : NULL;
        const JcePakAsset *entry = pak && entry_path && entry_path[0] ?
            jce_pak_find(pak, entry_path) : NULL;
        ok = entry && entry->original_size > 0 && entry->original_size <= SIZE_MAX;
        if (ok) {
            phase->payload_bytes = entry->original_size;
            phase->packed_bytes = entry->compressed_size;
            phase->unpacked = jce_malloc((size_t)phase->payload_bytes);
            ok = phase->unpacked && jce_pak_decompress_ex(pak, entry, phase->unpacked,
                (size_t)phase->payload_bytes) == phase->payload_bytes &&
                jce_pak_verify(entry, phase->unpacked, (size_t)phase->payload_bytes) != 0;
        }
        decompressed = jce_time_ticks_ns();
    }
    const void *payload = phase->unpacked ? phase->unpacked : phase->encoded;
    if (ok && (!strcmp(kind, "glb") || !strcmp(kind, "bundle"))) {
        ok = phase->payload_bytes <= UINT32_MAX;
        if (ok) phase->model = jce_model_decode_gltf_cpu_memory(payload,
            (uint32_t)phase->payload_bytes, !strcmp(kind, "bundle") ? entry_path : path);
        ok = phase->model != NULL;
    } else if (ok && !strcmp(kind, "texture")) {
        phase->texture = jce_texture_decode_cpu_mem(payload,
            (size_t)phase->payload_bytes, JCE_TEX_CLAMP);
        ok = phase->texture != NULL;
    } else if (ok && !strcmp(kind, "audio")) {
        phase->audio = jce_audio_decode_cpu_memory(payload, (size_t)phase->payload_bytes, path);
        ok = phase->audio != NULL;
    }
    phase->decoded_at = jce_time_ticks_ns();
    phase->read_ns = read_done - started;
    phase->open_ns = opened - read_done;
    phase->decompress_ns = decompressed - opened;
    phase->decode_ns = phase->decoded_at - decompressed;
    phase->ok = ok;
    return ok;
}

bool bench_load_submit(BenchLoadWitness *state, BenchLoadCpuPhase *phase,
                       const JceServices *services, bool synchronous)
{
    const uint64_t started = synchronous ? phase->decoded_at : jce_time_ticks_ns();
    state->active = true;
    state->texture = JCE_TEXTURE_INVALID;
    state->audio = services->audio;
    bool ok = phase->ok && jce_thread_is_main();
    if (ok && phase->model) {
        state->model = jce_model_upload_gltf_cpu(phase->model);
        phase->model = NULL; /* Upload consumes even on failure. */
        ok = state->model != NULL;
    }
    if (ok && phase->texture) {
        state->texture = jce_texture_upload_cpu(phase->texture);
        phase->texture = NULL;
        ok = jce_texture_valid(state->texture);
    }
    if (ok && phase->audio) {
        state->sound = jce_audio_upload_cpu(services->audio, phase->audio);
        phase->audio = NULL;
        ok = state->sound != JCE_SOUND_INVALID;
    }
    phase->submit_ns = jce_time_ticks_ns() - started;
    return ok;
}

void bench_load_phase_log(const BenchLoadCpuPhase *phase, const char *kind,
                           unsigned frame, bool ok)
{
    uint64_t total = phase->read_ns + phase->open_ns + phase->decompress_ns +
                     phase->decode_ns + phase->submit_ns;
    LOG_INFO("benchmark_load", "benchmark-load: frame=%u kind=%s ok=%u read_bytes=%llu packed_entry_bytes=%llu payload_bytes=%llu read_ns=%llu open_ns=%llu decompress_verify_ns=%llu decode_ns=%llu submit_ns=%llu total_ns=%llu",
        frame, kind, (unsigned)ok, (unsigned long long)phase->read_bytes,
        (unsigned long long)phase->packed_bytes, (unsigned long long)phase->payload_bytes,
        (unsigned long long)phase->read_ns, (unsigned long long)phase->open_ns,
        (unsigned long long)phase->decompress_ns, (unsigned long long)phase->decode_ns,
        (unsigned long long)phase->submit_ns, (unsigned long long)total);
}

bool bench_load_witness(BenchLoadWitness *state, const JceServices *services,
                        unsigned frame)
{
    const char *kind = getenv("JCE_BENCH_LOAD_KIND");
    if (getenv("JCE_BENCH_LOAD_ASYNC")) return true;
    if (!kind || !kind[0] || frame != 20 || state->active) return true;
    state->active = true;
    state->texture = JCE_TEXTURE_INVALID;
    state->audio = services->audio;
    BenchLoadCpuPhase phase = {0};
    bool ok = bench_load_cpu_decode(&phase, kind, getenv("JCE_BENCH_LOAD_FILE"),
                                    getenv("JCE_BENCH_LOAD_ENTRY"));
    if (ok) ok = bench_load_submit(state, &phase, services, true);
    bench_load_phase_log(&phase, kind, frame, ok);
    bench_load_cpu_destroy(&phase);
    return ok;
}
