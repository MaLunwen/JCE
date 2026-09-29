#!/usr/bin/env python3
"""Entry point; implementation: tools/migrate_models_to_glb.py."""
from pathlib import Path
_target = Path(__file__).resolve().parents[1] / "tools/migrate_models_to_glb.py"
__file__ = str(_target)
exec(compile(_target.read_bytes(), str(_target), "exec"), globals())
