"""Execute the owned deferred shader registry with observable GPU loader spies."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]

class DeferredShaderTests(unittest.TestCase):
    def test_prepare_submit_release_failure_and_capacity(self):
        spec = importlib.util.spec_from_file_location("deferred_test_driver", ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        if driver.HOST != "windows":
            self.skipTest("this recorded CPU probe uses MSVC")
        target = driver.resolve_target(None)
        env = driver.msvc_env(target)
        source = (ROOT / "engine/src/renderer/jce_shaders.c").read_text(encoding="utf-8")
        registry = (ROOT / "engine/src/renderer/jce_shader_deferred.h").read_text(encoding="utf-8") + source[source.index("static void deferred_source_free"):source.index("/* Load one shader (.bin)")]
        release_start = source.index("void jce_shader_program_destroy(")
        release_end = source.index("const char *jce_shaders_backend_suffix", release_start)
        release = source[release_start:release_end]
        harness = r'''
#include <assert.h>
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
static int loads, shader_loads, destroys, copies;
static bool fail, missing, host_present;
static unsigned char original = 7;
static const bgfx_caps_t *bgfx_get_caps(void) { return &caps; }
static int bgfx_get_renderer_type(void) { return 0; }
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
    assert(((const unsigned char *)data)[0] == 7); ++copies; return &mem;
}
static bgfx_shader_handle_t bgfx_create_shader(const bgfx_memory_t *mem) {
    assert(mem); ++shader_loads; return fail ? (bgfx_shader_handle_t){UINT16_MAX} : (bgfx_shader_handle_t){3};
}
static void bgfx_destroy_shader(bgfx_shader_handle_t shader) { assert(shader.idx == 3); }
static bgfx_program_handle_t bgfx_create_program(bgfx_shader_handle_t vs,
    bgfx_shader_handle_t fs, bool own) {
    assert(vs.idx == 3 && fs.idx == 3 && own); ++loads;
    return (bgfx_program_handle_t){(uint16_t)(40 + loads)};
}
static bool jce_shader_valid(JceShaderHandle handle) { return handle.idx != UINT16_MAX; }
static void bgfx_destroy_program(bgfx_program_handle_t handle) { assert(handle.idx < 65000); ++destroys; }
void jce_shader_program_destroy(JceShaderHandle);
''' + registry + r'''
static JceShaderDeferredState device;
JceShaderDeferredState *jce_renderer_shader_deferred_state(void) { return &device; }
''' + release + r'''
int main(void)
{
    JcePakArchive pak = {7};
    JceShaderHandle a = defer_stock(&pak, "override", "pbr", "pbr", false);
    assert(a.idx >= JCE_DEFERRED_FIRST && loads == 0 && copies == 0);
    original = 19; /* source lifetime/content no longer matters after prepare */
    uint16_t concrete = jce_shader_deferred_resolve(a.idx);
    assert(concrete == 41 && loads == 1 && copies == 2);
    assert(jce_shader_deferred_resolve(a.idx) == concrete && loads == 1);
    jce_shader_program_destroy(a);
    assert(destroys == 1 && jce_shader_deferred_resolve(a.idx) == UINT16_MAX);
    original = 7;
    JceShaderHandle b = defer_stock(&pak, NULL, "pbr", "pbr", false);
    jce_shader_program_destroy(b);
    assert(loads == 1 && destroys == 1);
    fail = true;
    b = defer_stock(&pak, NULL, "pbr", "pbr", false);
    assert(jce_shader_deferred_resolve(b.idx) == UINT16_MAX && shader_loads == 3);
    assert(jce_shader_deferred_resolve(b.idx) == UINT16_MAX && shader_loads == 3);
    jce_shader_program_destroy(b);
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
    puts("deferred shader CPU lifecycle PASS");
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
                                    env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(directory / "probe.exe")], cwd=directory,
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("CPU lifecycle PASS", result.stdout)
