# editor/src/viewers/ — File Previewers

## Identity
- **Language**: C++20 + ImGui
- **Role**: read-only inspectors for asset files (used by `jce_panel_file_viewer.cpp`).

## File map
- `jce_file_viewer.h`        — abstract `IFileViewer` + factory.
- `jce_fv_common.h`          — shared helpers (zoom, fit, scrollbar).
- `jce_fv_zoomable.cpp`      — zoom/pan widget mixin.
- `jce_fv_image.cpp`         — `.png/.jpg/.tga/.ktx2/.dds` (uses `stb_image` via engine).
- `jce_fv_audio.cpp`         — `.wav/.ogg/.opus/.aac/.mp3` waveform + transport.
- `jce_fv_video.cpp` (33 KB) — `.mp4/.webm/.ivf` frame preview (engine video pipeline).
- `jce_fv_model.cpp` (28 KB) — `.glb/.gltf/.fbx/.obj` 3D preview (assimp import, bgfx render).
- `jce_fv_material.cpp`      — `.jcematerial` shader/material preview.
- `jce_fv_physmat.cpp`       — `.physmat.json` PhysicsMaterial editor (friction / restitution / combine modes; writable, mirrors `jce_fv_material.cpp`'s edit-and-save pattern).
- `jce_fv_render_pipeline.cpp` — `.rp.json` Render Pipeline Asset editor (P3-E.4): feature toggles, quality knobs, target format, preset buttons; live-applies via `jce_render_pipeline_apply()` on save.
- `jce_fv_code.cpp` (25 KB)  — text/code (`.c/.h/.cpp/.lua/.sc/.json/.toml/.md`) with syntax tokens.
- `jce_fv_hex.cpp`           — binary fallback hex view.

## Rules
1. **Read-only**: viewers MUST NOT modify the file or any editor state beyond `jce_editor_state` view-cache fields.
2. **Lazy decode**: decode on first show; release GPU/audio resources on hide.
3. **Memory cap per viewer**: 64 MB (image), 8 MB (audio peak buffer), 256 MB (video frame ring). Anything larger streams.
4. **Use engine decoders** — image via `<jce/api_resource.h>`, audio/video via `<jce/api_audio.h>` / video middleware. No re-vendoring.
5. **Hex viewer is the universal fallback** when MIME unknown.
6. **`jce_fv_zoomable`** is the canonical pan/zoom mixin — image/video/material reuse it.

## Don't
- Don't load full video into RAM — stream frames.
- Don't run audio output through ImGui — route to engine mixer with a preview channel.
- Don't render 3D into a separate window — use the embedded bgfx framebuffer.

## Common tasks
- **Add a viewer** → `jce_fv_<type>.cpp` implements `IFileViewer` → register in factory in `jce_file_viewer.h`.

## Streaming media previews

Audio/video tabs retain paths and 64-bit file size, not encoded file bytes.
Audio uses `jce_audio_file` with a 2048-bin asynchronous waveform. Video uses
`jce_video_load_file` and bounds large-source previews on the decoder worker.
The resident preview-size setting still applies to buffered document/image
formats; it is not a host media-file size limit. Optional `JCE_VIDEO_TRACE`
and `JCE_AUDIO_TRACE` report actual playback, not fixed-step simulation.

Explicit video play waits for a published picture and ready audio before
starting either clock; the first playback tick excludes prior load time.
The first GPU texture also waits for two renderer submissions before either
clock starts, so backend shader/texture warmup cannot become a playback gap.
Pause, hide and scrubbing cancel pending starts as well as active playback.

Video start/release uses a resume request, not an immediate playing flag. Keep
the clock pinned and wait for a published target picture plus buffered audio.
Scrub destroys the old stream voice to discard device resampler cache; create
its replacement only after readiness. Rewind uses the same release path.
The opt-in JCE_DBG_FILE_PREVIEW_SEEKS probe drives these real scrub functions.

## Small-image cursor preview

The image toolbar offers an opt-in cursor preview using public
`jce_cursor_create_rgba8` and `jce_cursor_reset`. The tab manager retains only
decoded RGBA sources with both dimensions at most 64 pixels (16 KiB per tab),
never full-size image copies. Large and GPU-only images remain disabled.
The hotspot is the top-left pixel; this is a transient asset inspection effect
and does not modify source files or project settings.

The editor's normal cursor arbiter applies the preview only over the visible
image canvas and only for the arrow cursor outside game capture. Text and resize
cursors retain precedence. Switching, hiding or closing the tab, or shutdown,
restores the platform cursor before destroying the preview handle. Path ownership
survives the tab array's compaction. Platform creation failure is shown in the UI.

JCE_DBG_FILE_PREVIEW_MUTE=1 is an explicit QA-only video preview control. Each
viewer-owned audio instance sets master gain before creating any stream voice and
logs the actual public getter value. Seek/restarted voices use that same muted
instance; default product playback remains unchanged. The witness proves endpoint
gain, not OS-wide mute or acoustic output.

Video preview pairs reference-decoder backpressure with its actual PCM voice:
One source-frame interval of picture lag pauses voice/transport without a
re-seek; queued output is pumped, settling the video deadline only to PCM
already consumed after pause, and readiness within that same frame
interval resumes the same PCM cursor. Invalid FPS uses the existing30FPS
fallback. The threshold does not add the validator numerical tolerance.
Opt-in traces report cumulative stall time; this preserves synchronization
under insufficient decode throughput rather than claiming wall-clock speed.

Trace playing retains UI intent; requested_playing repeats that intent, while
effective_transport_active excludes buffering and requires actual voice play
readback when audio exists. voice_playing=-1 means no valid voice, not paused
proof. Paused callback settling is observed from actual PCM timestamps; no
fixed in-flight device period is promised. All buffering lag rows remain raw.

Opt-in buffer lifecycle events carry monotonically increasing episode id and
seek epoch plus actual PCM/picture/voice snapshots. Holds sample at most every
50 ms; no event claims settling without offline observation of stable actual
cursors. JCE_DBG_FILE_PREVIEW_TRACE_MS=100..1000 changes only QA snapshot
cadence; normal trace cadence remains1000ms.

Active picture admission and resume compare against the later of the public
video deadline and actual PCM cursor; interpolated deadline lead cannot hide
a late picture. Paused positive settling still targets consumed PCM only.

Viewer close and teardown retire an active buffer episode before destroying
its voice/video/audio, using the same actual-cursor cancellation witness as
explicit pause and seek. Process termination without orderly teardown remains
unobserved, never synthesized as cancellation by the validator.

JCE_DBG_FILE_PREVIEW_TRACE=1 enables only viewer snapshots, source/upload
dimensions and buffer lifecycle witnesses. It does not enable decoder/PCM
per-tick logs. Existing JCE_VIDEO_TRACE compatibility remains; default is off.
Mute getter and seek readiness witnesses retain their explicit QA controls.

Inactive-tab QA uses JCE_DBG_FILE_PREVIEW_SWITCH_TEXT with SWITCH_MS and
RETURN_MS (the same JCE_DBG_FILE_PREVIEW_ prefix), measured from first visible
media tab. It opens a normal text tab, then requests ordinary media-tab selection
without autoplay. The focus witness records actual paused voice and clocks before
releasing inactive video resources; it does not claim a retained frozen clock.

Buffer resume additionally requires actual stream quiescence acknowledgement.
Lifecycle quiescent=0/1 is the public query; -1 means no stream voice exists.
voice_playing=0 alone never proves consumption has finished.

Inactive focus release logs are emitted only after synchronous voice teardown and
video unload return. Their requested/voice/media/audio fields are snapshots taken
before release, not post-release retained-clock observations.

Ordinary active playback presents the existing due queue output before its
starvation check. An already active buffer episode still runs its full settling/
PCM/quiescence reconciliation before normal advancement. Existing seek/scrub/
inactive/paused guards precede this transport step. Budgets, PTS lookahead and
sync admission remain unchanged; a real queue underrun still fails post-advance
admission. The shared private ordering helper is used by production and native
negative controls; actual device seek/EOF tests remain separate acceptance.

Video admission first presents output due for an already-consumed PCM cursor,
using `fv_video_consumed_settle_delta` capped at one picture interval. It rereads
actual picture/video/PCM before the unchanged strict guard; it never predicts
future audio. Missing output still pauses/quiesces and existing buffering, seek,
inactive and paused paths retain their guards. Zero-duration buffering counts
must be reported separately from elapsed stall time; a successful clock check
is not an independent acoustic or displayed-image synchronization oracle.
