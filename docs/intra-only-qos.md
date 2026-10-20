# Intra-only decoders and GStreamer QoS: why ProRes, DNxHR, HAP and MJPEG stall for seconds, and the one-line fix

**Symptom:**
- A ProRes, DNxHR, HAP, MJPEG, PNG-video, CineForm or FFV1 clip plays fine.
- After a brief hiccup (another process busy, the start of playback, a seek) it collapses to 1-3 shown frames a
  second, and the lag grows for seconds before it recovers, if it does.

**Cause:**
- A video sink (anything derived from `GstVideoSink`, `kmssink` included) sends **QoS events** upstream when frames
  arrive late.
- `GstVideoDecoder` reacts by dropping frames until it's back on the clock. It works out how far ahead to drop from
  the reported lateness, up to twice it.
- **For intra-only codecs that saves nothing.** Every frame is decoded anyway and dropped after decoding. A decoder
  that runs at about real time therefore never gets ahead. It decodes, drops, reports late again, and drops more.
- **Measured on a Pi 4:** DNxHR HQ showed 1-2 frames a second with the lag growing to 4 s. ProRes Proxy, started
  during a burst of other work, showed 3 a second for seconds.

**Fix:**
- Turn QoS off on intra-only decoders. Late frames are then shown as they come, and the decoder catches up at its
  own spare speed.
- Inter-frame decoders (H.264, HEVC, VP9 in software) can skip non-reference frames *before* decoding, so leave
  QoS on for them.

Every `GstVideoDecoder` has a `qos` property (GStreamer ≥ 1.18):

```sh
gst-launch-1.0 filesrc location=clip.mov ! qtdemux ! avdec_prores qos=false ! ...
```

With `playbin`/`decodebin`, set it as decoders are created:

```c
static void
on_deep_element_added (GstBin * bin, GstBin * sub, GstElement * e, gpointer u)
{
  static const char *intra[] = { "avdec_prores", "avdec_dnxhd", "avdec_hap", "avdec_mjpeg", "avdec_png",
    "avdec_cfhd", "avdec_ffv1", "avdec_jpeg2000", "avdec_utvideo", "avdec_magicyuv", "avdec_huffyuv",
    "avdec_ffvhuff", "avdec_dvvideo", "avdec_v210", "avdec_qoi", "jpegdec", "pngdec",
    "cutepimjpegdec", "cutepipngvdec", "cutepicfhddec", NULL };
  GstElementFactory *f = gst_element_get_factory (e);
  if (!f) return;
  for (int i = 0; intra[i]; i++)
    if (g_str_equal (GST_OBJECT_NAME (f), intra[i]) &&
        g_object_class_find_property (G_OBJECT_GET_CLASS (e), "qos"))
      g_object_set (e, "qos", FALSE, NULL);
}
/* g_signal_connect (pipeline, "deep-element-added", G_CALLBACK (on_deep_element_added), NULL); */
```

Python (PyGObject):

```python
INTRA = {"avdec_prores", "avdec_dnxhd", "avdec_hap", "avdec_mjpeg", "avdec_png", "avdec_cfhd", "avdec_ffv1",
         "jpegdec", "pngdec", "cutepimjpegdec", "cutepipngvdec", "cutepicfhddec"}
def on_added(bin, sub, e):
    f = e.get_factory()
    if f and f.get_name() in INTRA and any(p.name == "qos" for p in e.list_properties()):
        e.set_property("qos", False)
pipeline.connect("deep-element-added", on_added)
```

The list is FFmpeg's `AV_CODEC_PROP_INTRA_ONLY` codecs that GStreamer exposes, plus GStreamer's and this
repository's decoders for them. Match by **factory name**. Go users: don't read the factory's metadata with go-gst
`GetMetadata`, which corrupts it ([`pitfalls.md`](pitfalls.md)).

**How to check:** `GST_DEBUG=videodecoder:5` shows `QoS: ... dropping` lines. After the change there are none, and
a stall recovers within a few frames.
