# Render liveness lab

This installed-SDK C99 consumer owns only controlled input, capture and evidence
policy. Production rendering, window/frame lifecycle, shader ownership, PAK
lookup, trace and readback remain the public JCE owners. No engine/src, editor/src
or third-party includes, no replacement GPU/frame loop and no new dependency.

- src/main.c: public JCE_MAIN descriptor and explicit run configuration.
- src/fixture.c: one renderer, stable anchor PAK, retained fallback archives,
  fixed receiver/grass/sun/camera, actual trace cursor and asynchronous captures.
- tools/prepare_inputs.py: verifies existing shader stock, copies registered
  bytes only and invokes the public PAK CLI with deep-verification BOMs.
- tools/validate.py: independent actual phase identity and pixel controls.
- tools/run.py: existing shared process sampler and backend/image readers.
- CMakeLists.txt: installed JCE::JCE, shared embedding helper, normal and
  missing-reset control targets.

The lab hard-refuses a populated embedded engine-shader PAK: otherwise a missing
project stage could silently render through fallback. Do not override the private
embedded accessor or alter production loader code to make this test run. A
separate matching no-embedded-shader SDK is a prerequisite, not a default SDK
replacement. Complete boot resources are still embedded through jce_add_pak.

Generated inputs, binaries and evidence belong under ignored build/. Keep
upstream/source stock untouched. Compile and run only under the root's build/GPU
lease. Tests are diagnostic correctness runs, not FPS, GPU execution timing,
physical-device qualification or single-file product delivery acceptance.

NoReset must compile and run, then fail the same real recovery witness normally
at colour_recovered. Compile failures, startup failure, crashes and absent GPU
execution do not count as that negative control.
