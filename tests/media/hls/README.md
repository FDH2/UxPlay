# Synthetic HTTP HLS fixtures

Four seconds of green AVC (16x16), blue HEVC (32x16), and silent AAC. These
contain no third-party media. The HTTP test deliberately advertises larger
resolutions in its master playlist to exercise filtering without storing large
test videos. Its request assertions identify the actual rendition chosen, and
the playback helper checks that both video and audio decode to EOS.
Multiple fragments let GStreamer's adaptive demuxer finish exposing both tracks
before either reaches EOS; a one-fragment fixture can race that startup.

Generated with FFmpeg 9.0.1 (the exact encoder version is not required to
regenerate equivalent fixtures):

```sh
ffmpeg -f lavfi -i 'color=c=green:s=16x16:r=10:d=4' -an \
  -c:v libx264 -preset ultrafast -g 10 -sc_threshold 0 \
  -f hls -hls_time 1 -hls_playlist_type vod -hls_segment_type fmp4 \
  -hls_fmp4_init_filename avc-init.mp4 -hls_segment_filename 'avc-%d.m4s' avc.m3u8
ffmpeg -f lavfi -i 'color=c=blue:s=32x16:r=10:d=4' -an \
  -c:v libx265 -preset ultrafast \
  -x265-params 'pools=1:frame-threads=1:log-level=error:keyint=10:min-keyint=10:scenecut=0' \
  -tag:v hvc1 -f hls -hls_time 1 -hls_playlist_type vod -hls_segment_type fmp4 \
  -hls_fmp4_init_filename hevc-init.mp4 -hls_segment_filename 'hevc-%d.m4s' hevc.m3u8
ffmpeg -f lavfi -i 'anullsrc=r=48000:cl=stereo' -t 4 -c:a aac \
  -f hls -hls_time 1 -hls_playlist_type vod -hls_segment_type fmp4 \
  -hls_fmp4_init_filename audio-init.mp4 -hls_segment_filename 'audio-%d.m4s' audio.m3u8
```

`hls_http` is registered with CTest when Python 3 is available and skips when
its HTTP/HLS or libav decoder plugins are missing. `hls_filter_input` tests
the manifest hook without those optional plugins, including every chunk size,
buffer lists, flush/restart, cancellation, oversized input and filter errors.
