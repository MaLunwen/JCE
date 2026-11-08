# Installed SDK smoke consumer

This C99 foreign project uses only installed public headers, `JCE::JCE` and
the shipped cook/pack tools. `scripts/jce.py sdk --smoke` is the native build
and execution authority; a Web consumer requires its own actual runner.

Native smoke descriptors explicitly request headless boot: selecting the noop
renderer alone still creates an OS window. Preserve Web's descriptor behavior
and the ordinary PAK/API/clock checks, update/quit callbacks and runtime markers.
Only an actual installed SDK consumer run qualifies the new headless entry.

The camera clone check requires the installed symbol and independent ownership
after source mutation/destruction, retaining an orthographic projection and
rolled pose. It does not inspect the engine private camera, prove pixels or
replace the renderer unit and Editor Play lifecycle tests. Preserve the
existing success/failure markers and ordinary embedded asset/clock checks.

The mandatory offscreen extent call requires the installed public C99 symbol
and verifies absent-owner output clearing. Actual allocation/clamp/detach
behavior belongs to test_jce_offscreen_trace; this SDK link check does not
certify graphical targets, readback dimensions or GPU completion.

The mandatory exclusive-copy call requires the installed public C99 symbol and
invalid-input ownership clearing, including the required out_created pointer.
Real exclusive creation, bounded streaming and read/write/close failure evidence
belong to test_jce_filesystem_host_atomic; this link check does not certify them.

The host-observation symbol checks consume the installed C99 Engine and ImGui
interfaces. Init is outside app.draw and must reject reports even if a capture
is configured; uninitialized ImGui admits a sized request with unavailable
flags and zero counters. This proves foreign archive linkage and those CPU
boundaries, not a native viewport, actual GPU submit or whole-frame timing.

The WaterSpectrum consumer uses installed public C99 component and shared-field
APIs. It checks version/size/default flags, TMA authoring and removal with stable
field ownership. A fixed PM field also checks a broad physical variance bound
so an older archive with uncalibrated FFT coefficients cannot pass by exposing
the new symbols alone. It creates no graphics device and does not certify water pixels
or replace spectrum mathematics, lifecycle and backend validation.

The installed public-only water_spectrum.c smoke also exercises WaterFoam
version/size defaults, set/get/has/remove and shared-field pointer stability.
It does not read private history or certify rendered foam.

The installed water smoke also links and exercises WaterCrest default, setter, getter, presence and removal.

The installed water smoke also exercises the public WaterCaustics component.

The installed water smoke exercises WaterNearshore authoring and verifies the
inert composite query leaves its outputs intact.
