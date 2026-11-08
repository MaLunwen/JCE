# editor/src/io/ — Scene & Prefab Serialization

All source, async, memory, parsed-JSON, Bundle and Catalog scene entries must
check stop_play_before_scene_swap before any old mount/stream/scene retirement.
The async entry checks before finishing a prior deferred load. A stopped but
retained failed Play restore is still an owner: refuse without unmounting it.
close_bundle_preview returns false for that condition; callers must not publish
a new project root. Final state destruction settles this through the separate,
explicitly failed terminal-discard boundary in the Play owner.


## Identity
- **Language**: C++20
- **Role**: load/save scene & prefab files (`.jcescene`, `.jceprefab`) — JSON-based, human-mergeable.

## File map
- `jce_editor_scene_serial.cpp` (26 KB) — write side (entity tree + components → JSON).
- `jce_editor_scene_parse.cpp`          — read side (JSON → ECS).
- `jce_editor_prefab.cpp`               — prefab create / apply / revert / overrides.
- `jce_editor_file_util.h`              — path / extension / atomic-write helpers.

## Rules

Bundle and Catalog preview own a temporary RenderPipeline override through
jce_editor_bundle_render_pipeline. Prepare reads only the mounted PAK sources
with the public bounded API; a present invalid/over-limit RP refuses preview,
while confirmed absence explicitly selects the current tier. Before scene
deserialization, apply commits deferred project edits, saves requested intent
and installs the Bundle descriptor. Close restores that intent exactly once,
including failure and source-switch paths. Both loaders stop Play before old
content teardown. Existing project-context preflight/admission wires remain.
This contract does not promise rollback of later partial ECS deserialization.

The existing bounded CPU trace records synchronous
editor.bundle_pipeline.prepare/apply/restore owners. Refused prepare/apply are
FAILED; confirmed archive absence succeeds with bundle_rp.tier_fallback=1.
Real render_pipeline.publish scopes are children of apply/restore; the public
Core counters attach bundle_rp.active to that current owner (normally 1/0).
These scopes end before return and do not allocate a cross-frame identity.
Inactive/repeated close emits no restore transaction. This observes synchronous
policy publication and ownership only, not scene completion or GPU execution.

The public Editor close-preview entry owns Stop/pending-Play cancellation and
streamer teardown before mount/RP retirement. Project transitions call it while
the outgoing root still owns modules and before publishing the incoming RP,
including transitions without a following scene. Internal scene-loader close
paths keep their existing pre-swap ordering; repeated close cannot restore twice.

1. **JSON via cJSON** (engine-provided) — no nlohmann/json.
2. **Atomic writes**: write to `*.tmp` → fsync → rename. Never partial-write a scene file.
3. **Stable key ordering** for diff-friendliness: components alphabetical, fields per reflection order.
4. **Reflection-driven**: serializer queries `editor/src/core/jce_reflect.cpp` — never hand-list fields per component.
5. **Backward-compat**: keep a `"version": N` at scene root. Migration handled in `_parse.cpp` with explicit upgrade chain.
6. **GUIDs**: every entity & asset reference uses 128-bit GUID stored as hex string. Never local pointer / index.
7. **Prefab overrides** stored as field-level patch (path → new value), not full re-serialization.
8. **All paths PhysFS-mounted** — never absolute filesystem paths in saved files.
9. **Bind content context before deserialize** — source loads follow their project root; Bundle/Catalog loads install the isolated VFS and refresh render/component paths before creating entities. Unmount on every return to a plain scene, including async loads. Startup/last-scene async dispatch recognizes .jbundle and .jbundle.json before the plain JSON parser, using the same isolated loader as Open Bundle.

## Don't
- Don't serialize transient runtime state (collision contacts, render IDs).
- Don't crash on unknown components — log a warning and skip with placeholder.
- Don't reorder JSON keys per save (causes spurious VCS churn).

## Common tasks
- **Add a new component to scene format** → register in `jce_reflect_builtin.cpp`; serializer picks it up.
- **Add format version migration** → bump `JCE_SCENE_FORMAT_VERSION`, add upgrade step in `_parse.cpp`.

Source scene ownership uses core/jce_editor_project_context.h and the public
jce_project_find_root resolver, without a second parent walk or depth-eight
limit. A Bundle's storage directory is independent of its authoring context:
preserve a valid current project for declared native script modules and only
follow the physical package ancestor when no valid context exists. Do this
before mounting the isolated Bundle VFS; archive assets still resolve through
that VFS. A contextual authoring module is not a shipped runtime sidecar.

Scene opening first selects its candidate identity through the existing stateless project-context resolver. A real project change, including leaving a project for a loose scene with no discovered manifest, snapshots and publishes outgoing state before streaming or Bundle teardown. Same-project scenes and a Bundle preserving a valid active project do not force publication. Preflight does not rebind the incoming project. A loose scene closes the outgoing project through the ordinary root transaction rather than assigning its root string. Sync/async host JSON and contract admission precede outgoing publication and old mount/stream retirement; the existing Play restoration gate still runs first. The full root transition occurs before entity creation/clear. Sync and Bundle paths give the same read bytes to the existing public scene codec; async retains its already-parsed document. This is not full rollback of later project publication, ECS/stream/OOM failures or a failed Play restoration.

The existing update_scene_dir_from_path Scenes-parent rule binds models and the
shared loose content base before synchronous or incremental entity creation.
The scene render owner stores this content address separately from an authoring
or build project. Successful-load finalization does not bind it a second time.
Virtual Bundle/Catalog addresses never become loose host roots; isolated VFS
policy remains authoritative. A Bundle with no discoverable manifest also keeps
its prior no-transition behavior: it must not close its newly installed mount
and pipeline through a late project-root close. The focused controls exercise actual
follow/preflight/update/Scene/Game content bodies and counted destinations, not
the complete file loader, script VMs, font rasterizer, GUI or GPU.

The project-context fixture consumes exact dialog/scene admission bodies, the real public manifest/ancestor resolver and real owned host files, with counted publication-refusal ports. Configuration checks the actual sync/async/Bundle wires and preserve policy; the old-always-flush contract must compile and fail ordinary assertions. These controls do not certify GUI Save flows, full SceneSerial/ECS execution or later scene rollback.

Scene follow and asset-root comparison use the same platform-aware private path identity as preflight (Windows case-insensitive, POSIX case-sensitive). Case-different POSIX projects must still publish outgoing state and commit the new root. The native fixture has host-conditional real manifest/directory controls; a Windows run does not certify a POSIX filesystem.
