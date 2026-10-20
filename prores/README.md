# Apple ProRes on the Raspberry Pi 4: stop the QoS frame-drop spiral, pack 10-bit 4:2:2 for display

FFmpeg's `avdec_prores` decodes ProRes (422 Proxy/LT/422/HQ, 4444/XQ) in software. On the Pi 4, ProRes LT and Proxy
played at 1080p60 on KMS display planes in our tests.

**1. Turn QoS off on the ProRes decoder.**
- **The problem:** ProRes is intra-only, so every frame is decoded, and a late one can only be dropped *after*
  decoding. When the display sink reports lateness (QoS), `GstVideoDecoder` drops ahead, and the decoder, running at
  about real time, never catches up. A brief stall at the start of a clip turned into 3 shown frames a second for
  seconds.
- **The fix:** `g_object_set(decoder, "qos", FALSE, NULL)` when the decoder is created. Code and the list of
  affected decoders: [`../docs/intra-only-qos.md`](../docs/intra-only-qos.md).

**2. Pack 10-bit 4:2:2 to I420 with NEON before a display plane.**
- **The problem:** `avdec_prores` gives 10-bit 4:2:2 (`I422_10LE`). Pi 4 display planes (`kmssink`) can't allocate
  4:2:2 buffers, so `videoconvert` falls back to RGB, at 0.3-1.4 fps.
- **The fix:** [`../common/yuv-pack-neon/`](../common/yuv-pack-neon/) packs to I420 in 2.35 ms a 1080p frame.
  ProRes 422/HQ/LT/Proxy then reached 37-54 fps on KMS planes.

```sh
gst-launch-1.0 filesrc location=prores.mov ! qtdemux ! avdec_prores qos=false ! cutepiyuvpack ! videoconvert ! kmssink
```

ProRes 4444 (with alpha, 12-bit) is heavier and doesn't reach 60 fps at 1080p on a Pi 4.
