# Editor/runtime parity

Editor Play and a shipped game must use shared runtime, scene and input services rather than duplicate game logic. Trace each host's construction, per-frame submission and teardown through its public API.

Compare effective runtime descriptors, camera settings, input actions, script tick order, entity changes, resource mounts, navigation, save paths and streaming ownership. A matching struct definition does not prove both hosts populate it the same way.

Resource lookup may differ between loose project files, cooked directories and embedded PAKs. Test a relocated package without source-tree or loose-asset fallback before claiming embedded portability.

Match render backend and effective rendering settings before a pixel comparison. Verify that temporal passes execute and use the same clock; similar call traces do not guarantee equivalent images.

```bash
python tools/lint/check_runtime_desc_parity.py
python tools/lint/check_project_settings_consumed.py
python tools/lint/check_play_mode_isolation.py
```

Test repeated Play/Stop cycles and language backends in both hosts. Runtime shutdown must not invalidate authoring-scene selection or leak resources into the next simulation. Report the tested language/backend pairs, not merely that a scene opened.
