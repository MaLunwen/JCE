# Platforms and low-end paths

Compatibility goals and actual dependency/toolchain floors are distinct.
Read AGENTS.md, conan profiles, CMakePresets.json and the selected consumer's
charter. Report platform evidence separately; one Windows run does not verify
macOS, Android, iOS or wasm.

Use python scripts/jce.py targets to enumerate current native targets.
Use python scripts/jce.py --dry-run sdk --arch x64 --variant release --no-debug
before a new build recipe. Native SDK examples:

    python scripts/jce.py sdk --arch x64 --variant release --no-debug --profiling off
    python scripts/jce.py sdk --arch arm64 --variant release --no-debug --profiling off
    python scripts/jce.py sdk --arch wasm --variant release --no-debug

The build driver resolves MSVC when needed and uses pinned Conan profiles.
Emscripten requires the installed emsdk environment. Mobile/manual entrypoints
require explicit JCE_GAME_PROJECT_DIR and JCE_GAME_TARGET. SDK/NDK locations
come from environment or arguments, never a developer's disk path. CK-specific
launchers belong under that consumer's scripts directory.

Android uses verified SDL Java from the external cache and installed Gradle
8.14.1. No upstream Java or generated Gradle wrappers belong in the repository.
iOS requires a macOS/Xcode host and consumer-supplied signing settings.

Design default behavior for one core, 512 MiB and integrated graphics. Choose
capability tiers using the engine's APIs; provide fallbacks instead of turning
hardware limits into silent rendering errors. The graphics tier is selected by
--graphics-api-tier stable|modern|current; wasm accepts stable only.

Run a bounded real frame or SDK consumer on each measured target. Keep frame
rate, GPU backend and memory evidence in local reports. Do not claim a target
from a configure-only check, a placeholder CI job or an absent toolchain.
