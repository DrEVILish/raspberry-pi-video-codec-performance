# Animated GIF, APNG, animated WebP, AVIF sequences and JPEG XL animations in GStreamer, with alpha: cutepianimdec

One GStreamer decoder for the five animated image formats. GStreamer 1.26 (Debian trixie, Raspberry Pi OS) plays
them slowly or not at all:

| Format | Stock GStreamer | `cutepianimdec`, 1080p on a Raspberry Pi 4 |
|---|---|---|
| Animated GIF | `avdec_gif`, one thread, ~33-35 fps | **87-92 fps** (106 with alpha): frames LZW-decoded in parallel, composited in order |
| Animated WebP | `webpdec` stills only; libwebp's `WebPAnimDecoder` 12 fps | **45.7 fps** (42.0 with alpha): libwebp demux API, frames in parallel |
| APNG | `pngdec` shows the first frame only (no APNG demuxer) | **58.9 fps** (50.5 with alpha), dispose/blend ops |
| AVIF (still and sequence `avis`) | `qtdemux` claims it and finds "no playable streams"; libavif alone decodes a sequence at 35.4 fps | **60.2 fps** (39.5 with alpha): own ISOBMFF parser, two frame-threaded dav1d instances, libavif YUV→RGB |
| JPEG XL (still and animated) | "Could not determine type of stream" | **plays**, with alpha (libjxl, its own threads) |

**How it works:**
- **Input:** the whole file is collected, then frames are decoded and pushed in order with their own timestamps
  and durations.
- **Output:** straight-alpha RGBA, one buffer per frame, from a pool.
- **Parallel formats:** in GIF, WebP and APNG every frame's compressed data is a complete image of its region (only
  compositing depends on earlier frames), so worker threads decode frames ahead into a ring and one task composites
  them in order.
- **Sequential formats:** AVIF and JPEG XL frames depend on earlier ones, so their libraries decode in order, with
  their own threads.
- **Seeking:** TIME seeks (loops) restart the decode and skip to the frame shown at the target time.

**Correctness**, checked against the reference decoders:
- GIF and APNG against FFmpeg, including interlaced GIFs and APNG dispose/blend.
- WebP against libwebp's `WebPAnimDecoder`.
- AVIF sequences against libavif.
- JPEG XL against the libjxl conformance samples.

**Type finders:**
- It registers type finders for APNG, AVIF and JPEG XL at `primary + 1`. Without them, GStreamer types an APNG as a
  plain PNG and doesn't know the other two.
- The element registers at `primary + 1`, so `playbin` picks it.

**Runtime libraries, no `-dev` packages:**
- It `dlopen`s `libwebpdemux.so.2`/`libwebp.so.7`, `libavif.so.16` (+ `libdav1d.so.7`), `libjxl.so.0.11`
  (+ `libjxl_threads`) and `libpng16.so.16`.
- It builds against the headers vendored in `include/` (same versions as Debian trixie). Their licences are next to
  them.
- A format whose library is missing is not offered.

## Install

```sh
sudo apt install build-essential pkg-config libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
# runtime libraries the element loads (any missing one disables just that format):
sudo apt install libwebpdemux2 libwebp7 libavif16 libdav1d7 libjxl0.11 libpng16-16t64 gstreamer1.0-plugins-good
git clone https://github.com/DrEVILish/raspberry-pi-video-codec-performance.git
cd raspberry-pi-video-codec-performance/animated-images/frame-parallel-animated-decoder
make && make check && make install      # or: sudo make install-system
```

Remove: `make uninstall`. Turn off without removing: `export GST_PLUGIN_FEATURE_RANK=cutepianimdec:0`.

## Use and verify

```sh
gst-launch-1.0 playbin uri=file:///path/to/animation.webp       # .gif .png/.apng .webp .avif .jxl
# Make test files:
ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=25 -t 4 -f gif test.gif
ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=25 -t 4 -c:v libwebp -loop 0 test.webp
ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=25 -t 4 -plays 0 -f apng test.apng
time gst-launch-1.0 filesrc location=test.webp ! cutepianimdec ! fakesink
```

**Alpha** stays straight (not premultiplied). Composite with a blend that expects straight alpha, as `kmssink`
planes and `compositor` do.

## Per-format notes

- [`../gif/`](../gif/)
- [`../apng/`](../apng/)
- [`../webp/`](../webp/)
- [`../avif/`](../avif/)
- [`../jpeg-xl/`](../jpeg-xl/)
