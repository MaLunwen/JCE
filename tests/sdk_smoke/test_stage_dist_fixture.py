"""Real-file staging and proof protocol controls; no cooker/compiler/GPU run.

Proof JSON ports model existing builder outputs. They cannot certify actual
archive authentication; the native require-run gate supplies that evidence.
"""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("sdk_smoke_dist_stage", HERE / "stage_dist_fixture.py")
STAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(STAGE)


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8", newline="\n")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class DistFixtureTests(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory(prefix="jce-sdk-dist-stage-")
        self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name).resolve()
        self.source = self.root / "source"
        self.source.mkdir()
        for name in STAGE.SELECTED:
            shutil.copyfile(HERE / "assets" / name, self.source / name)
        self.script_source = self.root / "script-source"
        (self.script_source / "scripts").mkdir(parents=True)
        shutil.copyfile(HERE.parent / "sdk_smoke_scripting/assets/scripts/smoke_py.py",
                        self.script_source / "scripts/smoke_py.py")
        self.build = self.root / "build"

    def stage(self, fixture="c99"):
        source = self.script_source if fixture == "scripting" else self.source
        return STAGE.stage_dist_fixture(source, self.build, fixture=fixture)

    def proof(self, fixture="c99"):
        project, manifest = self.stage(fixture)
        generated = self.build / "jce_generated"
        graph_dir = generated / "content_graph"
        graph_dir.mkdir(parents=True)
        archive = graph_dir / "admission.jbundle"
        archive.write_bytes(b"COUNTED_PUBLIC_BUILDER_ARCHIVE_NOT_A_RUNTIME_CERTIFICATE")
        assets = []
        # Public Bundle type authority, independently observed in native122;
        # do not derive producer ports from the validator's expected types.
        public_types = {"smoke.png": ("texture", "jcea.texture.v1"),
                        "smoke.wav": ("sound", "jcea.audio-pcm.v1"),
                        "scripts/smoke_py.py": ("script", "python.source")}
        declared = ["scripts/smoke_py.py"] if fixture == "scripting" else ["smoke.png", "smoke.wav"]
        for index, name in enumerate([*declared, STAGE.STARTUP], 1):
            kind, representation = public_types.get(name, ("scene", "json.utf8"))
            size = (self.script_source / name).stat().st_size if kind == "script" else 100 + index
            assets.append(dict(path=name, asset_id=f"{index:016x}", content_id=f"{index + 10:016x}",
                               size=size, type=kind, representation=representation, dependencies=[]))
        metadata = dict(contract=dict(name="jce.bundle", major=1, minor=1), id="admission",
                        scene_path=STAGE.STARTUP, archive_size=archive.stat().st_size,
                        content_hash="000000000000001f", assets=assets)
        graph = dict(contract=dict(name="jce.bundle.graph", major=1, minor=0),
                     assets=[dict(address=a["path"], **{k: v for k, v in a.items() if k not in ("path", "type", "representation")})
                             for a in assets])
        write_json(graph_dir / "admission.jbundle.json", metadata)
        write_json(graph_dir / "bundle_graph.json", graph)
        pak = generated / "project_assets.pak"
        pak.write_bytes(b"COUNTED_AUTHENTICATED_PAK_PORT")
        (generated / "project_assets.obj").write_bytes(b"COUNTED_EMBED_PORT")
        (generated / "jce_pak_key.c").write_bytes(b"COUNTED_KEY_SHARE_PORT")
        count = len(assets)
        bom = dict(schema="jce.pakbom.v1", file=str(pak), file_size=pak.stat().st_size, sources_verified=True,
                   header={k: True for k in ("header_verified", "encrypted", "secure_index", "authenticated", "auth_verified")},
                   totals=dict(entries=count, verify_ran=True, verified_count=count, encrypted_count=count,
                               authenticated_count=count, corrupt_count=0, skipped_count=0),
                   selected_source=dict(path=str(archive), size=archive.stat().st_size, content_id=metadata["content_hash"], imported_count=count),
                   sources=[dict(path=a["path"], source=str(archive), selected=True, size=a["size"], content_id=a["content_id"])
                            for a in assets])
        write_json(generated / "project_assets.bom.json", bom)
        self.refresh_receipt()
        return project, manifest, metadata, graph, bom

    def refresh_receipt(self):
        generated = self.build / "jce_generated"
        graph_dir = generated / "content_graph"
        count = len(json.loads((graph_dir / "admission.jbundle.json").read_text())["assets"])
        receipt = dict(schema="jce.dist.asset-receipt.v1", deep_verification="PASS", selected_count=count, packed_count=count,
                       inputs_sha256={str(graph_dir / name): digest(graph_dir / name)
                                      for name in ("admission.jbundle", "admission.jbundle.json", "bundle_graph.json")},
                       pak_sha256=digest(generated / "project_assets.pak"),
                       bom_sha256=digest(generated / "project_assets.bom.json"),
                       embed_sha256=digest(generated / "project_assets.obj"))
        write_json(generated / "project_assets.receipt.json", receipt)

    def test_real_owner_files_stage_only_declared_five_inputs(self):
        (self.source / "unreferenced.bin").write_bytes(b"must not copy")
        before = {name: digest(self.source / name) for name in STAGE.SELECTED}
        project, manifest = self.stage()
        names = {p.relative_to(project).as_posix() for p in project.rglob("*") if p.is_file()}
        self.assertEqual(names, {"jce_project.json", "assets/bundle_roots.json", "assets/" + STAGE.STARTUP,
                                 "assets/smoke.png", "assets/smoke.wav"})
        self.assertLessEqual(sum(p.stat().st_size for p in project.rglob("*") if p.is_file()), STAGE.MAX_STAGE_BYTES)
        self.assertEqual(before, {name: digest(project / "assets" / name) for name in STAGE.SELECTED})
        self.assertEqual(before, {name: digest(self.source / name) for name in STAGE.SELECTED})
        self.assertEqual(manifest["startup_scene"], STAGE.STARTUP)
        self.assertFalse((project / ".jce").exists())

    def test_repeat_uses_new_root_and_retains_previous_key_evidence(self):
        first, _ = self.stage()
        (first / ".jce").mkdir()
        key = first / ".jce/pak_key.hex"
        key.write_text("retained old evidence", encoding="utf-8")
        second, _ = self.stage()
        self.assertNotEqual(first, second)
        self.assertEqual(key.read_text(), "retained old evidence")
        self.assertEqual(STAGE._current_fixture(self.build, verify_payloads=True), second)

    def test_missing_source_refused_before_any_output(self):
        (self.source / "smoke.png").unlink()
        with self.assertRaisesRegex(ValueError, "smoke.png"):
            self.stage()
        self.assertFalse(self.build.exists())

    def test_same_size_tampered_owner_source_refused(self):
        path = self.source / "smoke.wav"
        data = bytearray(path.read_bytes())
        data[-1] ^= 1
        path.write_bytes(data)
        with self.assertRaisesRegex(ValueError, "identity mismatch"):
            self.stage()
        self.assertFalse(self.build.exists())

    def test_oversized_source_refused_by_bounded_read(self):
        with (self.source / "smoke.png").open("ab") as output:
            output.write(b"extra")
        with self.assertRaisesRegex(ValueError, "identity mismatch"):
            self.stage()

    def test_source_and_tracked_or_ancestral_build_paths_refused(self):
        for path in (self.source, self.root, STAGE.ROOT / "tests/sdk_smoke/output", STAGE.ROOT.parent):
            with self.subTest(path=path), self.assertRaises(ValueError):
                STAGE.stage_dist_fixture(self.source, path)

    def test_linked_selected_source_refused(self):
        path = self.source / "smoke.png"
        path.unlink()
        try:
            path.symlink_to(HERE / "assets/smoke.png")
        except OSError as error:
            self.skipTest("host does not permit real file symlinks: " + str(error))
        with self.assertRaisesRegex(ValueError, "linked"):
            self.stage()

    def test_pointer_escape_and_oversize_refused(self):
        self.stage()
        pointer = self.build / STAGE.POINTER
        original = pointer.read_bytes()
        value = json.loads(original)
        value["project"] = str(self.root / "outside-auth-fixture")
        write_json(pointer, value)
        with self.assertRaisesRegex(ValueError, "pointer"):
            self.stage()
        pointer.write_bytes(b" " * (STAGE.MAX_POINTER_BYTES + 1))
        with self.assertRaisesRegex(ValueError, "byte budget"):
            self.stage()

    def test_valid_proof_consumes_existing_graph_authority_and_actual_file_hashes(self):
        project, _, _, _, _ = self.proof()
        result = STAGE.validate_dist_proof(self.build)
        self.assertEqual(result["fixture_project"], str(project))
        self.assertEqual(result["selected_count"], 3)
        self.assertEqual(set(result["payloads"]), set(STAGE.SELECTED))
        self.assertEqual(result["native_run"], "NOT_RUN_BY_HELPER")
        json.dumps(result)

    def test_stage_payload_tamper_refused_after_build(self):
        project, _, _, _, _ = self.proof()
        path = project / "assets/smoke.png"
        data = bytearray(path.read_bytes()); data[0] ^= 1; path.write_bytes(data)
        with self.assertRaisesRegex(ValueError, "identity mismatch"):
            STAGE.validate_dist_proof(self.build)

    def test_raw_or_encoded_representation_never_passes_cooked_witness(self):
        _, _, metadata, _, _ = self.proof()
        sidecar = self.build / "jce_generated/content_graph/admission.jbundle.json"
        for name, representation in (("smoke.png", "image.encoded"), ("smoke.wav", "audio.encoded")):
            value = copy.deepcopy(metadata)
            next(a for a in value["assets"] if a["path"] == name)["representation"] = representation
            write_json(sidecar, value)
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "cooked type"):
                STAGE.validate_dist_proof(self.build)

    def test_public_sound_type_is_required_and_audio_alias_is_refused(self):
        _, _, metadata, _, _ = self.proof()
        result = STAGE.validate_dist_proof(self.build)
        self.assertEqual(result["payloads"]["smoke.wav"]["type"], "sound")
        next(asset for asset in metadata["assets"] if asset["path"] == "smoke.wav")["type"] = "audio"
        write_json(self.build / "jce_generated/content_graph/admission.jbundle.json", metadata)
        with self.assertRaisesRegex(ValueError, "cooked type: smoke.wav"):
            STAGE.validate_dist_proof(self.build)

    def test_unauthenticated_or_partial_deep_bom_refused(self):
        _, _, _, _, bom = self.proof()
        path = self.build / "jce_generated/project_assets.bom.json"
        for section, key, bad in (("header", "auth_verified", False), ("totals", "verified_count", 2),
                                  ("totals", "corrupt_count", 1), ("totals", "skipped_count", 1)):
            value = copy.deepcopy(bom); value[section][key] = bad; write_json(path, value)
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "verification incomplete"):
                STAGE.validate_dist_proof(self.build)

    def test_selected_source_byte_identity_and_duplicate_rows_refused(self):
        _, _, _, _, bom = self.proof()
        path = self.build / "jce_generated/project_assets.bom.json"
        value = copy.deepcopy(bom); value["sources"][0]["content_id"] = "000000000000ffff"; write_json(path, value)
        with self.assertRaisesRegex(ValueError, "byte identity"):
            STAGE.validate_dist_proof(self.build)
        value = copy.deepcopy(bom); value["sources"][1]["path"] = value["sources"][0]["path"]; write_json(path, value)
        with self.assertRaisesRegex(ValueError, "duplicate"):
            STAGE.validate_dist_proof(self.build)

    def test_graph_missing_or_outside_dependency_refused_by_real_authority(self):
        _, _, _, graph, _ = self.proof()
        path = self.build / "jce_generated/content_graph/bundle_graph.json"
        value = copy.deepcopy(graph); value["assets"].pop(); write_json(path, value)
        with self.assertRaisesRegex(ValueError, "closure mismatch"):
            STAGE.validate_dist_proof(self.build)
        value = copy.deepcopy(graph)
        value["assets"][0]["dependencies"] = [dict(address="missing.bin", asset_id="000000000000ffff")]
        write_json(path, value)
        with self.assertRaises(ValueError):
            STAGE.validate_dist_proof(self.build)

    def test_pak_receipt_tamper_and_missing_key_share_refused(self):
        self.proof()
        generated = self.build / "jce_generated"
        original = (generated / "project_assets.pak").read_bytes()
        (generated / "project_assets.pak").write_bytes(bytes([original[0] ^ 1]) + original[1:])
        with self.assertRaisesRegex(ValueError, "receipt identity"):
            STAGE.validate_dist_proof(self.build)
        (generated / "project_assets.pak").write_bytes(original)
        (generated / "jce_pak_key.c").unlink()
        with self.assertRaisesRegex(ValueError, "key"):
            STAGE.validate_dist_proof(self.build)

    def test_removed_identity_guard_fails_the_same_runtime_refusal_witness(self):
        source = (HERE / "stage_dist_fixture.py").read_text(encoding="utf-8")
        marker = "if len(data) != size or actual != expected:"
        self.assertEqual(source.count(marker), 1)
        namespace = dict(__file__=str(HERE / "stage_dist_fixture.py"), __name__="sdk_stage_old_guard")
        exec(compile(source.replace(marker, "if len(data) != size:"), str(HERE / "stage_dist_fixture.py"), "exec"), namespace)
        path = self.source / "smoke.png"
        data = bytearray(path.read_bytes()); data[-1] ^= 1; path.write_bytes(data)
        with self.assertRaises(AssertionError):
            with self.assertRaises(ValueError):
                namespace["stage_dist_fixture"](self.source, self.build)

    def test_script_stage_preserves_public_path_bytes_and_fresh_repeats(self):
        original = self.script_source / "scripts/smoke_py.py"
        expected = "0c2fa95f6645160ec47c19f264fd64217e7bfe9e0a9d894e86ff8e50bc2cd418"
        self.assertEqual(digest(original), expected)
        first, manifest = self.stage("scripting")
        files = {p.relative_to(first).as_posix() for p in first.rglob("*") if p.is_file()}
        self.assertEqual(files, {"jce_project.json", "assets/bundle_roots.json", "assets/" + STAGE.STARTUP,
                                 "assets/scripts/smoke_py.py"})
        self.assertEqual((first / "assets/scripts/smoke_py.py").read_bytes(), original.read_bytes())
        self.assertEqual(manifest["target"], "JceSdkSmokeScripting")
        second, _ = self.stage("scripting")
        self.assertNotEqual(first, second)
        self.assertEqual(digest(first / "assets/scripts/smoke_py.py"), expected)
        pointer = json.loads((self.build / STAGE.POINTER).read_text())
        self.assertEqual(pointer["fixture"], "scripting")
        self.assertEqual(Path(pointer["project"]), second)

    def test_script_proof_type_and_bytes_and_wrong_fixture_refusal(self):
        self.proof("scripting")
        result = STAGE.validate_dist_proof(self.build, fixture="scripting")
        self.assertEqual(result["fixture"], "scripting")
        self.assertEqual(set(result["payloads"]), {"scripts/smoke_py.py"})
        self.assertEqual(result["payloads"]["scripts/smoke_py.py"]["type"], "script")
        self.assertEqual(result["payloads"]["scripts/smoke_py.py"]["representation"], "python.source")
        self.assertEqual(result["payloads"]["scripts/smoke_py.py"]["size"], 1107)
        with self.assertRaisesRegex(ValueError, "pointer"):
            STAGE.validate_dist_proof(self.build)

    def test_cross_fixture_stage_never_replaces_existing_pointer(self):
        self.stage()
        previous = (self.build / STAGE.POINTER).read_bytes()
        with self.assertRaisesRegex(ValueError, "pointer"):
            self.stage("scripting")
        self.assertEqual((self.build / STAGE.POINTER).read_bytes(), previous)

    def test_script_source_and_staged_tamper_refused(self):
        path = self.script_source / "scripts/smoke_py.py"
        original = path.read_bytes()
        path.write_bytes(bytes([original[0] ^ 1]) + original[1:])
        with self.assertRaisesRegex(ValueError, "identity mismatch"):
            self.stage("scripting")
        self.assertFalse(self.build.exists())
        path.write_bytes(original)
        project, _, _, _, _ = self.proof("scripting")
        (project / "assets/scripts/smoke_py.py").write_bytes(bytes([original[0] ^ 1]) + original[1:])
        with self.assertRaisesRegex(ValueError, "identity mismatch"):
            STAGE.validate_dist_proof(self.build, fixture="scripting")

    def test_script_wrong_type_or_language_representation_refused(self):
        _, _, metadata, _, _ = self.proof("scripting")
        sidecar = self.build / "jce_generated/content_graph/admission.jbundle.json"
        for key, wrong in (("type", "binary"), ("representation", "lua.source")):
            value = copy.deepcopy(metadata)
            next(a for a in value["assets"] if a["path"] == "scripts/smoke_py.py")[key] = wrong
            write_json(sidecar, value)
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "cooked type"):
                STAGE.validate_dist_proof(self.build, fixture="scripting")

    def test_script_renaming_or_byte_count_change_refused(self):
        _, _, metadata, graph, _ = self.proof("scripting")
        directory = self.build / "jce_generated/content_graph"
        changed = copy.deepcopy(metadata)
        changed_graph = copy.deepcopy(graph)
        next(a for a in changed["assets"] if a["path"] == "scripts/smoke_py.py")["path"] = "scripts/smoke_py.jceasset"
        next(a for a in changed_graph["assets"] if a["address"] == "scripts/smoke_py.py")["address"] = "scripts/smoke_py.jceasset"
        write_json(directory / "admission.jbundle.json", changed)
        write_json(directory / "bundle_graph.json", changed_graph)
        with self.assertRaisesRegex(ValueError, "fixture addresses"):
            STAGE.validate_dist_proof(self.build, fixture="scripting")
        changed = copy.deepcopy(metadata)
        changed_graph = copy.deepcopy(graph)
        next(a for a in changed["assets"] if a["path"] == "scripts/smoke_py.py")["size"] = 1106
        next(a for a in changed_graph["assets"] if a["address"] == "scripts/smoke_py.py")["size"] = 1106
        write_json(directory / "admission.jbundle.json", changed)
        write_json(directory / "bundle_graph.json", changed_graph)
        with self.assertRaisesRegex(ValueError, "source byte count"):
            STAGE.validate_dist_proof(self.build, fixture="scripting")

    def test_legacy_c99_pointer_only_upgrades_during_fresh_staging(self):
        first, _ = self.stage()
        path = self.build / STAGE.POINTER
        pointer = json.loads(path.read_text())
        pointer.pop("fixture")
        pointer["schema"] = "jce.sdk-smoke-dist-fixture.v1"
        write_json(path, pointer)
        with self.assertRaisesRegex(ValueError, "pointer"):
            STAGE.validate_dist_proof(self.build)
        with self.assertRaisesRegex(ValueError, "pointer"):
            self.stage("scripting")
        second, _ = self.stage()
        self.assertNotEqual(first, second)
        self.assertTrue(first.is_dir())
        self.assertEqual(json.loads(path.read_text())["fixture"], "c99")

    def test_unknown_fixture_refused_before_mutation(self):
        with self.assertRaisesRegex(ValueError, "unknown fixture"):
            self.stage("other")
        self.assertFalse(self.build.exists())

    def test_script_parent_link_inside_source_is_refused(self):
        hidden = self.script_source / "hidden"
        (self.script_source / "scripts").rename(hidden)
        try:
            (self.script_source / "scripts").symlink_to(hidden, target_is_directory=True)
        except OSError as error:
            self.skipTest("host does not permit real directory symlinks: " + str(error))
        with self.assertRaisesRegex(ValueError, "linked"):
            self.stage("scripting")


if __name__ == "__main__":
    unittest.main()
