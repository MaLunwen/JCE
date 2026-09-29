# Consumer examples and completeness projects

examples/ holds SDK consumer projects developed or recreated to test engine
features and completeness through the public APIs available to project users.

| Project | Purpose | Source and terms |
| --- | --- | --- |
| [caged_kingdom](caged_kingdom/README.md) | Final game and feature laboratories | [terms](caged_kingdom/SOURCE_AND_TERMS.md) |
| [elemental_serenity](elemental_serenity/README.md) | Diorama recreation and AI scene orchestration | [terms](elemental_serenity/SOURCE_AND_TERMS.md) |
| [minesweeper](minesweeper/README.md) | Components, values and AI runtime recipes | [terms](minesweeper/SOURCE_AND_TERMS.md) |
| [snake_seven](snake_seven/AGENTS.md) | Seven scripting languages in one game | [terms](snake_seven/SOURCE_AND_TERMS.md) |

Keep project policy, generators, assets, dedicated probes and design contracts
inside each project. General tools take explicit project/input/output arguments
and do not embed game rules. Build through scripts/jce.py app <project> and the
installed SDK. Consumers use public API umbrellas, never private/dependency
headers. Author runtime behavior in scenes, components, values and scripts when
existing APIs support it.

Each project records origins, applicable notices, asset provenance limits and
acceptance evidence. Original external repositories remain external and read
only. Migration does not authorize vendoring original code or relicensing
imported assets. Generated output and private assets stay ignored. Report
missing engine/editor capabilities with a reproducible case; do not hide gaps
with project special cases. Keep current measurements separate from history.
