#!/bin/sh
# build.sh: the GoPro CineForm SDK on 64-bit ARM (Raspberry Pi 4/5, any AArch64 Linux) as one shared library.
#   ./build.sh                     clone upstream, add the AArch64 port, apply the patch, build libcutepi-cfhd.so
#   sudo ./build.sh install        ... and install it to /usr/local/lib/cutepi/libcutepi-cfhd.so
# Needs: git, a C and C++ compiler, uuid-dev (sudo apt install git build-essential uuid-dev). About 2 minutes on a
# Raspberry Pi 4. Also builds on x86-64 (the SDK's own SSE2 code; the port files are not used there).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SDK="$HERE/cineform-sdk"
COMMIT=11574d0295771edccadd17af14af74a539f924c7   # the upstream commit the patch was made against
if [ ! -d "$SDK/.git" ]; then
  git clone https://github.com/gopro/cineform-sdk.git "$SDK"
fi
cd "$SDK"
git checkout -q "$COMMIT"
git reset -q --hard "$COMMIT"
cp -r "$HERE/compat" .                      # x86 intrinsics headers -> NEON (sse2neon + mmx2neon.h)
cp "$HERE/Makefile" .                       # native build, no cmake
cp "$HERE/cutepi_rand.c" Codec/             # per-sample dither generator (deterministic output)
git apply "$HERE/cutepi.patch"              # bounds check + dither reset in Codec/decoder.c
make -j"$(nproc)"
if [ "${1:-}" = install ]; then make install; fi
echo "built $SDK/libcutepi-cfhd.so"
