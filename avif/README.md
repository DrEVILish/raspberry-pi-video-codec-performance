# AVIF images and AVIF sequences (avis) in GStreamer: playable, 60 fps at 1080p with frame-threaded dav1d

GStreamer on Debian trixie has no AVIF decoder, and `qtdemux` claims AVIF files and then reports no playable streams. `cutepianimdec` registers an AVIF type finder and parses the ISOBMFF container itself (`ftyp avif/avis`, `moov` or `meta`). It decodes the AV1 frames with **dav1d with frame threading** and converts YUV to RGB with libavif. libavif alone runs dav1d one frame at a time (`max_frame_delay = 1`, about one core): 35.4 fps for a 1080p sequence. With two frame-threaded dav1d instances: **60.2 fps** (39.5 with alpha, where colour and alpha each need a decode; about 2.4 cores). Alpha (an auxiliary alpha image) is supported. Output matches libavif's.

**Plugin:** `cutepianimdec`, in [`../animated-images/`](../animated-images/) (install steps there). It handles GIF, APNG,
WebP, AVIF and JPEG XL with one element.
