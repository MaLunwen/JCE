"""Exercise the actual stock resolver and real path normalization with namespace/bounds controls."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]

class StockShaderResolverTests(unittest.TestCase):
    def test_namespace_bounds_size_and_actual_file(self):
        spec = importlib.util.spec_from_file_location("postfx_format_driver", ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        source = (ROOT / "tools/jce_bundle_pack.c").read_text(encoding="utf-8")
        selector = source[source.index("static bool resolve_stock_shader("):source.index("static void pack_log(")]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jce/os/core/jce_path.h>
#include <jce/os/core/jce_alloc.h>
/* Counted allocator boundary required by the actual path owner. Path parsing
 * and overlap handling are compiled from jce_path.c, never reproduced here. */
static unsigned path_allocations, path_frees;
void *jce_malloc(size_t size) { ++path_allocations; return malloc(size); }
void jce_free(void *data) { if (data) ++path_frees; free(data); }
static bool jce_fs_host_exists_file(const char *path) {return !strcmp(path,"fixture/stock/fs_test_dx11.bin");}
''' + selector + r'''
int main(void)
{
 char out[1024];
 assert(resolve_stock_shader("shaders/fs_test_dx11.bin",out,sizeof(out),"fixture/stock"));
 assert(!strcmp(out,"fixture/stock/fs_test_dx11.bin"));
 assert(resolve_stock_shader("./shaders/../shaders/fs_test_dx11.bin",out,sizeof(out),"fixture/stock"));
 assert(!resolve_stock_shader("shaders/../../fs_test_dx11.bin",out,sizeof(out),"fixture/stock"));
 assert(!resolve_stock_shader("@ABSOLUTE_SHADER@",out,sizeof(out),"fixture/stock"));
 assert(!resolve_stock_shader("textures/fs_test_dx11.bin",out,sizeof(out),"fixture/stock"));
 assert(!resolve_stock_shader("shaders/unknown.bin",out,sizeof(out),"fixture/stock"));
 assert(!resolve_stock_shader("shaders/fs_test_dx11.json",out,sizeof(out),"fixture/stock"));
 assert(!resolve_stock_shader("shaders/fs_test_dx11.bin",out,4,"fixture/stock"));
 assert(path_allocations==0 && path_frees==0);
 /* This real forward-overlap branch proves the allocator ports are connected
  * and owned snapshots are released; ordinary resolver requests remain noheap. */
 char overlap[64]="shaders/./fs_test_dx11.bin";
 assert(jce_path_normalize(overlap+1,sizeof(overlap)-1,overlap));
 assert(!strcmp(overlap+1,"shaders/fs_test_dx11.bin"));
 assert(path_allocations==1 && path_frees==1);
 puts("Stock shader resolver PASS");return 0;
}
'''
        harness = harness.replace('@ABSOLUTE_SHADER@', (ROOT / 'shaders/fs_test_dx11.bin').as_posix())
        base = ROOT / "build/tests/stock-shader-tests"
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as temporary:
            directory = Path(temporary)
            file = directory / "probe.c"
            file.write_text(harness, encoding="utf-8")
            if driver.HOST == "windows":
                command = ["cl", "/nologo", "/TC", "/std:c11", "/W3", "/DJCE_BUILDING_ENGINE=1", "/I" + str(ROOT / "engine/include"), str(file), str(ROOT / "engine/src/os/core/jce_path.c"), "/Fe:probe.exe"]
                env = driver.msvc_env(driver.resolve_target(None))
                executable = directory / "probe.exe"
            else:
                compiler = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
                if not compiler:
                    self.skipTest("no C compiler available for the production selector probe")
                executable = directory / "probe"
                command = [compiler, "-std=c99", "-DJCE_BUILDING_ENGINE=1", "-I" + str(ROOT / "engine/include"), str(file), str(ROOT / "engine/src/os/core/jce_path.c"), "-o", str(executable)]
                env = None
            result = subprocess.run(command, cwd=directory, env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], cwd=directory, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("Stock shader resolver PASS", result.stdout)

if __name__ == "__main__":
    unittest.main()
