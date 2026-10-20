# cutepiyuvpack: NEON 10-bit 4:2:0/4:2:2 and 8-bit 4:2:2 to I420 for GStreamer

Packs `I420_10LE`, `I422_10LE` and `Y42B` frames to 8-bit `I420` in one NEON pass:
- **Luma:** a rounding, saturating shift-and-narrow.
- **Chroma:** rows averaged in pairs.

Any other format passes through untouched, so it can sit in a pipeline unconditionally.

| 1080p 10-bit 4:2:2 frame to I420, one Pi 4 core | Time |
|---|---|
| `videoconvert` | 20 ms |
| `cutepiyuvpack` | **2.35 ms** |

**Why:**
- **ProRes, DNxHR, H.264 High 10 and HEVC Main 10** decode in software to 10-bit and/or 4:2:2.
- **Display planes and `kmssink` on the Pi 4 take I420, but can't allocate 4:2:2 buffers.** `kmssink`'s pool fails to
  activate, so `videoconvert` falls back to RGB and runs at 0.8-14 fps.
- **With `cutepiyuvpack` first**, ProRes 422/HQ/LT/Proxy went from 0.3-1.4 fps to 37-54 fps, and DNxHR from 20-27 to
  60, on KMS planes.

**Trade-off:** 4:2:2 → 4:2:0 halves the vertical chroma resolution, and 10 → 8 bits drops two bits (rounded, no
dither). That's the same as any 8-bit 4:2:0 display path.

## Install and use

```sh
cd common/yuv-pack-neon && make && make check && make install
gst-launch-1.0 filesrc location=prores.mov ! qtdemux ! avdec_prores ! cutepiyuvpack ! videoconvert ! kmssink
```

`videoconvert` after it passes I420 through untouched when the sink takes I420.

## Verify

```sh
gst-launch-1.0 videotestsrc num-buffers=10 ! video/x-raw,format=I420_10LE ! cutepiyuvpack ! video/x-raw,format=I420 ! fakesink
```
