# Raspberry Pi 4 video codec performance: faster H.264, HEVC, AV1, ProRes, DNxHR, CineForm, HAP, MJPEG, PNG, GIF, WebP, AVIF and JPEG XL with GStreamer

Measured, reproducible ways to decode and play video faster on the **Raspberry Pi 4** (BCM2711, Cortex-A72, VideoCore VI)
and other 64-bit ARM Linux boards. You get:

- **GStreamer plugins** (C, NEON-optimised, frame-parallel decoders) that install alongside your distribution's GStreamer.
- **Firmware clock settings** for the Pi 4's hardware codec blocks.
- **Linux kernel patches** for the `bcm2835-codec` V4L2 driver.
- **An AArch64 port of the GoPro CineForm SDK.**
- The **measurements** behind each change, and the dead ends, so you don't repeat them.

Every change is documented in its codec's folder with step-by-step instructions to apply it, check it worked and take
it out again. Nothing here depends on any particular application: if your software plays video through GStreamer
(`playbin`, `decodebin`, `gst-launch-1.0`) the plugins are picked up automatically once installed.

> Developed for [CuTePi](https://github.com/DrEVILish/CuTePi), a cue-based media player for the Raspberry Pi 4, and
> released here as generic, application-independent code. Everything was measured on a Raspberry Pi 4 Model B
> (Rev 1.5, 4 GB) running Raspberry Pi OS (Debian 13 "trixie", 64-bit), kernel 6.18, GStreamer 1.26 and FFmpeg 7.1.

---

## Highlights: what got faster

| Codec / task | Before | After | How | Folder |
|---|---|---|---|---|
| **H.264 hardware decode, 1080p** | 53-54 fps | **90 fps** (98 overclocked) | ★ firmware clock floor for the codec block | [`hardware/`](hardware/), [`h264/`](h264/) |
| **H.264 hardware encode, 1080p 10 Mbit/s** | 41.7 fps | **74.4 fps** (93 overclocked) | ★ same firmware setting | [`hardware/`](hardware/) |
| **Deinterlacing 1080i** (H.264 decoded in hardware) | 59.6 pictures/s (`deinterlace method=linear`) | **124.3 pictures/s** | NEON bob deinterlacer, one pass, zero-copy input | [`common/bob-deinterlacer-neon/`](common/bob-deinterlacer-neon/) |
| **AV1 1080p60** | 32.1 fps (`av1dec`, libaom) | **76.4 fps** | GStreamer element for dav1d | [`av1/`](av1/) |
| **Motion JPEG 1080p 4:2:0** | 115 fps (`jpegdec`) | **294 fps** | frames decoded on several cores, in order | [`mjpeg/`](mjpeg/) |
| **Motion JPEG 1080p 4:2:2** | 92 fps | **241 fps** | same | [`mjpeg/`](mjpeg/) |
| **PNG video 1080p RGBA** (MOV/MKV) | 45.8 fps (`pngdec`) | **145 fps** | frames decoded on several cores by libpng | [`png/`](png/) |
| **Animated GIF 1080p** | ~33-35 fps (`avdec_gif`) | **87-92 fps** (106 with alpha) | frame-parallel LZW decode, in-order compositor | [`animated-images/`](animated-images/) |
| **Animated WebP 1080p** | not playable (`webpdec`: stills only; libwebp's `WebPAnimDecoder` 12 fps) | **45.7 fps** (42.0 with alpha) | libwebp demux API, frames in parallel | [`animated-images/`](animated-images/) |
| **APNG 1080p** | first frame only | **58.9 fps** (50.5 with alpha) | new decoder, libpng in parallel | [`apng/`](apng/) |
| **AVIF image sequences 1080p** | not playable ("no playable streams"; libavif alone 35.4) | **60.2 fps** (39.5 with alpha) | own ISOBMFF parser and frame-threaded dav1d | [`avif/`](avif/) |
| **JPEG XL animations** | not playable | **plays, with alpha** | libjxl | [`jpeg-xl/`](jpeg-xl/) |
| **CineForm 1080p on ARM** | 19-23 fps (FFmpeg `cfhd`) | **82 / 92 / 98 fps** (2/3/4 decoders) | GoPro CineForm SDK ported to AArch64 (SSE2/MMX to NEON) | [`cineform/`](cineform/) |
| **10-bit and 4:2:2 to I420** (ProRes, DNxHR, HEVC/H.264 10-bit) | 20 ms a 1080p frame (`videoconvert`) | **2.35 ms** | NEON packer | [`common/yuv-pack-neon/`](common/yuv-pack-neon/) |
| **VP8, two 1080p60 streams at once** | 6 + 6 fps (`vp8dec`, libvpx) | **56 + 56 fps** | rank FFmpeg's decoder first (one setting) | [`vp8/`](vp8/) |
| **HAP, DNxHR, QuickTime Animation in MKV** | "no decoder for video/x-unknown" | **plays** (frames identical to MOV) | parser for Matroska `V_QUICKTIME` tracks | [`hap/`](hap/) |
| **ProRes / DNxHR / HAP / MJPEG under load** | QoS spiral: 1-3 frames shown a second | **no spiral** | QoS off for intra-only decoders | [`prores/`](prores/), [`docs/intra-only-qos.md`](docs/intra-only-qos.md) |
| **Video thumbnails (ffmpeg)** | 6.2 s CPU (AV1), 6.4 s (H.264) | **0.4 s** | decode only the nearest keyframe | [`docs/background-work.md`](docs/background-work.md) |

★ = hardware optimisation (firmware/clock/memory/GPU specifics of the Pi 4). These are collected in
[`hardware/README.md`](hardware/README.md).

All throughput figures are "flat out": the decoder runs as fast as it can into `fakesink`, outside any player, on
the test Pi with its codec, core and GPU clocks at 650 MHz and the ARM at 2.0 GHz (see [`hardware/`](hardware/)).
Correctness was checked against the reference decoders (byte-identical or ±1, as stated in each folder).

---

## Hardware optimisations for the Raspberry Pi 4 (the short version)

These matter more than any software change. Details, evidence and caveats are in [`hardware/README.md`](hardware/README.md).

1. **The H.264 and ISP blocks run at half clock unless you tell the firmware otherwise.** With the default
   `ondemand` CPU governor a hardware decode never raises the ARM clock, so the firmware leaves the H.264, ISP and
   VPU core clocks at their minimum (250/250/200 MHz). The `bcm2835-codec` driver never asks for more.
   - **Fix:** three lines in `config.txt` (`h264_freq_min=500`, `isp_freq_min=500`, `core_freq_min=500`).
   - **Result:** 1080p H.264 decode 54 → 90 fps, at stock clocks with no overvoltage. See
     [`hardware/firmware-clocks/`](hardware/firmware-clocks/).
2. **Optional overclock**, measured stable and bit-exact on the test board: codec blocks, core and V3D at 650 MHz,
   ARM at 2.0 GHz, +75 mV. Decode 90 → 98 fps, encode 74 → 93 fps.
3. **The ISP (`v4l2convert`) and the H.264 decoder share the VideoCore VPU.** Running together, ISP throughput drops
   287 → 131 fps and decode drops 92 → 70-79 fps. Avoid ISP copies next to hardware decode (see the bob deinterlacer
   and the system dma-heap upload in [`common/`](common/)).
4. **Uncached memory is slow to read on the CPU.** V4L2 MMAP buffers are write-combined: a 1080p frame takes
   6.1 ms to read, against 3.3 ms cached. Read each source row once (the bob deinterlacer does this), or use cached
   dma-heap buffers with `DMA_BUF_IOCTL_SYNC`.
5. **The V3D GPU can't composite moving 1080p60 video with GStreamer GL.** A `glvideomixer` layer of 1080p
   video presents about 16 fps with the V3D render queue 90% busy. Use KMS hardware planes (`kmssink` or your own
   DRM plane sink) for video and keep the GPU for small overlays.
6. **The GPU can sample non-contiguous memory in place.** `/dev/dma_heap/system` buffers (cached, no CMA) import
   zero-copy into the V3D through EGL, pixel-identical to an upload from system memory. Display planes still need
   contiguous CMA memory.
7. **The kernel driver has a start-order bug and no cache hints.**
   - On non-decoder roles (ISP, encoders) the first frame is lost and the stream stalls for 490-500 ms when OUTPUT
     starts before CAPTURE.
   - Its MMAP buffers can't be cached.
   - Patches are in [`h264/bcm2835-codec-patches/`](h264/bcm2835-codec-patches/). They are compile-tested only;
     see that folder.

---

## Quick start

```sh
# 1. Build tools and GStreamer headers (Raspberry Pi OS / Debian / Ubuntu)
sudo apt install build-essential pkg-config git ffmpeg \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev gstreamer1.0-plugins-good gstreamer1.0-libav

# 2. Get the code
git clone https://github.com/DrEVILish/raspberry-pi-video-codec-performance.git
cd raspberry-pi-video-codec-performance

# 3. Build every plugin and test it (about 1 minute on a Pi 4; nothing is installed)
tools/smoke-test.sh

# 4. Install the plugins you want, for your user (no sudo) ...
make -C mjpeg/parallel-mjpeg-decoder install
#    ... or for every user and every service on the machine
sudo make -C mjpeg/parallel-mjpeg-decoder install-system

# 5. Check GStreamer sees it
gst-inspect-1.0 cutepimjpegdec
```

The decoders register at rank `primary + 1` (257), one above GStreamer's own, so `playbin`/`decodebin` pick them
without changes to your pipelines. To use one in a hand-written pipeline, name it (`... ! cutepimjpegdec ! ...`).
To try a plugin without installing it: `GST_PLUGIN_PATH=$PWD/mjpeg/parallel-mjpeg-decoder gst-launch-1.0 ...`.

The hardware settings are separate and need a reboot: see [`hardware/firmware-clocks/`](hardware/firmware-clocks/).

---

## What is in each folder

One folder per codec. Plugins that serve several codecs live in [`common/`](common/) and are linked from each
codec they help.

| Folder | Codec | What you'll find | Type |
|---|---|---|---|
| [`hardware/`](hardware/) | all (Pi 4) | ★ firmware clock settings, overclock, VideoCore/ISP/V3D/memory findings | config + notes |
| [`h264/`](h264/) | H.264 / AVC | ★ `bcm2835-codec` kernel patches, clock fix, 1080i deinterlacing, measurement pitfalls | kernel patches + notes |
| [`hevc/`](hevc/) | H.265 / HEVC | ★ hardware decoder clocks, measured rates | notes |
| [`mpeg2/`](mpeg2/) | MPEG-2 | 1080i deinterlacing, decoder choice | notes |
| [`mpeg4-asp/`](mpeg4-asp/) | MPEG-4 Part 2 (DivX, Xvid) | measured: no change needed | notes |
| [`mjpeg/`](mjpeg/) | Motion JPEG | `cutepimjpegdec`: frame-parallel decoder, 2.5× | plugin |
| [`png/`](png/) | PNG video | `cutepipngvdec`: frame-parallel decoder, 3.2× | plugin |
| [`animated-images/`](animated-images/) | GIF, APNG, WebP, AVIF, JPEG XL | `cutepianimdec`: frame-parallel animated image decoder with alpha | plugin |
| [`gif/`](gif/), [`apng/`](apng/), [`webp/`](webp/), [`avif/`](avif/), [`jpeg-xl/`](jpeg-xl/) | each format | format notes, pointing to `animated-images/` | notes |
| [`av1/`](av1/) | AV1 | `cutepidav1ddec`: dav1d decoder element, 2.4× libaom | plugin |
| [`vp8/`](vp8/) | VP8 | decoder rank change, 9× for two streams | config |
| [`vp9/`](vp9/) | VP9 (+ alpha) | measured decoder choice, alpha limits | notes |
| [`theora/`](theora/) | Theora | why libtheora is the limit, options | notes |
| [`prores/`](prores/) | Apple ProRes | intra-only QoS fix, 10-bit packing | config + notes |
| [`dnxhr/`](dnxhr/) | Avid DNxHR / DNxHD | MKV support, intra-only QoS fix | plugin (shared) + notes |
| [`hap/`](hap/) | HAP / HAP Alpha | `cutepiqtmkvparse`: HAP, DNxHR, QuickTime Animation in Matroska | plugin |
| [`qtrle/`](qtrle/) | QuickTime Animation | MKV bug (FFmpeg), copies to avoid | notes + upstream report |
| [`cineform/`](cineform/) | GoPro CineForm | ★ CineForm SDK on AArch64 (NEON) + `cutepicfhddec` element | port + plugin |
| [`ffv1/`](ffv1/) | FFV1 | measured limits (alpha) | notes |
| [`wmv/`](wmv/) | WMV / WMA / ASF | how to play `.wmv`/`.asf` without changing your other decoders | config |
| [`ac3/`](ac3/) | AC-3 / E-AC-3 audio | decoder-rank pitfall | config |
| [`common/`](common/) | interlaced video, 10-bit, GPU upload | `cutepibob` (NEON deinterlacer), `cutepiyuvpack` (NEON 10-bit/4:2:2 packer), `cutepisysdmabuf` (zero-copy GPU upload) | plugins |
| [`docs/`](docs/) | all | intra-only QoS, background work next to playback, pitfalls, upstream bug reports | notes |
| [`tools/`](tools/) | - | shared Makefile rules, `smoke-test.sh` | build/test |

---

## How the plugins fit into GStreamer

- Each plugin is one C file plus a three-line `Makefile`. `make` builds a `.so` in that folder, `make check` loads
  it with `gst-inspect-1.0`, `make install` / `sudo make install-system` copy it into a GStreamer plugin directory.
  The shared rules are in [`tools/plugin.mk`](tools/plugin.mk).
- **No `-dev` packages for the codec libraries.** The decoders load libdav1d, libpng, libwebp, libavif and libjxl at
  run time (`dlopen`), against headers vendored in their folder, so only the runtime libraries need to be installed
  (Debian trixie / Raspberry Pi OS sonames: `libdav1d.so.7`, `libpng16.so.16`, `libwebp.so.7`, `libwebpdemux.so.2`,
  `libavif.so.16`, `libjxl.so.0.11`). A missing library disables only the format that needs it.
- **Ranks.** Decoders register at `primary + 1`, so autoplugging prefers them. To turn one off without
  uninstalling it, rank it to zero: `GST_PLUGIN_FEATURE_RANK=cutepimjpegdec:0`.
- **Element names start with `cutepi`**: they were developed in CuTePi and keep their names, so pipelines and
  results from either project match.
- **Architecture.** NEON paths are used on AArch64 (and ARMv7 with NEON where noted). Each element also has a
  portable C path and builds and runs on x86-64.

---

## Test system and method

| | |
|---|---|
| Board | Raspberry Pi 4 Model B Rev 1.5, 4 GB, active cooling |
| OS | Raspberry Pi OS (Debian 13 trixie) 64-bit, kernel `6.18.50+rpt-rpi-v8` |
| Clocks | codec (H.264, ISP, HEVC), core and V3D 650 MHz; ARM 2.0 GHz; +75 mV (see `hardware/`) unless stated |
| Software | GStreamer 1.26.2, FFmpeg 7.1.5, dav1d 1.5.1, libpng 1.6.48, libwebp 1.5.0, libavif 1.2.1, libjxl 0.11.2 |
| Throughput | `gst-launch-1.0 ... ! fakesink`, frames counted, wall time; flat out (no clock sync) |
| Correctness | against the reference decoder in GStreamer or FFmpeg: byte-identical unless a tolerance is given |

**What this repository does not claim.** Earlier "1080p60 on screen" verdicts from CuTePi's GPU compositor were
withdrawn: that compositor wasn't drawing most layers when they were taken. Only flat-out decoder measurements and
correctness checks are quoted here. Things that were *not* tested are listed in each folder. In particular, the
kernel patches have never been loaded on hardware.

---

## FAQ

**Why is hardware H.264 decoding slow on my Raspberry Pi 4?**
The firmware keeps the H.264 block at 250 MHz unless the ARM runs at full speed, and the `bcm2835-codec` driver never
asks for more. Add `h264_freq_min=500`, `isp_freq_min=500` and `core_freq_min=500` to `config.txt`
([`hardware/firmware-clocks/`](hardware/firmware-clocks/)). 1080p decode goes from 54 to 90 fps.

**How do I deinterlace 1080i on a Raspberry Pi 4 in real time?**
GStreamer's `deinterlace` reads the decoder's uncached frames twice and manages about 60 pictures a second. The
NEON bob deinterlacer in [`common/bob-deinterlacer-neon/`](common/bob-deinterlacer-neon/) does 124 a second, with
the same output (±1) as `deinterlace method=linear fields=all`.

**How do I play AV1 smoothly on a Raspberry Pi 4?**
Use dav1d, not libaom: [`av1/`](av1/) has a GStreamer element that loads the system's libdav1d. It runs 2.4× faster
than `av1dec`.

**Why do animated WebP/APNG/AVIF files play slowly or show only the first frame in GStreamer?**
GStreamer 1.26's `webpdec` decodes WebP stills only, and it has no APNG demuxer and no AVIF-sequence or JPEG XL
support. FFmpeg 7.1 can't decode animated WebP either. [`animated-images/`](animated-images/) adds one decoder for all five, with alpha, decoding frames in
parallel.

**Can ProRes, DNxHR, CineForm or HAP play on a Raspberry Pi 4?**
They decode in software. ProRes LT and Proxy and DNxHR LB and HQ played at 1080p60 on KMS display planes in our
tests. CineForm decodes at 82-98 fps with the ported SDK ([`cineform/`](cineform/)). HAP in MKV needs the parser in [`hap/`](hap/). Turn QoS off for these intra-only
decoders ([`docs/intra-only-qos.md`](docs/intra-only-qos.md)), or a brief stall turns into seconds of dropped frames.

**Is overclocking needed?**
No. The big gain (54 → 90 fps for H.264) comes from holding the codec clocks at their *stock* maximum. The overclock
adds about 9% more.

**Does any of this work on the Raspberry Pi 5?**
The GStreamer plugins do: they are portable C with NEON paths. The Pi 5 has no H.264 decoder block and different
clock names, so the firmware settings and kernel patches are Pi 4 only.

---

## Contributing, upstream and licence

- Bugs found upstream along the way are written up, ready to file, in [`docs/upstream/`](docs/upstream/): a FFmpeg
  Matroska muxer bug, a GStreamer `matroskademux` gap, and more.
- Measurements from other boards and clocks are welcome as issues or pull requests.
  State the board, the clocks (`vcgencmd measure_clock`), the OS and the command.
- **Licence:**
  - GStreamer plugins and docs: [LGPL-2.1-or-later](LICENSE), the same as GStreamer.
  - Kernel patches: GPL-2.0 ([`h264/bcm2835-codec-patches/LICENSE`](h264/bcm2835-codec-patches/LICENSE)), as the
    kernel requires.
  - Vendored third-party headers keep their own licences, next to them: dav1d (BSD-2), libwebp (BSD-3), libavif
    (BSD-2), libjxl (BSD-3), sse2neon (MIT), and the CineForm SDK (Apache-2.0 / MIT).

<sub>Keywords: Raspberry Pi 4 video decoding performance, Raspberry Pi hardware video acceleration, BCM2711, VideoCore
VI, V3D, V4L2 M2M, bcm2835-codec, v4l2h264dec, h264_v4l2m2m, rpi-hevc-dec, config.txt h264_freq, isp_freq,
core_freq, GStreamer plugin, gst-launch-1.0, playbin, decodebin, NEON SIMD, AArch64, ARM64, Cortex-A72, deinterlace
1080i, bob deinterlacer, Motion JPEG decoder, MJPEG multithreaded, PNG video, animated GIF, APNG, animated WebP, AVIF
sequence, JPEG XL animation, AV1 dav1d GStreamer, VP8 VP9 libvpx, Theora, ProRes, DNxHR, DNxHD, CineForm SDK ARM
port, sse2neon, HAP codec Matroska, QuickTime Animation qtrle, FFV1, WMV ASF, dma-buf, dma-heap, CMA, zero-copy
EGLImage, kmssink, glvideomixer, QoS, frame dropping, video playback stutter Raspberry Pi.</sub>
