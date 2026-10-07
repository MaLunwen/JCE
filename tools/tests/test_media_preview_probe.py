"""Editor preview probes must confirm an isolated profile without HOME writes."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]


def preview_module():
    spec = importlib.util.spec_from_file_location("media_preview_profile_test", ROOT / "tools/media_preview_probe.py")
    module = importlib.util.module_from_spec(spec)
    with patch.object(sys, "path", [str(ROOT / "tools"), *sys.path]):
        spec.loader.exec_module(module)
    return module


class MediaCadenceTests(unittest.TestCase):
    def test_seek_with_similar_clock_delta_is_still_separate(self):
        rows = [dict(wall=2.928, media=1.752, playing=1, generation=1),
                dict(wall=5.933, media=4.576, playing=1, generation=2),
                dict(wall=6.933, media=5.576, playing=1, generation=2)]
        spans = preview_module().playback_spans(rows, 20)
        self.assertEqual([len(span) for span in spans], [1, 2])

    def test_pause_snapshot_ends_interval_without_seek(self):
        rows = [dict(wall=2, media=2, playing=1, generation=0),
                dict(wall=3, media=2, playing=0, generation=0),
                dict(wall=4, media=2, playing=1, generation=0)]
        self.assertEqual([len(span) for span in preview_module().playback_spans(rows, 20)], [1, 1])

    def test_true_slow_playback_is_not_split_or_hidden(self):
        rows = [dict(wall=2, media=2, playing=1, generation=0),
                dict(wall=3, media=2.5, playing=1, generation=0),
                dict(wall=4, media=3, playing=1, generation=0)]
        spans = preview_module().playback_spans(rows, 20)
        self.assertEqual(len(spans), 1)
        self.assertEqual((spans[0][-1]["media"] - spans[0][0]["media"]) /
                         (spans[0][-1]["wall"] - spans[0][0]["wall"]), .5)


class MediaProfileTests(unittest.TestCase):
    def probe(self, confirm):
        scratch = ROOT / "build/tests/media-preview-tool"
        scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as temporary:
            root = Path(temporary)
            module = preview_module()
            media, editor, output = root / "control.mp4", root / "editor", root / "preview.log"
            for path in (media, editor):
                path.write_bytes(b"first-party mock process input")

            def child(argv, **kwargs):
                profile = Path(kwargs["env"]["JCE_EDITOR_PROFILE_DIR"])
                self.assertEqual(profile.parent, output.parent)
                (profile / "imgui.ini").write_text("mock layout", encoding="utf-8")
                log = "viewer trace wall=0.000 media=0.000 pts=0.000 playing=0\n"
                if confirm:
                    log += "profile directory: " + str(profile) + "\n"
                kwargs["stdout"].write(log.encode("utf-8"))
                return SimpleNamespace(wait=lambda **_: 0)

            with patch("sys.argv", ["media_preview_probe", str(media), "--editor", str(editor),
                                    "--output", str(output)]), \
                    patch.object(module.subprocess, "Popen", side_effect=child), \
                    patch.object(Path, "home", side_effect=AssertionError("HOME must never be used")), \
                    contextlib.redirect_stdout(io.StringIO()):
                code = module.main()
            return code, json.loads(output.with_suffix(".json").read_text(encoding="utf-8"))

    def test_confirmed_isolated_profile_passes(self):
        code, result = self.probe(True)
        self.assertEqual(code, 0)
        self.assertTrue(result["settings_isolated"])

    def test_old_editor_ignoring_profile_is_not_a_pass(self):
        code, result = self.probe(False)
        self.assertEqual(code, 1)
        self.assertFalse(result["settings_isolated"])
        self.assertIn("editor did not confirm the isolated profile and saved layout", result["validation_failures"])


if __name__ == "__main__":
    unittest.main()
