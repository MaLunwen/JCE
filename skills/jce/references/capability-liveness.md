# Capability liveness

A declaration, menu item, dependency or passing unit test does not prove a feature is used by a shipped application. Classify the capability and cite actual definitions and callers.

| State | Evidence |
| --- | --- |
| absent | Searches of public headers, implementation and consumers find no supported path |
| header-only | Public declaration exists without usable implementation |
| stub | An implementation returns a placeholder or reports success without doing the work |
| implemented-unwired | Usable implementation exists without product callers |
| wired-editor-only | Editor callers exist, shipped runtime callers do not |
| shipped | The required product path executes in the tested build variant |

Search the public umbrellas, component registry, implementation, editor, scripting and the relevant examples project before declaring absence. An internal implementation is evidence that machinery exists, even when a consumer API is missing.

```bash
rg --files engine/include/jce
rg -n 'jce_runtime_create|jce_runtime_step' engine/src editor/src examples scripting
```

Classify definitions, declarations, comments, tests and product callers separately. tests/ is tracked; test callers still do not establish product wiring. Inspect compile-time gates and configuration for the requested release/dist variant.

Measure both editor and shipped runtime when a parity claim covers both. Prove the relevant path executed before evaluating output quality. Historical counts and old captures must be remeasured against the current source. Use concrete repository paths; an ellipsis is prose, never a path whose existence is evidence.
