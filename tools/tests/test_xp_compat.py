"""A1 PE audit acceptance + negative controls (no XP host or Clang required)."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("jce_a1_xp_compat", ROOT / "tools/build/xp_compat.py")
XP = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(XP)


def sample_pe(arch="x64", os_version=None, dlls=None, symbol=None, delay=False):
    """Generate a tiny, syntactically valid PE import table, never executable."""
    b = bytearray(4096)
    b[:2] = b"MZ"
    struct.pack_into("<I", b, 0x3c, 0x80)
    b[0x80:0x84] = b"PE\x00\x00"
    is64 = arch == "x64"
    opt_size = 240 if is64 else 224
    struct.pack_into("<HHIIIHH", b, 0x84,
                     0x8664 if is64 else 0x14c, 1, 0, 0, 0, opt_size, 0)
    opt = 0x98
    struct.pack_into("<H", b, opt, 0x20b if is64 else 0x10b)
    version = os_version if os_version else ((5, 2) if is64 else (5, 1))
    struct.pack_into("<HH", b, opt + 40, *version)
    struct.pack_into("<HH", b, opt + 48, *version)
    struct.pack_into("<I", b, opt + (108 if is64 else 92), 16)
    start = opt + (112 if is64 else 96)
    struct.pack_into("<II", b, start + 8, 0x1000, 100)
    if delay:
        struct.pack_into("<II", b, start + 13 * 8, 0x1400, 32)
    so = opt + opt_size
    b[so:so+8] = b".rdata\0\0"
    struct.pack_into("<IIII", b, so + 8, 0x800, 0x1000, 0x800, 0x200)

    def at(rva):
        return 0x200 + rva - 0x1000

    if dlls is None:
        dlls = {"KERNEL32.dll": "GetProcAddress",
                "msvcrt.dll": "printf", "advapi32.dll": "CryptGenRandom"}
    for idx, (dll, fn) in enumerate(dlls.items()):
        name_rva = 0x1100 + idx * 0x30
        thunk_rva = 0x1200 + idx * 0x30
        hint_name_rva = 0x1350 + idx * 0x40
        struct.pack_into("<IIIII", b, at(0x1000 + idx * 20),
                         thunk_rva, 0, 0, name_rva, thunk_rva)
        data = dll.encode("ascii") + b"\0"
        b[at(name_rva): at(name_rva) + len(data)] = data
        fn = symbol if dll.lower() == "advapi32.dll" and symbol else fn
        data = b"\x00\x00" + fn.encode("ascii") + b"\0"
        b[at(hint_name_rva):at(hint_name_rva)+len(data)] = data
        struct.pack_into("<Q" if is64 else "<I", b, at(thunk_rva), hint_name_rva)
    return bytes(b)


class XpCompatTests(unittest.TestCase):
    def audit(self, b, arch="x64"):
        with tempfile.TemporaryDirectory() as temp:
            p = Path(temp) / "probe.exe"
            p.write_bytes(b)
            return XP.audit_pe(p, arch)

    def test_x64_valid_static_imports(self):
        result = self.audit(sample_pe())
        self.assertEqual(result["os_version"], "5.2")
        self.assertEqual(result["imports"]["advapi32.dll"], ["CryptGenRandom"])

    def test_x86_valid_static_imports(self):
        result = self.audit(sample_pe("x86"), "x86")
        self.assertEqual(result["os_version"], "5.1")

    def test_x86_static_unwind_accepts_only_xp_psapi_export(self):
        dlls = {"kernel32.dll": "GetProcAddress", "msvcrt.dll": "printf",
                "advapi32.dll": "CryptGenRandom", "psapi.dll": "EnumProcessModules"}
        result = self.audit(sample_pe("x86", dlls=dlls), "x86")
        self.assertEqual(result["imports"]["psapi.dll"], ["EnumProcessModules"])
        dlls["psapi.dll"] = "EnumProcessModulesEx"
        with self.assertRaisesRegex(ValueError, "unverified XP probe PSAPI"):
            self.audit(sample_pe("x86", dlls=dlls), "x86")

    def test_modern_kernel_psapi_alias_rejected(self):
        dlls = {"kernel32.dll": "K32EnumProcessModules", "msvcrt.dll": "printf",
                "advapi32.dll": "CryptGenRandom"}
        with self.assertRaisesRegex(ValueError, "post-XP"):
            self.audit(sample_pe(dlls=dlls))

    def test_wrong_arch_rejected(self):
        with self.assertRaisesRegex(ValueError, "PE machine"):
            self.audit(sample_pe("x86"), "x64")

    def test_wrong_os_floor_rejected(self):
        with self.assertRaisesRegex(ValueError, "expected"):
            self.audit(sample_pe(os_version=(6, 1)))

    def test_vista_api_rejected(self):
        with self.assertRaisesRegex(ValueError, "post-XP"):
            self.audit(sample_pe(symbol="GetTickCount64"))

    def test_unknown_dll_rejected(self):
        with self.assertRaisesRegex(ValueError, "unexpected XP probe DLL"):
            self.audit(sample_pe(dlls={"kernel32.dll": "GetProcAddress", "msvcrt.dll": "printf", "bcrypt.dll": "BCryptGenRandom"}))

    def test_delay_import_not_falsely_approved(self):
        with self.assertRaisesRegex(ValueError, "delay imports"):
            self.audit(sample_pe(delay=True))

    def test_cryptoapi_required(self):
        with self.assertRaisesRegex(ValueError, "CryptoAPI"):
            self.audit(sample_pe(dlls={"kernel32.dll": "GetProcAddress", "msvcrt.dll": "printf"}))

    def test_truncated_pe_rejected(self):
        with self.assertRaises((ValueError, struct.error)):
            self.audit(sample_pe()[:200])

    def test_registry_does_not_route_xp_into_msvc_conan(self):
        spec = importlib.util.spec_from_file_location("jce_driver_xp_compat", ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        args = driver.build_parser().parse_args(["xp-probe", "--arch", "x64"])
        self.assertIs(args.func, driver.cmd_xp_probe)
        self.assertNotIn("windows-x64-xp-llvm", driver.TARGETS)


if __name__ == "__main__":
    unittest.main()
