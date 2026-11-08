/* jce_shaders.c
 *
 * Runtime bgfx shader loading from a PAK archive.
 * Decompresses .bin blobs, feeds them to bgfx_create_shader().
 */

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/os/core/jce_trace.h>
#include <jce/resource/jce_pak_loader.h>
#include <jce/renderer/jce_shaders.h>
#include <jce/renderer/jce_renderer_caps.h>
#include <jce/renderer/jce_renderer.h>

#include "os/core/jce_memory.h"
#include "os/core/jce_trace_scope_internal.h"
#include "os/core/jce_trace_capture_internal.h"
#include "os/core/jce_sha256_internal.h"
#include "jce_renderer_lifecycle_internal.h"
#include "jce_shader_deferred.h"
#include "jce_shader_set_owned_internal.h"

#include <bgfx/c99/bgfx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "jce_shaders"

/* -- Embedded engine-shader PAK fallback ----------------------------- *
 * The engine static library bundles all stock shader bins (vs_color,
 * fs_pbr, …) for every backend (dx11, spv, glsl, essl, mtl).  When the
 * user PAK is NULL — or doesn't contain a particular shader — we fall
 * back to this embedded archive so a freshly-scaffolded SDK consumer
 * project still renders without shipping a separate engine_shaders.pak.
 *
 * The symbols `jce_engine_shaders_pak_data[]` /
 * `jce_engine_shaders_pak_size` are emitted at build time by
 * `tools/jce_pak --symbol-prefix jce_engine_shaders_pak`.
 *
 * When the engine is built without the embedded shader pack
 * (`-DJCE_EMBED_ENGINE_SHADERS=OFF`), a 1-byte stub keeps the link
 * happy and the fallback becomes a no-op.
 */
extern const unsigned char jce_engine_shaders_pak[];
extern const size_t        jce_engine_shaders_pak_size;

static const JcePakArchive *embedded_pak(void)
{
    static const JcePakArchive *cached = NULL;
    static int                  tried  = 0;
    if (!tried) {
        tried = 1;
        if (jce_engine_shaders_pak_size > 1) {
            cached = jce_pak_open(jce_engine_shaders_pak,
                                  jce_engine_shaders_pak_size);
            if (!cached) {
                LOG_WARN(LOG_TAG,
                         "embedded engine shader pak failed to open "
                         "(size=%zu)", (size_t)jce_engine_shaders_pak_size);
            }
        }
    }
    return cached;
}

/* Public accessor so other engine subsystems (decals, GPU particles) can use
 * the same embedded-engine-shader fallback as load_single(). */
const JcePakArchive *jce_shaders_embedded_engine_pak(void)
{
    return embedded_pak();
}

/* Map bgfx renderer type to the file suffix produced by shaderc. */
static const char *shader_suffix(bgfx_renderer_type_t type)
{
    switch (type) {
    case BGFX_RENDERER_TYPE_DIRECT3D11:
    case BGFX_RENDERER_TYPE_DIRECT3D12:
        return "dx11";
    case BGFX_RENDERER_TYPE_VULKAN:
        return "spv";
    case BGFX_RENDERER_TYPE_OPENGL:
        return "glsl";
    case BGFX_RENDERER_TYPE_OPENGLES:
        return "essl";
    case BGFX_RENDERER_TYPE_METAL:
        return "mtl";
    default:
        return NULL;
    }
}

/* WebGL must not link unused shader families at the first frame. Capture the
 * source bytes now (no borrowed PAK/overlay lifetime), and create the GPU
 * program only at submission. Native platforms retain eager loading. */
static void deferred_source_free(JceDeferredSource *src)
{
    if (src->data) {
        if (src->host) jce_fs_buffer_free(src->data);
        else JCE_FREE(src->data);
    }
    memset(src, 0, sizeof *src);
}

/* bgfx may reject an interface mismatch before its destroyShaders handling.
 * Each successful create_shader owns a creator reference; program references
 * are independent. Release creators explicitly on both link outcomes. */
static bgfx_program_handle_t link_program_owned(bgfx_shader_handle_t vsh,
                                                bgfx_shader_handle_t fsh)
{
    JceTraceScope scope = jce_trace_scope_begin("renderer.shader.link");
    bgfx_program_handle_t program = { UINT16_MAX };
    unsigned released = 0;
    const bool stages_valid = vsh.idx != UINT16_MAX && fsh.idx != UINT16_MAX;
    if (stages_valid) program = bgfx_create_program(vsh, fsh, false);
    if (vsh.idx != UINT16_MAX) { bgfx_destroy_shader(vsh); ++released; }
    if (fsh.idx != UINT16_MAX) { bgfx_destroy_shader(fsh); ++released; }
    const bool succeeded = program.idx != UINT16_MAX;
    if (scope.id) {
#define LINK_COUNTER(name, value) jce_trace_counter_scoped_internal( \
    JCE_TRACE_CATEGORY_RENDER, name, (int64_t)(value), scope.id, scope.parent)
        LINK_COUNTER("shader.link.vs", vsh.idx);
        LINK_COUNTER("shader.link.fs", fsh.idx);
        LINK_COUNTER("shader.link.program", program.idx);
        LINK_COUNTER("shader.link.creator_releases", released);
        LINK_COUNTER("shader.link.success", succeeded);
        LINK_COUNTER("shader.link.rejection", succeeded ? 0 : stages_valid ? 2 : 1);
#undef LINK_COUNTER
    }
    jce_trace_scope_end(scope, succeeded ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_FAILED);
    return program;
}

/* Synchronous eager-load evidence only. Stack receipts describe individual
 * API acquisitions, never a global handle incarnation or GPU execution. The
 * existing Web deferred owner continues to use link_program_owned unchanged. */
typedef struct JceShaderAcquireEvidence {
    uint64_t create_id;
    uint64_t context_generation;
    uint32_t capture_generation;
    uint16_t handle;
    bool context_known;
} JceShaderAcquireEvidence;

static JceTraceScope shader_provenance_begin(const char *name)
{
    if (!jce_renderer_shader_provenance_policy_internal())
        return (JceTraceScope){0};
    return jce_trace_scope_begin(name);
}

static void shader_provenance_counter(JceTraceScope scope, const char *name,
                                      uint32_t value)
{
    jce_trace_counter_ring_internal(JCE_TRACE_CATEGORY_RENDER, name, value,
                                    scope.id, scope.parent);
}

static bgfx_shader_handle_t create_shader_evidenced(const bgfx_memory_t *mem,
    JceShaderAcquireEvidence *evidence)
{
    if (evidence) *evidence = (JceShaderAcquireEvidence){0};
    const JceTraceScope scope = shader_provenance_begin("renderer.shader.create");
    if (!scope.id) return bgfx_create_shader(mem);

    const uint32_t generation = jce_trace_capture_generation();
    uint64_t context = 0;
    uint32_t host_frame = 0;
    const bool context_known = jce_renderer_gpu_observation_context_internal(&context, &host_frame);
    const bool payload_known = mem && mem->data && mem->size;
    const uint32_t size = mem ? mem->size : 0;
    uint8_t digest[32] = {0};
    /* Hash the exact container before bgfx consumes/releases its Memory. */
    if (payload_known) jce_sha256_internal(mem->data, size, digest);
    const bgfx_shader_handle_t shader = bgfx_create_shader(mem);
    /* Capture changes require producer quiescence. Never stamp old evidence
     * into a new capture if a caller violates that contract during the API. */
    if (!jce_trace_enabled() || generation != jce_trace_capture_generation()) {
        jce_trace_task_restore_id(scope.restore);
        return shader;
    }
    shader_provenance_counter(scope, "shader.create.capture", generation);
    shader_provenance_counter(scope, "shader.create.context_known", context_known);
    shader_provenance_counter(scope, "shader.create.context_hi", (uint32_t)(context >> 32));
    shader_provenance_counter(scope, "shader.create.context_lo", (uint32_t)context);
    shader_provenance_counter(scope, "shader.create.bytes", size);
    shader_provenance_counter(scope, "shader.create.payload_known", payload_known);
    static const char *const words[8] = {
        "shader.create.sha256.0", "shader.create.sha256.1", "shader.create.sha256.2", "shader.create.sha256.3",
        "shader.create.sha256.4", "shader.create.sha256.5", "shader.create.sha256.6", "shader.create.sha256.7"
    };
    for (unsigned i = 0; i < 8; ++i) {
        const uint8_t *p = digest + 4*i;
        const uint32_t word = (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 |
                              (uint32_t)p[2]<<8 | p[3];
        shader_provenance_counter(scope, words[i], word); /* Digest-order big endian. */
    }
    shader_provenance_counter(scope, "shader.create.handle", shader.idx);
    shader_provenance_counter(scope, "shader.create.cpu_returned", 1);
    if (evidence && payload_known) {
        evidence->create_id = scope.id;
        evidence->capture_generation = generation;
        evidence->context_generation = context;
        evidence->context_known = context_known;
        evidence->handle = shader.idx;
    }
    jce_trace_scope_end(scope, shader.idx != UINT16_MAX ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_FAILED);
    return shader;
}

/* 0 available, 1 absent, 2 capture changed, 3 wrong returned handle,
 * 4 unavailable/changed device context. These are CPU evidence qualifiers. */
static unsigned shader_evidence_reason(const JceShaderAcquireEvidence *evidence,
                                        bgfx_shader_handle_t shader)
{
    if (!evidence || !evidence->create_id) return 1;
    if (evidence->capture_generation != jce_trace_capture_generation()) return 2;
    if (evidence->handle != shader.idx) return 3;
    uint64_t context = 0;
    uint32_t frame = 0;
    if (!evidence->context_known ||
        !jce_renderer_gpu_observation_context_internal(&context, &frame) ||
        context != evidence->context_generation) return 4;
    return 0;
}

static bgfx_program_handle_t link_program_evidenced(bgfx_shader_handle_t vsh,
    bgfx_shader_handle_t fsh, const JceShaderAcquireEvidence *vs,
    const JceShaderAcquireEvidence *fs)
{
    const JceTraceScope scope = shader_provenance_begin("renderer.shader.load_link");
    if (!scope.id) return link_program_owned(vsh, fsh);
    const uint32_t generation = jce_trace_capture_generation();
    const bgfx_program_handle_t program = link_program_owned(vsh, fsh);
    if (!jce_trace_enabled() || generation != jce_trace_capture_generation()) {
        jce_trace_task_restore_id(scope.restore);
        return program;
    }
    /* The unchanged child owner records the actual program return and both
     * explicit creator-release calls. No inferred implicit releases here. */
    const unsigned vs_reason = shader_evidence_reason(vs, vsh);
    const unsigned fs_reason = shader_evidence_reason(fs, fsh);
    const uint64_t vs_id = vs_reason ? 0 : vs->create_id;
    const uint64_t fs_id = fs_reason ? 0 : fs->create_id;
    shader_provenance_counter(scope, "shader.owner.capture", generation);
    shader_provenance_counter(scope, "shader.owner.vs_create_hi", (uint32_t)(vs_id >> 32));
    shader_provenance_counter(scope, "shader.owner.vs_create_lo", (uint32_t)vs_id);
    shader_provenance_counter(scope, "shader.owner.fs_create_hi", (uint32_t)(fs_id >> 32));
    shader_provenance_counter(scope, "shader.owner.fs_create_lo", (uint32_t)fs_id);
    shader_provenance_counter(scope, "shader.owner.vs_reason", vs_reason);
    shader_provenance_counter(scope, "shader.owner.fs_reason", fs_reason);
    shader_provenance_counter(scope, "shader.owner.cpu_returned", 1);
    jce_trace_scope_end(scope, program.idx != UINT16_MAX ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_FAILED);
    return program;
}

static void release_shader_evidenced(bgfx_shader_handle_t shader,
                                      const JceShaderAcquireEvidence *evidence)
{
    const JceTraceScope scope = shader_provenance_begin("renderer.shader.creator_release");
    const uint32_t generation = scope.id ? jce_trace_capture_generation() : 0;
    bgfx_destroy_shader(shader);
    if (!scope.id) return;
    if (!jce_trace_enabled() || generation != jce_trace_capture_generation()) {
        jce_trace_task_restore_id(scope.restore);
        return;
    }
    const unsigned reason = shader_evidence_reason(evidence, shader);
    const uint64_t create_id = reason ? 0 : evidence->create_id;
    shader_provenance_counter(scope, "shader.release.capture", generation);
    shader_provenance_counter(scope, "shader.release.create_hi", (uint32_t)(create_id >> 32));
    shader_provenance_counter(scope, "shader.release.create_lo", (uint32_t)create_id);
    shader_provenance_counter(scope, "shader.release.reason", reason);
    shader_provenance_counter(scope, "shader.release.handle", shader.idx);
    shader_provenance_counter(scope, "shader.release.cpu_returned", 1);
    jce_trace_scope_end(scope, JCE_TRACE_TASK_SUCCEEDED);
}

static JceDeferredSource deferred_source_read(const JcePakArchive *pak,
    const char *dev, const char *stage, const char *base, bool strict_fs)
{
    JceDeferredSource src = {0};
    const char *suffix = shader_suffix(bgfx_get_renderer_type());
    if (!suffix) return src;
    char path[256], full[1536];
    if (snprintf(path, sizeof path, "shaders/%s_%s_%s.bin", stage, base, suffix)
        >= (int)sizeof path) return src;
    if (dev && dev[0]) {
        if (snprintf(full, sizeof full, "%s/%s", dev, path) >= (int)sizeof full)
            return src;
        if (strict_fs || jce_fs_host_exists_file(full)) {
            uint64_t size = 0;
            src.data = jce_fs_host_read_all(full, &size);
            src.host = true;
            if (!src.data || !size || size > UINT32_MAX) {
                deferred_source_free(&src);
                return src;
            }
            src.size = (uint32_t)size;
            return src;
        }
    }
    if (strict_fs) return src;
    const JcePakAsset *asset = pak ? jce_pak_find(pak, path) : NULL;
    if (!asset) {
        const JcePakArchive *fallback = embedded_pak();
        asset = fallback ? jce_pak_find(fallback, path) : NULL;
    }
    if (!asset || !asset->original_size || asset->original_size > UINT32_MAX)
        return src;
    src.data = JCE_MALLOC((size_t)asset->original_size);
    if (!src.data) return src;
    src.size = (uint32_t)asset->original_size;
    if (jce_pak_decompress(asset, src.data, src.size) != src.size)
        deferred_source_free(&src);
    return src;
}

static JceShaderHandle defer_program(const JcePakArchive *pak, const char *dev,
                                  const char *vs, const char *fs, bool strict_fs)
{
    const bgfx_caps_t *caps = bgfx_get_caps();
    if (!JCE_PLATFORM_WEB || !vs || !fs || !caps ||
        caps->limits.maxPrograms >= JCE_DEFERRED_FIRST)
        return JCE_INVALID_SHADER;
    JceShaderDeferredState *state = jce_renderer_shader_deferred_state();
    for (unsigned i = 0; i < JCE_DEFERRED_COUNT; ++i) {
        JceDeferredProgram *p = &state->programs[i];
        if (p->used) continue;
        memset(p, 0, sizeof *p);
        p->vs = deferred_source_read(pak, dev, "vs", vs, strict_fs);
        p->fs = deferred_source_read(pak, dev, "fs", fs, strict_fs);
        if (!p->vs.data || !p->fs.data) {
            deferred_source_free(&p->vs);
            deferred_source_free(&p->fs);
            return JCE_INVALID_SHADER;
        }
        p->used = true; p->program = UINT16_MAX;
        LOG_DEBUG(LOG_TAG, "program prepared: token=%u vs=%s fs=%s backend=%s",
            JCE_DEFERRED_FIRST + i, vs, fs,
            bgfx_get_renderer_name(bgfx_get_renderer_type()));
        return (JceShaderHandle){ (uint16_t)(JCE_DEFERRED_FIRST + i) };
    }
    return JCE_INVALID_SHADER;
}

/* Stock material families retain their existing preparation policy. */
static JceShaderHandle defer_stock(const JcePakArchive *pak, const char *dev,
                                  const char *vs, const char *fs, bool strict_fs)
{
    if (!fs || (strncmp(fs, "pbr", 3) != 0 && strcmp(fs, "terrain") != 0))
        return JCE_INVALID_SHADER;
    return defer_program(pak, dev, vs, fs, strict_fs);
}

uint16_t jce_shader_deferred_resolve(uint16_t idx)
{
    if (!JCE_PLATFORM_WEB || idx < JCE_DEFERRED_FIRST || idx >= JCE_DEFERRED_FIRST + JCE_DEFERRED_COUNT)
        return idx;
    const bgfx_caps_t *caps = bgfx_get_caps();
    if (!caps || caps->limits.maxPrograms >= JCE_DEFERRED_FIRST) return idx;
    JceShaderDeferredState *state = jce_renderer_shader_deferred_state();
    JceDeferredProgram *p = &state->programs[idx - JCE_DEFERRED_FIRST];
    if (!p->used) return UINT16_MAX;
    if (!p->attempted) {
        p->attempted = true;
        const bgfx_memory_t *vs_mem = bgfx_copy(p->vs.data, p->vs.size);
        bgfx_shader_handle_t vs = { UINT16_MAX };
        bgfx_shader_handle_t fs = { UINT16_MAX };
        if (vs_mem) vs = bgfx_create_shader(vs_mem);
        if (vs.idx != UINT16_MAX) {
            const bgfx_memory_t *fs_mem = bgfx_copy(p->fs.data, p->fs.size);
            if (fs_mem) fs = bgfx_create_shader(fs_mem);
        }
        if (vs.idx != UINT16_MAX && fs.idx != UINT16_MAX)
            p->program = link_program_owned(vs, fs).idx;
        else if (vs.idx != UINT16_MAX)
            bgfx_destroy_shader(vs);
        if (p->program == UINT16_MAX)
            LOG_ERROR(LOG_TAG,
                "program realization failed: token=%u backend=%s vs_bytes=%u fs_bytes=%u vs_handle=%u fs_handle=%u",
                idx, bgfx_get_renderer_name(bgfx_get_renderer_type()),
                p->vs.size, p->fs.size, vs.idx, fs.idx);
        deferred_source_free(&p->vs);
        deferred_source_free(&p->fs);
    }
    return p->program;
}

void jce_shader_deferred_shutdown(void)
{
    JceShaderDeferredState *state = jce_renderer_shader_deferred_state();
    for (unsigned i = 0; i < JCE_DEFERRED_COUNT; ++i) {
        if (state->programs[i].used)
            jce_shader_program_destroy((JceShaderHandle){ (uint16_t)(JCE_DEFERRED_FIRST + i) });
    }
}

/* Load one shader (.bin) from the PAK, return a bgfx handle.
   Falls back to the engine-embedded shader pak when the caller-supplied
   pak is NULL or the path is missing. */
static bgfx_shader_handle_t load_single(const JcePakArchive *pak, const char *path,
                                        JceShaderAcquireEvidence *evidence)
{
    bgfx_shader_handle_t invalid;
    invalid.idx = UINT16_MAX;

    const JcePakAsset *asset = pak ? jce_pak_find(pak, path) : NULL;
    if (!asset) {
        const JcePakArchive *fb = embedded_pak();
        if (fb && fb != pak) {
            asset = jce_pak_find(fb, path);
            if (asset) pak = fb;
        }
    }
    if (!asset) {
        LOG_ERROR(LOG_TAG, "not found in PAK: %s", path);
        return invalid;
    }

    if (!asset->original_size || asset->original_size > UINT32_MAX) {
        LOG_ERROR(LOG_TAG, "shader size unsupported: key=%s bytes=%llu", path,
            (unsigned long long)asset->original_size);
        return invalid;
    }
    void *buf = JCE_MALLOC((size_t)asset->original_size);
    if (!buf) {
        LOG_ERROR(LOG_TAG, "shader source allocation failed: key=%s bytes=%llu",
            path, (unsigned long long)asset->original_size);
        return invalid;
    }

    size_t n = jce_pak_decompress(asset, buf, (size_t)asset->original_size);
    if (n != (size_t)asset->original_size) {
        LOG_ERROR(LOG_TAG, "shader decompression failed: key=%s expected=%llu actual=%zu",
            path, (unsigned long long)asset->original_size, n);
        JCE_FREE(buf);
        return invalid;
    }

    /* bgfx_copy allocates internal memory and copies; we can free buf. */
    const bgfx_memory_t *mem =
        bgfx_copy(buf, (uint32_t)asset->original_size);
    JCE_FREE(buf);

    if (!mem) {
        LOG_ERROR(LOG_TAG, "shader upload allocation failed: key=%s", path);
        return invalid;
    }
    bgfx_shader_handle_t shader = create_shader_evidenced(mem, evidence);
    if (shader.idx == UINT16_MAX)
        LOG_ERROR(LOG_TAG, "shader creation failed: key=%s backend=%s", path,
            bgfx_get_renderer_name(bgfx_get_renderer_type()));
    return shader;
}


/* Internal: load a program with explicit VS and FS base names.
   Skinned programs share the fragment shader with their non-skinned
   counterpart (e.g., vs_pbr_skinned + fs_pbr). */
static JceShaderHandle load_program_named(const JcePakArchive *pak,
                                          const char *vs_base,
                                          const char *fs_base)
{
    JceShaderHandle pending = defer_stock(pak, NULL, vs_base, fs_base, false);
    if (jce_shader_valid(pending)) return pending;

    const char *sfx = shader_suffix(bgfx_get_renderer_type());
    if (!sfx) {
        LOG_ERROR(LOG_TAG, "no shader suffix for renderer %s",
                  bgfx_get_renderer_name(bgfx_get_renderer_type()));
        return JCE_INVALID_SHADER;
    }

    char vs_path[256], fs_path[256];
    snprintf(vs_path, sizeof(vs_path), "shaders/vs_%s_%s.bin", vs_base, sfx);
    snprintf(fs_path, sizeof(fs_path), "shaders/fs_%s_%s.bin", fs_base, sfx);

    JceShaderAcquireEvidence vs_evidence = {0}, fs_evidence = {0};
    bgfx_shader_handle_t vsh = load_single(pak, vs_path, &vs_evidence);
    if (vsh.idx == UINT16_MAX) return JCE_INVALID_SHADER;

    bgfx_shader_handle_t fsh = load_single(pak, fs_path, &fs_evidence);
    if (fsh.idx == UINT16_MAX) {
        release_shader_evidenced(vsh, &vs_evidence);
        return JCE_INVALID_SHADER;
    }

    bgfx_program_handle_t prog = link_program_evidenced(vsh, fsh, &vs_evidence, &fs_evidence);
    if (prog.idx == UINT16_MAX)
        LOG_ERROR(LOG_TAG, "program link failed: vs=%s fs=%s backend=%s",
            vs_path, fs_path, bgfx_get_renderer_name(bgfx_get_renderer_type()));
    return (JceShaderHandle){ prog.idx };
}

/* Private opt-in for owned effect pipelines. Submission resolves the handle;
 * destruction retains the prepared identity so unused source bytes are freed. */
uint16_t jce_shader_program_prepare(const JcePakArchive *pak,
                                   const char *vs_base, const char *fs_base)
{
    JceShaderHandle pending = defer_program(pak, NULL, vs_base, fs_base, false);
    if (jce_shader_valid(pending)) return pending.idx;
    return shader_load_program_named(pak, vs_base, fs_base).idx;
}

JceShaderHandle shader_load_program(const JcePakArchive *pak, const char *name)
{
    return load_program_named(pak, name, name);
}

JceShaderHandle shader_load_program_named(const JcePakArchive *pak,
                                          const char *vs_base,
                                          const char *fs_base)
{
    return load_program_named(pak, vs_base, fs_base);
}

JceShaderHandle jce_renderer_create_program_from_blobs(
    const void *vs_blob, size_t vs_size,
    const void *fs_blob, size_t fs_size)
{
    const JceShaderHandle invalid = JCE_INVALID_SHADER;
    if (!vs_blob || !vs_size || vs_size > UINT32_MAX ||
        !fs_blob || !fs_size || fs_size > UINT32_MAX)
        return invalid;

    /* Consume each copied buffer before allocating the next stage. */
    const bgfx_memory_t *memory = bgfx_copy(vs_blob, (uint32_t)vs_size);
    if (!memory) return invalid;
    const bgfx_shader_handle_t vs = bgfx_create_shader(memory);
    if (!BGFX_HANDLE_IS_VALID(vs)) return invalid;

    memory = bgfx_copy(fs_blob, (uint32_t)fs_size);
    const bgfx_shader_handle_t fs = memory
        ? bgfx_create_shader(memory) : (bgfx_shader_handle_t){UINT16_MAX};
    return (JceShaderHandle){link_program_owned(vs, fs).idx};
}

void jce_renderer_destroy_program(JceShaderHandle prog)
{
    if (prog.idx == UINT16_MAX) return;
    jce_shader_program_destroy((JceShaderHandle){ ((bgfx_program_handle_t){ prog.idx }).idx });
}

void jce_shader_program_destroy(JceShaderHandle prog)
{
    if (!jce_shader_valid(prog)) return;
    const bgfx_caps_t *caps = bgfx_get_caps();
    if (JCE_PLATFORM_WEB && caps && caps->limits.maxPrograms < JCE_DEFERRED_FIRST &&
        prog.idx >= JCE_DEFERRED_FIRST &&
        prog.idx < JCE_DEFERRED_FIRST + JCE_DEFERRED_COUNT) {
        JceShaderDeferredState *state = jce_renderer_shader_deferred_state();
        JceDeferredProgram *slot = &state->programs[prog.idx - JCE_DEFERRED_FIRST];
        if (!slot->used) return;
        if (slot->program != UINT16_MAX)
            bgfx_destroy_program((bgfx_program_handle_t){ slot->program });
        deferred_source_free(&slot->vs);
        deferred_source_free(&slot->fs);
        memset(slot, 0, sizeof *slot);
        return;
    }
    bgfx_program_handle_t p = { prog.idx };
    bgfx_destroy_program(p);
}

const char *jce_shaders_backend_suffix(void)
{
    return shader_suffix(bgfx_get_renderer_type());
}

/* ================================================================== */
/* Filesystem overlay (hot-reload support)                            */
/* ================================================================== */

static bgfx_shader_handle_t load_single_fs(const char *full_path,
                                           JceShaderAcquireEvidence *evidence)
{
    bgfx_shader_handle_t invalid;
    invalid.idx = UINT16_MAX;

    /* Read through the filesystem adapter rather than raw SDL: the dev-dir
     * overlay is an ASSET read, so an active-VFS override (editor bundle
     * preview) must be able to serve the shader bin it shipped. */
    uint64_t sz  = 0;
    void    *buf = jce_fs_host_read_all(full_path, &sz);
    if (!buf) {
        /* The adapter folds open-failure, short read and OOM into NULL. */
        LOG_ERROR(LOG_TAG, "fs open/read failed: %s", full_path);
        return invalid;
    }

    if (sz == 0 || sz > UINT32_MAX) {
        jce_fs_buffer_free(buf);
        LOG_ERROR(LOG_TAG, "shader size unsupported: key=%s bytes=%llu",
            full_path, (unsigned long long)sz);
        return invalid;
    }

    const bgfx_memory_t *mem = bgfx_copy(buf, (uint32_t)sz);
    jce_fs_buffer_free(buf);
    if (!mem) {
        LOG_ERROR(LOG_TAG, "shader upload allocation failed: key=%s", full_path);
        return invalid;
    }
    bgfx_shader_handle_t shader = create_shader_evidenced(mem, evidence);
    if (shader.idx == UINT16_MAX)
        LOG_ERROR(LOG_TAG, "shader creation failed: key=%s backend=%s", full_path,
            bgfx_get_renderer_name(bgfx_get_renderer_type()));
    return shader;
}

static JceShaderHandle load_program_fs_named(const char *dev_dir,
                                             const char *vs_base,
                                             const char *fs_base)
{
    if (!dev_dir || !vs_base || !fs_base) return JCE_INVALID_SHADER;
    JceShaderHandle pending = defer_stock(NULL, dev_dir, vs_base, fs_base, true);
    if (jce_shader_valid(pending)) return pending;

    const char *sfx = shader_suffix(bgfx_get_renderer_type());
    if (!sfx) return JCE_INVALID_SHADER;

    char vs_path[512], fs_path[512];
    snprintf(vs_path, sizeof(vs_path),
             "%s/shaders/vs_%s_%s.bin", dev_dir, vs_base, sfx);
    snprintf(fs_path, sizeof(fs_path),
             "%s/shaders/fs_%s_%s.bin", dev_dir, fs_base, sfx);

    JceShaderAcquireEvidence vs_evidence = {0}, fs_evidence = {0};
    bgfx_shader_handle_t vsh = load_single_fs(vs_path, &vs_evidence);
    if (vsh.idx == UINT16_MAX) return JCE_INVALID_SHADER;

    bgfx_shader_handle_t fsh = load_single_fs(fs_path, &fs_evidence);
    if (fsh.idx == UINT16_MAX) {
        release_shader_evidenced(vsh, &vs_evidence);
        return JCE_INVALID_SHADER;
    }

    bgfx_program_handle_t prog = link_program_evidenced(vsh, fsh, &vs_evidence, &fs_evidence);
    if (prog.idx == UINT16_MAX)
        LOG_ERROR(LOG_TAG, "program link failed: vs=%s fs=%s backend=%s",
            vs_path, fs_path, bgfx_get_renderer_name(bgfx_get_renderer_type()));
    return (JceShaderHandle){ prog.idx };
}

JceShaderHandle shader_load_program_fs(const char *dev_dir, const char *name)
{
    return load_program_fs_named(dev_dir, name, name);
}

JceShaderHandle shader_load_program_fs_named(const char *dev_dir,
                                             const char *vs_base,
                                             const char *fs_base)
{
    return load_program_fs_named(dev_dir, vs_base, fs_base);
}

JceShaderHandle shader_load_program_overlay_named(
    const char *dev_dir, const JcePakArchive *pak,
    const char *vs_base, const char *fs_base)
{
    bgfx_shader_handle_t vsh, fsh;
    JceShaderAcquireEvidence vs_evidence = {0}, fs_evidence = {0};
    bgfx_program_handle_t program;
    const char *sfx;
    char vs_path[512], fs_path[512];
    bool vs_overlay, fs_overlay;

    if (!vs_base || !fs_base) return JCE_INVALID_SHADER;
    JceShaderHandle pending = defer_stock(pak, dev_dir, vs_base, fs_base, false);
    if (jce_shader_valid(pending)) return pending;
    sfx = shader_suffix(bgfx_get_renderer_type());
    if (!sfx) return JCE_INVALID_SHADER;

    snprintf(vs_path, sizeof(vs_path),
             "%s/shaders/vs_%s_%s.bin",
             dev_dir ? dev_dir : "", vs_base, sfx);
    snprintf(fs_path, sizeof(fs_path),
             "%s/shaders/fs_%s_%s.bin",
             dev_dir ? dev_dir : "", fs_base, sfx);
    vs_overlay = dev_dir && dev_dir[0] && jce_fs_host_exists_file(vs_path);
    fs_overlay = dev_dir && dev_dir[0] && jce_fs_host_exists_file(fs_path);

    vsh = vs_overlay ? load_single_fs(vs_path, &vs_evidence) : (bgfx_shader_handle_t){ UINT16_MAX };
    if (!vs_overlay) {
        char pak_path[256];
        snprintf(pak_path, sizeof(pak_path),
                 "shaders/vs_%s_%s.bin", vs_base, sfx);
        vsh = load_single(pak, pak_path, &vs_evidence);
    }
    if (vsh.idx == UINT16_MAX) return JCE_INVALID_SHADER;

    fsh = fs_overlay ? load_single_fs(fs_path, &fs_evidence) : (bgfx_shader_handle_t){ UINT16_MAX };
    if (!fs_overlay) {
        char pak_path[256];
        snprintf(pak_path, sizeof(pak_path),
                 "shaders/fs_%s_%s.bin", fs_base, sfx);
        fsh = load_single(pak, pak_path, &fs_evidence);
    }
    if (fsh.idx == UINT16_MAX) {
        release_shader_evidenced(vsh, &vs_evidence);
        return JCE_INVALID_SHADER;
    }

    program = link_program_evidenced(vsh, fsh, &vs_evidence, &fs_evidence);
    if (program.idx == UINT16_MAX)
        LOG_ERROR(LOG_TAG, "overlay program link failed: vs=%s fs=%s backend=%s",
            vs_base, fs_base, bgfx_get_renderer_name(bgfx_get_renderer_type()));
    return (JceShaderHandle){ program.idx };
}

#include "jce_shader_stock_table_internal.h"

/* Defined below, beside the other overlay loaders: the table loader has
 * to sit above jce_shaders_load_all and load_overlay below it, and one
 * declaration is cheaper than moving either. */
static JceShaderHandle load_overlay(const char *dev_dir,
                                    const JcePakArchive *pak,
                                    const char *vs_base,
                                    const char *fs_base);

typedef struct JceShaderStockContext {
    const char *dev_dir;
    const JcePakArchive *pak;
} JceShaderStockContext;

static JceShaderHandle load_stock_pair(void *user, const char *vs, const char *fs)
{
    const JceShaderStockContext *context = user;
    return load_overlay(context->dev_dir, context->pak, vs, fs);
}

static JceShaderStockProfile load_stock_table(const char *dev_dir,
    const JcePakArchive *pak, JceShaderSet *set, JceShaderStockAcquires *batch)
{
    JceShaderStockContext context = {dev_dir, pak};
    const JceRendererApiInfo api = jce_renderer_get_api_info();
    const JceShaderStockProfile profile = jce_shader_stock_profile_for_api(
        &api, bgfx_get_renderer_type());
    jce_shader_stock_load(set, batch, load_stock_pair, &context, profile);
    if (profile == JCE_SHADER_STOCK_PROFILE_COMPATIBILITY)
        jce_log_write(JCE_LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__,
            "stock shader profile=compatibility backend=%s acquired=%u; "
            "instanced/skinned/array/Forward+ families excluded",
            jce_renderer_backend_name(api.backend), (unsigned)batch->count);
    return profile;
}

JceShaderSet jce_shaders_load_all(const JcePakArchive *pak)
{
    JCE_PROFILE_ZONE_N("Shaders::LoadAll");
    JceShaderSet set;
    JceShaderStockAcquires acquires = {0};
    const JceShaderStockProfile profile = load_stock_table(NULL, pak, &set, &acquires);
    unsigned duplicate_refs = jce_shader_set_normalize_stock_acquires(&acquires);
    jce_shader_set_alias_stock(&set);
    if (jce_trace_enabled())
        jce_trace_counter(JCE_TRACE_CATEGORY_RENDER,
                          "renderer.stock_duplicate_acquires", duplicate_refs);

    if (!jce_shader_valid(set.color))
        LOG_ERROR(LOG_TAG, "failed to load 'color' shader");
    if (!jce_shader_valid(set.textured))
        LOG_WARN(LOG_TAG, "'textured' shader unavailable");
    if (!jce_shader_valid(set.mesh))
        LOG_WARN(LOG_TAG, "'mesh' shader unavailable");
    if (!jce_shader_valid(set.pbr))
        LOG_WARN(LOG_TAG, "'pbr' shader unavailable");
    if (profile == JCE_SHADER_STOCK_PROFILE_FULL && !jce_shader_valid(set.pbr_inst))
        LOG_WARN(LOG_TAG, "'pbr_inst' shader unavailable (GPU instancing disabled)");
    if (profile == JCE_SHADER_STOCK_PROFILE_FULL && !jce_shader_valid(set.pbr_inst_tint))
        LOG_WARN(LOG_TAG, "'pbr_inst_tint' shader unavailable (per-instance tint batching disabled)");
    if (profile == JCE_SHADER_STOCK_PROFILE_FULL && !jce_shader_valid(set.pbr_inst_tex_array))
        LOG_WARN(LOG_TAG, "'pbr_inst_tex_array' shader unavailable (texture-diverse instancing disabled)");
    if (profile == JCE_SHADER_STOCK_PROFILE_FULL && !jce_shader_valid(set.pbr_inst_fade))
        LOG_WARN(LOG_TAG, "'pbr_inst_fade' shader unavailable (LOD cross-fade dither disabled)");
    if (profile == JCE_SHADER_STOCK_PROFILE_FULL && !jce_shader_valid(set.pbr_skinned))
        LOG_WARN(LOG_TAG, "'pbr_skinned' shader unavailable");
    if (profile == JCE_SHADER_STOCK_PROFILE_FULL && !jce_shader_valid(set.pbr_fwdplus))
        LOG_WARN(LOG_TAG, "'pbr_fwdplus' shader unavailable (Forward+ disabled)");
    if (profile == JCE_SHADER_STOCK_PROFILE_FULL && !jce_shader_valid(set.pbr_toon))
        LOG_WARN(LOG_TAG, "'pbr_toon' shader unavailable (toon character disabled)");
    if (profile == JCE_SHADER_STOCK_PROFILE_FULL && !jce_shader_valid(set.outline_skinned))
        LOG_WARN(LOG_TAG, "'outline_skinned' shader unavailable (character outline disabled)");
    if (!jce_shader_valid(set.shadow))
        LOG_WARN(LOG_TAG, "'shadow' shader unavailable");
    if (profile == JCE_SHADER_STOCK_PROFILE_FULL && !jce_shader_valid(set.shadow_inst))
        LOG_WARN(LOG_TAG, "'shadow_inst' shader unavailable (shadow instancing disabled)");
    if (profile == JCE_SHADER_STOCK_PROFILE_FULL && !jce_shader_valid(set.shadow_skinned))
        LOG_WARN(LOG_TAG, "'shadow_skinned' shader unavailable");
    if (profile == JCE_SHADER_STOCK_PROFILE_FULL && !jce_shader_valid(set.terrain))
        LOG_WARN(LOG_TAG, "'terrain' shader unavailable");

    JCE_PROFILE_ZONE_END;
    return set;
}

/* ================================================================== */
/* FS-overlay variant of jce_shaders_load_all                         */
/* ================================================================== */

/* Stock and project overlays share stage selection and failure ownership. */
static JceShaderHandle load_overlay(const char *dev_dir,
                                    const JcePakArchive *pak,
                                    const char *vs, const char *fs)
{
    return shader_load_program_overlay_named(dev_dir, pak, vs, fs);
}

JceShaderSet jce_shaders_load_all_fs(const char *dev_dir,
                                     const JcePakArchive *pak)
{
    JCE_PROFILE_ZONE_N("Shaders::LoadAllFS");
    JceShaderSet set;
    JceShaderStockAcquires acquires = {0};
    (void)load_stock_table(dev_dir, pak, &set, &acquires);
    unsigned duplicate_refs = jce_shader_set_normalize_stock_acquires(&acquires);
    jce_shader_set_alias_stock(&set);
    if (jce_trace_enabled())
        jce_trace_counter(JCE_TRACE_CATEGORY_RENDER,
                          "renderer.stock_duplicate_acquires", duplicate_refs);
    JCE_PROFILE_ZONE_END;
    return set;
}
