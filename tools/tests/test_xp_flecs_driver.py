"""CPU-only controls for the actual Flecs probe driver; no native process runs."""
import contextlib
import io
import json
from pathlib import Path
import stat
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from tools.build import xp_flecs as subject
from tools.tests.test_xp_compat import sample_pe
ROOT=Path(__file__).resolve().parents[2]


class XpFlecsDriverTests(unittest.TestCase):
    def setUp(self):
        parent=ROOT/"build/validation/xp-flecs/cpu-driver-tests"
        parent.mkdir(parents=True,exist_ok=True)
        temporary=tempfile.TemporaryDirectory(dir=parent)
        self.addCleanup(temporary.cleanup)
        self.root=Path(temporary.name)/"repo with spaces"
        relatives=["tests/compatibility/xp_flecs/"+name for name in
                   ("CMakeLists.txt","main.c","bridge_control.c","atomic_control.c")]
        relatives += ["engine/src/middleware/scene/jce_scene_flecs_os.c",
            "engine/src/middleware/scene/jce_scene_flecs_os.h",
            "engine/src/os/platform/jce_native_atomic_integer_internal.h",
            "engine/src/os/platform/jce_native_atomic_word_internal.h",
            "engine/src/os/platform/jce_entropy.c","engine/src/os/core/jce_memory.h",
            "engine/include/jce/os/core/jce_defs.h","engine/include/jce/jce_compat.h",
            "engine/include/jce/os/core/jce_alloc.h","engine/include/jce/os/core/jce_str.h",
            "engine/include/jce/os/core/jce_thread.h","engine/include/jce/os/core/jce_timer.h",
            "engine/include/jce/os/platform/jce_entropy.h","tools/build/xp_flecs.py",
            "tools/build/xp_embedding.py","tools/build/xp_compat.py",
            "cmake/toolchains/windows-x86-xp-llvm.cmake","cmake/toolchains/windows-x64-xp-llvm.cmake",
            "cmake/toolchains/windows-xp-llvm-common.cmake"]
        for relative in relatives:
            target=self.root/relative;target.parent.mkdir(parents=True,exist_ok=True)
            target.write_bytes((ROOT/relative).read_bytes())
        self.include=self.root/"explicit Flecs headers"
        (self.include/"flecs").mkdir(parents=True)
        (self.include/"flecs.h").write_bytes(b"counted header input, never compiled")
        (self.include/"flecs/os_api.h").write_bytes(b"counted OS callback header")
        self.tc=self.root/"llvm-mingw-xp-22.1.7-msvcrt"
        (self.tc/"bin").mkdir(parents=True)
        for arch,triple in (("x86","i686-w64-mingw32"),("x64","x86_64-w64-mingw32")):
            for name in ("clang","clang++"):
                (self.tc/"bin"/(triple+"-"+name+".exe")).write_bytes((arch+name).encode())
            stem="i686" if arch=="x86" else "x86_64"
            (self.tc/"bin"/(stem+"-w64-windows-gnu.cfg")).write_bytes(b"counted target config")
        self.programs={}
        for name in ("cmake","ninja"):
            target=self.root/(name+".exe");target.write_bytes(name.encode());self.programs[name]=str(target)
        self.calls=[];self.fail_build=False;self.wrong_marker=False;self.mutation=None;self.pe_arch=None;self.fail_case="no-failed-case"
        self.original_shared_execute=subject.shared.compat._execute
        self.private=[]
        actual_factory=subject._compat
        def factory():
            module=actual_factory();self.private.append(module);return module
        self.addCleanup(patch.stopall)
        patch.object(subject,"_compat",side_effect=factory).start()
        patch.object(subject.shutil,"which",side_effect=self.programs.get).start()
        patch.object(subject.subprocess,"run",side_effect=self.execute).start()
        patch.object(subject.cache_lock,"cache_locks",side_effect=lambda *args,**kwargs:contextlib.nullcontext()).start()

    def output(self,arch="x64"):
        return self.root/"build/compat"/("windows-"+arch+"-xp-flecs-llvm-msvcrt")

    def execute(self,command,**options):
        self.calls.append(command)
        self.assertIsInstance(command,list);self.assertNotIn("shell",options)
        if "--version" in command:
            stem="i686" if "i686" in command[0] else "x86_64"
            return subprocess.CompletedProcess(command,0,"clang version 22.1.7\nTarget: "+stem+"-w64-windows-gnu\n","")
        if "-dM" in command:
            macros="#define __clang__ 1\n#define __MINGW32__ 1\n#define _WIN32 1\n"
            if "x86_64" in command[0]:macros+="#define _WIN64 1\n"
            return subprocess.CompletedProcess(command,0,macros,"")
        if "-S" in command:
            return subprocess.CompletedProcess(command,0,"configure port\n","configure stderr\n")
        if "--build" in command:
            if self.fail_build:return subprocess.CompletedProcess(command,9,"build stdout\n","build failed\n")
            output=Path(command[command.index("--build")+1]);(output/"out").mkdir(exist_ok=True)
            arch="x86" if "windows-x86-" in output.name else "x64"
            for target in subject.PLAN:(output/"out"/(target+".exe")).write_bytes(sample_pe(self.pe_arch or arch))
            if self.mutation=="headers":
                with (self.include/"flecs/os_api.h").open("ab") as stream:stream.write(b" changed")
            elif self.mutation=="source":
                with (self.root/"engine/src/middleware/scene/jce_scene_flecs_os.c").open("ab") as stream:stream.write(b" changed")
            return subprocess.CompletedProcess(command,0,"build port\n","")
        target=Path(command[0]).stem
        case=command[1] if len(command)>1 else None
        marker="JCE_XP_FLECS_ATOMICS_OK" if case is None else "JCE_XP_FLECS_BRIDGE_OK case="+case
        if self.wrong_marker:marker+="-wrong-case"
        if self.mutation=="executable":
            with Path(command[0]).open("ab") as stream:stream.write(b" changed")
        return subprocess.CompletedProcess(command,5 if self.fail_case==case else 0,marker+"\n","")

    def run_build(self,arch="x64",**options):
        with contextlib.redirect_stdout(io.StringIO()):
            return subject.build(self.root,arch,self.tc,self.include,**options)

    def receipt(self,arch="x64"):
        attempt=sorted((self.root/"build/validation/xp-flecs"/arch).iterdir())[-1]
        return json.loads((attempt/"receipt.json").read_text()),attempt

    def test_actual_cli_requires_arch_preserves_inputs_and_default_budget(self):
        argv=["--repo",str(self.root),"--arch","x86","--toolchain-root",str(self.tc),"--flecs-include",str(self.include)]
        with patch.object(subject,"build") as build:self.assertEqual(subject.main(argv),0)
        build.assert_called_once_with(self.root,"x86",self.tc,self.include,1,False,False)
        with contextlib.redirect_stderr(io.StringIO()),self.assertRaises(SystemExit):
            subject.build_parser().parse_args(["--toolchain-root",str(self.tc),"--flecs-include",str(self.include)])

    def test_negative_controls_require_an_explicit_local_execution_request(self):
        with self.assertRaisesRegex(ValueError, "require explicit --run-local"):
            self.run_build(negative_controls=True)
        self.assertFalse(any("-S" in command for command in self.calls))
        result,_ = self.receipt()
        self.assertEqual(result["state"], "FAILED")
        parsed=subject.build_parser().parse_args([
            "--arch","x64","--toolchain-root",str(self.tc),"--flecs-include",str(self.include),
            "--negative-controls","--run-local"])
        self.assertTrue(parsed.negative_controls and parsed.run_local)

    def test_both_archs_have_independent_caches_typed_paths_and_explicit_budget(self):
        for arch in ("x86","x64"):
            self.calls.clear();output=self.run_build(arch,jobs=2)
            self.assertEqual(output,self.output(arch))
            configure=next(argv for argv in self.calls if "-S" in argv)
            expected={"CMAKE_MAKE_PROGRAM:FILEPATH":Path(self.programs["ninja"]),
                "CMAKE_TOOLCHAIN_FILE:FILEPATH":self.root/"cmake/toolchains"/("windows-"+arch+"-xp-llvm.cmake"),
                "JCE_XP_TOOLCHAIN_ROOT:PATH":self.tc,"JCE_XP_FLECS_INCLUDE:PATH":self.include}
            for key,value in expected.items():
                self.assertIn("-D"+key+"="+value.as_posix(),configure)
            for flag in ("-S","-B"):self.assertNotIn(chr(92),configure[configure.index(flag)+1])
            build=next(argv for argv in self.calls if "--build" in argv)
            self.assertEqual(build[-2:],["--parallel","2"])
            self.assertEqual(len(self.calls),4,"default route unexpectedly executed fixture")
            receipt,_=self.receipt(arch)
            self.assertFalse(receipt["xp_execution_verified"])
            self.assertFalse(receipt["editor_or_graphics_verified"])
            self.assertFalse(receipt["local_execution_verified"])
            self.assertEqual(len(receipt["executables"]),4)

    def test_matching_cache_reuses_and_changed_header_or_owner_refuses_before_configure(self):
        output=self.run_build();self.run_build()
        marker=output/"xp-flecs-owner.json";old=marker.read_bytes()
        (self.include/"flecs/os_api.h").write_bytes(b"changed explicit source header")
        self.calls.clear()
        with self.assertRaisesRegex(ValueError,"ownership mismatch"):self.run_build()
        self.assertFalse(any("-S" in argv for argv in self.calls));self.assertEqual(marker.read_bytes(),old)
        request=json.loads(old);request["arch"]="x86";marker.write_text(json.dumps(request))
        with self.assertRaisesRegex(ValueError,"ownership mismatch"):self.run_build()

    def test_nonempty_unowned_output_is_preserved(self):
        output=self.output();output.mkdir(parents=True);foreign=output/"foreign.txt";foreign.write_text("other owner")
        with self.assertRaisesRegex(ValueError,"nonempty and unowned"):self.run_build()
        self.assertEqual(foreign.read_text(),"other owner")
        self.assertFalse(any("-S" in argv for argv in self.calls))

    def test_local_execution_uses_exact_case_markers_and_never_claims_xp_or_editor(self):
        self.run_build(run_local=True)
        receipt,_=self.receipt();self.assertTrue(receipt["local_execution_verified"])
        cases=[item["case"] for item in receipt["local_results"]]
        for case in ("smoke","thread-fast","rollback","foreign-empty","late-defaults","refused-set","contended","modern","track",None):
            self.assertIn(case,cases)
        self.assertEqual(len(cases),len(set(cases)))
        self.assertFalse(receipt["xp_execution_verified"]);self.assertFalse(receipt["editor_or_graphics_verified"])
        self.wrong_marker=True
        with self.assertRaisesRegex(ValueError,"exact scenario marker"):self.run_build(run_local=True)
        self.assertFalse(self.receipt()[0]["local_execution_verified"])

    def test_nonzero_case_cannot_pass_even_with_a_correct_marker(self):
        self.fail_case="thread-fast"
        with self.assertRaises(subprocess.CalledProcessError):self.run_build(run_local=True)
        receipt,_=self.receipt()
        self.assertEqual(receipt["state"],"FAILED")
        self.assertFalse(receipt["local_execution_verified"])
        self.assertEqual(receipt["commands"][-1]["returncode"],5)

    def test_executable_change_after_marker_refuses_success(self):
        self.mutation="executable"
        with self.assertRaisesRegex(ValueError,"fixture changed during local execution"):
            self.run_build(run_local=True)
        self.assertEqual(self.receipt()[0]["state"],"FAILED")

    def test_changed_source_or_headers_during_validation_refuses_success(self):
        for mutation in ("source","headers"):
            self.mutation=mutation
            if mutation=="headers":
                # Use a fresh architecture cache after the source-change attempt.
                arch="x86"
            else:arch="x64"
            with self.assertRaisesRegex(ValueError,"input changed"):self.run_build(arch)
            self.assertEqual(self.receipt(arch)[0]["state"],"FAILED")

    def test_build_failure_keeps_streams_and_does_not_run_any_case(self):
        self.fail_build=True
        with self.assertRaises(subprocess.CalledProcessError):self.run_build(run_local=True)
        receipt,attempt=self.receipt()
        self.assertEqual(receipt["state"],"FAILED");self.assertEqual(len(self.calls),4)
        self.assertEqual((attempt/"03-stdout.log").read_text(),"build stdout\n")
        self.assertEqual((attempt/"03-stderr.log").read_text(),"build failed\n")

    def test_actual_pe_reader_refuses_other_arch_fixture(self):
        self.pe_arch="x86"
        with self.assertRaisesRegex(ValueError,"PE machine"):self.run_build()
        self.assertEqual(self.receipt()[0]["state"],"FAILED")

    def test_preflight_uses_private_module_and_leaves_shared_execute_unchanged(self):
        self.run_build();self.run_build("x86")
        self.assertIs(subject.shared.compat._execute,self.original_shared_execute)
        self.assertEqual(len(self.private),2);self.assertIsNot(self.private[0],self.private[1])
        self.assertTrue(all(module is not subject.shared.compat for module in self.private))

    def test_missing_required_production_source_refuses_before_configure(self):
        (self.root/"engine/src/middleware/scene/jce_scene_flecs_os.c").unlink()
        with self.assertRaisesRegex(ValueError,"input is missing"):self.run_build()
        self.assertFalse(any("-S" in argv for argv in self.calls))

    def test_invalid_jobs_never_launch_or_create_cache(self):
        for jobs in (0,257,True,"2"):
            with self.assertRaises(ValueError):self.run_build(jobs=jobs)
        self.assertEqual(self.calls,[]);self.assertFalse(self.output().exists())

    def test_header_inventory_has_per_file_and_total_byte_limits(self):
        actual_stat=Path.stat
        def oversized(path,*args,**kwargs):
            result=actual_stat(path,*args,**kwargs)
            if path==self.include/"flecs.h":return SimpleNamespace(st_mode=stat.S_IFREG|0o600,st_size=16*1024*1024+1)
            return result
        with patch.object(Path,"stat",oversized),self.assertRaisesRegex(ValueError,"budget"):
            subject._headers(self.include)
        with patch.object(subject.shared,"_identity",return_value={"size":64*1024*1024+1,"sha256":"counted"}),self.assertRaisesRegex(ValueError,"budget"):
            subject._headers(self.include)

    def test_header_inventory_rejects_alias_observation_before_hashing(self):
        real_is_symlink=Path.is_symlink
        alias=self.include/"flecs/os_api.h"
        def aliased(path):return path==alias or real_is_symlink(path)
        with patch.object(Path,"is_symlink",aliased),self.assertRaisesRegex(ValueError,"redirected"):
            subject._headers(self.include)

    def test_header_entry_budget_is_incremental_and_includes_directories(self):
        observed=[]
        def entries(*args,**kwargs):
            for index in range(5000):
                observed.append(index)
                yield self.include/("directory%04d"%index)
        with patch.object(Path,"rglob",entries),patch.object(subject.shared,"_ordinary",side_effect=lambda path:path),self.assertRaisesRegex(ValueError,"budget"):
            subject._headers(self.include)
        self.assertLessEqual(len(observed),4097,"unbounded inventory was materialized before admission")


if __name__=="__main__":unittest.main()
