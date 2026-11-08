# engine/src/middleware/video — Video decode (L4)

> Container parsing + codec decode for cinematics and video textures.

## Identity

- **Layer**: L4. C99 (+ one `.cpp` for webm_parser bridge).
- **Public umbrella**: `<jce/api_video.h>`; shared audio types remain in the audio API.
- **Deps (PRIVATE)**: `dav1d` (AV1), `libvpx` (VP8/VP9), `webm` (parser), `Opus`. Optional `OpenH264` + `libhevc` gated by `JCE_PATENTED_CODECS_ENABLED`.

## File map

| Header | TU | Role |
|--------|----|------|
| `jce_video.h` / `jce_video_types.h` | `jce_video.cpp` | Public façade: open, decode frame, query metadata |
| (private) `jce_video_slot_internal.h` | `jce_video.cpp` | Decoder/slot/queue declarations; ownership stays in the façade |
| (private) `jce_video_lifecycle_trace.h` | `jce_video.cpp` | C99 seek-token/WAIT helpers over the existing core trace ring |
| `jce_mp4_parser.h` | `jce_mp4_parser.c` | MP4/ISO-BMFF container |
| `jce_webm_parser.h` | `jce_webm_parser.cpp` | WebM container (libwebm bridge) |
| `jce_av1_decode.h` | `jce_av1_decode.c` | AV1 decode (dav1d) — royalty-free default |
| `jce_vp8_decode.h` / `jce_vp9_decode.h` | `jce_vp8_decode.c` / `jce_vp9_decode.c` | VP8/VP9 decode |
| `jce_h264_decode.h` / `jce_h265_decode.h` | `jce_h264_decode.c` / `jce_h265_decode.c` | H.264/H.265 — **gated** behind `JCE_ENABLE_PATENTED_CODECS=1` |
| (internal) `jce_aac_decode.h` | `jce_aac_decode.c` | AAC-LC access-unit decode (fdk-aac) — **gated**. Sole fdk-aac wrapper; also consumed by `middleware/audio/jce_m4a_decode.c` |
| `jce_yuv_convert.h` | `jce_yuv_convert.c` / `jce_yuv_convert_avx2.c` | YUV→RGB. AVX2 TU compiled only on x86 with `/arch:AVX2` or `-mavx2` (CMake gates it). |
| `jce_video_color.h` | AV1/VP8/VP9 adapter TUs | Private decoder-owned matrix/range queries; no upstream types cross this boundary |

## Rules

1. **Royalty-free path first**: AV1 / VP8 / VP9 / Opus are always available. H.264 / H.265 / AAC are off by default in `dist`.
2. **Patent guards**: every patented-codec TU must compile to a no-op (or stub error) when `JCE_PATENTED_CODECS_ENABLED=0`.
3. **SIMD opt-ins**: new SIMD TUs need matching CMake `set_source_files_properties(... COMPILE_OPTIONS ...)` blocks + an `HEADER_FILE_ONLY` fallback for non-matching archs (mirror the AVX2 YUV pattern).
4. **Decoded frames** are returned as YUV planes + stride; conversion to RGB is opt-in. Renderer can sample YUV directly with a shader.
5. **No graphics calls** from this layer — pure CPU decode. Texture upload happens in renderer.
6. **third_party/** dirs are ignored and never modified. Builds use pristine,
   SHA256-verified sources from `contracts/vendor-sources.json` in the build
   cache. JCE compatibility/build adapters live in `os/platform/codec_ports/`
   and `engine/cmake/codec_ports/`. `jce_mp4_parser.c` extends original minimp4
   through its own box walker; codec records borrow the parser-owned bounded moov buffer.

## Pixel conversion witnesses

The native color regression checks the hardware-selected path against an
independent scalar BT.601 formula, then checks the non-AVX2 path. Widths 1-65,
odd height, padded planes, saturated values and output guards cover SIMD lanes
and tails. Never force unsupported CPU instructions in a portable test.
AVX2 green-channel madd pairs are [V, 0]: the 208 coefficient must occupy the
low word. Retained same-PTS YUV decoded by an independent implementation can
distinguish conversion errors from packet, stride or seek-content errors.
The opt-in JCE_TEST_VIDEO_SEEK_RGBA witness saves public CPU RGBA output only;
it is separate from cadence timings and does not prove GPU presentation.
BT.601, BT.709 and BT.2020 NCL limited/full-range kernels share one parameter
block across scalar, SSE2, AVX2 and NEON. The independent floating-point inverse
uses ITU-T H.273 Kr/Kb and a one-byte fixed-point rounding budget. Decoder color
travels with each queued picture and seek serial; previews retain it. AV1 and
VPx expose decoded-picture metadata, while HEVC uses libhevc's public VUI query.
This is 8-bit matrix/range conversion, not HDR tone mapping or gamut conversion.
For 4:2:0, odd decoded crop sizes require ceil(width/2) by ceil(height/2) UV
allocation, copy and stride. The committed odd AV1/VP9 controls compare the
entire public seek picture with independent reference bytes without FFmpeg.
Opt-in JCE_TEST_VIDEO_RESUME_SECONDS=1..5 retires the old public audio voice
before seeking, waits for current picture/PCM readiness, and starts a fresh
voice. Separate 50 ms clock samples verify resumed progression. This headless
probe does not prove editor upload or acoustic speaker presentation.

## Don't

- Don't hard-link patented codecs in `release`/`dist` default — must remain optional.
- Don't expose dav1d/libvpx types in public headers.
- Don't do YUV conversion on the render thread per-frame without batching.

## AV1 packet ownership

MP4 uses the private timestamped pipeline in `jce_av1_packet.h`, with dav1d
automatic frame delay for parallelism. A send that leaves bytes unconsumed must
retain its reference and retry that SAME sample after receive; non-display OBUs
can produce no picture. Use returned picture timestamps, drain all delayed
pictures at EOF, and test frame count/content against the low-latency decoder.
IVF owns the same pending-reference rule. Public WebM packet mode remains
low-latency. Never modify dav1d sources to change scheduling.

## Device clock cadence

The audio cursor advances in output blocks. jce_video_clock.h predicts between
blocks and applies bounded rate correction; it never rewinds and caps lead at
20 ms during an audio stall. Do not snap each video deadline to the raw cursor:
the 60 Hz / 10 ms negative control skips 200 of 600 pictures despite a full
queue. Seek still owns its discontinuity; zero-time preview does not advance.
Real-device probes are opt-in through JCE_TEST_VIDEO_AUDIO_DEVICE=1 in the
existing video test, with JCE_TEST_VIDEO_SECONDS controlling the interval.
Set JCE_TEST_VIDEO_AUDIO_MUTE=1 to retain real PCM consumption/device cadence
without audible test output. Embedded-audio EOF cannot be required from a probe
that never consumes that stream; a video-only picture clock is not an AV test.

## Streaming file input

`jce_mp4_source.c` preflights bounded moov tables and reads absolute box headers.
MP4, WebM and IVF source APIs retain `JceReadSource`; video and embedded audio
share input rather than copying encoded files. WebM clusters load on demand.
`jce_video_frame.c` prepares bounded previews on the worker; RGBA publication
swaps buffers. Source/packet references outlive all decoder workers and seeks.

## Codec link ownership

bimg also contains a C-only dav1d with identical exported names. Pin the
canonical SIMD-enabled archive through `JCECodecLink.cmake`, without editing
upstream packages. SDKs ship that unchanged archive as `jce_av1_codec`. The
MSVC editor post-link gate rejects any codec symbol bound to bimg.

## Presentation timestamps and resume

Keep progressive STTS/fragment DTS for sample seeking. Decode packets carry
separate normalized CTTS/TRUN presentation stamps (signed version 1 included);
use the codec output stamp after B-frame reordering. Never relabel a seek's
picture with the target clock. Drain OpenH264 EOS/FlushFrame and libhevc FLUSH
before EOF. libhevc RESET also resets core count: restore the configured count.

Seek generations retain the old displayed picture until target preroll is
ready. Skip preview preparation for discarded reference frames. The owner uses
jce_video_is_ready_to_play before resuming output; audio readiness requires PCM
of the current generation, not merely track metadata. Positive advance remains
pinned while a seek picture or audio buffer is missing.
MP4 AV1 forward seeks within the current GOP may reuse the active dav1d
context when its decoded picture safely precedes the target. Backward/close
seeks reopen at the indexed random-access sample; never relabel a decoded PTS.

Publish RGBA as well as YUV output already decoded by seek. A general YUV
queue policy does not imply every codec emits YUV. Pop due pictures first;
seek look-ahead is a single fallback only when no due picture exists. The
B-frame regression requires exact rewind PTS 0 and forward PTS 1.6.

JCE_VIDEO_SEEK_TRACE=1 enables worker-owned seek diagnostics through the existing
logger, including generation, requested/keyframe PTS, actual decoded/preroll
pictures and index/reset/decode work milliseconds. queue_ready means a current
generation target picture was queued, not UI upload, audio or speaker readiness.
Unknown keyframe data stays explicitly unknown. Worker elapsed starts when the
request is picked up; it is not the public request-to-ready latency. Timed decode
work includes cancel checks and excludes presentation conversion. Disabled traces
perform no additional clock reads or logging; this adds no public ABI.

Private timestamp normalization (`jce_video_timestamp.h`) preserves repeated-PTS
and post-seek clock policy through explicit state inputs. `jce_video_perf.h`
owns the existing frequency cache and EMA arithmetic; `jce_video_seek_trace.h`
owns opt-in stage accumulation and terminal reporting without decoder state.
The frame helper owns pointer/capacity swaps used by queue/display handoff.

Late presentation policy uses a mutex-published owner target and seek serial.
Reference decoding continues, but obsolete conversion may be omitted only
when playback is active, exact landing is complete and duration is known.
The last frame interval remains protected; paused and unknown-duration inputs
retain presentation. Opt-in presentation-skip records count omitted conversion
separately from consumer drops without altering public performance structs.

H264 preserves default legacy RGBA adapter callers; the video worker explicitly
opts into owned tight I420 feed/pending storage and uses the common YUV queue
and preview processor. Copy codec planes before draining (borrowed OpenH264
planes can become invalid), preserve each output picture's reordered timestamp,
and invalidate plane views on flush. The pinned public codec API lacks a matrix/
range getter; current H264 BT.601 limited behavior is preserved, not complete VUI
color support. Native B-frame adapter parity validates pixels, PTS and flush;
real viewer preview/seek/EOF validation remains a separate acceptance step.

HEVC reuses the pinned decoder's sleeping processing workers by default. The
host core count remains clamped to eight; a single core creates no helper workers.
Upstream workers wait on start conditions between pictures rather than busy-spin;
active-picture job queues can still yield while awaiting work. Decoder close keeps
the existing break/signal/join lifecycle, releasing retained stacks and handles.
JCE_DBG_HEVC_KEEP_THREADS_ACTIVE accepts exactly 0 (legacy per-picture workers)
or 1 (persistent workers); other values preserve the generic default. The bounded
JCE_DBG_HEVC_THREADS QA override accepts exactly one digit 1..8. Resolve the core
budget before decoder creation and retain the existing set-core fallback to one.
Tracing remains explicitly opt-in, independently of the worker lifetime default.
Viewer-only or native tracing logs actual resolved cores and the creation flag;
it does not prove worker utilization or nominal frame-rate throughput.

On x86, HEVC starts with the generic decoder baseline. Existing SDL public
SSE4.1 and SSE4.2 runtime queries jointly admit the SSE4.2 support family;
this is not an independent SSSE3-bit probe or physical old-device certification.
Only intrinsic source files receive higher GCC/Clang instruction flags.
SET_PROCESSOR success selects the architecture and RESET restores that selection;
control failure retries generic and refuses decode if even generic fails.
JCE_DBG_HEVC_GENERIC=1 explicitly forces generic for modern-host negative controls.
No AVX2 or new dependency is enabled. Non-x86 policy remains unchanged.

JCE_DBG_VIDEO_FRAME_PERF=1 enables private worker frame-preparation EMA samples.
scale_ms includes jce_yuv_preview_prepare and its plane allocation/copy/scale;
color_ms measures only the existing color converter, excluding RGBA allocation.
Existing public convert-total statistics retain their original scope and ABI.
Records reuse JCE_DBG_FILE_PREVIEW_TRACE_MS cadence. Disabled diagnostics do not
invoke additional clocks. These are sampled EMA observations, not frame percentiles.

Frame timing records identify context=worker or context=owner. The owner uses
an independent processor and lends its current display RGBA buffer before
preparation, then recovers ownership even on failure. No second persistent
owner full-frame RGBA buffer is retained; private preview planes are lazy and
bounded by the existing requested dimensions. Shared checked RGBA byte math
rejects 32-bit overflow before allocation or queue copy.

HEVC producer diagnostics reuse JCE_DBG_VIDEO_FRAME_PERF=1 and the existing
100-1000 ms preview trace cadence. Decoder-owned private state reports separate
source-read (lock/cache/file read), AnnexB preparation (including growth), actual
codec API call, and publish (VUI/legacy optional RGBA) EMA/counters. Codec calls
include header, seek and drain work; empty-input/no-output counters make that
scope explicit. Stage populations differ, so their EMAs must not be summed as
an output-frame percentile. Existing public decode total remains unchanged.
Disabled observations add no clock reads or allocations. jce_h265_perf.h is a
private adapter/packet bridge, including C linkage for the C++ video consumer;
public headers, codec archives and thread/presentation policies are unchanged.

Frame preparation rejects missing planes, nonpositive dimensions and undersized
luma/chroma strides before any allocation or conversion. Failure preserves the
previous RGBA pointer, capacity, dimensions and payload; this is a generic input
contract guard, independent of diagnostic experiments.

## Common lifecycle trace

The core trace gate owns lifecycle observations independently of
JCE_VIDEO_SEEK_TRACE logs. Disabled lifecycle producers do not allocate IDs or
read extra clocks. Open records metadata-only versus an available decoder;
decoder-open and actual decoder-read CPU scopes are nested under the owner.
A false decoder-read result is pending/no-picture or EOF, not a blanket failure.
The existing thread trampoline registers workers; video never registers twice.

Queue-mutex-owned request tokens carry an owner ID and actual seek serial.
Worker pickup moves the token, and only the worker terminates active work.
A request overwritten or closed before pickup records cancellation without a
false pickup. Queue readiness requires the actual q_count increment, current
serial and a target picture; a push helper returning true after interruption
or without pixels cannot certify readiness. Known seek failure/EOF and
supersession have distinct terminals. Actual full-queue and EOF condition
waits are paired WAIT events. Commit, pop, stale/old-generation/preroll/obsolete
drop, clear and known queue allocation failures record scalar observations.
No path, encoded input or pixel buffer appears in events.

Close records the real worker join before streaming-audio, decoder and source
release scopes. The private slot header only moves declarations out of the
existing large bridge; there is no second decoder or profiler implementation.
`test_jce_video_lifecycle_trace` consumes committed original AV1/VP9 clips through
the actual public facade, plus exact seek-token controls. It checks disabled
tracing, malformed open, queue WAIT, serial terminals and join-before-release.
These CPU observations do not certify GPU upload, visible frames, speaker
output or deadline/performance acceptance. Its silent clips do not exercise
streaming-audio destruction; the metadata-only codec branch remains covered
by the existing codec policy controls rather than being claimed by these clips.
