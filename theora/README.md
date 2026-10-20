# Theora on the Raspberry Pi 4: libtheora is single-threaded (69 fps at 1080p); FFmpeg's decoder is 2.4× faster but not in GStreamer

| 1080p Theora (q 7), Pi 4, flat out | fps |
|---|---|
| GStreamer `theoradec` (libtheora, one thread) | 68.9 |
| FFmpeg's theora decoder (frame threads), `ffmpeg -i in.ogv -f null -` | 166 |

- **No faster decoder in GStreamer.** gst-libav deliberately doesn't expose FFmpeg's Theora decoder (its source:
  "Use libtheora until theora decoder is fixed"), so in GStreamer `theoradec` is the only option.
- **It plays,** but with little margin: 1080p60 needs 60. Another busy process on the cores (thumbnails, an import)
  makes it drop frames. Run background work at low priority with one thread
  ([`../docs/background-work.md`](../docs/background-work.md)).
- **Not done:** a GStreamer element on FFmpeg's Theora decoder (gst-libav plus a one-line change, or a small element
  against `libavcodec`, as [`../av1/`](../av1/) does for dav1d).
