# GoPro CineForm on Raspberry Pi 4 / ARM64: the CineForm SDK ported to AArch64 (NEON), and a GStreamer decoder

CineForm (`cfhd`) is GoPro's wavelet intermediate codec, used in editing (MOV/AVI, 4:2:2 10-bit, RGB(A) 12-bit).
FFmpeg's `cfhd` decoder is slow (19-23 fps at 1080p on a Pi 4). The official [GoPro CineForm
SDK](https://github.com/gopro/cineform-sdk) (Apache-2.0 / MIT) is much faster, but it's hand-optimised with x86 SSE2 and
MMX intrinsics and doesn't build for ARM. This folder has:

| Folder | What |
|---|---|
| [`cineform-sdk-aarch64/`](cineform-sdk-aarch64/) | ★ the port: NEON stand-ins for the x86 intrinsics headers, a native Makefile, two decoder fixes, `build.sh` |
| [`gstreamer-element/`](gstreamer-element/) | `cutepicfhddec`: a GStreamer decoder that runs several single-threaded SDK decoders on alternate frames |

**Results** (1080p60 CineForm, 194 Mbit/s, to YUY2, Raspberry Pi 4):

| Decoder | fps |
|---|---|
| FFmpeg `cfhd` (`avdec_cfhd`) | 19-23 |
| CineForm SDK, one core | 48 |
| CineForm SDK, its own 4 threads | 87 (but ±1 run-to-run differences, also on x86) |
| `cutepicfhddec`, 2 / 3 / 4 decoders | **82 / 92 / 98** |

**Correctness:** single-threaded output is **bit-identical** to the x86 SSE2 build for every frame compared.

## The port (★ hardware: SSE2/MMX → NEON)

- **Headers:** `compat/emmintrin.h`, `xmmintrin.h`, `mmintrin.h` and `mm_malloc.h` stand in for the x86 headers.
  They're put ahead of the system ones (`-I compat`) and pull in:
  - [sse2neon](https://github.com/DLTcollab/sse2neon) (MIT): SSE/SSE2 on NEON.
  - `compat/mmx2neon.h` (new, in this repo): the ~50 64-bit MMX intrinsics the SDK uses that sse2neon lacks
    (`_mm_*_pi16`, `_si64`, ...), on 64-bit NEON vectors with Intel's saturation and shift-count semantics.
- **Makefile:** a native build without cmake, giving one `libcutepi-cfhd.so` (decoder + encoder). Flags:
  `-fsigned-char` (x86's `char`), `-fpermissive`, `-flax-vector-conversions`. On x86-64 it builds the SDK's own SSE2
  path.
- **Fix, bounds** (`cutepi.patch`, `Codec/decoder.c`): the threaded entropy decoder scanned for a band-end marker
  with no bound. A sample without one (FFmpeg's CineForm *encoder* writes such samples) ran it off the end of the
  buffer. It now stops at the end of the sample and fails it.
- **Fix, deterministic dither** (`cutepi_rand.c` + `-Drand=cutepi_rand`, and a reset in the patch): the SDK dithers
  10-to-8-bit output with libc `rand()`, whose global state made output vary between runs and threads. Each sample
  now dithers from the same generator state.

### Build the library

```sh
sudo apt install git build-essential uuid-dev
git clone https://github.com/DrEVILish/raspberry-pi-video-codec-performance.git
cd raspberry-pi-video-codec-performance/cineform/cineform-sdk-aarch64
./build.sh                  # clones gopro/cineform-sdk at the pinned commit, adds the port, builds (~2 min on a Pi 4)
sudo ./build.sh install     # -> /usr/local/lib/cutepi/libcutepi-cfhd.so
```

`build.sh` step by step, if you'd rather do it by hand:

```sh
git clone https://github.com/gopro/cineform-sdk.git && cd cineform-sdk
git checkout 11574d0295771edccadd17af14af74a539f924c7     # the commit the patch is made against
cp -r ../compat .  &&  cp ../Makefile .  &&  cp ../cutepi_rand.c Codec/
git apply ../cutepi.patch
make -j4 && sudo make install                              # PREFIX=/usr/local by default
```

The library exports the SDK's own C API (`CFHD_OpenDecoder`, `CFHD_DecodeSample`, ...), so other programs can link
it too. [`cineform-sdk-aarch64/cfdec.cpp`](cineform-sdk-aarch64/cfdec.cpp) is a small example and benchmark: it
decodes a CineForm MOV to raw YUY2 and times it.

## The GStreamer element

`cutepicfhddec` decodes `video/x-cineform`:
- **Decoders:** it runs `n-workers` single-threaded SDK decoders (default 3) on alternate frames, each in its own
  copy of the library (`dlmopen`), and puts the frames back in order.
- **Why not the SDK's threads:** its threaded path differs by ±1 between runs and doesn't recover from a rejected
  sample.
- **Output:** YUY2, or BGRA for RGBA CineForm.
- **FFmpeg fallback:** CineForm written by FFmpeg's encoder is partly rejected by the SDK. From the first rejected
  sample, the element decodes the rest of the stream with FFmpeg's decoder (an internal pipeline), so no frame is
  lost.
- **Rank** `primary + 1`.

```sh
sudo apt install build-essential pkg-config libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev gstreamer1.0-libav
cd cineform/gstreamer-element
make CINEFORM_SDK=../cineform-sdk-aarch64/cineform-sdk     # needs the SDK clone from build.sh (for its headers)
make check && make install                                 # or: sudo make install-system
gst-launch-1.0 playbin uri=file:///path/to/cineform.mov
```

**Where the library is:** the element loads `/usr/local/lib/cutepi/libcutepi-cfhd.so` by default. Point it elsewhere
with `CUTEPI_CFHD_LIB=/path/to/libcutepi-cfhd.so`. If the library is missing, the element doesn't register and
`avdec_cfhd` is used.

**Verified** on the test Pi with a fresh clone (2026-10-20):
- `build.sh` builds the library from upstream plus this port.
- The element decodes all 480 frames of a 1080p60 SDK-encoded CineForm 4:2:2 MOV.

| Property | Default | |
|---|---|---|
| `n-workers` | 3 | single-threaded decoders on alternate frames (1-4) |
