# Upstream bugs and suggestions found during this work

Ready-to-file reports (with reproductions, verified on the versions named in each):
- [`ffmpeg-matroska-qtrle.md`](ffmpeg-matroska-qtrle.md): FFmpeg's Matroska muxer writes an empty QuickTime sample
  description for `V_QUICKTIME` tracks, so QuickTime Animation MKVs it writes are unreadable, by FFmpeg too.
- [`gstreamer-matroskademux-v_quicktime.md`](gstreamer-matroskademux-v_quicktime.md): `matroskademux` exposes
  `V_QUICKTIME` tracks (HAP, DNxHR, QuickTime Animation) as `video/x-unknown`. The workaround is
  [`../../hap/`](../../hap/).

Not yet written up as reports:

| Project | Issue | Evidence here |
|---|---|---|
| FFmpeg | Animated WebP decoding only arrived in 8.0. Debian trixie ships 7.1 | [`../../webp/`](../../webp/) |
| gst-libav | `avdec_qtrle direct-rendering=false` outputs the first frame and drops the rest (1 of 480). Reproduce with `qtdemux ! avdec_qtrle direct-rendering=false ! fakevideosink silent=false -v` | [`../../qtrle/`](../../qtrle/) |
| GStreamer / gst-libav | for decoders that reuse their last frame (`reget_buffer`), `finish_frame`'s `make_writable` deep-copies every output frame, because the AVFrame still maps the memory. That's one 8 MB copy per 1080p ARGB frame | [`../../qtrle/`](../../qtrle/) |
| GStreamer `webpdec` | animated WebP: libwebp's demux API decodes frames independently, so frames can be decoded in parallel and composited in order. 45.7 fps on a Pi 4 against 12 for `WebPAnimDecoder` | [`../../webp/`](../../webp/) |
| GStreamer | an APNG demuxer (or animation in `pngdec`): today an APNG plays its first frame only | [`../../apng/`](../../apng/) |
| GStreamer / Debian | AVIF and JPEG XL elements and type finders in a default install. `qtdemux` claims AVIF and finds "no playable streams" | [`../../avif/`](../../avif/), [`../../jpeg-xl/`](../../jpeg-xl/) |
| GStreamer `avdec_gif` | one thread, about 33-35 fps at 1080p. GIF frames can be decoded in parallel: 87-92 fps here | [`../../gif/`](../../gif/) |
| libavif | image sequences on multi-core ARM: dav1d runs with `max_frame_delay = 1` (one frame at a time). Sequences played start to end could use frame threading: 35 → 60 fps here | [`../../avif/`](../../avif/) |
| gst-libav | FFmpeg's Theora decoder (frame-threaded, 2.4× libtheora on a Pi 4) is deliberately not exposed | [`../../theora/`](../../theora/) |
| Linux (`bcm2835-codec`) | clock requests, start order, cache hints | [`../../h264/bcm2835-codec-patches/`](../../h264/bcm2835-codec-patches/) |
| go-gst | `ElementFactory.GetMetadata` calls `C.free` on the string `gst_element_factory_get_metadata` returns (transfer none). That corrupts the factory: after a few calls `decodebin` stops recognising it as a decoder | [`../pitfalls.md`](../pitfalls.md) |
