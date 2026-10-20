# bcm2835-codec kernel patches: H.264 clock requests, start-order fix and cache hints (Raspberry Pi 4)

> **Status: compile-tested, NOT tested on hardware.**
> - All three patches apply cleanly to `raspberrypi/linux` branch `rpi-6.18.y` (checked 2026-10-20 at commit
>   `5d64d1e`).
> - The patched module builds against the running `6.18.50+rpt-rpi-v8` headers (vermagic matches).
> - The patched module has never been loaded, and a kernel with 0001 + 0002 has never been booted. The clock
>   behaviour, cache coherence with cached buffers, and the encoder's start with the new order are unverified.
> - If you only want the speed-up, use the `config.txt` route in
>   [`../../hardware/firmware-clocks/`](../../hardware/firmware-clocks/) instead. It's measured and safe.
>
> Please report results (board, kernel, what you tested) as an issue.

| Patch | Subsystem | What it changes |
|---|---|---|
| [`0001-clk-raspberrypi-export-the-H.264-clock.patch`](0001-clk-raspberrypi-export-the-H.264-clock.patch) | `drivers/clk/bcm/clk-raspberrypi.c` | registers the H.264 firmware clock (mailbox clock id 6), like the ISP and HEVC clocks, so drivers can request it |
| [`0002-ARM-dts-bcm2711-rpi-give-vchiq-the-codec-clocks.patch`](0002-ARM-dts-bcm2711-rpi-give-vchiq-the-codec-clocks.patch) | `bcm2711-rpi.dtsi` | `clocks = h264, isp, core` on the vchiq node, where `bcm2835-codec` finds them |
| [`0003-staging-bcm2835-codec-clocks-start-order-cache-hints.patch`](0003-staging-bcm2835-codec-clocks-start-order-cache-hints.patch) | `drivers/staging/vc04_services/bcm2835-codec` | (1) hold the role's clock and the core clock at max while streaming; (2) `ignore_cap_streaming` for DECODE only; (3) `allow_cache_hints` on both queues |

## What each fix is for

1. **Clocks (0001 + 0002 + 0003).**
   - **Problem:** the firmware keeps the H.264, ISP and core clocks at their minimum unless the ARM is at full
     speed, and the driver never asks for more, so 1080p H.264 decodes at 54 fps instead of 90-97
     ([`../../hardware/`](../../hardware/README.md#1-codec-clocks-the-half-speed-h264-decoder--biggest-single-win)).
   - **Fix:** with the patches the driver requests its role's clock (`h264` for decode/encode, `isp` for the ISP,
     deinterlacer and JPEG encoder) and the VPU core clock at their maximum while any context streams, and drops
     the request when the last one stops.
   - **Details:** the request is refcounted per device and made under `dev_mutex`. The clocks are optional: with
     no DT properties, or no maximum from the firmware, nothing changes.
2. **Start order (0003).**
   - **Problem:** `ignore_cap_streaming` was set for every role, so the ISP, deinterlacer and encoders send OUTPUT
     buffers to the firmware before the CAPTURE port is enabled. The firmware drops such a buffer after about
     0.5 s.
   - **Measured:** raw V4L2 on the ISP, STREAMON(OUTPUT) then STREAMON(CAPTURE), loses frame 0 and delivers the
     first frame 490-500 ms late. In the other order the first frame arrives after 3.8 ms.
   - **Fix:** only the stateful decoder needs to start without CAPTURE (to parse headers).
   - GStreamer starts both queues first and is unaffected; raw V4L2 and FFmpeg users can be.
3. **Cache hints (0003).**
   - **Problem:** without `allow_cache_hints`, MMAP buffers are always write-combined, and CPU reads of them run at
     about 510 MB/s (6.1 ms per 1080p YU12 frame). The same read from a cached buffer plus a sync takes 3.3 ms.
   - **Fix:** with the flag, userspace can ask for cached buffers (`V4L2_MEMORY_FLAG_NON_COHERENT`).

## How to apply

### Option A: rebuild only the `bcm2835-codec` module (patch 0003 alone)

This gives you the start-order fix and cache hints. Without 0001 and 0002 the clock request finds no clocks and does
nothing (that part is optional by design), so keep the `config.txt` clock floor for speed.

```sh
# 1. Kernel headers for the running kernel (Raspberry Pi OS)
sudo apt install build-essential bc flex bison libssl-dev linux-headers-rpi-v8   # Pi 4, 64-bit kernel
ls /lib/modules/$(uname -r)/build                                               # must exist

# 2. Only the driver sources from the Raspberry Pi kernel tree for your branch (6.18 here)
git clone --depth 1 --filter=blob:none --sparse -b rpi-6.18.y https://github.com/raspberrypi/linux.git rpi-linux
cd rpi-linux
git sparse-checkout set drivers/staging/vc04_services

# 3. Apply the patch and build the module against the installed headers
git apply /path/to/raspberry-pi-video-codec-performance/h264/bcm2835-codec-patches/0003-staging-bcm2835-codec-clocks-start-order-cache-hints.patch
make -C /lib/modules/$(uname -r)/build M=$PWD/drivers/staging/vc04_services/bcm2835-codec \
     CONFIG_VIDEO_CODEC_BCM2835=m modules
modinfo drivers/staging/vc04_services/bcm2835-codec/bcm2835-codec.ko | grep vermagic   # must match uname -r
```

4. Load it. **Stop everything that uses the codecs first**: media players, a desktop compositor using `/dev/video*`,
   `camera`/`libcamera` apps.

```sh
sudo rmmod bcm2835_codec                       # fails while anything has /dev/video10-12,18,31 open
sudo insmod drivers/staging/vc04_services/bcm2835-codec/bcm2835-codec.ko
dmesg | tail                                   # look for bcm2835-codec probe messages
```

5. To keep it across reboots, copy it over the distribution's module and refresh dependencies. Keep a backup;
   a kernel update replaces it.

```sh
M=/lib/modules/$(uname -r)/kernel/drivers/staging/vc04_services/bcm2835-codec
sudo cp $M/bcm2835-codec.ko.xz $M/bcm2835-codec.ko.xz.orig     # the name may be .ko or .ko.xz
sudo cp drivers/staging/vc04_services/bcm2835-codec/bcm2835-codec.ko $M/ && sudo rm -f $M/bcm2835-codec.ko.xz
sudo depmod -a
```

**Undo:** restore the `.orig` file, `sudo depmod -a`, reboot.

### Option B: the full kernel (all three patches, clock requests included)

Build a full Raspberry Pi kernel with the three patches applied, following the official guide
(https://www.raspberrypi.com/documentation/computers/linux_kernel.html, "Building the kernel locally" or "Cross-compiling"):

```sh
git clone --depth 1 -b rpi-6.18.y https://github.com/raspberrypi/linux.git && cd linux
git am /path/to/raspberry-pi-video-codec-performance/h264/bcm2835-codec-patches/*.patch
KERNEL=kernel8 make bcm2711_defconfig
make -j4 Image.gz modules dtbs
sudo make modules_install
sudo cp arch/arm64/boot/dts/broadcom/*.dtb /boot/firmware/
sudo cp arch/arm64/boot/Image.gz /boot/firmware/kernel8-codec.img   # a new name: your old kernel stays bootable
echo "kernel=kernel8-codec.img" | sudo tee -a /boot/firmware/config.txt
sudo reboot
```

Remove the `kernel=` line from `config.txt` to go back.

## How to check

| Fix | Test |
|---|---|
| Clocks (B only) | remove the `*_freq_min` lines from `config.txt`, keep the governor at `ondemand`, then run `vcgencmd measure_clock h264` during a hardware decode: 500 MHz or more, and 1080p decode at ~90 fps (it was 250 MHz and 54 fps) |
| Start order | ISP via raw V4L2: STREAMON(OUTPUT) before STREAMON(CAPTURE). The first frame should come back in a few ms, not ~0.5 s |
| Cache hints | `VIDIOC_REQBUFS` with `V4L2_MEMORY_FLAG_NON_COHERENT` in `flags`: accepted, and CPU reads of the buffers run at cached speed (with `VIDIOC_PREPARE_BUF`/sync as V4L2 requires) |

## Other findings, not patched

- **One lock per role.** `dev_mutex` is the vb2 queue lock for every open instance of a role, so instances
  serialise their ioctls. This is not measurable as a limit: two decoders already saturate the engine.
- **Header pool size.** The firmware buffer header pool is sized `VB2_MAX_FRAME + 1` per port, whatever the
  request. This only costs firmware memory.
- **Cosmetic.** There are dead NULL checks on `container_of` results in `ip_buffer_cb`/`op_buffer_cb`, and
  unlocked debug counters.

Licence: GPL-2.0 ([`LICENSE`](LICENSE)), as the kernel.
