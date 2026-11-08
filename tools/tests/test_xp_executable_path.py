"""XP OS adapter source/command controls; native compilation is explicitly separate."""
import contextlib
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
from tools.build import xp_os as subject
from tools.tests.test_xp_compat import sample_pe
ROOT=Path(__file__).resolve().parents[2]
HEADER=ROOT/"engine/src/os/platform/jce_executable_path_native_internal.h"


class XpExecutablePathControls(unittest.TestCase):
    @staticmethod
    def check_source(source):
        assert "const uint32_t unit = wide[i];" in source
        assert "unit >= 0xD800 && unit <= 0xDBFF" in source
        assert "++i >= size || wide[i] < 0xDC00 || wide[i] > 0xDFFF" in source
        assert "unit >= 0xDC00 && unit <= 0xDFFF" in source
        start=source.index("        wide[size] =")
        branch=source[start:source.index("        const int bytes =",start)]
        assert "if (!jce_fs_executable_path_utf16_valid_internal(wide, size)) {" in branch
        xp,modern=branch.split("#else",1)
        assert "const DWORD conversion_flags = 0;" in xp
        assert "const DWORD conversion_flags = WC_ERR_INVALID_CHARS;" in modern
        assert source.count("WideCharToMultiByte(CP_UTF8, conversion_flags,")==2
        assert "#define WC_ERR_INVALID_CHARS" not in source

    def test_actual_full_header_retains_xp_validation_and_modern_strict_flags(self):
        self.check_source(HEADER.read_text())

    def test_omitting_actual_validation_wire_fails_ordinary_source_assertion(self):
        source=HEADER.read_text()
        wire="if (!jce_fs_executable_path_utf16_valid_internal(wide, size)) {"
        self.assertEqual(source.count(wire),1)
        with self.assertRaises(AssertionError):
            self.check_source(source.replace(wire,"if (false) {"))

    @staticmethod
    def check_fixture_bytes(data):
        assert data.isascii(), "native fixture must contain ASCII source bytes only"
        assert bytes((39,92,39)) not in data, "unescaped C backslash character literal"
        for index,value in enumerate(data):
            if value==92:
                assert index+1<len(data) and data[index+1] in (10,110), "unexpected C source escape"
        assert b"const unsigned char expected_bmp[] = {0x43, 0x3a, 0x5c, 0x70, 0x2e, 0x65, 0x78, 0x65, 0};" in data
        assert b"const unsigned char expected_pair[] = {0x43, 0x3a, 0x5c, 0xf0, 0x9f, 0x98, 0x83, 0};" in data

    def test_native_fixture_ascii_bytes_and_known_utf8_byte_arrays(self):
        data=(ROOT/"tests/compatibility/xp_os/executable_path.c").read_bytes()
        self.check_fixture_bytes(data)
        with self.assertRaises(AssertionError):
            self.check_fixture_bytes(data+bytes((0xc3,0xb0)))
        with self.assertRaises(AssertionError):
            self.check_fixture_bytes(data.replace(b"0x5c",bytes((39,92,39)),1))

    def test_native_fixtures_own_surrogate_truncation_real_query_and_legacy_window_cases(self):
        fixture=(ROOT/"tests/compatibility/xp_os/executable_path.c").read_text()
        for wire in ("reject_surrogate(high, 4)","reject_surrogate(low, 4)",
                     "reject_surrogate(high_ascii, 5)","reject_surrogate(high_high, 6)",
                     "select_input(pair, 5)","select_input(bmp, 0)","module_calls == 8",
                     "conversion_calls == 0", "use_real_image = 1", "tiny[1] == 'y'",
                     "strlen(output) + 1", "last_flags == 0"):
            self.assertIn(wire,fixture)
        cmake=(ROOT/"tests/compatibility/xp_os/CMakeLists.txt").read_text()
        self.assertIn('"if (false) {"',cmake)
        self.assertIn('engine/src/os/platform/jce_window_adapter.c',cmake)
        window=(ROOT/"tests/compatibility/xp_os/window_adapter.c").read_text()
        self.assertIn('JCE_WINDOW_ADAPTER_CCD_UNAVAILABLE',window)
        self.assertIn('JCE_WINDOW_ADAPTER_INVALID_ARGUMENT',window)
        self.assertIn('memcmp(&output, &expected, sizeof output)',window)
        self.assertNotIn('CreateWindow',window)


class XpOsCommandControls(unittest.TestCase):
    def setUp(self):
        parent=ROOT/"build/validation/xp-os/cpu-tests"
        parent.mkdir(parents=True,exist_ok=True)
        temporary=tempfile.TemporaryDirectory(dir=parent)
        self.addCleanup(temporary.cleanup)
        self.root=Path(temporary.name)/"repo with spaces"
        paths=["tests/compatibility/xp_os/"+name for name in
               ("CMakeLists.txt","executable_path.c","window_adapter.c")]
        paths += ["engine/src/os/platform/"+name for name in
                  ("jce_executable_path_native_internal.h","jce_window_adapter.c",
                   "jce_window_adapter_internal.h","jce_entropy.c")]
        paths += ["engine/include/jce/os/core/jce_alloc.h","engine/include/jce/os/core/jce_defs.h",
                  "engine/include/jce/os/platform/jce_window.h","engine/include/jce/os/platform/jce_entropy.h",
                  "cmake/JCEWindowsTarget.cmake","cmake/toolchains/windows-xp-llvm-common.cmake",
                  "cmake/toolchains/windows-x64-xp-llvm.cmake","cmake/toolchains/windows-x86-xp-llvm.cmake",
                  "tools/build/xp_os.py","tools/build/xp_embedding.py","tools/build/xp_compat.py",
                  "tests/cmake/JceGeneratedFixture.cmake"]
        for relative in paths:
            target=self.root/relative
            target.parent.mkdir(parents=True,exist_ok=True)
            target.write_bytes((ROOT/relative).read_bytes())
        self.tc=self.root/"llvm-xp-msvcrt"
        self.tc.mkdir()
        self.programs={}
        for name in ("cmake","ninja"):
            target=self.root/(name+".exe")
            target.write_bytes(name.encode())
            self.programs[name]=str(target)
        self.calls=[]
        self.fail_build=False
        self.negative_stdout="JCE_XP_PATH_SURROGATE_REJECT_FAILED\nXP_PATH_CHECK_FAILED:42\n"
        self.negative_code=1
        self.addCleanup(patch.stopall)
        patch.object(subject.shutil,"which",side_effect=self.programs.get).start()
        patch.object(subject.compat,"preflight",side_effect=lambda root,arch:{
            "target":subject.compat.ARCH[arch]["triple"],"toolchain_root":str(root)}).start()
        patch.object(subject.compat,"audit_pe",return_value={"imports":{"advapi32.dll":["CryptGenRandom"]}}).start()
        patch.object(subject.subprocess,"run",side_effect=self.execute).start()

    def execute(self,command,**options):
        self.calls.append(command)
        self.assertIsInstance(command,list)
        self.assertNotIn("shell",options)
        if "-S" in command:
            output=Path(command[command.index("-B")+1])
            values={item[2:].split("=",1)[0].split(":",1)[0]:item.split("=",1)[1]
                    for item in command if item.startswith("-D")}
            arch="x86" if "windows-x86-" in output.name else "x64"
            values.update(CMAKE_HOME_DIRECTORY=command[command.index("-S")+1],CMAKE_GENERATOR="Ninja",
                CMAKE_C_COMPILER=(self.tc/"bin"/(subject.compat.ARCH[arch]["triple"]+"-clang.exe")).as_posix())
            (output/"CMakeCache.txt").write_text("".join(k+":STRING="+v+"\n" for k,v in values.items()))
            return subprocess.CompletedProcess(command,0,"configure port\n","configure stderr\n")
        if "--build" in command:
            if self.fail_build: return subprocess.CompletedProcess(command,8,"build port\n","compiler failure port\n")
            output=Path(command[command.index("--build")+1])
            (output/"out").mkdir(exist_ok=True)
            arch="x86" if "windows-x86-" in output.name else "x64"
            for target in subject.TARGETS: (output/"out"/(target+".exe")).write_bytes(sample_pe(arch))
            return subprocess.CompletedProcess(command,0,"build port\n","")
        target=Path(command[0]).stem
        marker=subject.TARGETS[target]
        return subprocess.CompletedProcess(command,0 if marker else self.negative_code,
            marker+"\n" if marker else self.negative_stdout,"")

    def run_build(self,arch="x64",**options):
        with contextlib.redirect_stdout(io.StringIO()):
            return subject.build(self.root,arch,self.tc,**options)

    def receipt(self,arch="x64"):
        attempt=sorted((self.root/"build/validation/xp-os"/arch).iterdir())[-1]
        return json.loads((attempt/"receipt.json").read_text()),attempt

    def test_reusable_parser_keeps_standalone_default_and_allows_unified_omission(self):
        argv=["--arch","x64","--toolchain-root",str(self.tc)]
        self.assertEqual(subject.build_parser().parse_args(argv).jobs,1)
        parser=subject.build_parser();parser.set_defaults(jobs=None)
        self.assertIsNone(parser.parse_args(argv).jobs)
        self.assertEqual(parser.parse_args(argv+["--jobs","2"]).jobs,2)

    def test_actual_cli_preserves_paths_arch_budget_and_local_flag(self):
        with patch.object(subject,"build") as build:
            self.assertEqual(subject.main(["--repo",str(self.root),"--arch","x86",
                "--toolchain-root",str(self.tc),"--jobs","2"]),0)
        build.assert_called_once_with(self.root,"x86",self.tc,2,False)

    def test_both_arch_paths_are_typed_forward_slashes_and_no_execution_by_default(self):
        for arch in ("x86","x64"):
            self.calls.clear()
            output=self.run_build(arch,jobs=2)
            self.assertEqual(output.name,f"windows-{arch}-xp-os-llvm-msvcrt")
            configure=self.calls[0]
            for key in ("CMAKE_MAKE_PROGRAM:FILEPATH","CMAKE_TOOLCHAIN_FILE:FILEPATH","JCE_XP_TOOLCHAIN_ROOT:PATH"):
                value=next(item.split("=",1)[1] for item in configure if item.startswith("-D"+key+"="))
                self.assertNotIn(chr(92),value)
            self.assertEqual(self.calls[1][-2:],["--parallel","2"])
            self.assertEqual(len(self.calls),2)
            receipt,_=self.receipt(arch)
            self.assertFalse(receipt["runtime_verified"])
            self.assertEqual(len(receipt["executables"]),3)

    def test_matching_cache_reuse_and_wrong_owner_refusal(self):
        output=self.run_build()
        self.run_build()
        owner=output/"jce-xp-embedding-owner.json"
        request=json.loads(owner.read_text());request["arch"]="x86";owner.write_text(json.dumps(request))
        self.calls.clear()
        with self.assertRaisesRegex(ValueError,"ownership tuple"):self.run_build()
        self.assertEqual(self.calls,[])

    def test_local_plan_requires_two_positive_markers_and_ordinary_negative_failure(self):
        self.run_build(run_local=True)
        receipt,_=self.receipt()
        self.assertTrue(receipt["runtime_verified"])
        self.assertFalse(receipt["xp_execution_verified"])
        self.assertTrue(receipt["local_results"]["jce_xp_executable_path_omitted"]["expected_failure_observed"])
        self.negative_stdout="XP_PATH_CHECK_FAILED:42\n"
        with self.assertRaises(subprocess.CalledProcessError):self.run_build(run_local=True)
        self.negative_code=0
        with self.assertRaises(ValueError):self.run_build(run_local=True)

    def test_compile_failure_cannot_satisfy_negative_runtime_control(self):
        self.fail_build=True
        with self.assertRaises(subprocess.CalledProcessError):self.run_build(run_local=True)
        receipt,attempt=self.receipt()
        self.assertEqual(receipt["state"],"FAILED")
        self.assertFalse(receipt["runtime_verified"])
        self.assertEqual(len(self.calls),2)
        self.assertIn("compiler failure port",(attempt/"01-stderr.log").read_text())

    def test_invalid_jobs_never_configures(self):
        for jobs in (0,257,True,"2"):
            with self.assertRaises(ValueError):self.run_build(jobs=jobs)
        self.assertEqual(self.calls,[])


if __name__=="__main__":unittest.main()
