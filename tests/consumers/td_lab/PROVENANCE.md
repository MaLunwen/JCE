# Tower Defense source and asset provenance

Reference repository: [Casmo/tower-defense](https://github.com/Casmo/tower-defense).
The acquisition tool requires the exact immutable commit
`784129a685627a18f3d3f8fa594208000d9313e8` and a clean original checkout.
Repository notice: [Casmo MIT](resources/licenses/Casmo-MIT.txt), copyright
Mathieu, 2014. The upstream JavaScript/CSS runtime and browser libraries remain
reference-only; this C99 SDK consumer is independently authored.

## Reproduction records

[acquire_reference.py](tools/acquire_reference.py) records upstream file paths,
byte lengths, SHA-256 values, classification and license basis in the ignored
`build/reference/manifest.json`; model conversion details are in
`build/reference/imports.json`. These are reproduction records, not caches to
check into Git. The original reference checkout remains read-only and outside
the repository's tracked tree. [author_ground.py](tools/author_ground.py)
reconstructs the original ground geometry; `ground-reference.json` records its
project-level parameters.

The recorded upstream LICENSE hash is
`b0726f052c1a0cafcc61afdb0fe426c6440a97bf3bf9e23605b0d0fbb60cf901`
(1,093 bytes). The shipped notice is LF-normalized, 1,073 bytes, SHA-256
`e2ac78714c6988662df08906ece56a3489ca0fa7787be4f511d494e764602b3a`.
Reconstructing CRLF produces the recorded upstream hash exactly; source and
notice byte identities are distinguished rather than declared identical.

## Selected model records

The following values come from the recorded acquisition manifest and import
record; current output hashes were independently matched. Conversion uses the
public `jce_cook --convert-model --geometry-only` entry point. Tower 02 uses the
first OBJ object, matching the reference loader's first-child selection; other
listed models use the complete object.

| Upstream asset | Upstream SHA-256 | Consumer GLB | Output SHA-256 | Object policy |
|---|---|---|---|---|
| `assets/towers/tower-01.obj` | `3bbce440e64cf8b62fe37739c5cf48a03f913847ff4790f1b672ce24c906183c` | `resources/models/tower-01.glb` | `dae3639e11c91d30140d53dbccf0cc3a2e5f8ac02a8e867f9d5f415945b64fe8` | whole object |
| `assets/towers/tower-02.obj` | `31f675d15fc1aa2823f48488936ecef7e7121c55145b7a4b90445118284dc354` | `resources/models/tower-02.glb` | `d6e694e27cd44b217ae1460d9a28b29fe423cbe33a42591e12274561a3318628` | first OBJ object |
| `assets/towers/tower-03.obj` | `6d514773beefb2138861f7991e1b8a1004f09e71d6c057e23163ad8d5829c6ba` | `resources/models/tower-03.glb` | `010522b7642ab8317dba6ce394980afa0d92213fcd572dbd946cb5100fea31c3` | whole object |
| `assets/enemies/ghost.obj` | `d79f64d4c5e4a455bdadaf27223cf9a5bb56d90f655c36ff157dc45509840b72` | `resources/models/ghost.glb` | `76dc4bafdc2059c78ebde52f740d94d34e1291c0ab4434ff885defdd5d4d6994` | whole object |
| `assets/enemies/ufo.obj` | `2707186f406b9ae46d5e6782083732016f37d6c80ca552d09e1c3a86ce979ef9` | `resources/models/ufo.glb` | `4f99bb22424c5913ae6ea6b362ac2546eaad1c12398fefd27f518a5d36c10466` | whole object |

## Published asset inventory

[ASSET_PROVENANCE.json](ASSET_PROVENANCE.json) records all 186 selected resource
files with lengths, current SHA-256 identities and their source or generation
policy. It covers original texture artwork, converted models, reconstructed
ground, generated UI images, metadata and notices. The project-specific
[write_asset_provenance.py](tools/write_asset_provenance.py) `--check` verifies
this inventory against current files. Raw acquisition/import records remain
local reproduction evidence; the selected inventory is public source metadata.
Repository-license classifications retain their stated scope and are not an
independent legal determination for every upstream asset.

Generated text artwork now uses Pillow 12.3.0's bundled Aileron default instead
of the original Distant Galaxy or a hard-coded Windows font. The font source
states [No Rights Reserved](https://dotcolon.net/fonts/aileron/); the selected
subset identity and provider are recorded in
[ui-font-provenance.json](resources/licenses/ui-font-provenance.json). Only
rasterized UI images are distributed, not font software. An explicit authoring
font input requires its own terms. The typography change is deliberate; original
captures remain reference evidence, and the replacement still needs final
artifact-bound visual acceptance.

Original browser/runtime/font files are listed as excluded in the inventory.
No ShyFonts-derived text glyphs, original browser dependency tree, build cache
or raw captures are intended publication inputs. File identity and provenance
checks do not establish visual, functional or performance acceptance.

[The Bundle catalog](../../../examples/td_lab/README.md) publishes one contextual
content archive with source, terms and exact provenance. Editor Play requires
the native TdController class and td_module authoring factory. Two contextual
reopen/Play/Stop controls do not establish stock-editor portable gameplay.
Independent single-EXE/HTML execution and screenshot/performance acceptance
remain separate artifact-bound claims.
