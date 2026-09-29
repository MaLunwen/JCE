# Editor integration

Read editor/AGENTS.md and the closest panel/core charter. Search for an existing panel, shared widget and public SDK mechanism before adding another UI path.

Panel IDs are persisted: preserve existing enum values and append new IDs. Trace registration, dispatch, configuration serialization, menus, tabs and visibility; adding a switch case alone does not wire a panel.

UI text uses the editor i18n mechanism. Register component defaults, serialization and inspector/undo behavior through the existing owners. Do not add a separate component schema or bypass SDK boundaries.

For input and Play changes, verify activation, focus, mouse capture/release, keyboard delivery, stop/restore and entity selection across repeated cycles. A temporary editor-input gesture must not accidentally drag dock panels or leave stale runtime handles in the inspector.

File viewing tests include activation on open, session restoration, tab selection and background audio/video suspension. Runtime resources and play state require their matching teardown paths.

```bash
python tools/lint/check_editor_consumer_purity.py
python tools/lint/check_inspector_undo_scope.py
python tools/lint/check_editor_consumption.py
```

Run the relevant interaction/runtime regressions and the required editor build. Split large files by responsibility instead of duplicating panel state.
