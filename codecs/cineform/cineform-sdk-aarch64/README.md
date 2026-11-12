# CineForm SDK on AArch64: port files

`build.sh` clones GoPro's SDK at the pinned commit, adds these files and applies `cutepi.patch`. Full description and
instructions: [`../README.md`](../README.md).

| File | What |
|---|---|
| `build.sh` | clone + port + build (`./build.sh`, `sudo ./build.sh install`) |
| `compat/` | x86 intrinsics headers mapped to NEON: `sse2neon.h` (MIT, upstream DLTcollab/sse2neon) and `mmx2neon.h` (new) |
| `Makefile` | native build of `libcutepi-cfhd.so` without cmake |
| `cutepi.patch` | `Codec/decoder.c`: bounds check in the band-end scan; dither generator reset per sample |
| `cutepi_rand.c` | the per-thread dither generator (`-Drand=cutepi_rand`) |
| `cfdec.cpp` | example/benchmark: decode a CineForm MOV to raw YUY2 and time it |
| `LICENSE-APACHE`, `LICENSE-MIT` | the CineForm SDK's licences |
