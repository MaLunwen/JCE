# Single-file Bundle catalogs

`examples/` publishes single-file content Bundles with source links, licenses,
terms and provenance. Authoring source, project policy and dedicated regression
tools live under `tests/consumers/`; reusable build tools remain under `tools/`.

| Catalog | Published content | Authoring source / terms |
|---|---|---|
| [Elemental Serenity](elemental_serenity/README.md) | `elemental_serenity.jbundle`, portable Lua variant | [consumer](../tests/consumers/elemental_serenity/), [terms](elemental_serenity/SOURCE_AND_TERMS.md) |
| [Minesweeper](minesweeper/README.md) | `minesweeper.jbundle`, portable Lua variant | [consumer](../tests/consumers/minesweeper/), [terms](minesweeper/SOURCE_AND_TERMS.md) |
| [Snake Seven](snake_seven/README.md) | `snake.jbundle`, portable Lua variant | [consumer](../tests/consumers/snake_seven/), [terms](snake_seven/SOURCE_AND_TERMS.md) |
| [Tower Defense](td_lab/README.md) | `tower-defense.jbundle`, contextual content; native `TdController`/`td_module` required | [consumer](../tests/consumers/td_lab/), [terms](td_lab/SOURCE_AND_TERMS.md), [Bundle provenance](td_lab/BUNDLE_PROVENANCE.md) |
| [Caged Kingdom](caged_kingdom/README.md) | Owned game scaffold; implementation currently absent | [project charter](caged_kingdom/AGENTS.md) |

CK is retained as the owned production project. All its frozen/reference/AI
materials under `caged_kingdom/way/` are temporarily local and untracked by owner
instruction. Scaffold metadata is not runnable-game evidence.

Open the published `.jbundle` directly in the editor. A content archive does not
imply native code or optional external VMs are embedded; each catalog states its
actual driver and requirements. TD currently needs its native authoring module
for contextual Editor Play; independent single-EXE/HTML application verification
is separate from portable content publication.

Consumers develop through public API/SDK entry points and `scripts/jce.py` with
explicit project inputs. Original external repositories stay outside Git and
unchanged. Build trees, cooked caches, raw evidence and private materials remain
ignored. Source licenses and per-asset terms retain their own scope; migration
alone does not establish redistribution rights or visual/functional acceptance.
