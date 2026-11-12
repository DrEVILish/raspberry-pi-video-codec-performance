# Animated WebP in GStreamer: frame-parallel decoding, 45.7 fps at 1080p on a Pi 4 (3.8× libwebp's animation decoder)

GStreamer's `webpdec` decodes WebP stills only, and FFmpeg 7.1 can't decode animated WebP (support arrived in 8.0). libwebp's own `WebPAnimDecoder` decodes frames one at a time: 12 fps for a 1080p30 file on a Pi 4. `cutepianimdec` uses libwebp's demux API to decode each frame's data independently on several threads (`WebPDecodeRGBAInto` straight into the frame patch), then composites in order: **45.7 fps** (42.0 with alpha), matching `WebPAnimDecoder`'s output.

**Plugin:** `cutepianimdec`, in [`../animated-images/`](../animated-images/) (install steps there). It handles GIF, APNG,
WebP, AVIF and JPEG XL with one element.
