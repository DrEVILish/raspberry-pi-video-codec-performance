# JPEG XL (JXL) stills and animations in GStreamer, with alpha

GStreamer on Debian trixie has no JPEG XL element or type finder ("Could not determine type of stream"). `cutepianimdec` registers a JXL type finder (bare codestream and container) and decodes with libjxl (`libjxl.so.0.11`, loaded at run time) using libjxl's own thread runner, into straight-alpha RGBA at each frame's duration. It was checked against the libjxl conformance samples.

**Plugin:** `cutepianimdec`, in [`../animated-images/`](../animated-images/) (install steps there). It handles GIF, APNG,
WebP, AVIF and JPEG XL with one element.
