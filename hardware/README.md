# Raspberry Pi 4 hardware optimisations for video: codec clocks, VideoCore, ISP, V3D GPU and memory

These are the Pi 4-specific findings behind everything else in this repository: what the BCM2711's video hardware
does, where it waits, and what to change. They come first because they matter more than any decoder. Fixing the
firmware clocks alone takes hardware H.264 decode from 54 to 90 fps.

| # | Finding | What to do | Gain |
|---|---|---|---|
| 1 | ★ The H.264, ISP and VPU core clocks sit at their minimum (250/250/200 MHz) during hardware decode | `h264_freq_min=500`, `isp_freq_min=500`, `core_freq_min=500` in `config.txt` ([`firmware-clocks/`](firmware-clocks/)) | H.264 1080p decode **54 → 90 fps**; encode **41.7 → 74.4 fps** |
| 2 | ★ Overclock tested bit-exact on the test board | [`firmware-clocks/overclock.txt`](firmware-clocks/overclock.txt) (optional) | decode 90 → 98 fps, encode 74 → 93 fps |
| 3 | ★ The ISP and the H.264 decoder share the VideoCore VPU | keep ISP copies (`v4l2convert`) away from hardware decode; copy with the CPU or the GPU instead | ISP 287 → 131 fps and decode 92 → 70-79 fps when they run together |
| 4 | ★ V4L2 MMAP buffers are uncached (write-combined) | read each row once; use cached dma-heap buffers + `DMA_BUF_IOCTL_SYNC` | 1080p frame read 6.1 ms → 3.3 ms |
| 5 | ★ The V3D GPU cannot composite moving 1080p60 with GStreamer GL | put video on KMS display planes; keep the GPU for small layers | 1 GL layer of 1080p video: ~16 fps, V3D render queue 90% busy |
| 6 | ★ The V3D samples non-contiguous `/dev/dma_heap/system` buffers in place | upload into system dma-heap buffers, not CMA; keep CMA for scanout | no CMA used; pixel-identical to a normal upload |
| 7 | The `bcm2835-codec` driver: start-order bug, no cache hints, no clock requests | [`../h264/bcm2835-codec-patches/`](../h264/bcm2835-codec-patches/) (compile-tested only) | first-frame loss and 0.5 s stall on non-decoder roles fixed; cached MMAP possible |
| 8 | NEON on the Cortex-A72 | NEON versions of deinterlacing, pixel packing, SSE2/MMX code ([`../common/`](../common/), [`../cineform/`](../cineform/)) | 2.1× (bob), 8.5× (10-bit pack), CineForm on ARM at all |
| 9 | ★ The memory bus is the shared limit for several streams | pair a hardware stream with a cheap software codec; one atomic commit per refresh for all planes | hardware + CPU mixes total ~127 decoded 1080p frames/s |

---

## 1. Codec clocks: the half-speed H.264 decoder (★ biggest single win)

**Symptom:** 1080p H.264 through `v4l2h264dec` (GStreamer) or `h264_v4l2m2m` (FFmpeg) tops out at about 50-54 fps,
and a 1080p60 file drops frames, although HEVC (a harder codec) decodes at 140+ fps on the same board.

**Cause:**
- **Firmware policy.** The firmware runs the H.264, ISP and core clocks at their minimum (250/250/200 MHz) unless
  the ARM is at full speed. A hardware decode barely uses the ARM, so under the default `ondemand` governor the ARM
  never speeds up and the decoder stays at half clock.
- **Driver.** The H.264 driver (`bcm2835-codec`, the V4L2 stateful codec) requests no clock. The HEVC driver
  (`rpi-hevc-dec`) does (`clk_set_min_rate`), which is why HEVC "beats" H.264.
- **Why it hides on older images.** Older Raspberry Pi OS images tuned the CPU governor, which hid this.
- **Not the driver's speed.** The driver costs 0.06 of a core and keeps the firmware engine fed 99% of the time.

**Measured** (1080p H.264, raw V4L2 decode of an 8 Mbit/s stream):

| Clocks | Raw V4L2 decode | No B-frames | FFmpeg `h264_v4l2m2m` | Raw V4L2 encode, 10 Mbit/s | HEVC hardware |
|---|---|---|---|---|---|
| Default (ondemand) | **53-54** | 59.7 | 50.7 | 41.7 | 143.9 |
| Codec clocks held at 500 MHz (stock max) | **90.0** | 99.4 | 80.1 | 74.4 | 146.7 |
| 600 MHz | 96.6 | 107.0 | 82.2 | 87.5 | 150.0 |
| 650 MHz + ARM 2.0 GHz | **98.2** | 106.7 | 87.0 | 93.0 | 155.9 |

- From 250 to 500 MHz the engine speeds up 54%, then 7% more from 500 to 600 and 1.7% from 600 to 650.
- Every clock step was booted with `tryboot` and checked bit-exact against 500 MHz decodes and encodes.
- A 180 s soak gave 0 mismatches at 53.5 °C maximum, with no throttling.

**How to apply:** [`firmware-clocks/README.md`](firmware-clocks/README.md). It's three lines of `config.txt`,
no overclock and no overvoltage.

---

## 2. Optional overclock

[`firmware-clocks/overclock.txt`](firmware-clocks/overclock.txt):
- Codec blocks (H.264, ISP, HEVC), core and V3D at 650 MHz, the ARM at 2.0 GHz, `over_voltage_delta=75000`.
- No `force_turbo`, so the warranty bit isn't set.

On the test board this was bit-exact and stable with active cooling: decode 90 → 98 fps, encode 74 → 93 fps.
**Every board differs.** Try it with `tryboot` first, keep the board cooled, and watch `vcgencmd get_throttled`.

---

## 3. The ISP and the H.264 decoder share the VideoCore

The ISP (`/dev/video12`, GStreamer `v4l2convert`) converts, scales and copies video in hardware, and is fast on its
own:

| Test (1080p YU12 → YU12, clocks 650 MHz) | Rate |
|---|---|
| ISP, raw V4L2, buffer depth 1, 2 or 4 | **287 fps** (3.5 ms a frame) |
| ISP with a CPU copy into its input buffer each frame | 182 (depth 1) to 257 fps (depth 4) |
| ISP through GStreamer `v4l2convert` | 225 fps with a `queue` in front, 177 without |
| **ISP and H.264 decode at the same time** | ISP **287 → 131 fps**; decode **92 → 70-79 fps** |

**Lesson:** the ISP is not a free copy engine when hardware H.264 decode runs. Where you only need to move a frame
into GPU-importable memory, copy it with the CPU into a dma-heap buffer
([`../common/system-dmabuf-upload/`](../common/system-dmabuf-upload/)), or write it there in the first place (the
bob deinterlacer's `dmabuf=true`).

The driver side is not the limit: the completion path (VCHIQ → workqueue → vb2) is 20-27 µs median and < 0.1 ms at
p99, idle or with all four cores busy. Dmabuf imports to the firmware happen once per buffer, then are cached.

---

## 4. Uncached memory: read it once

- The decoder's V4L2 MMAP buffers are write-combined (uncached).
  - Reading a 1080p I420 frame from them on the CPU takes about **6.1 ms** (~510 MB/s).
  - The same read from a cached CMA dma-heap buffer, including the cache sync, takes about **3.3 ms**.
- **Read each source row once.** GStreamer's `deinterlace method=linear` reads every row twice from uncached
  memory. The NEON bob deinterlacer copies each row once into a small cached ring and works there, which is why it
  is 2.1× faster.
- **Ask for cached buffers where the driver allows it.** `bcm2835-codec` doesn't set `allow_cache_hints`, so
  userspace can't request cached (`V4L2_MEMORY_FLAG_NON_COHERENT`) MMAP buffers. Patch 0003 in
  [`../h264/bcm2835-codec-patches/`](../h264/bcm2835-codec-patches/) enables it.
- **Cache maintenance for dma-heap buffers written by the CPU:**
  - Bracket the writes with `DMA_BUF_IOCTL_SYNC` (`START|WRITE` before, `END|WRITE` after).
  - Never issue a `START` (cache invalidate) on a buffer whose writer may still have dirty lines: it discards them.

---

## 5. The V3D GPU budget: video belongs on display planes

A GStreamer GL compositor (`glupload ! glcolorconvert ! glvideomixer`) rendering into a 1080p60 output can't carry
one moving 1080p layer on the Pi 4's V3D:

| Test | Result |
|---|---|
| One 1080p60 H.264 layer, mixer at 60/s | **15.5 presented/s**, V3D render queue **89-92% busy** (`/sys/class/drm/card0/device/gpu_stats`) |
| Idle wall | render queue 19% busy |
| A still image layer | 30% busy |
| Standalone `v4l2h264dec ! glupload ! glcolorconvert ! glcolorbalance` | 88 fps: each stage is fine alone, the combination is not |

- **Use KMS hardware planes for video:** `kmssink`, or a DRM plane sink that does one atomic commit per refresh.
- **The display controller (HVS) scales, blends and alpha-fades planes** at no GPU cost.
- **Keep the GPU for small overlays.**

**Two traps when GStreamer GL is used on the V3D anyway:**
- `glupload` imports a DMABuf as an external-OES texture, and `glvideomixer` can't draw one: the layer is silently
  skipped and the output is black. Force `texture-target=2D` after `glcolorconvert`.
- A texture bound to an EGLImage with no `GL_TEXTURE_MIN_FILTER`/`MAG_FILTER` set is incomplete and samples as
  opaque black (GL's default minifier uses mipmaps).

---

## 6. dma-heap: system vs CMA

| Heap | Contiguous | Cached | V3D (GPU) import | Display plane (HVS) scanout | Typical use |
|---|---|---|---|---|---|
| `/dev/dma_heap/linux,cma` (`vidbuf_cached`) | yes | yes | yes | **yes** | buffers a display plane shows |
| `/dev/dma_heap/system` | no | yes | **yes** (V3D has an MMU) | no | buffers only the GPU reads |

- Checked on the Pi 4: system-heap buffers import in place through EGL and are pixel-identical to an upload from
  system memory.
  - RGBA, A420 and A444: exact.
  - BGRA, BGRx and RGBx: within 1.
  - I420 and NV12: bit-identical.
  - The ARGB byte order doesn't negotiate a dma-buf import in glupload.
- System-heap buffers use no CMA. CMA is 512 MB on a default Pi 4 and about 190 MB is free with a desktop running;
  one 1080p RGBA frame is 8 MB.
- The users need to be in the `video` group (`/dev/dma_heap/*` is `root:video 0660`).

---

## 7. The `bcm2835-codec` driver

The driver source was read in full (identical to `rpi-6.18.y`). Its findings, and three patches, are in
[`../h264/bcm2835-codec-patches/`](../h264/bcm2835-codec-patches/):
- **Clocks:** no clock requests, which is item 1 above, fixed in the kernel.
- **Start order:** `ignore_cap_streaming` is set for every role, so the ISP and the encoders lose frame 0 and stall
  490-500 ms when OUTPUT starts before CAPTURE.
- **Cache hints:** `allow_cache_hints` isn't set (item 4).

Applications can work around the start order: start the CAPTURE queue first. GStreamer already does.

---

## 8. NEON

The Cortex-A72's NEON unit works on 128-bit vectors (16 pixels of 8-bit video per instruction), which is what makes these software paths keep up:

- [`../common/bob-deinterlacer-neon/`](../common/bob-deinterlacer-neon/): line averaging with `vrhaddq_u8`, 16 pixels
  an instruction, over a three-row cached ring.
- [`../common/yuv-pack-neon/`](../common/yuv-pack-neon/): 10-bit to 8-bit narrowing and 4:2:2 to 4:2:0 chroma
  averaging in one pass (2.35 ms against 20 ms for `videoconvert` on 1080p 10-bit 4:2:2).
- [`../cineform/`](../cineform/): the CineForm SDK's x86 SSE2 code through sse2neon, plus `mmx2neon.h`, about 50 64-bit
  MMX intrinsics on NEON with Intel's saturation and shift semantics. Single-threaded output is bit-identical to x86.
- **NEON performance pitfall found along the way:** for row conversions, single-register `vld1q`/`vst1q` were faster
  on the A72 than the multi-register `vld1q_u8_x4` forms.

---

## 9. The memory bus is the shared limit

- **Every hardware + CPU decoder mix totals about 127 1080p frames/s decode-only**, and 105-108 frames/s on screen.
- **An ARM `memcpy` slows as the hardware decoder works:**

  | Alongside | `memcpy` |
  |---|---|
  | nothing | 4.47 GB/s |
  | one H.264 hardware decode at 60 fps | 2.48 GB/s |
  | that decode flat out | 0.65 GB/s |

- **Cheap software decoders leave room:** MPEG-1/2 and H.263 run 60 + 60 next to each other. Heavier ones (VP8,
  VP9, AV1) don't.

Two 1080p60 streams on screen, measured:

| Pair | fps |
|---|---|
| H.264 HW + HEVC HW | 59.95 + 59.93 |
| VP9 software + HEVC HW | 59.87 + 60.00 |
| AV1 (dav1d) + HEVC HW | 59.1 + 60.0 |
| ProRes Proxy (NEON pack) + HEVC HW | 60.00 + 60.00 |
| MPEG-2 + MPEG-2 | 60.00 + 60.00 |
| VP8 + VP8 (FFmpeg decoder) | 57.6 + 58.1 |
| VP9 + VP9 | 53.7 + 53.7 |
| AV1 + AV1 (dav1d) | 50.7 + 50.6 |
| ProRes Proxy + ProRes Proxy | 45.8 + 46.0 |
| H.264 HW + H.264 HW (one engine) | 35 + 33 |

**Display:**
- Put every plane in **one atomic commit per refresh**. Two `kmssink`s on one CRTC starve each other: 59.3 + 0.7 fps
  with test patterns.
- An HEVC hardware frame can't go on a vc4 plane as decoded. The decoder writes Y and UV with different column
  heights, and DRM describes only one per framebuffer. A working route points the plane at the decoder's Y and
  GPU-copies only the UV.
