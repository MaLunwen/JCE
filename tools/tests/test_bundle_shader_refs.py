"""Exercise the production shader dependency selection with actual availability controls."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]

class BundleShaderReferenceTests(unittest.TestCase):
    def test_actual_variants_and_missing_required_alias(self):
        spec = importlib.util.spec_from_file_location("postfx_format_driver", ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        source = (ROOT / "engine/src/resource/jce_bundle_deps.c").read_text(encoding="utf-8")
        start = source.index('                if (strcmp(key, "customProgramVs") == 0')
        end = source.index("\n            }\n            if (key && cJSON_IsObject", start)
        branch = source[start:end]
        helper = ""
        if "static bool harvest_shader_program(" in source:
            start = source.index("static bool harvest_shader_program(")
            helper = source[start:source.index("static bool walk(", start)]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct { const char *valuestring; } cJSON;
typedef struct { unsigned count; char paths[16][1024]; } JceBundleDepList;
typedef bool (*JceBundleShaderReadableFn)(const char *,void *);
static const char *available;
static bool fail_push;
static JceBundleDepList scanned;
static bool readable(const char *path,void *user) {(void)user;return available && strcmp(path,available)==0;}
static const char *sibling_bundle_tag(const cJSON *node) {(void)node;return "force-shared";}
static int list_push(JceBundleDepList *out,const char *path,const char *tag)
{ if(fail_push)return 0; assert(!strcmp(tag,"force-shared"));assert(out->count<16);snprintf(out->paths[out->count++],1024,"%s",path); return 1; }
''' + helper + r'''
static bool scan(const char *key,const char *path)
{
 memset(&scanned,0,sizeof(scanned));JceBundleDepList *out=&scanned;
 cJSON child_data={path};const cJSON *child=&child_data;const cJSON *node=&child_data;
 JceBundleShaderReadableFn shader_readable=readable;void *shader_user=NULL;
''' + branch + r'''
 return true;
}
int main(void)
{
 available="shaders/fs_stock_dx11.bin";
 assert(scan("customProgramFs","shaders/fs_stock.bin"));
 JceBundleDepList result=scanned;
 assert(result.count==1 && !strcmp(result.paths[0],available));
 available=NULL;assert(scan("customProgramFs","shaders/fs_unknown.bin"));result=scanned;
 assert(result.count==1 && !strcmp(result.paths[0],"shaders/fs_unknown.bin"));
 available="shaders/user.bin";assert(scan("customProgramVs",available));result=scanned;
 assert(result.count==1 && !strcmp(result.paths[0],available));
 available="shaders/user_dx11.bin";assert(scan("customProgramVs",available));result=scanned;
 assert(result.count==1 && !strcmp(result.paths[0],available));
 available=NULL;assert(scan("albedoTex","textures/missing.png"));result=scanned;
 assert(result.count==1 && !strcmp(result.paths[0],"textures/missing.png"));
 fail_push=true;assert(!scan("customProgramFs","shaders/required.bin"));
 assert(!scan("albedoTex","textures/required.png"));
 puts("Bundle shader refs PASS");return 0;
}
'''
        base = ROOT / "build/bundle-shader-tests"
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
            self.assertIn("Bundle shader refs PASS", result.stdout)

if __name__ == "__main__":
    unittest.main()
