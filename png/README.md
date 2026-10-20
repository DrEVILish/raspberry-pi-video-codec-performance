# PNG video decoding on several cores: cutepipngvdec for GStreamer, 3.2× faster (PNG in MOV/MKV, with alpha)

"PNG video" is a MOV or MKV file whose frames are PNG images: a common lossless format with alpha for motion
graphics, screen recordings and overlays. GStreamer's `pngdec` decodes one frame at a time, 21-22 ms a 1080p RGBA
frame on a Pi 4. That's 45 fps flat out, and a 1080p60 PNG video can't play.

`cutepipngvdec` decodes frames on 4 workers with libpng's simplified API, straight into the output buffers, and
finishes them in order:
- **Output:** straight-alpha RGBA. Every PNG colour type, palette transparency and 16-bit input give 8-bit RGBA.
- **Byte-identical** to `pngdec`.

| 1080p RGBA PNG video, Raspberry Pi 4 (flat out) | fps |
|---|---|
| `pngdec` | 45.8 |
| `cutepipngvdec` | **145** |

- **Moving pictures only** (non-zero framerate): PNG stills keep `pngdec`.
- **No libpng headers needed:** `libpng16.so.16` is loaded at run time, and the element declares the few
  simplified-API types it uses itself, so `libpng-dev` isn't required (only the runtime `libpng16-16`).

| Property | Default | |
|---|---|---|
| `n-workers` | 4 | frames decoded in parallel (1-4) |

## Install

```sh
sudo apt install build-essential pkg-config libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
git clone https://github.com/DrEVILish/raspberry-pi-video-codec-performance.git
cd raspberry-pi-video-codec-performance/png/parallel-png-video-decoder
make            # builds libgstcutepipngvdec.so here
make check      # gst-inspect-1.0 cutepipngvdec: shows rank, pads, properties
make install    # for your user: ~/.local/share/gstreamer-1.0/plugins
# or, for every user and service on the machine:
sudo make install-system
```

Try it without installing: `GST_PLUGIN_PATH=$PWD gst-launch-1.0 ...` from this folder.
Remove: `make uninstall` (or `sudo make uninstall-system`). Turn it off without removing it:
`export GST_PLUGIN_FEATURE_RANK=cutepipngvdec:0`.

## Use and verify

```sh
ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=60 -t 10 -c:v png -pix_fmt rgba test_png.mov
time gst-launch-1.0 filesrc location=test_png.mov ! qtdemux ! pngdec ! fakesink
time gst-launch-1.0 filesrc location=test_png.mov ! qtdemux ! cutepipngvdec ! fakesink
gst-launch-1.0 playbin uri=file://$PWD/test_png.mov     # autoplugged
```

**Displaying it:** PNG video is RGBA, 8 MB a 1080p frame. On a Pi 4, show it on a display plane with per-pixel
alpha (`kmssink`, ARGB8888 planes) rather than a GL compositor (see [`../hardware/`](../hardware/README.md#5-the-v3d-gpu-budget-video-belongs-on-display-planes)).
