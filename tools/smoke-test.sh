#!/bin/sh
# smoke-test.sh: build every plugin in this repository and check that each one loads and does its job.
# Makes small test files with ffmpeg in a temporary folder, runs each element through gst-launch-1.0 with the
# freshly built plugins only (GST_PLUGIN_PATH), and counts the frames that come out.
# Needs: build tools (see tools/plugin.mk), ffmpeg, gst-launch-1.0, gst-inspect-1.0, and for the decoders the
# libraries they load at run time (libdav1d, libpng16, libwebp, libavif, libjxl; any that are missing are skipped).
# Usage: tools/smoke-test.sh            (from the repository root)
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
PLUGINS="common/bob-deinterlacer-neon common/yuv-pack-neon common/system-dmabuf-upload mjpeg/parallel-mjpeg-decoder
png/parallel-png-video-decoder animated-images/frame-parallel-animated-decoder av1/dav1d-decoder
hap/quicktime-in-matroska-parser"
GP=""
for d in $PLUGINS; do
  make -s -C "$ROOT/$d" >/dev/null || { echo "BUILD FAILED: $d"; exit 1; }
  GP="$GP:$ROOT/$d"
done
export GST_PLUGIN_PATH="${GP#:}" GST_REGISTRY="$TMP/registry.bin"
pass=0; fail=0
# frames DESC WANT: run a pipeline ending in "fakesink name=out" and compare the buffers it got with WANT.
frames() {
  n=$(gst-launch-1.0 $1 ! fakesink name=out silent=false -v 2>/dev/null | grep -c "out.*chain")
  if [ "$n" -eq "$2" ]; then echo "ok    $3 ($n frames)"; pass=$((pass+1)); else echo "FAIL  $3 ($n frames, want $2)"; fail=$((fail+1)); fi
}
used() { # used URI ELEMENT LABEL: the element was autoplugged for the file
  if GST_DEBUG=GST_ELEMENT_FACTORY:4 gst-launch-1.0 -q playbin uri="file://$1" video-sink=fakesink audio-sink=fakesink 2>&1 | grep -q "creating element \"$2\""; then
    echo "ok    $3 (autoplugged $2)"; pass=$((pass+1)); else echo "FAIL  $3 ($2 not autoplugged)"; fail=$((fail+1)); fi
}
ff() { ffmpeg -v error -y -f lavfi -i testsrc2=size=320x240:rate=25 -frames:v 10 "$@" </dev/null; }

frames "videotestsrc num-buffers=10 ! video/x-raw,format=I420,width=320,height=240,framerate=25/1,interlace-mode=interleaved ! cutepibob" 20 "cutepibob: 10 interlaced frames -> 20 pictures"
frames "videotestsrc num-buffers=10 ! video/x-raw,format=I420_10LE,width=320,height=240 ! cutepiyuvpack ! video/x-raw,format=I420" 10 "cutepiyuvpack: 10-bit to I420"
if [ -e /dev/dma_heap/system ]; then
  frames "videotestsrc num-buffers=10 ! video/x-raw,format=I420,width=320,height=240 ! cutepisysdmabuf" 10 "cutepisysdmabuf: I420 into system dma-heap buffers"
else echo "skip  cutepisysdmabuf (no /dev/dma_heap/system)"; fi
ff -c:v mjpeg -q:v 3 -pix_fmt yuvj420p "$TMP/m.mov" && used "$TMP/m.mov" cutepimjpegdec "Motion JPEG in MOV"
ff -c:v png -pix_fmt rgba "$TMP/p.mov" && used "$TMP/p.mov" cutepipngvdec "PNG video in MOV"
ff -f gif "$TMP/a.gif" && used "$TMP/a.gif" cutepianimdec "animated GIF"
ff -c:v libwebp -loop 0 "$TMP/a.webp" 2>/dev/null && used "$TMP/a.webp" cutepianimdec "animated WebP"
if ff -c:v libsvtav1 -preset 12 "$TMP/v.mkv" 2>/dev/null || ff -c:v libaom-av1 -cpu-used 8 "$TMP/v.mkv" 2>/dev/null; then
  used "$TMP/v.mkv" cutepidav1ddec "AV1 in MKV"; else echo "skip  AV1 (no AV1 encoder in this ffmpeg)"; fi
ff -c:v hap "$TMP/h.mkv" && used "$TMP/h.mkv" cutepiqtmkvparse "HAP in MKV"
ff -c:v dnxhd -profile:v dnxhr_lb -pix_fmt yuv422p "$TMP/d.mkv" && used "$TMP/d.mkv" cutepiqtmkvparse "DNxHR in MKV"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
