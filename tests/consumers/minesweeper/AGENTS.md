# Minesweeper consumer example

Only public JCE SDK and ScriptHost APIs. No engine/editor business-code edits.

- `src/board.*`: bounded deterministic C99 rules; `src/test_board.c`: property checks.
- `src/controller.c`: C script class shared by standalone and editor Play.
- `src/view.*`: component presentation through the script host, no renderer internals.
- `resources/scripts/director.lua`: button commands and bounded AI component proposals.
- `src/main.c`: SDK entry shim, persistence and asynchronous LLM service glue.
- `tools/ai_director.py`: uses the existing JCE LLM transport; validates six integers.
- `tools/run.py --exe <game>`: explicitly selected application and launch-time provider configuration; no build-path guessing or keys saved to disk.

Scene writes use JCE automation changesets. AI output is data, never executable code.
Three classic presets stay fixed; AI changes custom configuration and background colour.
Existing games finish under their original rules. Report editor-specific limitations explicitly.
Cook and build via `python scripts/jce.py cook tests/consumers/minesweeper` and
`python scripts/jce.py build-project tests/consumers/minesweeper` from the repository root.
Build outputs, screenshots and local settings are ignored; do not stage them.

- `tools/author_scene.py`: schema-checked entity creation; handles contract-wrapped scenes.
- `tools/verify_game.py --exe <game> --editor-exe <editor> --out <fresh-directory> --extended --editor --ai`: actual synthetic replay, isolated Editor profile and optional live AI. Each child receipt records the selected executable hashes, normal result and observed wall duration; these are functional runs, not performance baselines.
- Verification timeout, launch failure and executable-read failure persist available output and explicit hash availability before raising; counted subprocess controls in `test_native_source_contract.py` do not launch a native application.
- `tools/verify_ai.py`: four real loopback protocols; `tools/test_ai.py`: offline wire/validation regression.
- `WORKFLOW.md`: asset provenance and component-based AI lifecycle; `REPORT.md`: measured limits.

UIButton state tint currently draws over label text. Use transparent state alpha in this consumer; retain UIImage fill. Do not work around this by modifying renderer internals. Always cook after scene/script changes before build-project.

## Native producer closure

The scene requires Lua and C. `resources/scripts/MinesController.jcec` is the
addressable public class reference; behavior is implemented in `src/controller.c`.
The manifest binds its canonical selected address to the actual authored path
and `mines_module`. CMake declares that factory in `jce_script_enable` with
REQUIRED and NO_STAGE; main calls the generated shim with NULL exactly once,
checks failure before scene loading, and never republishes the same module.
Editor provider DLL output uses native/build in single and multiple configuration
generators. Catalog Bundle files remain a separate portable Lua consumer.
`tools/test_native_source_contract.py` covers real source addresses and the
existing driver plan/admission contract. Fixture typed IDs do not replace the
public packer's actual BOM or the generated shim's native execution receipt.
