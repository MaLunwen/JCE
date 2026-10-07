"""Source dependency graph regression for the actual i18n writer/packer."""
from pathlib import Path
import re
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
    if reaches(graph, "jce_i18n_auto", "PackEditorAssets"):
        raise ValueError("normalization/packing dependency cycle")
    # A sibling prerequisite on the editor is not sufficient ordering.
    if not reaches(graph, "JCE_Editor", "jce_i18n_auto"):
        raise ValueError("editor normalization prerequisite missing")


class I18nPackOrderTests(unittest.TestCase):
    def test_actual_build_graph_orders_source_writer_before_reader(self):
        validate_i18n_pack_order((ROOT / "CMakeLists.txt").read_text(encoding="utf-8"))

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


if __name__ == "__main__":
    unittest.main()
