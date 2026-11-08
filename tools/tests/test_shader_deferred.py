"""Execute production shader owners with counted CPU backend/trace ports.

These controls observe source ownership and command admission, not GPU work.
"""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

class DeferredShaderTests(unittest.TestCase):
    def _owned_link_probe(self, *, old_auto_ownership=False):
        from tools.tests.test_material_owned_cleanup import function
        source = (ROOT / "engine/src/renderer/jce_shaders.c").read_text(encoding="utf-8")
        selected = function(source, "static bgfx_program_handle_t link_program_owned(")
        deferred_resolve = function(source, "uint16_t jce_shader_deferred_resolve(")
        self.assertEqual(deferred_resolve.count("link_program_owned("), 1)
        self.assertNotIn("bgfx_create_program(", deferred_resolve)
        self.assertEqual(source.count("bgfx_create_program("), 1)
        if old_auto_ownership:
            for before, after in (
                ("bgfx_create_program(vsh, fsh, false)", "bgfx_create_program(vsh, fsh, true)"),
                ("bgfx_destroy_shader(vsh); ", ""),
                ("bgfx_destroy_shader(fsh); ", ""),
            ):
                self.assertEqual(selected.count(before), 1)
                selected = selected.replace(before, after, 1)
        harness = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "shader owned-link control failed: %s at %d\n", #value, __LINE__); exit(1); \
} } while (0)
typedef struct { uint16_t idx; } bgfx_shader_handle_t;
typedef struct { uint16_t idx; } bgfx_program_handle_t;
typedef struct { uint64_t id, parent; } JceTraceScope;
#define JCE_TRACE_CATEGORY_RENDER 5
#define JCE_TRACE_TASK_SUCCEEDED 0
#define JCE_TRACE_TASK_FAILED 1
static unsigned shader_refs[4], program_refs, links, creator_destroys, submits;
static bool mismatch, exhausted, tracing = true;
static unsigned trace_started, trace_finished, trace_counters, active_scope, columns;
static int64_t last_fields[6];
static const char *field_names[] = {
    "shader.link.vs", "shader.link.fs", "shader.link.program",
    "shader.link.creator_releases", "shader.link.success", "shader.link.rejection"
};
static JceTraceScope jce_trace_scope_begin(const char *name) {
    CHECK(strcmp(name, "renderer.shader.link") == 0 && active_scope == 0);
    if (!tracing) return (JceTraceScope){0, 0};
    active_scope = ++trace_started; columns = 0;
    return (JceTraceScope){active_scope, 37};
}
static void jce_trace_counter_scoped_internal(int category, const char *name,
    int64_t value, uint64_t id, uint64_t parent) {
    CHECK(tracing && category == 5 && id == active_scope && parent == 37);
    CHECK(columns < 6 && strcmp(name, field_names[columns]) == 0);
    last_fields[columns++] = value; ++trace_counters;
}
static void jce_trace_scope_end(JceTraceScope scope, int state) {
    if (!scope.id) return;
    CHECK(scope.id == active_scope && columns == 6);
    CHECK(state == (last_fields[4] ? 0 : 1));
    CHECK((last_fields[5] == 0) == (last_fields[4] != 0));
    active_scope = 0; ++trace_finished;
}
static void shader_drop(bgfx_shader_handle_t shader) {
    CHECK(shader.idx < 4 && shader_refs[shader.idx] > 0);
    --shader_refs[shader.idx];
}
static void bgfx_destroy_shader(bgfx_shader_handle_t shader) {
    shader_drop(shader); ++creator_destroys;
}
static bgfx_program_handle_t bgfx_create_program(bgfx_shader_handle_t vs,
    bgfx_shader_handle_t fs, bool own) {
    CHECK(vs.idx == 0 && fs.idx == 3 && shader_refs[0] && shader_refs[3]);
    ++links;
    /* The pinned Context returns on interface mismatch BEFORE _destroyShaders.
     * A valid cached acquire increments both program and stage references. */
    if (mismatch) return (bgfx_program_handle_t){UINT16_MAX};
    if (!exhausted) { ++program_refs; ++shader_refs[0]; ++shader_refs[3]; }
    if (own) { shader_drop(vs); shader_drop(fs); }
    return (bgfx_program_handle_t){exhausted ? UINT16_MAX : 0};
}
static void submit_program(bgfx_program_handle_t program) {
    CHECK(program.idx == 0 && program_refs > 0);
    CHECK(shader_refs[0] == program_refs && shader_refs[3] == program_refs);
    ++submits;
}
static void bgfx_destroy_program(bgfx_program_handle_t program) {
    CHECK(program.idx == 0 && program_refs > 0);
    --program_refs; shader_drop((bgfx_shader_handle_t){0}); shader_drop((bgfx_shader_handle_t){3});
}
static void creators(void) { ++shader_refs[0]; ++shader_refs[3]; }
static void closed(void) { CHECK(program_refs == 0 && shader_refs[0] == 0 && shader_refs[3] == 0); }
''' + selected + r'''
static void mismatch_control(void) {
    creators(); mismatch = true;
    unsigned prior_destroy = creator_destroys;
    bgfx_program_handle_t program = link_program_owned(
        (bgfx_shader_handle_t){0}, (bgfx_shader_handle_t){3});
    CHECK(program.idx == UINT16_MAX && links > 0 && program_refs == 0);
    CHECK(shader_refs[0] == 0 && shader_refs[3] == 0);
    CHECK(creator_destroys == prior_destroy + 2);
    CHECK(last_fields[0] == 0 && last_fields[1] == 3 && last_fields[2] == UINT16_MAX);
    CHECK(last_fields[3] == 2 && last_fields[4] == 0 && last_fields[5] == 2);
    mismatch = false; closed();
}
int main(void) {
#if OLD_LINK_CONTROL
    mismatch_control();
#else
    /* 1: program 0 survives creator release and remains consumable. */
    creators();
    bgfx_program_handle_t first = link_program_owned(
        (bgfx_shader_handle_t){0}, (bgfx_shader_handle_t){3});
    CHECK(first.idx == 0 && creator_destroys == 2 && program_refs == 1);
    CHECK(last_fields[3] == 2 && last_fields[4] == 1 && last_fields[5] == 0);
    submit_program(first); bgfx_destroy_program(first); closed();
    /* 2: exact early mismatch failure rolls back both caller references. */
    mismatch_control();
    /* 3: program-handle admission failure also retires both creators. */
    creators(); exhausted = true;
    first = link_program_owned((bgfx_shader_handle_t){0}, (bgfx_shader_handle_t){3});
    CHECK(first.idx == UINT16_MAX && last_fields[3] == 2 && last_fields[5] == 2);
    exhausted = false; closed();
    /* 4: an invalid stage never enters the backend link, but retires its peer. */
    ++shader_refs[0]; unsigned prior_links = links, prior_destroy = creator_destroys;
    first = link_program_owned((bgfx_shader_handle_t){0}, (bgfx_shader_handle_t){UINT16_MAX});
    CHECK(first.idx == UINT16_MAX && links == prior_links && creator_destroys == prior_destroy + 1);
    CHECK(last_fields[3] == 1 && last_fields[4] == 0 && last_fields[5] == 1); closed();
    /* 5: no stages means no invalid-handle destroy or link. */
    prior_destroy = creator_destroys;
    first = link_program_owned((bgfx_shader_handle_t){UINT16_MAX}, (bgfx_shader_handle_t){UINT16_MAX});
    CHECK(first.idx == UINT16_MAX && links == prior_links && creator_destroys == prior_destroy);
    CHECK(last_fields[3] == 0 && last_fields[5] == 1); closed();
    /* 6: cached program acquisitions keep an independent reference each. */
    creators(); first = link_program_owned((bgfx_shader_handle_t){0}, (bgfx_shader_handle_t){3});
    creators(); bgfx_program_handle_t second = link_program_owned(
        (bgfx_shader_handle_t){0}, (bgfx_shader_handle_t){3});
    CHECK(first.idx == second.idx && program_refs == 2);
    submit_program(first); bgfx_destroy_program(first);
    CHECK(program_refs == 1); submit_program(second); bgfx_destroy_program(second); closed();
    /* 7: trace disabled has no counters/spans and changes no ownership. */
    unsigned prior_trace = trace_counters, prior_started = trace_started;
    tracing = false; creators();
    first = link_program_owned((bgfx_shader_handle_t){0}, (bgfx_shader_handle_t){3});
    CHECK(trace_counters == prior_trace && trace_started == prior_started);
    submit_program(first); bgfx_destroy_program(first); closed();
    CHECK(active_scope == 0 && trace_started == trace_finished && trace_counters == trace_finished * 6);
    puts("shader owned-link CPU PASS (7 controls; cached refs and rejection/creator-release trace observed)");
#endif
    return 0;
}
'''
        return "#define OLD_LINK_CONTROL " + str(int(old_auto_ownership)) + "\n" + harness

    def test_owned_link_retains_program_and_closes_failure_creators(self):
        from tools.tests.test_material_owned_cleanup import execute
        execute(self, self._owned_link_probe())

    def test_old_auto_ownership_leaves_mismatch_creators_runtime_negative(self):
        from tools.tests.test_material_owned_cleanup import execute
        execute(self, self._owned_link_probe(old_auto_ownership=True),
                expected_runtime_failure="shader owned-link control failed")

    def test_prepare_submit_release_failure_and_capacity(self):
        spec = importlib.util.spec_from_file_location("deferred_test_driver", ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        if driver.HOST != "windows":
            self.skipTest("this recorded CPU probe uses MSVC")
        target = driver.resolve_target(None)
        env = driver.msvc_env(target)
        source = (ROOT / "engine/src/renderer/jce_shaders.c").read_text(encoding="utf-8")
        from tools.tests.test_material_owned_cleanup import function
        # Keep complete Web/shared owners without absorbing adjacent Native eager
        # provenance helpers, whose device/trace ports this Web probe does not own.
        deferred_signatures = (
            "static void deferred_source_free(",
            "static bgfx_program_handle_t link_program_owned(",
            "static JceDeferredSource deferred_source_read(",
            "static JceShaderHandle defer_program(",
            "static JceShaderHandle defer_stock(",
            "uint16_t jce_shader_deferred_resolve(",
            "void jce_shader_deferred_shutdown(",
        )
        registry = (ROOT / "engine/src/renderer/jce_shader_deferred.h").read_text(encoding="utf-8")
        registry += "\n".join(function(source, signature) for signature in deferred_signatures)
        self.assertNotIn("JceShaderAcquireEvidence", registry)
        self.assertNotIn("shader_provenance_begin(", registry)
        if "uint16_t jce_shader_program_prepare(" in source:
            prepare_start = source.index("uint16_t jce_shader_program_prepare(")
            prepare_end = source.index("\nJceShaderHandle shader_load_program(", prepare_start)
            registry += source[prepare_start:prepare_end]
        postfx = (ROOT / "engine/src/renderer/jce_postfx.c").read_text(encoding="utf-8")
        loader = postfx[postfx.index("static bgfx_program_handle_t load_postfx_prog("):
                        postfx.index("bool jce_postfx_load_shaders(")]
        release_start = source.index("void jce_shader_program_destroy(")
        release_end = source.index("const char *jce_shaders_backend_suffix", release_start)
        release = source[release_start:release_end]
        harness = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { uint16_t idx; } JceShaderHandle;
typedef struct { uint16_t idx; } bgfx_program_handle_t;
typedef struct { uint16_t idx; } bgfx_shader_handle_t;
typedef struct { uint64_t original_size; } JcePakAsset;
typedef struct JcePakArchive { int identity; } JcePakArchive;
typedef struct { struct { uint32_t maxPrograms; } limits; } bgfx_caps_t;
typedef struct { int unused; } bgfx_memory_t;
#define JCE_PLATFORM_WEB 1
#define JCE_INVALID_SHADER ((JceShaderHandle){UINT16_MAX})
#define JCE_MALLOC(n) malloc(n)
#define JCE_FREE(p) free(p)
static bgfx_caps_t caps = {{512}};
static int loads, shader_loads, destroys, copies, shader_destroys;
static int fail_copy_at, fail_shader_at;
static bool fail_link;
typedef struct { uint64_t id, parent; } JceTraceScope;
#define JCE_TRACE_CATEGORY_RENDER 5
#define JCE_TRACE_TASK_SUCCEEDED 0
#define JCE_TRACE_TASK_FAILED 1
static unsigned trace_open, trace_columns, trace_failures;
static bool trace_success;
static JceTraceScope jce_trace_scope_begin(const char *name) {
    assert(strcmp(name, "renderer.shader.link") == 0 && trace_open == 0);
    ++trace_open; trace_columns = 0;
    return (JceTraceScope){1, 9};
}
static void jce_trace_counter_scoped_internal(int category, const char *name,
    int64_t value, uint64_t id, uint64_t parent) {
    assert(category == 5 && id == 1 && parent == 9 && trace_open == 1);
    assert(strncmp(name, "shader.link.", 12) == 0);
    ++trace_columns;
    if (strcmp(name, "shader.link.success") == 0) trace_success = value != 0;
    if (strcmp(name, "shader.link.rejection") == 0) {
        assert((value == 0) == trace_success);
        if (value != 0) ++trace_failures;
    }
}
static void jce_trace_scope_end(JceTraceScope scope, int state) {
    assert(scope.id == 1 && trace_open == 1 && trace_columns == 6);
    assert(state == (trace_success ? 0 : 1)); --trace_open;
}
static bool fail, missing, host_present;
static unsigned char original = 7;
static const bgfx_caps_t *bgfx_get_caps(void) { return &caps; }
static int bgfx_get_renderer_type(void) { return 0; }
static const char *bgfx_get_renderer_name(int type) { assert(type == 0); return "cpu-gles"; }
static const char *shader_suffix(int type) { (void)type; return "essl"; }
static const JcePakArchive *embedded_pak(void) { return NULL; }
static const JcePakAsset *jce_pak_find(const JcePakArchive *pak, const char *path) {
    static JcePakAsset asset = {4}; (void)path;
    assert(pak->identity == 7); return missing ? NULL : &asset;
}
static size_t jce_pak_decompress(const JcePakAsset *asset, void *data, size_t size) {
    assert(size == asset->original_size); memset(data, original, size); return size;
}
static bool jce_fs_host_exists_file(const char *path) { (void)path; return host_present; }
static void *jce_fs_host_read_all(const char *path, uint64_t *size) {
    (void)path; if (!host_present) return NULL;
    *size = 4; void *data = malloc(4); memset(data, original, 4); return data;
}
static void jce_fs_buffer_free(void *data) { free(data); }
static const bgfx_memory_t *bgfx_copy(const void *data, uint32_t size) {
    static bgfx_memory_t mem; assert(size == 4);
    assert(((const unsigned char *)data)[0] == 7);
    if (++copies == fail_copy_at) return NULL;
    return &mem;
}
static bgfx_shader_handle_t bgfx_create_shader(const bgfx_memory_t *mem) {
    assert(mem); ++shader_loads;
    return fail || shader_loads == fail_shader_at
        ? (bgfx_shader_handle_t){UINT16_MAX} : (bgfx_shader_handle_t){3};
}
static void bgfx_destroy_shader(bgfx_shader_handle_t shader) { assert(shader.idx == 3); ++shader_destroys; }
static bgfx_program_handle_t bgfx_create_program(bgfx_shader_handle_t vs,
    bgfx_shader_handle_t fs, bool own) {
    assert(vs.idx == 3 && fs.idx == 3 && !own); ++loads;
    return fail_link ? (bgfx_program_handle_t){UINT16_MAX}
        : (bgfx_program_handle_t){(uint16_t)(40 + loads)};
}
static bool jce_shader_valid(JceShaderHandle handle) { return handle.idx != UINT16_MAX; }
static void bgfx_destroy_program(bgfx_program_handle_t handle) { assert(handle.idx < 65000); ++destroys; }
#define LOG_TAG "probe"
static unsigned log_count[3]; /* DEBUG, WARN, ERROR are observable ports. */
static char last_debug[512], last_error[512];
static void log_port(unsigned level, const char *tag, const char *format, ...)
{
    assert(level < 3 && strcmp(tag, LOG_TAG) == 0);
    char text[512];
    va_list args; va_start(args, format);
    int size = vsnprintf(text, sizeof text, format, args);
    va_end(args); assert(size >= 0 && (size_t)size < sizeof text);
    ++log_count[level];
    if (level == 0) strcpy(last_debug, text);
    if (level == 2) strcpy(last_error, text);
}
#define LOG_DEBUG(tag, ...) log_port(0, tag, __VA_ARGS__)
#define LOG_WARN(tag, ...) log_port(1, tag, __VA_ARGS__)
#define LOG_ERROR(tag, ...) log_port(2, tag, __VA_ARGS__)
static JceShaderHandle shader_load_program_named(const JcePakArchive *pak,
    const char *vs, const char *fs) {
    (void)pak; (void)vs; (void)fs; ++loads; copies += 2;
    return (JceShaderHandle){(uint16_t)(40 + loads)};
}
void jce_shader_program_destroy(JceShaderHandle);
''' + registry + r'''
static JceShaderDeferredState device;
JceShaderDeferredState *jce_renderer_shader_deferred_state(void) { return &device; }
''' + release + loader + r'''
int main(void)
{
    JcePakArchive pak = {7};
    JceShaderHandle a = defer_stock(&pak, "override", "pbr", "pbr", false);
    assert(a.idx >= JCE_DEFERRED_FIRST && loads == 0 && copies == 0);
    assert(log_count[0] == 1 && log_count[2] == 0);
    assert(strstr(last_debug, "program prepared:") && strstr(last_debug, "token="));
    assert(strstr(last_debug, "vs=pbr fs=pbr backend=cpu-gles"));
    original = 19; /* source lifetime/content no longer matters after prepare */
    uint16_t concrete = jce_shader_deferred_resolve(a.idx);
    assert(concrete == 41 && loads == 1 && copies == 2);
    assert(jce_shader_deferred_resolve(a.idx) == concrete && loads == 1);
    assert(log_count[2] == 0); /* successful realization is not failure */
    jce_shader_program_destroy(a);
    assert(destroys == 1 && jce_shader_deferred_resolve(a.idx) == UINT16_MAX);
    original = 7;
    JceShaderHandle b = defer_stock(&pak, NULL, "pbr", "pbr", false);
    jce_shader_program_destroy(b);
    assert(loads == 1 && destroys == 1);
    fail = true;
    b = defer_stock(&pak, NULL, "pbr", "pbr", false);
    unsigned errors_before = log_count[2];
    assert(jce_shader_deferred_resolve(b.idx) == UINT16_MAX && shader_loads == 3);
    assert(log_count[2] == errors_before + 1);
    assert(strstr(last_error, "program realization failed:") && strstr(last_error, "token="));
    assert(strstr(last_error, "backend=cpu-gles vs_bytes=4 fs_bytes=4"));
    assert(strstr(last_error, "vs_handle=65535 fs_handle=65535"));
    assert(jce_shader_deferred_resolve(b.idx) == UINT16_MAX && shader_loads == 3);
    assert(log_count[2] == errors_before + 1); /* attempted once, not every resolve */
    jce_shader_program_destroy(b);
    assert(log_count[2] == errors_before + 1);
    fail = false;
    missing = true;
    assert(!jce_shader_valid(defer_stock(&pak, NULL, "pbr", "pbr", false)));
    missing = false;
    assert(!jce_shader_valid(defer_stock(&pak, "missing", "pbr", "pbr", true)));
    host_present = true;
    b = defer_stock(NULL, "override", "pbr", "pbr", true);
    assert(jce_shader_valid(b)); jce_shader_program_destroy(b);
    assert(!jce_shader_valid(defer_stock(&pak, NULL, "color", "color", false)));
    caps.limits.maxPrograms = JCE_DEFERRED_FIRST;
    assert(!jce_shader_valid(defer_stock(&pak, NULL, "pbr", "pbr", false)));
    assert(jce_shader_deferred_resolve(JCE_DEFERRED_FIRST + 1) == JCE_DEFERRED_FIRST + 1);
    caps.limits.maxPrograms = 512;
    for (unsigned i = 0; i < JCE_DEFERRED_COUNT; ++i)
        assert(jce_shader_valid(defer_stock(&pak, NULL, "pbr", "pbr", false)));
    assert(!jce_shader_valid(defer_stock(&pak, NULL, "pbr", "pbr", false)));
    jce_shader_deferred_shutdown();
    assert(loads == 1 && destroys == 1);
    int before = loads, copied = copies;
    bgfx_program_handle_t effect = load_postfx_prog(&pak, "fxaa");
    assert(effect.idx >= JCE_DEFERRED_FIRST && effect.idx != UINT16_MAX);
    assert(loads == before && copies == copied); /* unused effects never link */
    original = 19;
    concrete = jce_shader_deferred_resolve(effect.idx);
    assert(concrete == 42 && loads == before + 1 && copies == copied + 2);
    assert(jce_shader_deferred_resolve(effect.idx) == concrete && loads == before + 1);
    jce_shader_program_destroy((JceShaderHandle){effect.idx});
    original = 7;
    effect = load_postfx_prog(&pak, "bloom_down");
    jce_shader_program_destroy((JceShaderHandle){effect.idx});
    assert(loads == before + 1 && destroys == 2);

    /* Negative ports below exercise the extracted production state machine;
     * none reimplements resolve, rollback, log emission or the attempted gate. */
    host_present = false;
    int created_before = shader_loads, copied_before = copies, linked_before = loads;
    int retired_before = shader_destroys, programs_before = destroys;
    errors_before = log_count[2];
    b = defer_stock(&pak, NULL, "pbr", "pbr", false);
    fail_copy_at = copies + 1;
    assert(jce_shader_deferred_resolve(b.idx) == UINT16_MAX);
    assert(copies == copied_before + 1 && shader_loads == created_before);
    assert(loads == linked_before && log_count[2] == errors_before + 1);
    assert(jce_shader_deferred_resolve(b.idx) == UINT16_MAX);
    assert(copies == copied_before + 1 && log_count[2] == errors_before + 1);
    jce_shader_program_destroy(b); fail_copy_at = 0;
    assert(destroys == programs_before && shader_destroys == retired_before);

    b = defer_stock(&pak, NULL, "pbr", "pbr", false);
    errors_before = log_count[2]; created_before = shader_loads;
    fail_shader_at = shader_loads + 2; /* VS accepted, FS create rejects */
    assert(jce_shader_deferred_resolve(b.idx) == UINT16_MAX);
    assert(shader_loads == created_before + 2 && shader_destroys == retired_before + 1);
    assert(log_count[2] == errors_before + 1);
    assert(strstr(last_error, "vs_handle=3 fs_handle=65535"));
    assert(jce_shader_deferred_resolve(b.idx) == UINT16_MAX);
    assert(shader_loads == created_before + 2 && shader_destroys == retired_before + 1);
    assert(log_count[2] == errors_before + 1);
    jce_shader_program_destroy(b); fail_shader_at = 0;

    b = defer_stock(&pak, NULL, "pbr", "pbr", false);
    errors_before = log_count[2]; created_before = shader_loads; linked_before = loads;
    retired_before = shader_destroys;
    fail_link = true;
    assert(jce_shader_deferred_resolve(b.idx) == UINT16_MAX);
    assert(shader_loads == created_before + 2 && loads == linked_before + 1);
    assert(shader_destroys == retired_before + 2);
    assert(log_count[2] == errors_before + 1);
    assert(strstr(last_error, "vs_handle=3 fs_handle=3"));
    assert(jce_shader_deferred_resolve(b.idx) == UINT16_MAX);
    assert(shader_loads == created_before + 2 && loads == linked_before + 1);
    assert(log_count[2] == errors_before + 1);
    jce_shader_program_destroy(b); fail_link = false;
    assert(destroys == programs_before && log_count[1] == 0);
    assert(log_count[2] == 4); /* VS create, copy OOM, FS create, program link */
    assert(trace_open == 0 && trace_failures == 1);
    puts("deferred shader CPU lifecycle PASS (4 failure logs; repeated resolves do not retry or relog)");
    return 0;
}
'''
        base = ROOT / "build/shader-deferred-tests"
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as temporary:
            directory = Path(temporary)
            file = directory / "probe.c"
            file.write_text(harness, encoding="utf-8")
            result = subprocess.run(["cl", "/nologo", "/TC", "/std:c11", "/W3", str(file),
                                     "/Fe:probe.exe", "/Fo:probe.obj"], cwd=directory,
                                    env=env, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(directory / "probe.exe")], cwd=directory,
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("CPU lifecycle PASS", result.stdout)
            self.assertIn("4 failure logs; repeated resolves do not retry or relog", result.stdout)

if __name__ == "__main__":
    unittest.main()
