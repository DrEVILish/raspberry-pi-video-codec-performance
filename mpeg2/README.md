# MPEG-2 on the Raspberry Pi 4: software decode, 1080i deinterlacing, decoder choice

- **Decoding is fine:** FFmpeg's `avdec_mpeg2video` decodes 1080p MPEG-2 (25 Mbit/s) comfortably. Two 1080p60
  MPEG-2 streams play at once at 60 + 60 fps on screen.
- **1080i (broadcast, DVD, HDV):** deinterlace with the NEON bob,
  [`../common/bob-deinterlacer-neon/`](../common/bob-deinterlacer-neon/). It's 2.1× GStreamer's `deinterlace`, and
  each field becomes a picture (1080i50 → 50p):
  `... ! avdec_mpeg2video ! cutepibob tff=<true|false> ! queue ! ...` (field order: `ffprobe -show_entries stream=field_order`).
- **4:2:2 MPEG-2** (e.g. 50 Mbit/s broadcast) decodes to Y42B: pack it to I420 with
  [`../common/yuv-pack-neon/`](../common/yuv-pack-neon/) for display planes.
- **Keep `avdec_mpeg2video`:** if you install `gstreamer1.0-plugins-ugly` (for ASF/WMV, see [`../wmv/`](../wmv/)), it
  brings `mpeg2dec` (libmpeg2). It ranks below `avdec_mpeg2video` today. To be sure, pin
  `GST_PLUGIN_FEATURE_RANK=mpeg2dec:0`.
