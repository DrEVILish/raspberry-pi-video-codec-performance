# Raspberry Pi 4 config.txt for fast hardware video: h264_freq_min, isp_freq_min, core_freq_min

**What it does:** stops the Pi 4 firmware from running the hardware H.264 decoder/encoder, the ISP and the VPU core
at half clock. **Gain:** 1080p H.264 hardware decode **54 → 90 fps**, hardware encode **41.7 → 74.4 fps**, at stock
clocks with no overvoltage. Optional overclock: 98 fps decode, 93 fps encode. Background and measurements:
[`../README.md`](../README.md#1-codec-clocks-the-half-speed-h264-decoder--biggest-single-win).

| File | What it holds |
|---|---|
| [`codec-clocks.txt`](codec-clocks.txt) | `h264_freq_min=500`, `isp_freq_min=500`, `core_freq_min=500`: the stock maximum clocks as a floor |
| [`overclock.txt`](overclock.txt) | optional: H.264, ISP, HEVC, core and V3D at 650 MHz, ARM 2.0 GHz, `over_voltage_delta=75000` |
| [`install-firmware-config.sh`](install-firmware-config.sh) | adds either set to `config.txt` between markers; `--remove` takes it out |

Raspberry Pi 4, Pi 400 and Compute Module 4 only. The Pi 5 has no H.264 block and different clock names.

---

## Step by step

### 1. Measure first (so you can see the difference)

```sh
# Any 1080p H.264 file; this makes a 20 s test clip if you have none:
ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=60 -t 20 -c:v libx264 -preset veryfast -b:v 8M test1080.mp4

# Decode as fast as the hardware can, no display. Note the "Execution ended after" time:
gst-launch-1.0 filesrc location=test1080.mp4 ! qtdemux ! h264parse ! v4l2h264dec ! fakevideosink sync=false
# 1200 frames / that time = fps. Default clocks: about 50-54 fps.

# The codec clocks: run this in a second terminal while the decode above runs (an idle block reads 0):
vcgencmd measure_clock h264; vcgencmd measure_clock isp; vcgencmd measure_clock core
```

`vcgencmd` reports the clock at the moment you ask. Run it during the decode, from a second terminal, to see the
H.264 clock stuck at 250 MHz.

### 2. Install the setting

```sh
git clone https://github.com/DrEVILish/raspberry-pi-video-codec-performance.git
cd raspberry-pi-video-codec-performance/hardware/firmware-clocks
sudo ./install-firmware-config.sh          # stock-clock floors only (recommended)
sudo reboot
```

Or edit `config.txt` by hand (`/boot/firmware/config.txt` on current Raspberry Pi OS, `/boot/config.txt` on older
images) and add at the end:

```ini
[all]
h264_freq_min=500
isp_freq_min=500
core_freq_min=500
```

### 3. Check it worked

```sh
vcgencmd measure_clock h264      # run it *during* a decode: frequency(28)=500000000 or more (it reads 0 when the block is idle)
gst-launch-1.0 filesrc location=test1080.mp4 ! qtdemux ! h264parse ! v4l2h264dec ! fakevideosink sync=false
# 1080p decode: about 90 fps
```

### 4. Optional: the overclock

```sh
sudo ./install-firmware-config.sh --overclock
sudo reboot
vcgencmd get_throttled           # must stay throttled=0x0 under load
```

- **Before making it permanent, try it once with `tryboot`.** Copy `config.txt` to `tryboot.txt`, add the lines
  there, then `sudo reboot '0 tryboot'`. A failed boot falls back to your normal `config.txt`.
- **Keep the board actively cooled.**
- **Not every Pi 4 runs at these clocks.** If it misbehaves (crashes, artefacts, `get_throttled` not 0), remove the
  overclock.

### 5. Undo

```sh
sudo ./install-firmware-config.sh --remove && sudo reboot
```

The script also keeps your previous file as `config.txt.codec-perf-bak`.

---

## Why a clock *floor* works and is safe

- **The settings don't overclock anything.** `h264_freq_min` sets the lowest clock the firmware may choose, here the
  same 500 MHz the block already runs at when the ARM is at full speed. The firmware simply stops dropping to
  250 MHz.
- **Cost:** a little idle power and heat. Measuring that was out of scope.
- **The proper fix is in the kernel:** the driver should request the clock while it streams, as `rpi-hevc-dec` does.
  Patches for that are in [`../../h264/bcm2835-codec-patches/`](../../h264/bcm2835-codec-patches/). They are not
  tested on hardware yet, so the `config.txt` route is the one to use today.
