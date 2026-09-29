#!/usr/bin/env python3
"""Validate the maintained English skill and its Chinese translation."""
import json
from pathlib import Path
import re
import shutil
import sys
import tempfile
ROOT = Path(__file__).resolve().parents[2]
CJK = re.compile(r"[\u3400-\u9fff]")
BLOCK = re.compile(r"(?ms)^```[^\n]*\n(.*?)^```$")


def violations(root):
    contract = json.loads((root / "contracts/skill-locales.json").read_text(encoding="utf-8"))
    errors = []
    if contract["default"] != "en-US":
        errors.append("the default JCE skill must be en-US")
    expected = {"SKILL.md", "INSTALL.md"} | {"references/" + n for n in contract["references"]}
    editions = {}
    for locale, config in contract["locales"].items():
        directory = root / config["root"]
        texts = {p.relative_to(directory).as_posix(): p.read_text(encoding="utf-8")
                 for p in directory.rglob("*.md") if p.name != "MANIFEST.md"}
        editions[locale] = texts
        if set(texts) != expected:
            errors.append(locale + ": reference inventory differs from the contract")
        entry = texts.get("SKILL.md", "")
        if not re.search(r"(?m)^name: " + re.escape(config["name"]) + r"$", entry):
            errors.append(locale + ": skill name differs from the contract")
        if not re.search(r"(?m)^  language: " + re.escape(locale) + r"$", entry):
            errors.append(locale + ": language metadata differs from the contract")
        if locale == "en-US":
            errors.extend("en-US: untranslated text in " + name for name, text in texts.items() if CJK.search(text))
    english = editions["en-US"]
    chinese = editions["zh-CN"]
    for name in sorted(expected):
        if BLOCK.findall(english.get(name, "")) != BLOCK.findall(chinese.get(name, "")):
            errors.append(name + ": executable examples differ between editions")
    return errors


def self_check():
    with tempfile.TemporaryDirectory(prefix="jce-skill-locales-") as tmp:
        root = Path(tmp)
        (root / "contracts").mkdir()
        shutil.copy2(ROOT / "contracts/skill-locales.json", root / "contracts/skill-locales.json")
        for name in ("jce", "jce-zh-cn"):
            shutil.copytree(ROOT / "skills" / name, root / "skills" / name)
        assert not violations(root)
        mutations = [
            ("skills/jce/SKILL.md", lambda s: s + "中文\n"),
            ("skills/jce-zh-cn/SKILL.md", lambda s: s.replace("language: zh-CN", "language: en-US")),
            ("skills/jce-zh-cn/references/user-project-sdk.md", lambda s: s.replace("--variant dist", "--variant release")),
            ("contracts/skill-locales.json", lambda s: s.replace('"default": "en-US"', '"default": "zh-CN"')),
        ]
        for name, mutate in mutations:
            path = root / name
            original = path.read_bytes()
            path.write_text(mutate(original.decode("utf-8")), encoding="utf-8")
            assert violations(root), name
            path.write_bytes(original)
        missing = root / "skills/jce-zh-cn/references/build-and-gate.md"
        original = missing.read_bytes()
        missing.unlink()
        assert violations(root)
        missing.write_bytes(original)
        assert not violations(root)
    print("skill locales self-check: PASS (5 negative controls)")


def main():
    if "--self-check" in sys.argv:
        self_check()
    errors = violations(ROOT)
    for error in errors:
        print("FAIL:", error)
    print("skill locales:", "FAIL" if errors else "PASS (en-US default, zh-CN translation, identical commands)")
    return int(bool(errors))


if __name__ == "__main__":
    raise SystemExit(main())
