# editor/src/game_modules/ — In-Editor Game Module Implementations

## Identity
- **Language**: C++17 (wraps C `JceGameModule` API from `engine/src/runtime/`)
- **Role**: built-in / demo game modules compiled into the editor for Play-mode without external game DLL.

## File map
- `jce_fps_demo_module.cpp` — minimal FPS demo (player controller, weapon, enemy spawner) showcasing the `JceGameModule` interface.

## Rules
1. **Use only `<jce/api.h>`** as if it were external game code — these modules are reference implementations that must compile against the public API.
2. **Register via `jce_runtime_register_game_module(...)`** from `engine/src/runtime/`.
3. **Pure gameplay** — no engine internals, no editor state.
4. **Compile flag**: `JCE_EDITOR_BUILTIN_GAME_MODULES` gates inclusion; default ON for editor.
5. **Demo modules are documentation** — keep them small, idiomatic, and commented. They double as integration tests.

## Don't
- Don't add proprietary game code here — that belongs in `examples/caged_kingdom/` or a separate game repo.
- Don't reach into ImGui — these modules also run in the standalone game runtime.

## Common tasks
- **Add a demo** → `<name>_demo_module.cpp` → register on editor startup → add menu entry in `editor/src/ui/jce_editor_panels.cpp` Game menu.
