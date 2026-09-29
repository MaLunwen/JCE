#!/usr/bin/env python3
"""Entry point; implementation: tools/build/fetch_vendor_sources.py."""
from pathlib import Path
_target = Path(__file__).resolve().parents[1] / "tools/build/fetch_vendor_sources.py"
__file__ = str(_target)
exec(compile(_target.read_bytes(), str(_target), "exec"), globals())
