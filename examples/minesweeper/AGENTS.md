# Minesweeper consumer example

Only public JCE SDK and ScriptHost APIs. No engine/editor business-code edits.

- `src/board.*`: bounded deterministic C99 rules; `src/test_board.c`: property checks.
- `src/controller.c`: C script class shared by standalone and editor Play.
- `src/view.*`: component presentation through the script host, no renderer internals.
- `resources/scripts/director.lua`: button commands and bounded AI component proposals.
- `src/main.c`: SDK entry shim, persistence and asynchronous LLM service glue.
- `tools/ai_director.py`: uses the existing JCE LLM transport; validates six integers.
- `tools/run.py`: launch-time provider configuration; no keys saved to disk.

Scene writes use JCE automation changesets. AI output is data, never executable code.
Three classic presets stay fixed; AI changes custom configuration and background colour.
Existing games finish under their original rules. Report editor-specific limitations explicitly.
Cook and build via `python scripts/jce.py cook examples/minesweeper` and
`python scripts/jce.py build-project examples/minesweeper` from the repository root.
Build outputs, screenshots and local settings are ignored; do not stage them.

- `tools/author_scene.py`: schema-checked entity creation; handles contract-wrapped scenes.
- `tools/verify_game.py --extended --editor --ai`: actual replay, editor session restore and live AI.
- `tools/verify_ai.py`: four real loopback protocols; `tools/test_ai.py`: offline wire/validation regression.
- `WORKFLOW.md`: asset provenance and component-based AI lifecycle; `REPORT.md`: measured limits.

UIButton state tint currently draws over label text. Use transparent state alpha in this consumer; retain UIImage fill. Do not work around this by modifying renderer internals. Always cook after scene/script changes before build-project.
