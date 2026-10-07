> Historical authoring contract; sources now live in `tests/consumers/minesweeper`.
> The current normal single-file delivery hash is in BUNDLE_PROVENANCE.json;
> this document does not substitute for actual acceptance evidence.

# Portable Lua bundle validation candidate

Use resources/scenes/minesweeper.bundle.scene.json as the selected scene. It replaces only the native controller script reference with scripts/MinesController.lua; the original scene and native project remain unchanged. Existing director.lua still validates AI component proposals and submits keyboard commands. No fake native descriptor is included.

| Capability | Candidate status |
|---|---|
| Three presets/custom bounds, exact mines, first-click 3x3 safety, flood reveal, flags/chord, victory/loss, HUD/timer | Implemented in Lua; CPU rule checks passed; actual editor simulation pending |
| Left/right/middle pointer input | Existing public pointer position/button bindings; previous-state edges, reference Canvas scaling; actual simulation pending |
| AI proposal component validation/custom configuration/theme | Existing director.lua reused; data-only proposals, no model execution |
| Best record | Session and stored records separated; public host statistics require available service and successful set plus flush; actual cross-run validation pending |
| External LLM request/poll | UNAVAILABLE in this Lua host; UI says so, no fake success |
| Quit application | Public request_quit result checked; actual host exit validation pending |

CPU check: lua resources/scripts/test_portable_rules.lua resources/scripts/MinesController.lua. Use an explicit Lua5.4-compatible executable in automation. The controller also runs pure rule self-checks on start. These are not GPU, UI or bundle reopen acceptance.

Export only the selected bundle scene using the existing public packer, with resources as root. Verify complete scripts and asset closure, reopen away from the original native project and simulate before accepting. This is a migration-before-validation candidate, not final parity or permission to remove native source. Pending generic host capabilities must be completed or honestly declared before full delivery. No vendor/platform dependencies are added.
