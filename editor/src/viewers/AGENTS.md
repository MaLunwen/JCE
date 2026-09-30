# editor/src/viewers/ — File Previewers

## Identity
- **Language**: C++17 + ImGui
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
