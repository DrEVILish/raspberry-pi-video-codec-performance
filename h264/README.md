# H.264 / AVC on the Raspberry Pi 4: hardware decode at full speed, 1080i deinterlacing, pitfalls

The Pi 4 has a hardware H.264 decoder and encoder in the VideoCore. Linux exposes them through the `bcm2835-codec`
V4L2 stateful driver: decoder `/dev/video10`, encoder `/dev/video11`, ISP `/dev/video12`. GStreamer uses them as
`v4l2h264dec`/`v4l2h264enc`, FFmpeg as `h264_v4l2m2m`.

## What is here

| Change | Gain | Where |
|---|---|---|
| ★ Firmware clock floor for the H.264/ISP/core clocks | 1080p decode **54 → 90 fps** (98 overclocked); encode **41.7 → 74.4** (93) | [`../hardware/firmware-clocks/`](../hardware/firmware-clocks/) |
| ★ `bcm2835-codec` kernel patches: clock requests, start order, cache hints | the clock fix done in the kernel instead; no lost first frame on the ISP/encoders | [`bcm2835-codec-patches/`](bcm2835-codec-patches/) (compile-tested only) |
| 1080i deinterlacing after hardware decode | **124.3 pictures/s** against 59.6 with `deinterlace method=linear` | [`../common/bob-deinterlacer-neon/`](../common/bob-deinterlacer-neon/) |
| 10-bit (High 10) to I420 | 2.35 ms against 20 ms a 1080p frame | [`../common/yuv-pack-neon/`](../common/yuv-pack-neon/) |

## Measured (Raspberry Pi 4, 1080p)

| Path | Default clocks | Codec clocks 500 MHz | 650 MHz + ARM 2.0 GHz |
|---|---|---|---|
| Raw V4L2 decode, 8 Mbit/s | 53-54 | 90.0 | 98.2 |
| GStreamer `v4l2h264dec ! fakevideosink` | - | - | 94.7 |
| FFmpeg `h264_v4l2m2m` | 50.7 | 80.1 | 87.0 |
| Raw V4L2 encode, 10 Mbit/s | 41.7 | 74.4 | 93.0 |

**Two 1080p60 streams at once** (on screen, one atomic display commit per refresh):

| Pair | Result |
|---|---|
| H.264 hardware + H.264 hardware (one engine) | 35 + 33 fps (50.6 + 50.5 without B-frames) |
| H.264 hardware + HEVC hardware | **59.95 + 59.93** |
| H.264 hardware + H.264 software (FFmpeg) | 59.9 + 45.5 |

For two H.264 streams, decode one in hardware and pair it with a codec on another engine (HEVC) or with a cheap
software codec. See the memory-bus note in [`../hardware/`](../hardware/README.md#9-the-memory-bus-is-the-shared-limit).

## 1080i (interlaced) H.264

The hardware decoder outputs interlaced frames woven, and **labels 1080i as progressive in its caps**. Decide from
the stream's field order instead, for example `ffprobe -show_entries stream=field_order`: `tt`/`bb`/`tb`/`bt`
means interlaced.

```sh
# Hardware decode + NEON bob deinterlacer: each field becomes a picture, 50/60 pictures a second, no combing.
gst-launch-1.0 filesrc location=in1080i.mov ! qtdemux ! h264parse ! v4l2h264dec ! cutepibob tff=true ! \
    queue ! videoconvert ! autovideosink
```

`tff=false` for bottom-field-first material: ffprobe `bb`, or `tb` (coded top first, shown bottom first). See
[`../common/bob-deinterlacer-neon/`](../common/bob-deinterlacer-neon/).

What did not work, so you needn't try it:
- **`deinterlace` (GStreamer):** `method=linear` reads each uncached frame twice (59.6 pictures/s);
  `method=greedyh` costs 32 ms a frame.
- **The VideoCore deinterlacer** (`/dev/video18`, `v4l2convert`): it can't be driven 1:2 by GStreamer's converter
  and returns the frames combed.
- **A GPU bob in a GL compositor** overloaded the V3D.

## H.264 High 10 (10-bit)

There is no hardware decoder for it; FFmpeg's software decoder does it.
- **Use frame threads:** `ffmpeg -thread_type frame` decodes a 1080p60 High 10 test file at 70.6 fps, against
  35.3 fps with slice threads. GStreamer's `avdec_h264` exposes this as `thread-type`.
- **Convert 10-bit for display** with `cutepiyuvpack` (I420_10LE → I420) instead of `videoconvert`.

## Pitfalls when measuring

- **`fakesink` makes `v4l2h264dec` copy every frame** (68.6 vs 93.6 fps). Use `fakevideosink`, or a sink that takes
  the decoder's buffers (anything that answers the allocation query with video meta).
- **GStreamer's and FFmpeg's H.264 encoders deliver 3.3-4.0 Mbit/s when asked for 10.** The raw V4L2 harness gets
  9.77. Check the output bit rate before comparing encoders.
- **The ISP slows the decoder.** Running the ISP at the same time costs the decoder 15-25%
  ([`../hardware/`](../hardware/README.md#3-the-isp-and-the-h264-decoder-share-the-videocore)).
- **QoS from a display sink makes `v4l2h264dec` (a GstVideoDecoder) drop frames after decoding them.** That's
  harmless when it's rare, but under sustained load the picture stutters. See
  [`../docs/intra-only-qos.md`](../docs/intra-only-qos.md).
