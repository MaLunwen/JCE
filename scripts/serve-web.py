#!/usr/bin/env python3
from pathlib import Path
_target = Path(__file__).resolve().parents[1] / "tools/serve_web.py"
__file__ = str(_target)
exec(compile(_target.read_bytes(), str(_target), "exec"), globals())
