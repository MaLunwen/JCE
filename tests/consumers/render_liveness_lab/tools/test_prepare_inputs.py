"""CPU controls for actual PAK BOM admission; no native packer or GPU runs."""
import copy
from pathlib import Path
import tempfile
import unittest

from prepare_inputs import ROOT, verify_pack, xxh32


class PackReceiptControls(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=ROOT / "build")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.archive = self.root / "fixture.pak"
        self.archive.write_bytes(b"synthetic PAK identity only")
        self.expected, entries, sources = {}, [], []
        for name, data in (("shaders/vs_grass_dx11.bin", b"vertex stage"),
                           ("shaders/fs_grass_dx11.bin", b"colour stage")):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            self.expected[name] = path
            entries.append({"path": name, "verified": True, "original_size": len(data),
                            "content_crc": f"0x{xxh32(data):08x}"})
            sources.append({"path": name, "source": str(path), "size": len(data), "selected": False})
        self.bom = {"schema": "jce.pakbom.v1", "file": str(self.archive),
                    "file_size": self.archive.stat().st_size, "sources_verified": True,
                    "header": {"header_verified": True, "entry_count": 2},
                    "totals": {"entries": 2, "verify_ran": True, "verified_count": 2,
                               "corrupt_count": 0, "skipped_count": 0,
                               "original_size": sum(e["original_size"] for e in entries)},
                    "entries": entries, "sources": sources}

    def test_complete_receipt_accepted(self):
        verify_pack(self.bom, self.archive, self.expected)

    def test_deep_receipt_and_byte_corruptions_rejected(self):
        mutations = [
            lambda d: d.update(sources_verified=False),
            lambda d: d["header"].update(header_verified=False),
            lambda d: d["totals"].update(verify_ran=False),
            lambda d: d["totals"].update(verified_count=1),
            lambda d: d["totals"].update(corrupt_count=1),
            lambda d: d["totals"].update(skipped_count=1),
            lambda d: d["entries"][0].update(verified=False),
            lambda d: d["entries"][0].update(content_crc="0x00000000"),
            lambda d: d["entries"][0].update(original_size=999),
            lambda d: d["sources"][0].update(source=str(self.root / "wrong.bin")),
        ]
        for index, mutate in enumerate(mutations):
            with self.subTest(index=index):
                control = copy.deepcopy(self.bom)
                mutate(control)
                with self.assertRaises(ValueError):
                    verify_pack(control, self.archive, self.expected)

    def test_duplicate_address_cannot_hide_missing_stage(self):
        self.bom["entries"][1] = copy.deepcopy(self.bom["entries"][0])
        with self.assertRaisesRegex(ValueError, "addresses"):
            verify_pack(self.bom, self.archive, self.expected)

    def test_omitted_stage_remaining_in_archive_rejected(self):
        selected = dict(self.expected)
        del selected["shaders/fs_grass_dx11.bin"]
        with self.assertRaisesRegex(ValueError, "count"):
            verify_pack(self.bom, self.archive, selected)

    def test_source_bytes_changed_after_packing_rejected(self):
        self.expected["shaders/vs_grass_dx11.bin"].write_bytes(b"Vertex stage")
        with self.assertRaisesRegex(ValueError, "byte verification"):
            verify_pack(self.bom, self.archive, self.expected)


if __name__ == "__main__":
    unittest.main()
