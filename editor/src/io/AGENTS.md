# editor/src/io/ — Scene & Prefab Serialization

## Identity
- **Language**: C++17
- **Role**: load/save scene & prefab files (`.jcescene`, `.jceprefab`) — JSON-based, human-mergeable.

## File map
- `jce_editor_scene_serial.cpp` (26 KB) — write side (entity tree + components → JSON).
- `jce_editor_scene_parse.cpp`          — read side (JSON → ECS).
- `jce_editor_prefab.cpp`               — prefab create / apply / revert / overrides.
- `jce_editor_file_util.h`              — path / extension / atomic-write helpers.

## Rules
1. **JSON via cJSON** (engine-provided) — no nlohmann/json.
2. **Atomic writes**: write to `*.tmp` → fsync → rename. Never partial-write a scene file.
3. **Stable key ordering** for diff-friendliness: components alphabetical, fields per reflection order.
4. **Reflection-driven**: serializer queries `editor/src/core/jce_reflect.cpp` — never hand-list fields per component.
5. **Backward-compat**: keep a `"version": N` at scene root. Migration handled in `_parse.cpp` with explicit upgrade chain.
6. **GUIDs**: every entity & asset reference uses 128-bit GUID stored as hex string. Never local pointer / index.
7. **Prefab overrides** stored as field-level patch (path → new value), not full re-serialization.
8. **All paths PhysFS-mounted** — never absolute filesystem paths in saved files.
9. **Bind content context before deserialize** — source loads follow their project root; Bundle/Catalog loads install the isolated VFS and refresh render/component paths before creating entities. Unmount on every return to a plain scene, including async loads.

## Don't
- Don't serialize transient runtime state (collision contacts, render IDs).
- Don't crash on unknown components — log a warning and skip with placeholder.
- Don't reorder JSON keys per save (causes spurious VCS churn).

## Common tasks
- **Add a new component to scene format** → register in `jce_reflect_builtin.cpp`; serializer picks it up.
- **Add format version migration** → bump `JCE_SCENE_FORMAT_VERSION`, add upgrade step in `_parse.cpp`.
