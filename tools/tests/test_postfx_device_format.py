"""Exercise the actual PostFX format selector across device and policy changes."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]

class PostFXDeviceFormatTests(unittest.TestCase):
    def test_device_replacement_and_live_hdr_policy(self):
        spec = importlib.util.spec_from_file_location("postfx_format_driver", ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        source = (ROOT / "engine/src/renderer/jce_postfx.c").read_text(encoding="utf-8")
        selector_start = source.index("static bgfx_texture_format_t postfx_color_format(")
        selector_end = source.index("\n}", selector_start) + 2
        selector = source[selector_start:selector_end]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef enum { BGFX_TEXTURE_FORMAT_RGBA8, BGFX_TEXTURE_FORMAT_RGBA16F } bgfx_texture_format_t;
typedef struct { uint32_t formats[2]; } bgfx_caps_t;
#define BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER 0x1000u
#define LOG_TAG "format-test"
#define LOG_WARN(...) ((void)0)
static bgfx_caps_t device;
static bool initialized = true;
static bool hdr = true;
static const bgfx_caps_t *bgfx_get_caps(void) { return initialized ? &device : NULL; }
static bool jce_render_pipeline_is_feature_enabled(const char *name) { (void)name; return hdr; }
''' + selector + r'''
int main(void)
{
 device.formats[BGFX_TEXTURE_FORMAT_RGBA16F] = BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER;
 assert(postfx_color_format() == BGFX_TEXTURE_FORMAT_RGBA16F);
 /* Replacement adapter/context cannot inherit the old device capability. */
 device.formats[BGFX_TEXTURE_FORMAT_RGBA16F] = 0;
 assert(postfx_color_format() == BGFX_TEXTURE_FORMAT_RGBA8);
 device.formats[BGFX_TEXTURE_FORMAT_RGBA16F] = BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER;
 assert(postfx_color_format() == BGFX_TEXTURE_FORMAT_RGBA16F);
 hdr = false; assert(postfx_color_format() == BGFX_TEXTURE_FORMAT_RGBA8);
 hdr = true; assert(postfx_color_format() == BGFX_TEXTURE_FORMAT_RGBA16F);
 initialized = false; assert(postfx_color_format() == BGFX_TEXTURE_FORMAT_RGBA8);
 initialized = true; assert(postfx_color_format() == BGFX_TEXTURE_FORMAT_RGBA16F);
 puts("PostFX device format PASS"); return 0;
}
'''
        base = ROOT / "build/postfx-format-tests"
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as temporary:
            directory = Path(temporary)
            file = directory / "probe.c"
            file.write_text(harness, encoding="utf-8")
            if driver.HOST == "windows":
                command = ["cl", "/nologo", "/TC", "/std:c11", "/W3", str(file), "/Fe:probe.exe", "/Fo:probe.obj"]
                env = driver.msvc_env(driver.resolve_target(None))
                executable = directory / "probe.exe"
            else:
                compiler = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
                if not compiler:
                    self.skipTest("no C compiler available for the production selector probe")
                executable = directory / "probe"
                command = [compiler, "-std=c99", str(file), "-o", str(executable)]
                env = None
            result = subprocess.run(command, cwd=directory, env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], cwd=directory, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PostFX device format PASS", result.stdout)

if __name__ == "__main__":
    unittest.main()
