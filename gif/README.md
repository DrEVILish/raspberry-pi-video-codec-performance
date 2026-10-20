# Animated GIF decoding in GStreamer on Raspberry Pi 4 / ARM: frame-parallel, 87-92 fps at 1080p

GStreamer decodes animated GIF with `avdec_gif` on one thread: about 33-35 fps at 1080p on a Pi 4 (24.5 ms a frame). GIF frames' LZW data are independent of each other (only compositing depends on earlier frames), so `cutepianimdec` decodes them on several threads with its own LZW decoder and composites them in order: **87-92 fps** (106 for a GIF with transparency). Disposal methods, transparency and interlaced GIFs are handled, and output matches FFmpeg's decoder. A 50 fps GIF plays at its full rate.

**Plugin:** `cutepianimdec`, in [`../animated-images/`](../animated-images/) (install steps there). It handles GIF, APNG,
WebP, AVIF and JPEG XL with one element.
