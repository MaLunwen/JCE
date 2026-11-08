# Public SDK bundle-template consumer

This headless consumer verifies the installed project-template and bundle APIs.
Use public <jce/...> headers only. No engine/src, upstream headers, private
state, project-specific gameplay or platform macros. It emits SDK templates
under an explicit output directory; it never copies build trees or replaces
an existing project. Generated applications and test records stay under build/.
Build through scripts/jce.py app against an explicit installed SDK.

The emitter declares a minimal empty startup scene so current DIST builds use
the ordinary authenticated selected-content admission. This scene is fixture
bootstrap content; it does not replace the explicit input Bundle or certify
its rendering. Build against the matching installed SDK variant.
