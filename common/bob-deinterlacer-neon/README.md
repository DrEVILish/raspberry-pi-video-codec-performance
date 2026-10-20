# cutepibob: fast NEON bob deinterlacer for GStreamer (1080i in real time on a Raspberry Pi 4)

Turns interlaced video (two fields woven into each frame) into **one progressive picture per field**: 1080i50 →
1080p50 and 1080i60 → 1080p60. The missing lines are the rounded mean of the lines above and below (line-interpolated
"bob"). The output is the same as GStreamer's `deinterlace method=linear fields=all` within ±1, and twice as fast.

| 1080i H.264, hardware decode (`v4l2h264dec`), Pi 4 at 650 MHz | Pictures out per second |
|---|---|
| `... ! deinterlace method=linear fields=all mode=interlaced` | 59.6 |
| `... ! cutepibob` | **124.3** |

**Why it's faster** ([`../../hardware/`](../../hardware/README.md#4-uncached-memory-read-it-once)):
- **One read of the source.** A hardware decoder's frames sit in uncached (write-combined) memory, where CPU reads
  are slow. `deinterlace` reads every source row twice. `cutepibob` copies each row once into a cached three-row ring
  and writes both output pictures from there, in one NEON pass (`vrhaddq_u8`, 16 pixels an instruction).
- **No copy on the way in.** It answers the allocation query with video meta, so the decoder hands over its own
  buffers.

**Formats:** I420, YV12, Y42B, Y444, NV12, NV21, GRAY8, A420, A444, GBR, GBRA, AYUV, RGBA, BGRA, ARGB, ABGR, RGBx,
BGRx, xRGB, xBGR, RGB, BGR, YUY2, UYVY, YVYU: any 8-bit layout whose planes are rows of bytes, alpha included.

**Properties:**

| Property | Default | Meaning |
|---|---|---|
| `tff` | `true` | top field first. `false` for bottom-field-first material: ffprobe `field_order` `bb`, or `tb` (coded top first, displayed bottom first) |
| `dmabuf` | `false` | write the pictures into contiguous **CMA dma-heap** buffers (`/dev/dma_heap/linux,cma`) and output `video/x-raw(memory:DMABuf),format=DMA_DRM,drm-format=YU12`. A GPU or a display plane can then use them without another copy. **I420 input only.** Strides: luma 64 bytes, chroma 32 |

Timestamps: the first picture of a frame keeps its PTS, the second gets PTS + duration/2; each lasts half a frame.

## Install

```sh
sudo apt install build-essential pkg-config libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
cd common/bob-deinterlacer-neon
make && make check          # builds libgstcutepibob.so and shows the element
make install                # ~/.local/share/gstreamer-1.0/plugins  (or: sudo make install-system)
```

## Use

```sh
# 1080i H.264 decoded in hardware, deinterlaced, shown:
gst-launch-1.0 filesrc location=in.mov ! qtdemux ! h264parse ! v4l2h264dec ! cutepibob ! queue ! videoconvert ! autovideosink

# MPEG-2 1080i (software decoder), bottom field first:
gst-launch-1.0 filesrc location=in.ts ! tsdemux ! mpegvideoparse ! avdec_mpeg2video ! cutepibob tff=false ! queue ! videoconvert ! autovideosink

# Straight into DMABufs for a GL or KMS sink (I420):
gst-launch-1.0 ... ! v4l2h264dec ! cutepibob dmabuf=true ! queue ! glimagesink
```

**Put a `queue` after it,** so the next stage (conversion, upload) runs on its own thread.

**Telling interlaced sources apart:** `v4l2h264dec` labels 1080i H.264 as progressive in its caps, so `cutepibob`
deinterlaces whatever it is given, and you decide when to plug it. Use the stream's metadata:
`ffprobe -v error -select_streams v:0 -show_entries stream=field_order -of csv=p=0 in.mov` prints
`progressive` / `tt` / `bb` / `tb` / `bt`.

## Verify

```sh
# 10 interlaced frames in, 20 pictures out:
gst-launch-1.0 videotestsrc num-buffers=10 ! video/x-raw,format=I420,interlace-mode=interleaved ! cutepibob ! fakesink silent=false -v | grep -c chain
```

## Not done

- **Motion-adaptive or motion-compensated deinterlacing.** Bob is spatial only: fine detail flickers slightly
  between fields, as with any bob.
- **10-bit input.** Pack it to 8-bit first with [`../yuv-pack-neon/`](../yuv-pack-neon/).
