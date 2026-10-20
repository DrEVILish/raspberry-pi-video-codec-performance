# Pitfalls found while optimising video on the Raspberry Pi 4 (GStreamer, V4L2, EGL, dma-buf)

| Pitfall | What happens | What to do |
|---|---|---|
| `fakesink` after `v4l2h264dec` | the decoder copies every frame for it: 68.6 vs 93.6 fps | benchmark with `fakevideosink`, or a sink that answers the allocation query with video meta |
| GStreamer's/FFmpeg's H.264 encoders and bit rate | asked for 10 Mbit/s they deliver 3.3-4.0 | check the output bit rate before comparing encoders |
| `v4l2h264dec` and 1080i | labels interlaced H.264 as progressive in its caps | decide on deinterlacing from the stream metadata (ffprobe `field_order`) |
| QoS and intra-only decoders | seconds of 1-3 fps after a hiccup | `qos=false` on them ([`intra-only-qos.md`](intra-only-qos.md)) |
| A plugin checking other elements in `plugin_init` | the plugin scanner runs it before the registry is complete; `gst_element_factory_find()` fails and the plugin is blacklisted | check for dependencies when the element starts, not at registration |
| go-gst `ElementFactory.GetMetadata` | frees a string GStreamer owns; the factory's klass is corrupted, and from the second pipeline on, `decodebin` reports "no decoder" for every FFmpeg codec it touched | don't call it; check for a property with `g_object_class_find_property` (`GetPropertyType`) instead |
| `glupload` of a DMABuf + `glvideomixer` | the texture is external-OES, the mixer skips it silently: black output | force `texture-target=2D` after `glcolorconvert` |
| A texture bound to an EGLImage with no filters set | incomplete under GL's default mipmap minifier: samples as opaque black | set `GL_TEXTURE_MIN_FILTER`/`MAG_FILTER` (and `CLAMP_TO_EDGE`) after binding |
| `gldownload` of an imported texture that `glcolorconvert` passed through | maps empty (all zeros) in a test | draw it first (a mixer, a shader), or force a conversion |
| CPU writes to a dma-heap buffer, then GPU reads | stale data if the cache isn't cleaned | `DMA_BUF_IOCTL_SYNC` `START|WRITE` / `END|WRITE` around the writes; never `START` (invalidate) a buffer whose writer may still have dirty lines |
| `kmssink` and 4:2:2 | its pool can't allocate 4:2:2 buffers, so `videoconvert` falls back to RGB (0.3-14 fps) | pack to I420 first ([`../common/yuv-pack-neon/`](../common/yuv-pack-neon/)) |
| Two `kmssink`s on one CRTC | they starve each other: 59.3 + 0.7 fps | one atomic commit per refresh carrying every plane |
| `avdec_qtrle direct-rendering=false` | 1 frame of 480 comes out | leave direct rendering on |
| `videotestsrc` with `framerate=0/1` | one frame only, so throughput tests measure nothing | give a frame rate |
| NEON multi-register loads (`vld1q_u8_x4`) on the Cortex-A72 | slower than four single-register `vld1q_u8` in row conversions | measure both |
| Measuring a compositor by counting presented frames | it can report 60 presented while the layers are black or undrawn | read the picture back (the scanned-out framebuffer) when you measure |
