# WMV / WMA / ASF on Raspberry Pi OS: play .wmv and .asf files without changing your other decoders

- **The decoders are there:** FFmpeg's WMV1/WMV2/WMV3(VC-1) and WMA decoders come with `gstreamer1.0-libav`.
- **The container isn't:** GStreamer needs `asfdemux` to open `.wmv`/`.wma`/`.asf`, and that's in
  **`gstreamer1.0-plugins-ugly`**.

```sh
sudo apt install gstreamer1.0-plugins-ugly
```

**But pin two decoders it brings,** or they take over formats you already play with FFmpeg's decoders:

| Element (plugins-ugly) | Rank | Effect |
|---|---|---|
| `a52dec` (liba52) | secondary (128) | **outranks `avdec_ac3` (marginal, 64)**: every AC-3 track switches decoder |
| `mpeg2dec` (libmpeg2) | secondary (128) | below `avdec_mpeg2video` (primary), but pin it to be sure |

```sh
export GST_PLUGIN_FEATURE_RANK=a52dec:0,mpeg2dec:0          # or the same in a systemd Environment= line
```

It also adds:
- `rmdemux`/`rademux` (RealMedia).
- `dvdlpcmdec`: the only DVD LPCM decoder, so LPCM audio in `.vob` starts decoding.
- `dvdsubdec` at rank none, never autoplugged.

**Measured:** WMV1 in `.asf` and WMV2 in `.wmv`, 1080p60 with WMA audio, play at 60 fps. WMV3/VC-1 wasn't tested
(no encoder to make a test file).
