# VP9 on the Raspberry Pi 4: decoder choice, two streams, alpha

| 1080p60 VP9 (8 Mbit/s, libvpx realtime), Pi 4, flat out | fps |
|---|---|
| `vp9dec` (libvpx), GStreamer's default | 97 |
| `avdec_vp9` (FFmpeg) | 92 |

- **Keep the default** (libvpx) for VP9. Unlike VP8, it's slightly faster here.
- **Two VP9 1080p60 streams at once:** 53.7 + 53.7 fps (the memory bus is the limit). VP9 plus a hardware HEVC
  stream: 59.87 + 60.00.
- **VP9 with alpha (WebM)** decodes through `vp9alphadecodebin`: two VP9 decoders, colour and alpha, combined to
  A420.
  - **Speed:** 61 fps flat out as A420 at 1080p, but 50 fps once `videoconvert` turns it into RGBA on the CPU.
  - **Better:** a sink or compositor that takes A420 directly. GStreamer GL's `glcolorconvert` does, and the GPU
    then converts. Or use [`../common/system-dmabuf-upload/`](../common/system-dmabuf-upload/)-style dma-heap
    buffers.
