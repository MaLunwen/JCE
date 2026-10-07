# Source, terms and project contract

Authored for the user request and local design prompt .docs/way/扫雷提示词.txt. No third-party game implementation or separate upstream game repository was supplied. README.md and AGENTS.md define the scene/runtime and AI integration contract.

Source: [MaLunwen/JCE](https://github.com/MaLunwen/JCE), examples/minesweeper.
Repository source terms: [MPL-2.0](../../LICENSE). Imported assets/dependencies
retain their original terms; do not infer asset rights from the code license.
Dedicated tools and policy are retained in `../../tests/consumers/minesweeper`. Consume only public SDK
interfaces. Original third-party code is not vendored here.

## Catalog layout

The Git example directory now contains the normal single-file `.jbundle` and
notices only. Authoring inputs and project regression tools moved to
`tests/consumers/minesweeper`; ignored local archives/generated output did not move.
The original source snapshot revision and asset rights limitations stated above
remain unchanged. See BUNDLE_PROVENANCE.json for the exact normal artifact hash.
