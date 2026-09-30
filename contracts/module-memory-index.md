# Module ownership index

AGENTS.md is the root engineering authority. Each charter below owns the
rules for its module; follow the closest applicable charter. This index is a
maintained map, not a delivery plan or a snapshot of feature counts.

| Scope | Charter |
| --- | --- |
| Repository | [AGENTS.md](../AGENTS.md) |
| conan | [conan/AGENTS.md](../conan/AGENTS.md) |
| editor | [editor/AGENTS.md](../editor/AGENTS.md) |
| editor/resources | [editor/resources/AGENTS.md](../editor/resources/AGENTS.md) |
| editor/src/core | [editor/src/core/AGENTS.md](../editor/src/core/AGENTS.md) |
| editor/src/dialogs | [editor/src/dialogs/AGENTS.md](../editor/src/dialogs/AGENTS.md) |
| editor/src/game_modules | [editor/src/game_modules/AGENTS.md](../editor/src/game_modules/AGENTS.md) |
| editor/src/gizmo | [editor/src/gizmo/AGENTS.md](../editor/src/gizmo/AGENTS.md) |
| editor/src/io | [editor/src/io/AGENTS.md](../editor/src/io/AGENTS.md) |
| editor/src/panels | [editor/src/panels/AGENTS.md](../editor/src/panels/AGENTS.md) |
| editor/src/panels/material_graph | [editor/src/panels/material_graph/AGENTS.md](../editor/src/panels/material_graph/AGENTS.md) |
| editor/src/scene | [editor/src/scene/AGENTS.md](../editor/src/scene/AGENTS.md) |
| editor/src/shadergraph | [editor/src/shadergraph/AGENTS.md](../editor/src/shadergraph/AGENTS.md) |
| editor/src/ui | [editor/src/ui/AGENTS.md](../editor/src/ui/AGENTS.md) |
| editor/src/viewers | [editor/src/viewers/AGENTS.md](../editor/src/viewers/AGENTS.md) |
| editor/src/widgets | [editor/src/widgets/AGENTS.md](../editor/src/widgets/AGENTS.md) |
| engine/include/jce | [engine/include/jce/AGENTS.md](../engine/include/jce/AGENTS.md) |
| engine/include/jce/middleware/net | [engine/include/jce/middleware/net/AGENTS.md](../engine/include/jce/middleware/net/AGENTS.md) |
| engine/include/jce/middleware/physics | [engine/include/jce/middleware/physics/AGENTS.md](../engine/include/jce/middleware/physics/AGENTS.md) |
| engine/include/jce/middleware/scene | [engine/include/jce/middleware/scene/AGENTS.md](../engine/include/jce/middleware/scene/AGENTS.md) |
| engine/include/jce/renderer | [engine/include/jce/renderer/AGENTS.md](../engine/include/jce/renderer/AGENTS.md) |
| engine/include/jce/runtime | [engine/include/jce/runtime/AGENTS.md](../engine/include/jce/runtime/AGENTS.md) |
| engine/java | [engine/java/AGENTS.md](../engine/java/AGENTS.md) |
| engine/resources | [engine/resources/AGENTS.md](../engine/resources/AGENTS.md) |
| engine/shaders/graph | [engine/shaders/graph/AGENTS.md](../engine/shaders/graph/AGENTS.md) |
| engine/src/application | [engine/src/application/AGENTS.md](../engine/src/application/AGENTS.md) |
| engine/src/middleware/ai | [engine/src/middleware/ai/AGENTS.md](../engine/src/middleware/ai/AGENTS.md) |
| engine/src/middleware/animation | [engine/src/middleware/animation/AGENTS.md](../engine/src/middleware/animation/AGENTS.md) |
| engine/src/middleware/audio | [engine/src/middleware/audio/AGENTS.md](../engine/src/middleware/audio/AGENTS.md) |
| engine/src/middleware/net | [engine/src/middleware/net/AGENTS.md](../engine/src/middleware/net/AGENTS.md) |
| engine/src/middleware/physics | [engine/src/middleware/physics/AGENTS.md](../engine/src/middleware/physics/AGENTS.md) |
| engine/src/middleware/save | [engine/src/middleware/save/AGENTS.md](../engine/src/middleware/save/AGENTS.md) |
| engine/src/middleware/scene | [engine/src/middleware/scene/AGENTS.md](../engine/src/middleware/scene/AGENTS.md) |
| engine/src/middleware/ui | [engine/src/middleware/ui/AGENTS.md](../engine/src/middleware/ui/AGENTS.md) |
| engine/src/middleware/video | [engine/src/middleware/video/AGENTS.md](../engine/src/middleware/video/AGENTS.md) |
| engine/src/middleware/world | [engine/src/middleware/world/AGENTS.md](../engine/src/middleware/world/AGENTS.md) |
| engine/src/os/core | [engine/src/os/core/AGENTS.md](../engine/src/os/core/AGENTS.md) |
| engine/src/os/platform | [engine/src/os/platform/AGENTS.md](../engine/src/os/platform/AGENTS.md) |
| engine/src/renderer | [engine/src/renderer/AGENTS.md](../engine/src/renderer/AGENTS.md) |
| engine/src/resource | [engine/src/resource/AGENTS.md](../engine/src/resource/AGENTS.md) |
| engine/src/runtime | [engine/src/runtime/AGENTS.md](../engine/src/runtime/AGENTS.md) |
| engine/tools_include | [engine/tools_include/AGENTS.md](../engine/tools_include/AGENTS.md) |
| engine/ui | [engine/ui/AGENTS.md](../engine/ui/AGENTS.md) |
| examples/caged_kingdom | [examples/caged_kingdom/AGENTS.md](../examples/caged_kingdom/AGENTS.md) |
| examples/elemental_serenity | [examples/elemental_serenity/AGENTS.md](../examples/elemental_serenity/AGENTS.md) |
| examples/minesweeper | [examples/minesweeper/AGENTS.md](../examples/minesweeper/AGENTS.md) |
| examples/snake_seven | [examples/snake_seven/AGENTS.md](../examples/snake_seven/AGENTS.md) |
| scripts | [scripts/AGENTS.md](../scripts/AGENTS.md) |
| tests | [tests/AGENTS.md](../tests/AGENTS.md) |
| tools | [tools/AGENTS.md](../tools/AGENTS.md) |
| tools/audit | [tools/audit/AGENTS.md](../tools/audit/AGENTS.md) |
| tools/build | [tools/build/AGENTS.md](../tools/build/AGENTS.md) |
| tools/coverage | [tools/coverage/AGENTS.md](../tools/coverage/AGENTS.md) |
| tools/lint | [tools/lint/AGENTS.md](../tools/lint/AGENTS.md) |

Native standalone editor build and PE verification: tools/build/standalone.py,
owned by tools/build/AGENTS.md; editor selection: JCE_EDITOR_STANDALONE.
Native module dependency loading: engine/src/os/platform/jce_library.c;
regression: tests/os/platform/test_jce_library.c and its sibling DLL fixtures.

Public ownership and ignore boundaries: [source-layout.json](source-layout.json).
Public skill: [JCE en-US](../skills/jce/SKILL.md);
[zh-CN translation](../skills/jce-zh-cn/SKILL.md). Locale inventory: [skill-locales.json](skill-locales.json). `docs/`, `.docs/` and `private/`
are local-only and are not required by public source checks or builds.
