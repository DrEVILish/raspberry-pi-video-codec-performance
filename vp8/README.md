# VP8 on the Raspberry Pi 4: use FFmpeg's decoder, not libvpx (two 1080p60 streams: 6+6 → 56+56 fps)

GStreamer's `vp8dec` (libvpx) has rank primary and wins autoplugging. With two 1080p60 VP8 layers at once on a
Pi 4, libvpx's threads spin against each other: **6 + 6 fps**. FFmpeg's VP8 decoder (`avdec_vp8`, gst-libav)
does **56 + 56 fps** in the same test.

**Change (no code):** rank FFmpeg's decoder above libvpx.

```sh
# For a shell / one program:
export GST_PLUGIN_FEATURE_RANK=avdec_vp8:257
# For a systemd service (drop-in):
#   [Service]
#   Environment=GST_PLUGIN_FEATURE_RANK=avdec_vp8:257
# In code, before gst_init():  setenv("GST_PLUGIN_FEATURE_RANK", "avdec_vp8:257", 0);
gst-inspect-1.0 avdec_vp8 | grep Rank          # primary + 1 (257)
```

`GST_PLUGIN_FEATURE_RANK` is a comma-separated list. Join it with other overrides, e.g.
`avdec_vp8:257,v4l2jpegdec:0`.

**WebM with alpha** (VP8 + alpha channel) goes through `vp8alphadecodebin`, which ranks above both and is
unaffected.
