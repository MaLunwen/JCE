# Architecture and dependency audits

This directory owns reusable ABI, layering, dependency-license and duplication
checks. Inputs live under tracked `contracts/`; local `docs/` is never required.
`run_architecture_audit.py` runs the public audit suite. `run_dedup_audit.py`
compares against `dedup_baseline.json`. Refresh a baseline only with the reviewed
change that explains it. Prove new gates with positive and negative controls.
Optional unavailable tools report SKIPPED explicitly; never call a skip a pass.
