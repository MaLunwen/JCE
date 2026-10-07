# Delivered portable Minesweeper Bundle

`minesweeper.jbundle` is one normal scene/script package using the public Lua
ScriptHost. It needs no native script DLL or loose asset sidecar.

Observed controls: editor menu/preset selection, beginner reveal, flag/chord,
application quit, and statistics persistence across two editor processes.
The normal package also loaded/rendered from the public SDK host after copying
only its embedded executable to an unrelated directory.

This does not certify every victory/loss path, a complete 600-case game matrix,
external LLM execution, or all platforms/devices. Statistics require a configured
host service; absence remains explicit. The historical design and regression
sources are in `../../tests/consumers/minesweeper/`.

See BUNDLE_PROVENANCE.json for exact normal package bytes and SOURCE_AND_TERMS.md
for code/asset terms. No muted QA package is delivered here.
