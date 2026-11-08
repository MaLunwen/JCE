# Bundle project template consumer

The emitter opens a real bundle through the public SDK, creates the standard
empty-project template, copies only that bundle and saves the declared bundle
list. The resulting application uses the SDK default lifecycle and embedded
bundle support, rather than an independently implemented runtime.

Set JCE_TEMPLATE_INPUT_BUNDLE, JCE_TEMPLATE_OUTPUT_PROJECT and
JCE_TEMPLATE_PROJECT_NAME, then run jce_bundle_template_emit. The public
JCE_MAIN bootstrap is configured headless and exits after project emission.
The project directory must be new. Build that emitted project with
`scripts/jce.py app <project> --delivery-profile embedded --sdk <sdk>`.
This console emitter creates no rendering window. Runtime graphics validation
is separate and uses hidden windows and isolated output directories.

The emitter declares a minimal empty startup scene so current DIST builds use
the ordinary authenticated selected-content admission. This scene is fixture
bootstrap content; it does not replace the explicit input Bundle or certify
its rendering. Build against the matching installed SDK variant.
