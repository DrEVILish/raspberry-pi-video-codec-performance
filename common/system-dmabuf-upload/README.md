# cutepisysdmabuf: software video into system dma-heap buffers for zero-copy GPU import (Raspberry Pi 4 V3D)

Copies each software-decoded `I420` or `NV12` frame, plane by plane, into a buffer from **`/dev/dma_heap/system`**
and outputs it as `video/x-raw(memory:DMABuf),format=DMA_DRM,drm-format=YU12|NV12`. `glupload` then imports it as
an EGLImage, which the V3D samples in place (`DirectDmabufExternal`).

Why (Raspberry Pi 4):
- **The usual route goes through the ISP.** It gets a software frame into GPU-importable memory with `v4l2convert`,
  which costs a CPU copy into the ISP's input plus an ISP pass on the VideoCore. The VideoCore is shared with the
  hardware H.264 decoder: with both running, the decode drops from 92 to 70-79 fps
  ([`../../hardware/`](../../hardware/README.md#3-the-isp-and-the-h264-decoder-share-the-videocore)).
- **This element does one CPU copy and nothing else.** The writes are bracketed by `DMA_BUF_IOCTL_SYNC`, so the GPU
  sees them.
- **No contiguous memory needed.** The system heap is cached and page-backed (no CMA); the V3D has its own MMU.

**Checked:**
- 1920×1080 I420 and NV12 frames go through `glupload`, `glcolorconvert` and `gldownload`. All 12 moving frames in
  each format were **bit-identical** to the same frames uploaded from system memory.
- In a GL compositor on the Pi 4 this route beat the ISP route by 40-60%: MPEG-2 21-23 fps against 14. The
  compositor itself was V3D-bound
  ([`../../hardware/`](../../hardware/README.md#5-the-v3d-gpu-budget-video-belongs-on-display-planes)), so the
  absolute numbers are low.

**Details:**
- Strides are padded to 64 bytes, one buffer per frame with the plane offsets and a `GstVideoMeta`.
- Buffers are allocated as needed, up to 24 per element, and reused once downstream frees them.
- The element registers only if `/dev/dma_heap/system` can be opened. The user must be in the `video` group.

## Install and use

```sh
cd common/system-dmabuf-upload && make && make check && make install
gst-launch-1.0 filesrc location=in.mkv ! matroskademux ! avdec_vp9 ! videoconvert ! video/x-raw,format=I420 ! \
    cutepisysdmabuf ! glupload ! glcolorconvert ! glimagesink
```

**Black output?** If you composite with `glvideomixer`, force `texture-target=2D` after `glcolorconvert`:
`... ! glupload ! glcolorconvert ! "video/x-raw(memory:GLMemory),format=RGBA,texture-target=2D" ! glvideomixer`.
The mixer can't draw the external-OES texture `glupload` makes, and skips the layer silently.
