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
