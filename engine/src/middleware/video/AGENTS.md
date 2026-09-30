# engine/src/middleware/video — Video decode (L4)

> Container parsing + codec decode for cinematics and video textures.

## Identity

- **Layer**: L4. C99 (+ one `.cpp` for webm_parser bridge).
- **Public umbrella**: no dedicated public; consumed via resource/streaming and audio (shared codec types).
- **Deps (PRIVATE)**: `dav1d` (AV1), `libvpx` (VP8/VP9), `webm` (parser), `Opus`. Optional `OpenH264` + `libhevc` gated by `JCE_PATENTED_CODECS_ENABLED`.

## File map

| Header | TU | Role |
|--------|----|------|
| `jce_video.h` / `jce_video_types.h` | `jce_video.cpp` | Public façade: open, decode frame, query metadata |
| `jce_mp4_parser.h` | `jce_mp4_parser.c` | MP4/ISO-BMFF container |
| `jce_webm_parser.h` | `jce_webm_parser.cpp` | WebM container (libwebm bridge) |
| `jce_av1_decode.h` | `jce_av1_decode.c` | AV1 decode (dav1d) — royalty-free default |
| `jce_vp8_decode.h` / `jce_vp9_decode.h` | `jce_vp8_decode.c` / `jce_vp9_decode.c` | VP8/VP9 decode |
| `jce_h264_decode.h` / `jce_h265_decode.h` | `jce_h264_decode.c` / `jce_h265_decode.c` | H.264/H.265 — **gated** behind `JCE_ENABLE_PATENTED_CODECS=1` |
| (internal) `jce_aac_decode.h` | `jce_aac_decode.c` | AAC-LC access-unit decode (fdk-aac) — **gated**. Sole fdk-aac wrapper; also consumed by `middleware/audio/jce_m4a_decode.c` |
| `jce_yuv_convert.h` | `jce_yuv_convert.c` / `jce_yuv_convert_avx2.c` | YUV→RGB. AVX2 TU compiled only on x86 with `/arch:AVX2` or `-mavx2` (CMake gates it). |

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
