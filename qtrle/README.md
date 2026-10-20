# QuickTime Animation (qtrle) on the Raspberry Pi 4: MKV bug, colour-conversion cost, copies

QuickTime Animation is run-length coded RGB, `RGB` (24-bit) or `ARGB` (with alpha). Each frame is a delta on the
previous one, so frames can't be skipped or decoded in parallel. GStreamer uses FFmpeg's `avdec_qtrle`.

- **Don't put it in MKV when writing with FFmpeg.** FFmpeg's Matroska muxer writes the QuickTime sample description
  zeroed after the fourcc (depth, width and height 0). Neither FFmpeg nor GStreamer can then decode the file:
  "Unsupported colorspace: 0 bits/sample?". Use MOV. A correctly written MKV plays through
  [`../hap/`](../hap/)'s parser. Upstream report: [`../docs/upstream/`](../docs/upstream/).
- **Avoid a CPU colour pass on ARGB.**
  - **The cost:** `avdec_qtrle` outputs `ARGB`, and converting it to `RGBA` with `videoconvert` costs 17.7 ms of CPU
    a 1080p frame, which capped the alpha variant at about 10 fps.
  - **The fix:** feed a sink or upload path that takes `ARGB` as it is. DRM's `BGRA8888` is `ARGB` in GStreamer's
    byte order, so a display plane that lists `BGRA8888` can take it directly (check your plane's formats with
    `modetest -p`). Or do the byte reorder with NEON inside a copy you make
    anyway.
- **One 8 MB copy a frame you can't avoid in GStreamer today.** For decoders that reuse their previous frame
  (`reget_buffer`), `GstVideoDecoder`'s `finish_frame` deep-copies every output frame, because the AVFrame still
  maps the memory.
- **Don't set `direct-rendering=false`** on `avdec_qtrle`: it outputs the first frame and drops the rest (1 of 480),
  a gst-libav bug.
