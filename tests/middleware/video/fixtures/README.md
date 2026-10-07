# Synthetic MP4 regression fixtures

Original test patterns and silent video, generated for JCE under the repository
license. No user media or third-party source code is included. Both files have
90 display pictures at 30 fps, B-frame reordering, and normalized JCE
presentation PTS 0 through 89/30 seconds. The fragmented container has a
nonzero raw timestamp origin, which the player normalizes on real output.
The second uses fragmented MP4 sample tables. Normal unit tests require these
files; they do not depend on installed ffmpeg or an audio output device.

To regenerate the signals (ffmpeg with libx264):

```sh
ffmpeg -f lavfi -i testsrc2=size=320x240:rate=30:duration=3 -c:v libx264 -threads 1 -bf 3 -g 60 -an -movflags +faststart bframes.mp4
ffmpeg -i bframes.mp4 -c copy -movflags +frag_keyframe+empty_moov bframes-fragmented.mp4
ffprobe -select_streams v:0 -count_frames -show_entries stream=has_b_frames,nb_read_frames,duration bframes.mp4
```

The unit test checks output count/final PTS and forward seek/rewind readiness.
Before the EOS drain fix these fixtures yielded 87 rather than 90 pictures.

## Odd crop and SDR color fixtures

`odd-av1.mp4` and `odd-vp9.webm` contain self-generated 67x51 color bars, 60
pictures at 30 fps, limited range, BT.709 matrix, no audio. Their `.rgba`
companions use independent FFmpeg-decoded YUV frame 30 (PTS 1.0), the ITU-T
H.273 floating-point inverse and explicit nearest chroma reconstruction,
matching the declared CPU kernel policy. Normal unit tests compare all channels
after an exact seek, allowing at most one byte for fixed-point conversion.
No external tool is required
at test execution. These are original procedural test signals under the
repository license, not user media or third-party source.

The first-party generator is `tests/consumers/benchmark/tools/media_color_fixtures.py`;
use explicit `--ffmpeg`, `--out`, `--size 67x51 --codec av1 --codec vp9` inputs.
`media_pixel_suite.py --oracle nearest-yuv` retains independent frame records,
YUV planes and reference pixels. Default FFmpeg RGB reconstruction is a separate
filter policy; it must not be silently substituted for this nearest oracle.
Original generation used FFmpeg N-120833-gdb2af6fd42-20250825: AV1 libaom with
cpu-used=8, crf=20, threads=2; VP9 libvpx lossless=1, realtime, cpu-used=8,
threads=2. Encoder versions may produce different encoded bytes on regeneration;
retain the actual commands and hashes rather than claiming byte identity.

| Fixture | SHA256 |
| --- | --- |
| odd-av1.mp4 | 97ecffcc8c6a861c20aee3e53b38f511e39cfc4997d46bcb8db59a164bbb8ae6 |
| odd-av1.rgba | 9ec13e762f921cecb6af8d6dc4d777da995eca761c0d09df5d161318f79cab82 |
| odd-vp9.webm | 99f4bb99ace1d7f1af0b39f843dbb880062c1b0fd6c44b899160856f4a21f980 |
| odd-vp9.rgba | b6bff7e1ad5381e97db159e96a25233fcdb59c15f775e8df99f3bbcb1e657b1e |

Before the queue-size fix, twelve independently decoded 67x51 matrix/range
controls failed. Chroma extents must round UP to cover the final luma row/column.
