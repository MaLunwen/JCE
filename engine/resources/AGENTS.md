# engine/resources/ — Engine-Shipped Runtime Assets

## Identity
- **Role**: assets bundled INTO the engine binary / PAK and always available.
- **Mount**: registered as the first PhysFS search path on init (mount point `/engine`).

## File map
- `JCE_icon.png`        — default window/app icon.
- `assets/shaders/`     — compiled bgfx shader bins (one per backend), output of `tools/compile_shaders.cmake`.

## Rules
1. **Tiny and universal only.** Targets the 512 MB / no-GPU baseline — keep total < 4 MB compressed.
2. **Compiled outputs go here**; sources live in `engine/shaders/`.
3. **Cross-platform**: no per-OS resource here (use runtime selection if needed).
4. **PAK mount path**: assets referenced as `/engine/assets/shaders/<backend>/<name>.bin`.
5. **Don't ship sources** — only baked artifacts.

## Common tasks
- **Replace icon** → swap PNG; rerun packaging scripts in `scripts/`.
- **Update shaders** → trigger `scripts/build-*.bat` which runs the shader compile step.
