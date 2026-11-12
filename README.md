# Raspberry Pi 4 video codec performance: faster H.264, HEVC, AV1, ProRes, DNxHR, CineForm, HAP, MJPEG, PNG, GIF, WebP, AVIF and JPEG XL with GStreamer

**The Raspberry Pi 4 can play far more video smoothly than it does out of the box.** Out of the box many formats play
slowly, play only their first frame, or don't open at all. This repository fixes that with:

- **Add-on decoders for GStreamer** (the media framework most Linux players use). Install them and they are used
  automatically. Your player needs no changes.
- **A few settings** for the Pi's firmware and for GStreamer.
- **The measurements behind every change**, including what we tried that didn't help.

Everything here was built for [CuTePi](https://github.com/DrEVILish/CuTePi), a cue-based media player for the Pi 4,
and is released as stand-alone code that any GStreamer player can use.

**Contents:** [Results at a glance](#results-at-a-glance) · [Picture quality](#picture-quality-did-anything-get-worse) ·
[Install](#install) · [Pi 4 hardware settings](#pi-4-hardware-settings) · [What's next](#whats-next-formats-not-covered-yet) ·
[Detailed results](#detailed-results-and-history) · [Folders](#what-is-in-each-folder) · [FAQ](#faq)

---

## Results at a glance

**How to read these tables.** The numbers are frames per second (fps): how many pictures the Pi can decode in one
second when it goes as fast as it can. A video plays smoothly when this number is comfortably above the video's own
frame rate (usually 25, 30 or 60). "Stock" is a standard Raspberry Pi OS install with GStreamer's own decoders.
"With this repo" adds the plugins and settings from this repository.

All rows were measured on 12 November 2026 on the same Pi 4, one clip at a time, the median of three runs. While
they ran, the Pi was also showing a picture on its display, which takes some memory speed, as it would in real use.
The test system is described [further down](#test-system-and-method).

### Formats this repository speeds up or makes playable

| Format | Test clip | Stock | With this repo | Change |
|---|---|---|---|---|
| **Motion JPEG** | 1080p30, AVI | 57.5 fps (hardware JPEG decoder) | **334 fps** | **5.8× faster** |
| **PNG video** (with transparency) | 1080p, MOV | 49.2 fps | **178 fps** | **3.6× faster** |
| **Animated GIF** | 1080p | 43.6 fps | **168 fps** | **3.9× faster** |
| Animated GIF with transparency | 1080p | 62.3 fps | **134 fps** | 2.1× faster |
| **AV1** | 1080p30, MKV | 33.3 fps | **108 fps** | **3.2× faster** |
| **VP8** | 720p30, WebM | 240 fps | **466 fps** | 1.9× faster (one setting) |
| **GoPro CineForm** 4:2:2 | 1080p25, MOV | 21.1 fps | **81.4 fps** | **3.9× faster** |
| GoPro CineForm 4:2:2 | 4K (2160p25) | 1.4 fps | **23.0 fps** | 16× faster |
| GoPro CineForm RGB / RGBA 12-bit | 1080p25 | 11.5 / 8.5 fps | **48.3 / 47.9 fps** | 4-5.6× faster |
| **APNG** (animated PNG) | 1080p | first frame only | **149 fps** (93 with transparency) | now plays |
| **Animated WebP** | 1080p | doesn't play | **55.7 fps** (69.7 with transparency) | now plays |
| **AVIF animation** | 1080p | doesn't play | **65.4 fps** (52.5 with transparency) | now plays |
| **JPEG XL** animation | - | doesn't play | plays, with transparency | now plays |
| **HAP / HAP Alpha in MKV** | 1080p | doesn't play | **95.6 / 84.3 fps** | now plays |
| **DNxHR in MKV** | 1080p HQ | doesn't play | **145 fps** | now plays |
| **1080i deinterlacing** (after hardware H.264 decode) | 1080i25 | 59.6 pictures/s | **124 pictures/s** | 2.1× faster |
| **H.264 hardware decode** | 1080p | 53-54 fps | **90 fps** (98 with the overclock) | 1.7× faster (firmware setting) |

### Formats that already play well (measured, nothing to change)

| Format | Test clip | Speed | Decoder |
|---|---|---|---|
| H.264 (hardware) | 1080p30 / 1080p60 / 720p50 | 81 / 80 / 200 fps* | Pi hardware decoder |
| H.264 High 10 (10-bit) | 1080p30 | 73-78 fps | FFmpeg (software) |
| HEVC / H.265 (hardware) | 1080p30 / 1080p60 / 4K30 | 140 / 143 / 110 fps | Pi hardware decoder |
| HEVC Main 10 (hardware) | 1080p30 | 102-106 fps | Pi hardware decoder |
| VP9 | 1080p30 | 113-117 fps (65 with transparency) | libvpx |
| MPEG-2 | 1080i25 | 178 fps | FFmpeg |
| MPEG-4 Part 2 (DivX, Xvid) | 720p30 / 1080p | 401-414 / 200 fps | FFmpeg |
| Theora | 720p30 | 159 fps | libtheora |
| WMV 2 | 720p30 | 161-165 fps | FFmpeg ([needs one package](codecs/wmv/)) |
| Apple ProRes 422 / HQ / LT / Proxy | 1080p25 | 74-88 fps | FFmpeg |
| Apple ProRes 422 HQ | 4K (2160p25) | 21.5 fps | FFmpeg (too slow for 4K) |
| Apple ProRes 4444 / 4444 XQ | 1080p25 | 47 / 51-54 fps | FFmpeg |
| Avid DNxHR HQ (MOV) | 1080p25 | 142-146 fps | FFmpeg |
| HAP (MOV) | 1080p25 | 96 fps | FFmpeg |
| QuickTime Animation | 720p25 | 312 fps | FFmpeg |
| FFV1 | 1080p25 | 114-118 fps | FFmpeg |

\* These H.264 figures include one extra copy of every frame that the measuring setup adds. Without it the Pi
decodes 1080p at 90-95 fps (see [`codecs/h264/`](codecs/h264/)). Where a range is given, the two runs (stock and
with this repo) used the same decoder, so the range is run-to-run variation, not a gain or loss.

The figures for 1080i deinterlacing and H.264 hardware decode come from earlier measurements on this Pi, because
they need a reboot with different firmware settings to repeat. Details are in [`common/`](common/) and
[`hardware/`](hardware/).

### Why the gains are so large

- **Several frames at once.** Formats where every frame stands on its own (Motion JPEG, PNG, CineForm, animated
  images) were decoded one frame at a time, on one of the Pi's four cores. The plugins here decode several frames in
  parallel and hand them on in the right order.
- **A faster decoder that was already there.** For AV1 the plugin uses dav1d, which is much faster on ARM than
  GStreamer's default (libaom). For VP8, one setting picks FFmpeg's decoder over libvpx.
- **Code ported to the Pi.** GoPro's CineForm SDK only built for Intel and AMD processors. It is ported here to the
  Pi's ARM processor.
- **Formats GStreamer can't open.** GStreamer has no decoder for animated WebP, APNG, AVIF or JPEG XL animations, and
  can't read HAP or DNxHR from MKV files. The plugins add them.
- **The Pi's video hardware runs at half speed by default.** One firmware setting keeps it at full speed while it
  decodes.

---

## Picture quality: did anything get worse?

**Nothing we checked looks worse, and the one real loss we found (transparency in some CineForm files) is
fixed.** We compared every frame of every test clip decoded with this repository against the same clip decoded by
the standard decoder. Where the results are "identical", every pixel of every frame matches exactly. A few checks
are still pending and are marked as such.

| What we compared | Result |
|---|---|
| **Motion JPEG**: our decoder against GStreamer's `jpegdec` (3 clips, 750 frames) | **identical** |
| **PNG video**: our decoder against GStreamer's `pngdec` (480 frames) | **identical** |
| **AV1**: our decoder (dav1d) against GStreamer's `av1dec` (libaom), 2 clips | **identical** |
| **Animated GIF**: against FFmpeg's GIF decoder (3 files, 630 frames) | **identical** |
| **APNG**, with and without transparency: against FFmpeg | **identical** |
| **Animated WebP**, with and without transparency: against libwebp's own decoder | **identical** |
| **JPEG XL**: against FFmpeg's libjxl decoder | **identical** |
| **VP8**: FFmpeg's decoder (our setting) against libvpx | **identical** |
| **HAP, HAP Alpha, DNxHR, ProRes, CineForm in MKV**: against the same video in MOV | **identical** |
| **HEVC**: the Pi's hardware decoder against FFmpeg's software decoder | **identical** |
| **CineForm 4:2:2**: the ported GoPro SDK against FFmpeg's CineForm decoder | 58 dB: two different decoders, the difference can't be seen |
| **CineForm RGBA made by FFmpeg**: our decoder against FFmpeg's | **was losing the transparency** (the SDK misread the format, so the output had no alpha). **Fixed**: alpha now within 56.6 dB, colour 57-61 dB. Tested on x86-64; the Pi re-check is pending |
| **CineForm RGB 12-bit**: our YUY2 output (made for the Pi's display planes) against FFmpeg's | 42 dB. Converting RGB to 4:2:2 YUV costs some colour detail; being checked |
| **1080i bob deinterlacer** against GStreamer's `deinterlace method=linear` | **identical** when its `tff` property matches the clip's field order (it doesn't read it from the stream). Tested on x86-64; the Pi re-check is pending |
| **10-bit to 8-bit packer** against GStreamer's `videoconvert` | 46-53 dB: both drop to 8-bit 4:2:0 and round slightly differently; the difference can't be seen |
| **H.264**: the Pi's hardware decoder at the raised clocks against FFmpeg's software decoder | 79-80 dB: almost identical but not bit-exact; being investigated |
| Animated GIF with transparency, AVIF animations | comparison pending (the first test was set up wrongly) |

**What "PSNR" means.** Where two pictures are not exactly identical, we give the PSNR: a standard measure of how
close two pictures are, in decibels (dB). Higher is closer. Above about 45 dB the difference can't be seen;
50-60 dB means a handful of pixels a tiny step apart.

---

## Install

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
make -C codecs/mjpeg/parallel-mjpeg-decoder install
#    ... or for every user and every service on the machine
sudo make -C codecs/mjpeg/parallel-mjpeg-decoder install-system

# 5. Check GStreamer sees it
gst-inspect-1.0 cutepimjpegdec
```

- **Automatic use.** The decoders register one rank above GStreamer's own (`primary + 1`, 257), so `playbin` and
  `decodebin` pick them with no change to your pipelines. In a hand-written pipeline, name the element
  (`... ! cutepimjpegdec ! ...`).
- **Try without installing:** `GST_PLUGIN_PATH=$PWD/codecs/mjpeg/parallel-mjpeg-decoder gst-launch-1.0 ...`.
- **Turn one off** without uninstalling it: `GST_PLUGIN_FEATURE_RANK=cutepimjpegdec:0`.
- **CineForm** also needs the ported SDK built first: see [`codecs/cineform/`](codecs/cineform/).
- **Motion JPEG and the Pi's hardware JPEG decoder.** Stock GStreamer on the Pi picks `v4l2jpegdec`, the
  hardware JPEG decoder, at the same rank as ours. It decoded 57 fps and never signalled the end of the clip in our
  tests. With `cutepimjpegdec` installed, ours was picked. To be sure, rank the hardware one down:
  `GST_PLUGIN_FEATURE_RANK=v4l2jpegdec:0`.
- **The hardware settings** are separate and need a reboot: see [`hardware/firmware-clocks/`](hardware/firmware-clocks/).

---

## Pi 4 hardware settings

These matter more than any software change. Details, evidence and caveats are in [`hardware/README.md`](hardware/README.md).

1. **The video hardware runs at half speed unless you tell the firmware otherwise.** When the processor is idle,
   the firmware also slows the H.264 decoder, the image processor and the core of the graphics chip. Hardware
   decoding doesn't wake the processor, so the decoder stays slow.
   - **Fix:** three lines in `config.txt`: `h264_freq_min=500`, `isp_freq_min=500`, `core_freq_min=500`.
   - **Result:** 1080p H.264 decode from 54 to 90 fps, at stock clocks and stock voltage.
     See [`hardware/firmware-clocks/`](hardware/firmware-clocks/).
2. **Optional overclock**, measured stable and bit-exact on the test Pi: video blocks, core and 3D at 650 MHz,
   processor at 2.0 GHz, +75 mV. Decode 90 → 98 fps, encode 74 → 93 fps.
3. **Two hardware blocks share one processor inside the graphics chip.** The image processor (used by
   `v4l2convert` to convert and scale) slows the H.264 decoder when both run: decode drops from 92 to 70-79 fps.
   Avoid image-processor copies next to hardware decode.
4. **Memory the hardware decoder writes is slow for the main processor to read** (it is uncached): 6.1 ms to read a
   1080p frame, against 3.3 ms normally. Read each frame once.
5. **The 3D chip can't mix moving 1080p60 video with GStreamer's OpenGL elements** (about 16 fps). Use the display's
   hardware layers (`kmssink`, or your own DRM plane sink) for video and keep the 3D chip for small overlays.
6. **The 3D chip can read ordinary system memory directly** (`/dev/dma_heap/system`), with no copy. The display's
   hardware layers still need contiguous memory.
7. **The kernel driver for the video blocks has a start-order bug and no cache hints.** Patches are in
   [`codecs/h264/bcm2835-codec-patches/`](codecs/h264/bcm2835-codec-patches/). They are compile-tested only.

---

## What's next: formats not covered yet

We looked at which widely used formats this repository doesn't cover yet and where work on the Pi 4 would pay off.
None of these have been measured on the Pi yet. The speeds marked "est." are estimates.

| Format | Where you meet it | Opportunity |
|---|---|---|
| **NotchLC** | media servers (Disguise, Notch) | **high:** every frame stands alone, so decoding several at once should help as it did for Motion JPEG; FFmpeg's decoder is likely slow (est.) |
| **Resolume DXV3** | VJ software (Resolume) | **medium-high:** same pattern as HAP; first check that GStreamer recognises the files |
| **JPEG 2000 / DCP** | cinema packages at events | **medium:** a 2K cinema package probably decodes in single-digit fps today (est.); parallel frames plus a faster colour conversion could make it playable |
| **HEIC / HEIF stills** | iPhone photos | **medium:** no GStreamer decoder; the picture tiles are HEVC, which the Pi has hardware for |
| **Image sequences** (DPX, EXR, TIFF) | post-production | **medium-low:** limited by storage and memory speed; read ahead and decode in parallel |
| **VVC / H.266** | rare so far | **medium-low:** two software decoders exist; measure them and pick the faster |
| **Uncompressed** (v210, UYVY) | capture and broadcast | **low-medium:** needs a fast SSD; the unpacking could reuse [`common/yuv-pack-neon/`](common/yuv-pack-neon/) |
| VC-1, MPEG-1, H.263, Cinepak and other legacy formats | old files | **low:** they need little processing; check they play and note it |
| Audio: AAC, MP3, Opus, FLAC, PCM, E-AC-3, DTS | everywhere | **low for speed** (a few percent of one core each); start-up delay and channel mixing are worth a look |

Not worth starting yet: **AV2** (finalised in 2026, but no fast decoder or real content yet), **EVC** (almost
unused), **JPEG XS** (a live broadcast format, not a file format).

---

## Detailed results and history

<details>
<summary><b>First round: the original gains</b> (October/November 2026)</summary>

Measured with the Pi's codec, core and 3D clocks at 650 MHz and the processor at 2.0 GHz, flat out into `fakesink`,
with other clips than the tables above. Correctness was checked against the reference decoders (byte-identical or
±1, as stated in each folder).

| Codec / task | Before | After | How | Folder |
|---|---|---|---|---|
| **H.264 hardware decode, 1080p** | 53-54 fps | **90 fps** (98 overclocked) | ★ firmware clock floor for the codec block | [`hardware/`](hardware/), [`codecs/h264/`](codecs/h264/) |
| **H.264 hardware encode, 1080p 10 Mbit/s** | 41.7 fps | **74.4 fps** (93 overclocked) | ★ same firmware setting | [`hardware/`](hardware/) |
| **Deinterlacing 1080i** (H.264 decoded in hardware) | 59.6 pictures/s (`deinterlace method=linear`) | **124.3 pictures/s** | NEON bob deinterlacer, one pass, zero-copy input | [`common/bob-deinterlacer-neon/`](common/bob-deinterlacer-neon/) |
| **AV1 1080p60** | 32.1 fps (`av1dec`, libaom) | **76.4 fps** | GStreamer element for dav1d | [`codecs/av1/`](codecs/av1/) |
| **Motion JPEG 1080p 4:2:0** | 115 fps (`jpegdec`) | **294 fps** | frames decoded on several cores, in order | [`codecs/mjpeg/`](codecs/mjpeg/) |
| **Motion JPEG 1080p 4:2:2** | 92 fps | **241 fps** | same | [`codecs/mjpeg/`](codecs/mjpeg/) |
| **PNG video 1080p RGBA** (MOV/MKV) | 45.8 fps (`pngdec`) | **145 fps** | frames decoded on several cores by libpng | [`codecs/png/`](codecs/png/) |
| **Animated GIF 1080p** | ~33-35 fps (`avdec_gif`) | **87-92 fps** (106 with alpha) | frame-parallel LZW decode, in-order compositor | [`codecs/animated-images/`](codecs/animated-images/) |
| **Animated WebP 1080p** | not playable (`webpdec`: stills only; libwebp's `WebPAnimDecoder` 12 fps) | **45.7 fps** (42.0 with alpha) | libwebp demux API, frames in parallel | [`codecs/animated-images/`](codecs/animated-images/) |
| **APNG 1080p** | first frame only | **58.9 fps** (50.5 with alpha) | new decoder, libpng in parallel | [`codecs/apng/`](codecs/apng/) |
| **AVIF image sequences 1080p** | not playable ("no playable streams"; libavif alone 35.4) | **60.2 fps** (39.5 with alpha) | own ISOBMFF parser and frame-threaded dav1d | [`codecs/avif/`](codecs/avif/) |
| **JPEG XL animations** | not playable | **plays, with alpha** | libjxl | [`codecs/jpeg-xl/`](codecs/jpeg-xl/) |
| **CineForm 1080p on ARM** | 19-23 fps (FFmpeg `cfhd`) | **82 / 92 / 98 fps** (2/3/4 decoders) | GoPro CineForm SDK ported to AArch64 (SSE2/MMX to NEON) | [`codecs/cineform/`](codecs/cineform/) |
| **10-bit and 4:2:2 to I420** (ProRes, DNxHR, HEVC/H.264 10-bit) | 20 ms a 1080p frame (`videoconvert`) | **2.35 ms** | NEON packer | [`common/yuv-pack-neon/`](common/yuv-pack-neon/) |
| **VP8, two 1080p60 streams at once** | 6 + 6 fps (`vp8dec`, libvpx) | **56 + 56 fps** | rank FFmpeg's decoder first (one setting) | [`codecs/vp8/`](codecs/vp8/) |
| **HAP, DNxHR, QuickTime Animation in MKV** | "no decoder for video/x-unknown" | **plays** (frames identical to MOV) | parser for Matroska `V_QUICKTIME` tracks | [`codecs/hap/`](codecs/hap/) |
| **ProRes / DNxHR / HAP / MJPEG under load** | QoS spiral: 1-3 frames shown a second | **no spiral** | QoS off for intra-only decoders | [`codecs/prores/`](codecs/prores/), [`docs/intra-only-qos.md`](docs/intra-only-qos.md) |
| **Video thumbnails (ffmpeg)** | 6.2 s CPU (AV1), 6.4 s (H.264) | **0.4 s** | decode only the nearest keyframe | [`docs/background-work.md`](docs/background-work.md) |

★ = hardware optimisation (firmware/clock/memory/GPU specifics of the Pi 4), collected in
[`hardware/README.md`](hardware/README.md).

</details>

<details>
<summary><b>November 2026: animated images faster again</b> (Pi 4 and x86-64)</summary>

Synthetic FFmpeg `testsrc2` clips, 1080p30, 150 frames (the alpha WebP: 90), best of three runs, flat out into
`fakesink`. "Before" is `cutepianimdec` as first published. Both give byte-identical frames, seeks included.

| Clip, 1080p, Raspberry Pi 4 | Before | After | Change |
|---|---|---|---|
| Animated GIF | 60.5 fps | **99 fps** | +64% |
| APNG | 75 fps | **125 fps** | +67% |
| Animated WebP, opaque | 58 fps | **94 fps** | +62% |
| Animated WebP, with alpha | 38 fps | **44 fps** | +16% |

- **GIF:** the LZW decoder reads codes 8 bytes at a time and writes each string straight to its place (since
  improved again, see the second round).
- **WebP, APNG:** next to four decode workers the memory bus is the limit (one copy of a 1080p RGBA frame costs
  5-14 ms). A frame that replaces its region is decoded straight into the output buffer, and only what lies outside
  it is copied from the previous frame.
- **Seek fix:** a flushing seek while frames were being decoded could hang the element. It no longer does.

The same change on x86-64 (a Debian 13 container with 2 cores of an Intel Core i5-12600K, 4 GB):

| Clip, 1080p, x86-64 | Before | After | Peak memory before → after |
|---|---|---|---|
| Animated GIF | 160 fps | **309 fps** | 72 → 73 MB |
| APNG, opaque | 207 fps | **237 fps** | 146 → 176 MB |
| APNG, with alpha | 188 fps | **202 fps** | - |
| Animated WebP, opaque | 185 fps | **245 fps** | 126 → 124 MB |
| Animated WebP, with alpha | 190 fps | **200 fps** | 121 → 137 MB |
| Animated WebP, lossless | 186 fps | **217 fps** | - |

Peak memory grows by at most 30 MB: frames decoded ahead can each hold an output buffer (up to 8, 8 MB each at
1080p).

</details>

<details>
<summary><b>November 2026, second round: every decoder CuTePi uses</b></summary>

Steady-state rate from the first frame to the last into `fakesink`, median of five runs, the previous version
against this one in alternation, with CuTePi showing a picture on the display. Same frames, byte for byte, on every
clip, and CuTePi's own tests for these elements pass.

| Decoder, clip | Before | After | Change | What changed |
|---|---|---|---|---|
| `cutepianimdec` GIF, 1080p30 `testsrc2` | 111 fps | **166 fps** | +50% | an LZW code is a copy from earlier in the output (every dictionary string is already there), not a walk down its prefix chain a byte at a time |
| `cutepianimdec` GIF, CuTePi test image | 852 fps | **1250 fps** | +47% | same |
| `cutepidav1ddec` AV1, 1080p60 (SVT-AV1 preset 11, CRF 35) | 115 fps | **133 fps** | +16% | `max-frame-delay` 4 by default, not dav1d's 2 on four cores: the cores stay busy |
| `cutepidav1ddec` AV1, CuTePi test clip 1080p30 | 110 fps | **128 fps** | +16% | same |
| `cutepimjpegdec` MJPEG, CuTePi test clip (AVI 1080p30) | 322 fps | **360 fps** | +12% | any free worker takes the next frame, two frames a worker in flight |
| `cutepimjpegdec` MJPEG 4:2:2 `-q:v 2`, 1080p60 | 281 fps | **307 fps** | +9% | same |
| `cutepimjpegdec` MJPEG 4:2:0 `-q:v 3`, 1080p60 | 341 fps | **366 fps** | +7% | same; three workers at 8.1 ms a frame top out at 370 |
| `cutepianimdec` AVIF sequence, 1080p30 | 74 fps | **81 fps** | +9% | dav1d `max_frame_delay` 4, as for AV1 |
| `cutepipngvdec` PNG RGBA in MOV, 1080p60 | 161 fps | **171 fps** | +6% (+11% in a second A/B run) | as MJPEG; now at libpng's own four-thread ceiling (174 fps) |
| `cutepianimdec` APNG / WebP / WebP with alpha | 157 / 107 / 48 fps | 157 / 106 / 49 fps | 0 | none found: the cores are busy in libpng/zlib and libwebp |

</details>

<details>
<summary><b>Tried, and not worth it</b></summary>

| Idea | Result |
|---|---|
| Skip PNG checksums (`PNG_IGNORE_ADLER32`, `png_set_crc_action`) | about 1% on one thread, within run-to-run noise on four: Debian's libpng is not built with `inflateValidate`, so the Adler-32 option has no effect |
| libdeflate instead of zlib for PNG | 72 vs 46 fps on one thread, but 84 vs 174 fps on four: inflating a whole frame at once saturates the memory bus |
| `-O3 -mcpu=cortex-a72` instead of `-O2` | no measurable change |
| NEON fast path in the WebP/APNG compositor | no gain for APNG, WebP 58 → 50 fps: the cost was memory passes, not arithmetic |
| Four MJPEG workers by default | 7-11% faster, but the default of 3 keeps a core free for the rest of the system |
| CineForm: frames to any free decoder, two in flight each (as MJPEG) | no change, 82 fps: one decoder 44 fps, two 72, three 83, four 78-85: the memory bus is the limit |
| WebP with alpha: the blend without its divisions where the picture under is clear or opaque | no change: libwebp's decode (57 ms of CPU a frame) and the blend (19 ms) keep all four cores busy |
| Faster bob deinterlacer or 10-bit packer | nothing to gain end to end: bob outputs 145-152 pictures/s, twice the hardware decoder's 77 fps, and ProRes plays at 72 fps with or without the packer |

</details>

---

## What is in each folder

One folder per codec, all under [`codecs/`](codecs/). Plugins that serve several codecs live in [`common/`](common/)
and are linked from each codec they help.

| Folder | Codec | What you'll find | Type |
|---|---|---|---|
| [`hardware/`](hardware/) | all (Pi 4) | ★ firmware clock settings, overclock, VideoCore/ISP/V3D/memory findings | config + notes |
| [`codecs/h264/`](codecs/h264/) | H.264 / AVC | ★ `bcm2835-codec` kernel patches, clock fix, 1080i deinterlacing, measurement pitfalls | kernel patches + notes |
| [`codecs/hevc/`](codecs/hevc/) | H.265 / HEVC | ★ hardware decoder clocks, measured rates | notes |
| [`codecs/mpeg2/`](codecs/mpeg2/) | MPEG-2 | 1080i deinterlacing, decoder choice | notes |
| [`codecs/mpeg4-asp/`](codecs/mpeg4-asp/) | MPEG-4 Part 2 (DivX, Xvid) | measured: no change needed | notes |
| [`codecs/mjpeg/`](codecs/mjpeg/) | Motion JPEG | `cutepimjpegdec`: frame-parallel decoder | plugin |
| [`codecs/png/`](codecs/png/) | PNG video | `cutepipngvdec`: frame-parallel decoder | plugin |
| [`codecs/animated-images/`](codecs/animated-images/) | GIF, APNG, WebP, AVIF, JPEG XL | `cutepianimdec`: frame-parallel animated image decoder with alpha | plugin |
| [`codecs/gif/`](codecs/gif/), [`codecs/apng/`](codecs/apng/), [`codecs/webp/`](codecs/webp/), [`codecs/avif/`](codecs/avif/), [`codecs/jpeg-xl/`](codecs/jpeg-xl/) | each format | format notes, pointing to `animated-images/` | notes |
| [`codecs/av1/`](codecs/av1/) | AV1 | `cutepidav1ddec`: dav1d decoder element | plugin |
| [`codecs/vp8/`](codecs/vp8/) | VP8 | decoder rank change, 9× for two streams | config |
| [`codecs/vp9/`](codecs/vp9/) | VP9 (+ alpha) | measured decoder choice, alpha limits | notes |
| [`codecs/theora/`](codecs/theora/) | Theora | why libtheora is the limit, options | notes |
| [`codecs/prores/`](codecs/prores/) | Apple ProRes | intra-only QoS fix, 10-bit packing | config + notes |
| [`codecs/dnxhr/`](codecs/dnxhr/) | Avid DNxHR / DNxHD | MKV support, intra-only QoS fix | plugin (shared) + notes |
| [`codecs/hap/`](codecs/hap/) | HAP / HAP Alpha | `cutepiqtmkvparse`: HAP, DNxHR, QuickTime Animation in Matroska | plugin |
| [`codecs/qtrle/`](codecs/qtrle/) | QuickTime Animation | MKV bug (FFmpeg), copies to avoid | notes + upstream report |
| [`codecs/cineform/`](codecs/cineform/) | GoPro CineForm | ★ CineForm SDK on AArch64 (NEON) + `cutepicfhddec` element | port + plugin |
| [`codecs/ffv1/`](codecs/ffv1/) | FFV1 | measured limits (alpha) | notes |
| [`codecs/wmv/`](codecs/wmv/) | WMV / WMA / ASF | how to play `.wmv`/`.asf` without changing your other decoders | config |
| [`codecs/ac3/`](codecs/ac3/) | AC-3 / E-AC-3 audio | decoder-rank pitfall | config |
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
| Speed | `playbin` (video only) into `fakesink sync=false`; frames counted from the first to the last; median of 3 runs, stock and with this repo alternating |
| Test clips | CuTePi's codec test set: FFmpeg `testsrc2` with a frame counter, 5-16 s per clip |
| Picture quality | an MD5 checksum of the visible pixels of every frame, against the reference decoder; PSNR (FFmpeg) where frames differ |

**What this repository does not claim.** Earlier "1080p60 on screen" verdicts from CuTePi's GPU compositor were
withdrawn: that compositor wasn't drawing most layers when they were taken. Only flat-out decoder measurements and
correctness checks are quoted here. Things that were *not* tested are listed in each folder. In particular, the
kernel patches have never been loaded on hardware.

---

## FAQ

**Why is hardware H.264 decoding slow on my Raspberry Pi 4?**
The firmware keeps the H.264 block at 250 MHz unless the processor runs at full speed, and the `bcm2835-codec`
driver never asks for more. Add `h264_freq_min=500`, `isp_freq_min=500` and `core_freq_min=500` to `config.txt`
([`hardware/firmware-clocks/`](hardware/firmware-clocks/)). 1080p decode goes from 54 to 90 fps.

**How do I deinterlace 1080i on a Raspberry Pi 4 in real time?**
GStreamer's `deinterlace` reads the decoder's uncached frames twice and manages about 60 pictures a second. The
NEON bob deinterlacer in [`common/bob-deinterlacer-neon/`](common/bob-deinterlacer-neon/) does 124 a second, with
the same output as `deinterlace method=linear fields=all`. Set its `tff` property to the clip's field order: it
doesn't read it from the stream.

**How do I play AV1 smoothly on a Raspberry Pi 4?**
Use dav1d, not libaom: [`codecs/av1/`](codecs/av1/) has a GStreamer element that loads the system's libdav1d. It
decodes 1080p at 108 fps, 3.2× GStreamer's default `av1dec`, with identical pictures.

**Why do animated WebP/APNG/AVIF files play slowly or show only the first frame in GStreamer?**
GStreamer 1.26's `webpdec` decodes WebP stills only, and it has no APNG demuxer and no AVIF-sequence or JPEG XL
support. FFmpeg 7.1 can't decode animated WebP either. [`codecs/animated-images/`](codecs/animated-images/) adds one
decoder for all five, with transparency, decoding frames in parallel.

**Can ProRes, DNxHR, CineForm or HAP play on a Raspberry Pi 4?**
Yes, at 1080p, in software. ProRes 422 decodes at 74-88 fps, DNxHR HQ at 142-146, HAP at 96, and CineForm at 81
with the ported SDK ([`codecs/cineform/`](codecs/cineform/)). 4K ProRes (21 fps) and 4K CineForm (23 fps) are too slow. HAP and
DNxHR in MKV need the parser in [`codecs/hap/`](codecs/hap/). Turn QoS off for these decoders
([`docs/intra-only-qos.md`](docs/intra-only-qos.md)), or a brief stall turns into seconds of dropped frames.

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
  - Kernel patches: GPL-2.0 ([`codecs/h264/bcm2835-codec-patches/LICENSE`](codecs/h264/bcm2835-codec-patches/LICENSE)), as the
    kernel requires.
  - Vendored third-party headers keep their own licences, next to them: dav1d (BSD-2), libwebp (BSD-3), libavif
    (BSD-2), libjxl (BSD-3), sse2neon (MIT), and the CineForm SDK (Apache-2.0 / MIT).

<sub>Keywords: Raspberry Pi 4 video decoding performance, Raspberry Pi hardware video acceleration, BCM2711, VideoCore
VI, V3D, V4L2 M2M, bcm2835-codec, v4l2h264dec, h264_v4l2m2m, rpi-hevc-dec, config.txt h264_freq, isp_freq,
core_freq, GStreamer plugin, gst-launch-1.0, playbin, decodebin, NEON SIMD, AArch64, ARM64, Cortex-A72, deinterlace
1080i, bob deinterlacer, Motion JPEG decoder, MJPEG multithreaded, PNG video, animated GIF, APNG, animated WebP, AVIF
sequence, JPEG XL animation, AV1 dav1d GStreamer, VP8 VP9 libvpx, Theora, ProRes, DNxHR, DNxHD, CineForm SDK ARM
port, sse2neon, HAP codec Matroska, QuickTime Animation qtrle, FFV1, WMV ASF, dma-buf, dma-heap, CMA, zero-copy
EGLImage, kmssink, glvideomixer, QoS, frame dropping, video playback stutter Raspberry Pi, picture quality PSNR.</sub>
