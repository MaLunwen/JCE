# engine/src/middleware/save — Save / Snapshot (L4)

> Save-game and runtime snapshot persistence.

## Identity

- **Layer**: L4. C99.
- **Public header**: `<jce/middleware/save/jce_snapshot.h>` (consumed via `<jce/api_middleware.h>`).
- **Deps**: `jce_core` (filesystem, json, zstd, xxHash).

## File map

| Header | TU | Role |
|--------|----|------|
| `jce_snapshot.h` | `jce_snapshot.c` | Capture ECS world → blob; restore blob → world. Versioned + checksummed. |

## Rules

1. **All I/O via PhysFS** (writes go to the user-writable mount, never the read-only PAK).
2. **Versioning**: every snapshot has a magic + version + xxHash checksum. On version mismatch, run a migration or refuse the load (never silently load corrupted data).
3. **Compression**: zstd by default; pass-through on tiny payloads (< 4KB).
4. **Component (de)serialization** reuses `scene/jce_scene_components_json.c` — do NOT duplicate per-component schema.
5. **Async save** via `jce_jobs` to avoid stalling gameplay.

## Don't

- Don't serialize raw memory layouts — go through component JSON or a stable binary schema.
- Don't write to host paths directly — use PhysFS write dir.
- Don't load a snapshot whose checksum fails.
