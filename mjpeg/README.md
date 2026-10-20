# Motion JPEG (MJPEG) decoding on several cores: cutepimjpegdec for GStreamer, 2.5× faster on a Raspberry Pi 4

Every Motion JPEG frame is a complete JPEG, but GStreamer's `jpegdec` (libjpeg-turbo) decodes one frame at a time.
`cutepimjpegdec` hands frames round-robin to several `jpegdec` instances (default 3, up to 4), each in a small
internal pipeline, and puts the results back in order. The decoded buffers go downstream as they are, with no copy.
The output is **byte-identical** to `jpegdec`.

| 1080p, Raspberry Pi 4 (flat out) | `jpegdec` | `cutepimjpegdec` |
|---|---|---|
| 4:2:0 | 115 fps (8.1 ms a frame) | **294 fps** |
| 4:2:2, high quality | 92 fps (10.3 ms) | **241 fps** |

- **Cameras and capture cards:** USB webcams and HDMI capture sticks deliver MJPEG. At 1080p60 a single `jpegdec`
  sits close to its limit, so a second stream can't play.
- **Moving pictures only:** the sink caps need a non-zero framerate, so JPEG stills keep `jpegdec`.
- **Rank** `primary + 1`: `decodebin`/`playbin` pick it automatically.

| Property | Default | |
|---|---|---|
| `n-workers` | 3 | `jpegdec` instances decoding in parallel (1-4). 3 leaves a core for the rest of the pipeline |

**Hardware note:** the Pi 4's hardware JPEG decoder (`v4l2jpegdec`, `/dev/video10` via the firmware) is unreliable
here: buffer-pool activation fails or stalls silently, so streams intermittently never preroll. Rank it out with
`GST_PLUGIN_FEATURE_RANK=v4l2jpegdec:0`, so that software (this element or `jpegdec`) is used.

## Install

```sh
sudo apt install build-essential pkg-config libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
git clone https://github.com/DrEVILish/raspberry-pi-video-codec-performance.git
cd raspberry-pi-video-codec-performance/mjpeg/parallel-mjpeg-decoder
make            # builds libgstcutepimjpegdec.so here
make check      # gst-inspect-1.0 cutepimjpegdec: shows rank, pads, properties
make install    # for your user: ~/.local/share/gstreamer-1.0/plugins
# or, for every user and service on the machine:
sudo make install-system
```

Try it without installing: `GST_PLUGIN_PATH=$PWD gst-launch-1.0 ...` from this folder.
Remove: `make uninstall` (or `sudo make uninstall-system`). Turn it off without removing it:
`export GST_PLUGIN_FEATURE_RANK=cutepimjpegdec:0`.

## Use

```sh
# A file: autoplugged
gst-launch-1.0 playbin uri=file:///path/to/mjpeg.mov
# A USB camera, 1080p MJPEG:
gst-launch-1.0 v4l2src device=/dev/video0 ! image/jpeg,width=1920,height=1080,framerate=30/1 ! jpegparse ! \
    cutepimjpegdec ! videoconvert ! autovideosink
```

## Verify

```sh
ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=60 -t 10 -c:v mjpeg -q:v 3 -pix_fmt yuvj420p test.mov
time gst-launch-1.0 filesrc location=test.mov ! qtdemux ! jpegdec ! fakesink          # one core
time gst-launch-1.0 filesrc location=test.mov ! qtdemux ! cutepimjpegdec ! fakesink   # several
```
