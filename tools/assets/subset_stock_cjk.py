"""Rebuild the optional stock fallback from the pinned upstream TTF."""
import argparse
import hashlib
import io
import json
from pathlib import Path

from fontTools import subset
from fontTools.ttLib import TTFont


SOURCE_URL = (
    "https://github.com/lxgw/LxgwWenKai/releases/download/"
    "v1.522/LXGWWenKai-Regular.ttf"
)
SOURCE_SHA256 = "39ad71264b588165b469e35e6afb162a378dacd1f95348160240ba9038ac3009"
FAMILY = "JCE CJK Fallback"
FILE_NAME = "JCE-CJK-Fallback"
MAX_BYTES = 3 * 1024 * 1024


def locale_codepoints(directory):
    paths = sorted(directory.glob("*.json"))
    if not paths:
        raise ValueError("no editor translations found")
    return {
        ord(char)
        for path in paths
        for message in json.loads(path.read_text(encoding="utf-8")).values()
        for char in message
        if not char.isspace() and ord(char) >= 32
    }


def common_han():
    # GB2312 level one consists of 3,755 commonly used simplified characters.
    for row in range(0xB0, 0xD8):
        for cell in range(0xA1, 0xFF):
            char = bytes((row, cell)).decode("gb2312", errors="ignore")
            if char:
                yield ord(char)


def rebuild(source, locales, output):
    if hashlib.sha256(source.read_bytes()).hexdigest() != SOURCE_SHA256:
        raise ValueError("source differs from upstream LXGW WenKai v1.522")
    output.mkdir(parents=True, exist_ok=True)
    with TTFont(source, recalcTimestamp=False) as font:
        # Reuse the upstream solid right triangle for the editor breadcrumb.
        for table in font["cmap"].tables:
            if table.isUnicode() and 0x25B6 in table.cmap:
                table.cmap[0x25B8] = table.cmap[0x25B6]
        cmap = font.getBestCmap()
        required = locale_codepoints(locales) | set(common_han()) | {0xFFFD}
        if required - cmap.keys():
            raise ValueError(f"upstream cmap is missing {sorted(required - cmap.keys())}")
        ranges = [(0x20, 0xFF), (0x2000, 0x26FF), (0x3000, 0x30FF), (0xFF00, 0xFFEF)]
        symbols = {cp for cp in cmap if any(lo <= cp <= hi for lo, hi in ranges)}
        names = {1: FAMILY, 3: FAMILY + ":1.0", 4: FAMILY, 6: FILE_NAME + "-Regular",
                 16: FAMILY, 18: FAMILY, 21: FAMILY}
        for record in font["name"].names:
            if record.nameID in names:
                record.string = names[record.nameID].encode(record.getEncoding())
        options = subset.Options()
        options.name_IDs = list(range(26))
        options.name_languages = ["*"]
        worker = subset.Subsetter(options=options)
        worker.populate(unicodes=required | symbols)
        worker.subset(font)
        face_path = output / (FILE_NAME + ".ttf")
        buffer = io.BytesIO()
        font.save(buffer)
        data = buffer.getvalue()
        if len(data) > MAX_BYTES:
            raise ValueError(f"fallback exceeds the {MAX_BYTES}-byte stock font budget")
        face_path.write_bytes(data)
        manifest = {
            "schema": 1,
            "asset": "fonts/" + face_path.name,
            "family": FAMILY,
            "style": "Regular",
            "source": SOURCE_URL,
            "source_sha256": SOURCE_SHA256,
            "license": "OFL-1.1",
            "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
            "codepoints": len(font.getBestCmap()),
            "glyphs": len(font.getGlyphOrder()),
            "coverage": "Editor translations, GB2312 level one and selected UI symbols",
            "modified": True,
        }
    (output / (FILE_NAME + "-provenance.json")).write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("locales", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    rebuild(args.source, args.locales, args.output)


if __name__ == "__main__":
    main()
