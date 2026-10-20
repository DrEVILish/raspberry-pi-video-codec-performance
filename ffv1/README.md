# FFV1 on the Raspberry Pi 4: what limits it

- **1080p60 FFV1 4:2:0** plays: `avdec_ffv1` (4 frame threads at ~30% of a core each).
- **FFV1 with alpha (`yuva420p`, A420)** is decoder-bound:
  - **Decode:** 73.5 fps flat out standalone, 29 ms of CPU a frame over 4 threads.
  - **With `videoconvert` A420 → RGBA:** 57.6 fps, already under 60.
  - **Beside other work:** the decoder needs about 1.8 cores of pure decode for 60 fps, so it doesn't reach 60 fps
    while anything else runs.
- **What helps a bit:** take A420 without converting it on the CPU (a GPU path via `glcolorconvert`, which accepts
  A420), and turn QoS off (FFV1 is intra-only, [`../docs/intra-only-qos.md`](../docs/intra-only-qos.md)).
