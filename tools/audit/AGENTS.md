# Architecture and dependency audits

This directory owns reusable ABI, layering, dependency-license and duplication
checks. Inputs live under tracked `contracts/`; local `docs/` is never required.
`run_architecture_audit.py` runs the public audit suite. `run_dedup_audit.py`
compares against `dedup_baseline.json`. Refresh a baseline only with the reviewed
change that explains it. Prove new gates with positive and negative controls.
Optional unavailable tools report SKIPPED explicitly; never call a skip a pass.

The default failure transcript is build/reports/architecture-audit-failure.txt.
An explicit --log keeps its caller-selected path and success/failure behavior;
IO failure still preserves the real audit result. Legacy reports/ history is
not migrated or deleted. See contracts/build-output-layout.md and the actual
Python controls in tools/tests/test_generated_output_defaults.py.

Binding parity scans the actual desktop host at
engine/src/application/jce_jni_bridge.c. JNI target ownership is Application,
not Platform; export-name scanning alone does not qualify a native JVM link/run.

Host PAK dependency admission remains target-specific in the same ownership
matrix. Its actual Bundle-loader/PAK-image-adapter source closure permits direct
cJSON/SDL_image linking without permitting tool call-site headers or a second
parser/decoder. `tools/tests/test_host_pak_dependency_ownership.py` exercises the
real gate with removed permissions and consumer/header bypass controls; it is
consumed by the existing `tools/lint/run_all.py` aggregate. No native ABI/link or
product execution claim follows from these CPU ownership controls.

ABI snapshot locations are diagnostic provenance, separate from declaration
identity. Exact normalized declarations may be reported MOVED for all emitted
kinds; signature/layout/member-order edits remain fatal. The working-tree gate
admits compatible drift, while committed mode still rejects any unrecorded
move/add/append. Header re-export/source include reachability and compiled ABI
remain separate checks. test_abi_ordered_prefix.py consumes the real emitter,
diff and CLI, including a full-checker omitted-location ordinary-failure control;
never update the baseline to mask a classification defect.

Singleton state inventory excludes C++ using aliases, which own no storage,
while retaining mutable anonymous-namespace variables and callback-typed state.
It consumes the shared cached/untracked-not-ignored working-tree inventory in
strict mode, subtracting only Git-declared deletions; Git/read failures refuse
the count. Existing source roots, extensions and vendor/generated exclusions
remain unchanged, as does the JSON declaration-list format. The baseline is a
separate reviewed authority. tools/tests/test_singleton_ownership.py exercises
actual source declarations/files, temporary Git indexes, counted command ports
and ordinary missing-classifier/untracked-wire assertion failures. These Python
controls do not extend the lexical scanner to direct function-pointer syntax
or certify native ownership/lifetime behavior.
