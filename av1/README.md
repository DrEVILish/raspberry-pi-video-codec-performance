# AV1 on the Raspberry Pi 4: a GStreamer element for dav1d (2.4× faster than libaom's av1dec)

The Pi 4 has no AV1 hardware, so AV1 is decoded in software. GStreamer on Debian trixie / Raspberry Pi OS offers
`av1dec` (libaom), which is slow on ARM. FFmpeg links dav1d, but gst-libav doesn't expose it (`avdec_libdav1d` is
absent) and the GStreamer dav1d plugin isn't packaged. `cutepidav1ddec` is a small GStreamer video decoder around
the system's **libdav1d** (`libdav1d.so.7`, loaded at run time against vendored 1.5.1 headers).

| AV1 1080p60 (SVT-AV1 preset 12, CRF 35), Raspberry Pi 4 at 650 MHz/2.0 GHz, flat out | fps |
|---|---|
| `av1dec` (libaom) | 32.1 |
| `cutepidav1ddec` (dav1d, 4 threads) | **76.4** |

**How it works:**
- **Threads:** dav1d runs one thread per core with frame threading.
- **Zero-copy:** when downstream takes `GstVideoMeta`, the element hands dav1d's own picture buffers downstream, with
  no copy. dav1d keeps reading a picture while it is a reference, and downstream only reads too.
- **Outputs:** the stream's own layout: I420, Y42B, Y444 or GRAY8, and their 10-bit forms (I420_10LE, I422_10LE, Y444_10LE, GRAY10_LE16). Film grain is applied.
- **Rank** `primary + 1`: `playbin` picks it over `av1dec`.

| Property | Default | |
|---|---|---|
| `n-threads` | 0 | dav1d worker threads, 0 = one per core |
| `max-frame-delay` | 0 | frames decoded in parallel, 0 = automatic. 1 = lowest latency, slowest |
| `zero-copy` | true | hand dav1d's buffers downstream when it takes video meta (false: copy every picture) |

## Install

```sh
sudo apt install build-essential pkg-config libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libdav1d7
git clone https://github.com/DrEVILish/raspberry-pi-video-codec-performance.git
cd raspberry-pi-video-codec-performance/av1/dav1d-decoder
make && make check && make install          # or: sudo make install-system
```

`libdav1d.so.7` must be installed (`libdav1d7`); no `libdav1d-dev` is needed. Remove: `make uninstall`.

## Use and verify

```sh
gst-launch-1.0 playbin uri=file:///path/to/av1.mkv               # .mkv .webm .mp4 (AV1)
ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=60 -t 8 -c:v libsvtav1 -preset 12 -crf 35 av1.mkv
time gst-launch-1.0 filesrc location=av1.mkv ! matroskademux ! av1parse ! av1dec ! fakesink
time gst-launch-1.0 filesrc location=av1.mkv ! matroskademux ! av1parse ! cutepidav1ddec ! fakesink
```

**Two AV1 streams at once:** 50.7 + 50.6 fps at 1080p60. One AV1 stream next to a hardware HEVC stream: 59.1 + 60.0.
The memory bus is the shared limit ([`../hardware/`](../hardware/README.md#9-the-memory-bus-is-the-shared-limit)).

The same dav1d (with frame threading) also decodes AVIF image sequences in [`../animated-images/`](../animated-images/).
