# Architecture and ownership

Read AGENTS.md and contracts/module-memory-index.md before choosing a file.
Use public engine/include/jce/api_*.h umbrellas to discover APIs; inspect their
implementation and call sites before adding a second implementation.

| Layer | Owner |
| --- | --- |
| Compatibility and platform | engine/src/os/core and engine/src/os/platform |
| Rendering and resource primitives | engine/src/renderer and engine/src/resource |
| Subsystems | engine/src/middleware |
| Runtime and lifecycle | engine/src/runtime and engine/src/application |
| Consumers | editor, examples, scripting and SDK tools |

Low layers never depend on higher ones. Consumers use the packaged SDK;
editor panels do not include engine/src or upstream headers. Cross-language
bindings consume the flat C ABI. Generic tools take explicit project inputs.
Project balance, art, scenes and dedicated generators belong with the project.

Automated build/check implementations live in tools/build, tools/lint and
tools/audit. scripts holds manual entrypoints and native shell feedback.
contracts holds maintained machine inputs and interface specs. docs and .docs
are local content; private contains unpublished workflow implementations.
The public core must work when none of these local directories are present.

Original vendor copies are untracked and immutable. Active dependencies come
from verified pins in contracts/vendor-sources.json and the external source
cache. Do not format or patch them. Put JCE adaptations in owned wrapper files.

Do not use old line counts or API totals to decide whether a module is current.
Run the repository gates and inspect the present tree.
