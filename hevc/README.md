# HEVC / H.265 on the Raspberry Pi 4: hardware decoder speed and clocks

The Pi 4 decodes HEVC (Main and Main 10) in a separate hardware block (`rpi-hevc-dec`, V4L2 stateless; FFmpeg
`-hwaccel drm`, GStreamer `v4l2slh265dec`).

| 1080p HEVC hardware decode (FFmpeg `-hwaccel drm`) | fps |
|---|---|
| Default clocks | 143.9 |
| Codec clocks held at 500 MHz | 146.7 |
| 600 MHz | 150.0 |
| 650 MHz + ARM 2.0 GHz | 155.9 |

- **HEVC is less affected by the clock floor than H.264.** `rpi-hevc-dec` already asks for its own clock
  (`clk_set_min_rate`), so the [`../hardware/firmware-clocks/`](../hardware/firmware-clocks/) setting matters less
  here.
- **HEVC frames are not a plain layout.** The decoder writes them tiled (SAND128 column format), with Y and UV
  columns of different heights. DRM can describe only one height per framebuffer, so an HEVC frame can't be put on a
  vc4 display plane as decoded. A working route points the plane at the decoder's Y plane and GPU-copies only the UV
  ([`../hardware/`](../hardware/README.md#9-the-memory-bus-is-the-shared-limit)).
- **Pairs well with H.264:** one HEVC and one H.264 1080p60 stream in hardware run at once at 59.95 + 59.93 fps on
  screen. They use different engines.
- **Copy cost:** asking the decoder for untiled system-memory frames costs a CPU detile, about 1 fps at 1080p. Keep
  HEVC frames as DMABufs.
