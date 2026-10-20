# GStreamer: matroskademux exposes V_QUICKTIME tracks as video/x-unknown

Ready to file at https://gitlab.freedesktop.org/gstreamer/gstreamer/-/issues (component: gst-plugins-good, matroska).

**Summary:** For a `V_QUICKTIME` track, matroskademux outputs `video/x-unknown, codec-id=V_QUICKTIME` instead of
mapping the fourcc in the CodecPrivate sample description to caps, as qtdemux does. QuickTime Animation (`rle `) in
MKV therefore cannot be decoded, although `avdec_qtrle` is available.

**Version:** GStreamer 1.26.2 (Debian trixie, aarch64).

**Reproduce:**
```
ffmpeg -f lavfi -i testsrc2=size=320x240:rate=25 -t 2 -c:v qtrle qtrle.mkv
gst-launch-1.0 filesrc location=qtrle.mkv ! matroskademux ! fakesink -v
```
**Result:** `caps = video/x-unknown, codec-id=(string)V_QUICKTIME`

**Expected:** the fourcc from the sample description (bytes 4-7 of CodecPrivate, here `rle `) mapped as qtdemux maps it
(`video/x-rle, layout=quicktime, depth=24`), so decodebin plugs `avdec_qtrle`.
