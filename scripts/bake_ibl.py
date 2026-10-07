#!/usr/bin/env python3
"""Entry point; implementation: tools/assets/bake_ibl.py."""
from pathlib import Path
_target = Path(__file__).resolve().parents[1] / "tools/assets/bake_ibl.py"
__file__ = str(_target)
exec(compile(_target.read_bytes(), str(_target), "exec"), globals())
