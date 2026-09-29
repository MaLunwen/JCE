#!/usr/bin/env python3
"""Entry point; implementation: tools/report_parity.py."""
from pathlib import Path
_target = Path(__file__).resolve().parents[1] / "tools/report_parity.py"
__file__ = str(_target)
exec(compile(_target.read_bytes(), str(_target), "exec"), globals())
