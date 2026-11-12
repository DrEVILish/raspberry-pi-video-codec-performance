# APNG (animated PNG) in GStreamer: all frames with alpha, 58.9 fps at 1080p on a Pi 4

GStreamer has no APNG demuxer: `pngdec` shows the first frame and gst-libav exposes no `avdemux_apng`. `cutepianimdec` parses the `acTL`/`fcTL`/`fdAT` chunks, rebuilds each frame as a single PNG, decodes them in parallel (libpng's simplified API) and composites them in order with the APNG dispose and blend ops. Output matches FFmpeg's. It also registers an APNG type finder, so `playbin` sees `video/x-apng` instead of a still PNG. APNG files usually have the `.png` extension; `.apng` works too.

**Plugin:** `cutepianimdec`, in [`../animated-images/`](../animated-images/) (install steps there). It handles GIF, APNG,
WebP, AVIF and JPEG XL with one element.
