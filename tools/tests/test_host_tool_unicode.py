"""UTF-8 host-tool closure; optional real binaries via JCE_HOST_TOOLS_TEST_DIR."""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import unittest
import uuid
import wave
import zlib

ROOT = Path(__file__).resolve().parents[2]


class SourceContracts(unittest.TestCase):
    def test_shared_entry_and_utf8_streams(self):
        for name in ("jce_pak", "jce_cook", "jce_bin2obj"):
            source = (ROOT/"tools"/(name+".c")).read_text()
            self.assertIn("int jce_tool_main(", source)
            for invalid in ("FindFirstFileA", "opendir(", "FILE *f = fopen(", "SDL3/"):
                self.assertNotIn(invalid, source)
        pak = (ROOT/"tools/jce_pak.c").read_text()
        self.assertIn("jce_fs_host_write_all_atomic", pak)
        self.assertIn("if (same) return;", pak)
        self.assertIn("jce_tool_fread(hdr, 1, JARC_HEADER_SIZE", pak)
        io = (ROOT/"tools/cli/jce_tool_io_sdl.c").read_text()
        self.assertIn("count > SIZE_MAX / size", io)
        self.assertNotIn("windows.h", io)


class NativeUnicodeControls(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        supplied = os.environ.get("JCE_HOST_TOOLS_TEST_DIR")
        if not supplied:
            raise unittest.SkipTest("explicit host-tool directory required for native evidence")
        cls.tools = Path(supplied).resolve()
        cls.root = ROOT/"build/tests/host-tool-unicode"/uuid.uuid4().hex[:12]
        cls.root.mkdir(parents=True)
        cls.records = []
        cls.identities = {}
        for name in ("jce_pak", "jce_cook", "jce_bin2obj"):
            path = cls.tools/(name+(".exe" if os.name == "nt" else ""))
            if not path.is_file():
                raise RuntimeError("missing explicit host tool: " + str(path))
            cls.identities[name] = hashlib.sha256(path.read_bytes()).hexdigest()

    @classmethod
    def tearDownClass(cls):
        (cls.root/"result.json").write_text(json.dumps({"identities": cls.identities,
            "commands": cls.records, "scope": "real CPU host-tool UTF-8 I/O, no GPU or SDK refresh"},
            indent=2, ensure_ascii=False)+"\n", encoding="utf-8", newline="\n")
        print("Unicode evidence:", cls.root/"result.json", flush=True)

    def run_tool(self, name, *args, success=True):
        exe = self.tools/(name+(".exe" if os.name == "nt" else ""))
        self.assertEqual(hashlib.sha256(exe.read_bytes()).hexdigest(), self.identities[name])
        command = [str(exe), *map(str, args)]
        result = subprocess.run(command, capture_output=True, timeout=120)
        index = len(self.records)
        for stream in ("stdout", "stderr"):
            (self.root/(str(index)+"."+stream+".log")).write_bytes(getattr(result, stream))
        self.records.append({"command": command, "returncode": result.returncode,
            "expected_success": success})
        if success:
            self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8", "replace"))
        else:
            self.assertNotEqual(result.returncode, 0)
        return result

    def test_cook_pack_inspect_incremental_closed_loop(self):
        project = self.root/"项目 Ω 空格"
        raw, cooked = project/"原始 资源", project/"烘焙 输出"
        raw.mkdir(parents=True)
        audio = raw/"音效 你好.wav"
        with wave.open(str(audio), "wb") as stream:
            stream.setnchannels(1); stream.setsampwidth(2); stream.setframerate(8000)
            stream.writeframes(struct.pack("<h", 1000)*256)
        single = project/"单音效.jceasset"
        self.run_tool("jce_cook", audio, single, "--platform", "windows")
        self.assertEqual(single.read_bytes()[:4], b"JCEA")
        self.run_tool("jce_cook", "--batch", raw, cooked, "--platform", "windows")
        self.assertTrue(any(p.is_file() for p in cooked.rglob("*")))
        archive, header, manifest, obj = [project/name for name in
            ("归档.pak", "嵌入.h", "清单.cmake", "归档.obj")]
        options = ("--resource-dir", cooked, "--pak-file", archive,
            "--header-file", header, "--manifest-file", manifest,
            "--obj-file", obj, "--obj-format", "coff", "--obj-arch", "x64")
        self.run_tool("jce_pak", *options)
        files = (archive, header, manifest, obj)
        before = {str(p): (hashlib.sha256(p.read_bytes()).hexdigest(), p.stat().st_mtime_ns)
                  for p in files}
        self.run_tool("jce_pak", *options)
        for p in files:
            self.assertEqual((hashlib.sha256(p.read_bytes()).hexdigest(), p.stat().st_mtime_ns), before[str(p)])
        bom = project/"校验 清单.json"
        self.run_tool("jce_pak", "--inspect", archive, "--verify", "--json", bom)
        self.assertIsInstance(json.loads(bom.read_text()), dict)
        self.assertIn(archive.read_bytes(), obj.read_bytes())

    def test_unicode_texture_and_terrain_inputs(self):
        directory = self.root/"图像 你好"
        directory.mkdir()
        image = directory/"高度 Ω.png"
        def chunk(kind, data):
            return struct.pack(">I", len(data))+kind+data+struct.pack(">I", zlib.crc32(kind+data)&0xffffffff)
        # Real grayscale PNG keeps the fixture independent of external assets.
        header = struct.pack(">IIBBBBB", 17, 17, 8, 0, 0, 0, 0)
        rows = b"".join(b"\0"+bytes(range(17)) for _ in range(17))
        image.write_bytes(b"\x89PNG\r\n\x1a\n"+chunk(b"IHDR", header)+
            chunk(b"IDAT", zlib.compress(rows))+chunk(b"IEND", b""))
        texture = directory/"纹理.jceasset"
        self.run_tool("jce_cook", image, texture, "--rgba8")
        self.assertEqual(texture.read_bytes()[:4], b"JCEA")
        terrain = directory/"地形.terrain.json"
        self.run_tool("jce_cook", "--terrain", image, "--out", terrain, "--verts", "16")
        self.assertIsInstance(json.loads(terrain.read_text()), dict)
        tiled = directory/"平铺.jter"
        self.run_tool("jce_cook", "--terrain", image, "--tiled", "--tile", "8", "--out", tiled)
        self.assertGreater(tiled.stat().st_size, 0)

    def test_model_conversion_and_corrupt_input(self):
        directory = self.root/"模型 Ω"
        directory.mkdir()
        source = directory/"三角形 你好.obj"
        source.write_text("v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nf 1//1 2//1 3//1\n",
                          encoding="utf-8", newline="\n")
        output = directory/"转换.glb"
        self.run_tool("jce_cook", "--convert-model", source, "--out", output)
        data = output.read_bytes()
        self.assertEqual(struct.unpack_from("<4sII", data), (b"glTF", 2, len(data)))
        length, kind = struct.unpack_from("<II", data, 12)
        self.assertEqual(kind, 0x4e4f534a)
        document = json.loads(data[20:20+length])
        self.assertEqual(document["asset"]["version"], "2.0")
        primitives = [primitive for mesh in document["meshes"] for primitive in mesh["primitives"]]
        self.assertEqual(len(primitives), 1)
        primitive = primitives[0]
        self.assertEqual(primitive.get("mode", 4), 4)
        self.assertEqual(document["accessors"][primitive["attributes"]["POSITION"]]["count"], 3)
        self.assertEqual(document["accessors"][primitive["indices"]]["count"], 3)
        binary_offset = 20+length
        binary_length, binary_kind = struct.unpack_from("<II", data, binary_offset)
        self.assertEqual(binary_kind, 0x004e4942)
        self.assertGreater(binary_length, 0)
        self.assertEqual(binary_offset+8+binary_length, len(data))
        broken = directory/"损坏.obj"
        broken.write_bytes(b"not a model\x00")
        rejected = directory/"不得生成.glb"
        self.run_tool("jce_cook", "--convert-model", broken, "--out", rejected, success=False)
        self.assertFalse(rejected.exists())

    def test_binary_architectures_and_asm(self):
        directory = self.root/"二进制 Ω"
        directory.mkdir()
        source = directory/"输入 你好.bin"; payload = bytes(range(37)); source.write_bytes(payload)
        for arch, machine, width in (("x86", 0x14c, 4), ("x64", 0x8664, 8),
                                     ("arm", 0x1c4, 4), ("arm64", 0xaa64, 8)):
            output = directory/("输出 "+arch+".obj")
            self.run_tool("jce_bin2obj", "--input", source, "--output", output,
                "--symbol", "assets_pak_data", "--format", "coff", "--arch", arch)
            data = output.read_bytes()
            self.assertEqual(struct.unpack_from("<H", data)[0], machine)
            self.assertEqual(data[60:97], payload)
            offset = (len(payload)+width-1)&~(width-1)
            self.assertEqual(int.from_bytes(data[60+offset:60+offset+width], "little"), len(payload))
        asm = directory/"输出 汇编.S"
        self.run_tool("jce_bin2obj", "--input", source, "--output", asm,
            "--symbol", "assets_pak_data", "--format", "asm")
        self.assertIn(source.name, asm.read_text())

    def test_failure_controls_do_not_fabricate_outputs(self):
        missing = self.root/"不存在 你好.bin"; output = self.root/"不得输出.obj"
        self.run_tool("jce_bin2obj", "--input", missing, "--output", output,
            "--symbol", "assets_pak_data", "--format", "coff", success=False)
        self.assertFalse(output.exists())
        self.run_tool("jce_cook", missing, output, success=False)
        self.assertFalse(output.exists())
        broken = self.root/"坏归档.pak"; broken.write_bytes(b"short")
        self.run_tool("jce_pak", "--inspect", broken, "--verify", success=False)


if __name__ == "__main__":
    unittest.main()
