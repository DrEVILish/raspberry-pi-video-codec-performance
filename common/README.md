# Shared GStreamer elements: NEON deinterlacer, NEON 10-bit/4:2:2 packer, zero-copy GPU upload

Elements that help several codecs. Each folder builds one GStreamer plugin (`make`, `make check`, `make install`).

| Element | Folder | Helps | Gain |
|---|---|---|---|
| `cutepibob` | [`bob-deinterlacer-neon/`](bob-deinterlacer-neon/) | every interlaced source (1080i/576i H.264, MPEG-2, DV, ProRes...) | **2.1×** GStreamer's `deinterlace method=linear` (124.3 vs 59.6 pictures/s, 1080i H.264, hardware decode) |
| `cutepiyuvpack` | [`yuv-pack-neon/`](yuv-pack-neon/) | 10-bit and 4:2:2 sources (ProRes, DNxHR, H.264 High 10, HEVC Main 10 in software) | **8.5×** `videoconvert` to I420 (2.35 vs 20 ms a 1080p 10-bit 4:2:2 frame) |
| `cutepisysdmabuf` | [`system-dmabuf-upload/`](system-dmabuf-upload/) | software-decoded I420/NV12 going to the GPU (EGL/GLES, `glupload`) | replaces an ISP copy (`v4l2convert`) by one CPU copy; frees the VideoCore for hardware decode |
