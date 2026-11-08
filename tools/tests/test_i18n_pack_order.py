"""Source graph and compiler-free scheduling controls for i18n asset ordering."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def dependency_graph(source):
    source = re.sub(r"#[^\n]*", "", source)
    graph = {}
    for arguments in re.findall(r"add_dependencies\s*\(([^)]*)\)", source):
        names = arguments.split()
        if names:
            graph.setdefault(names[0], set()).update(names[1:])
    return graph


def reaches(graph, start, target):
    pending = list(graph.get(start, ()))
    visited = set()
    while pending:
        node = pending.pop()
        if node == target:
            return True
        if node not in visited:
            visited.add(node)
            pending.extend(graph.get(node, ()))
    return False


def validate_i18n_pack_order(source):
    graph = dependency_graph(source)
    if not reaches(graph, "PackEditorAssets", "jce_i18n_auto"):
        raise ValueError("packer does not wait for source JSON normalization")
    if not reaches(graph, "CookEditorAssets", "jce_i18n_auto"):
        raise ValueError("cooker scans source JSON beside the normalization writer")
    if reaches(graph, "jce_i18n_auto", "CookEditorAssets"):
        raise ValueError("normalization/cooking dependency cycle")
    if reaches(graph, "jce_i18n_auto", "PackEditorAssets"):
        raise ValueError("normalization/packing dependency cycle")
    # A sibling prerequisite on the editor is not sufficient ordering.
    if not reaches(graph, "JCE_Editor", "jce_i18n_auto"):
        raise ValueError("editor normalization prerequisite missing")


class I18nPackOrderTests(unittest.TestCase):
    def test_actual_build_graph_orders_source_writer_before_reader(self):
        validate_i18n_pack_order((ROOT / "CMakeLists.txt").read_text(encoding="utf-8"))

    def test_packer_order_does_not_order_its_cooker_sibling(self):
        source = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        edge = "add_dependencies(CookEditorAssets jce_i18n_auto)"
        self.assertEqual(source.count(edge), 1)
        with self.assertRaisesRegex(ValueError, "cooker scans"):
            validate_i18n_pack_order(source.replace(edge, "", 1))

    def test_sibling_targets_are_not_ordered(self):
        with self.assertRaises(ValueError):
            validate_i18n_pack_order("add_dependencies(JCE_Editor PackEditorAssets jce_i18n_auto)")

    def test_reverse_dependency_is_rejected(self):
        with self.assertRaises(ValueError):
            validate_i18n_pack_order("add_dependencies(JCE_Editor jce_i18n_auto)\n"
                                     "add_dependencies(jce_i18n_auto PackEditorAssets)")

    def test_cycle_is_rejected_without_infinite_traversal(self):
        with self.assertRaises(ValueError):
            validate_i18n_pack_order("add_dependencies(JCE_Editor jce_i18n_auto)\n"
                                     "add_dependencies(PackEditorAssets jce_i18n_auto)\n"
                                     "add_dependencies(jce_i18n_auto intermediate)\n"
                                     "add_dependencies(intermediate PackEditorAssets)")

    def test_comment_does_not_fake_an_edge(self):
        with self.assertRaises(ValueError):
            validate_i18n_pack_order("add_dependencies(JCE_Editor jce_i18n_auto)\n"
                                     "# add_dependencies(PackEditorAssets jce_i18n_auto)")


    def _scheduler_control(self, remove_cook_edge=False):
        cmake = shutil.which("cmake")
        ninja = shutil.which("ninja")
        if not cmake or not ninja:
            self.skipTest("CMake/Ninja unavailable for compiler-free i18n scheduler control")

        source = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        blocks = re.findall(
            r"^[ \t]*if\(TARGET CookEditorAssets\)[ \t]*\n"
            r".*?^[ \t]*endif\(\)[ \t]*(?:\n|$)", source, re.MULTILINE | re.DOTALL)
        self.assertEqual(len(blocks), 1, "expected one actual CookEditorAssets dependency block")
        block = blocks[0]
        edge = "add_dependencies(CookEditorAssets jce_i18n_auto)"
        self.assertEqual(block.count(edge), 1)
        if remove_cook_edge:
            # Remove only the real production wire; preserve the enclosing guard.
            block = block.replace(edge, "", 1)

        scratch = ROOT / "build" / "tests" / "i18n-pack-order"
        scratch.mkdir(parents=True, exist_ok=True)
        # Retain fresh inputs and raw CMake/Ninja output for either outcome.
        root = Path(tempfile.mkdtemp(prefix="removed-edge-" if remove_cook_edge else "wired-",
                                     dir=scratch))
        out = root / "out"
        (root / "writer.cmake").write_text('''file(WRITE "${PROBE_OUT}/writer.marker" "normalized\n")
file(WRITE "${PROBE_OUT}/writer.stamp" "complete\n")
''', encoding="utf-8", newline="\n")
        (root / "cooker.cmake").write_text('''if(NOT EXISTS "${PROBE_OUT}/writer.marker")
  message(FATAL_ERROR "MISSING_I18N_WRITER_MARKER")
endif()
file(READ "${PROBE_OUT}/writer.marker" normalized)
if(NOT normalized STREQUAL "normalized\n")
  message(FATAL_ERROR "INVALID_I18N_WRITER_MARKER")
endif()
file(WRITE "${PROBE_OUT}/cooked.output" "cooked after normalization\n")
''', encoding="utf-8", newline="\n")
        recipe = '''cmake_minimum_required(VERSION 3.20)
project(I18nPackOrder LANGUAGES NONE)
add_custom_command(
  OUTPUT "${CMAKE_BINARY_DIR}/writer.stamp"
  COMMAND "${CMAKE_COMMAND}" "-DPROBE_OUT=${CMAKE_BINARY_DIR}"
          -P "${CMAKE_SOURCE_DIR}/writer.cmake"
  VERBATIM)
add_custom_target(jce_i18n_auto ALL DEPENDS "${CMAKE_BINARY_DIR}/writer.stamp")
add_custom_command(
  OUTPUT "${CMAKE_BINARY_DIR}/cooked.output"
  COMMAND "${CMAKE_COMMAND}" "-DPROBE_OUT=${CMAKE_BINARY_DIR}"
          -P "${CMAKE_SOURCE_DIR}/cooker.cmake"
  VERBATIM)
add_custom_target(CookEditorAssets DEPENDS "${CMAKE_BINARY_DIR}/cooked.output")
'''
        (root / "CMakeLists.txt").write_text(recipe + block, encoding="utf-8", newline="\n")

        def run(argv, phase):
            result = subprocess.run(argv, capture_output=True, text=True,
                                    encoding="utf-8", errors="replace", timeout=60)
            (root / (phase + ".stdout.log")).write_text(result.stdout, encoding="utf-8", newline="\n")
            (root / (phase + ".stderr.log")).write_text(result.stderr, encoding="utf-8", newline="\n")
            return result

        configured = run([cmake, "-S", str(root), "-B", str(out), "-G", "Ninja",
                          "-DCMAKE_MAKE_PROGRAM=" + str(Path(ninja).resolve())], "configure")
        self.assertEqual(configured.returncode, 0,
                         f"configure failed; raw logs: {root}\n{configured.stdout}{configured.stderr}")
        built = run([cmake, "--build", str(out), "--target", "CookEditorAssets",
                     "--parallel", "2"], "build")
        return out, built

    def test_real_cmake_wire_schedules_writer_before_explicit_cook_target(self):
        out, result = self._scheduler_control()
        self.assertEqual(result.returncode, 0,
                         f"raw logs: {out.parent}\n{result.stdout}{result.stderr}")
        self.assertEqual((out / "writer.marker").read_text(encoding="utf-8"), "normalized\n")
        self.assertEqual((out / "writer.stamp").read_text(encoding="utf-8"), "complete\n")
        self.assertEqual((out / "cooked.output").read_text(encoding="utf-8"),
                         "cooked after normalization\n")

    def test_removed_real_cook_wire_fails_without_selecting_all_writer(self):
        out, result = self._scheduler_control(remove_cook_edge=True)
        raw = result.stdout + result.stderr
        self.assertGreater(result.returncode, 0, f"expected ordinary build failure; raw logs: {out.parent}\n{raw}")
        self.assertIn("MISSING_I18N_WRITER_MARKER", raw, f"raw logs: {out.parent}\n{raw}")
        self.assertFalse((out / "writer.marker").exists(), "ALL writer was unexpectedly selected")
        self.assertFalse((out / "writer.stamp").exists())
        self.assertFalse((out / "cooked.output").exists())


if __name__ == "__main__":
    unittest.main()
