# HAP, DNxHR and QuickTime Animation in MKV (Matroska) with GStreamer: cutepiqtmkvparse

**Problem:**
- **The codec details are inside the track.** Matroska stores QuickTime-only codecs (HAP, HAP Alpha/Q, DNxHR/DNxHD,
  QuickTime Animation `qtrle`) as `V_QUICKTIME` tracks, whose `CodecPrivate` is the QuickTime sample description:
  fourcc, size, depth.
- **GStreamer drops them.** `matroskademux` doesn't map those tracks: its pad says only
  `video/x-unknown, codec-id=V_QUICKTIME`, with no fourcc, size or frame rate. Nothing can be plugged after it, so
  `playbin` fails with "no decoder for video/x-unknown".
- **FFmpeg reads the same files**, and the MOV versions play.

**Fix:**
- `cutepiqtmkvparse` is a parser autoplugged on exactly those pads.
- It takes the track number from the stream-id (`matroskademux` ends it with `/<track number>`).
- It asks upstream for the file's URI and reads that track's header with a minimal EBML reader: Tracks only, a few
  KB, stopping at the first Cluster.
- It then sets the caps `qtdemux` gives the same stream in a MOV:

| Fourcc | Caps | Decoder then plugged |
|---|---|---|
| `Hap1`, `Hap5`, `HapY`, `HapM`, `HapA`, ... | `video/x-hap, variant=<fourcc>` | `avdec_hap` |
| `AVdh`, `AVdn` | `video/x-dnxhd` | `avdec_dnxhd` |
| `rle ` | `video/x-rle, layout=quicktime, depth=<depth>` | `avdec_qtrle` |

- **Extra fields:** width and height come from `PixelWidth`/`PixelHeight`, the frame rate from `DefaultDuration`.
- **Frames pass through untouched.** Frames decoded from MKV are **byte-identical** to the same streams in MOV
  (tested with HAP, HAP Alpha, DNxHR LB, QuickTime Animation).
- **Unmapped fourccs**, or a QuickTime Animation track without a depth, give a clear element error instead of a
  wrong picture.

**Known limit:** QuickTime Animation MKVs **written by FFmpeg** can't be decoded by anyone. FFmpeg's Matroska muxer
zeroes the sample description after the fourcc (depth, width and height 0). See
[`../qtrle/`](../qtrle/) and [`../docs/upstream/`](../docs/upstream/).

## Install

```sh
sudo apt install build-essential pkg-config libgstreamer1.0-dev gstreamer1.0-libav
git clone https://github.com/DrEVILish/raspberry-pi-video-codec-performance.git
cd raspberry-pi-video-codec-performance/hap/quicktime-in-matroska-parser
make && make check && make install          # or: sudo make install-system
```

## Use and verify

```sh
ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=60 -t 5 -c:v hap hap.mkv
gst-launch-1.0 playbin uri=file://$PWD/hap.mkv                 # plays (was: no decoder for video/x-unknown)
GST_DEBUG=cutepiqtmkvparse:4 gst-launch-1.0 filesrc location=hap.mkv ! matroskademux ! cutepiqtmkvparse ! fakesink
#   INFO ... track 1 of .../hap.mkv: video/x-hap, variant=(string)Hap1, width=(int)1920, height=(int)1080, framerate=(fraction)60/1
```

The parser needs a `file://` source (it reads the track header from the file): `filesrc`, or `playbin`/`uridecodebin`
with a file URI. Over HTTP it reports an error.

## HAP performance on the Pi 4

- **Decoding:** `avdec_hap` decompresses Snappy and then the DXT/BCn texture on the CPU, frame-threaded. 1080p HAP
  decodes at 73 fps flat out; HAP Alpha (DXT5) is heavier.
- **The bandwidth cost:** the decoder outputs 8 MB of RGBA a 1080p frame, so moving it to the display is a large part
  of the cost.
- **Intra-only:** HAP is intra-only, so turn QoS off for it ([`../docs/intra-only-qos.md`](../docs/intra-only-qos.md)).
