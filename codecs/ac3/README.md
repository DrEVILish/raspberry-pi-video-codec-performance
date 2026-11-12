# AC-3 / E-AC-3 audio in GStreamer: keep FFmpeg's decoder when plugins-ugly is installed

`gstreamer1.0-plugins-ugly` (needed for ASF/WMV, [`../wmv/`](../wmv/)) installs `a52dec` at rank secondary. FFmpeg's
`avdec_ac3` is only marginal, so after the install every AC-3 track silently changes decoder. Keep the decoder you
tested with:

```sh
export GST_PLUGIN_FEATURE_RANK=a52dec:0
gst-inspect-1.0 a52dec | grep Rank        # none (0)
```

E-AC-3 stays on `avdec_eac3` either way.
