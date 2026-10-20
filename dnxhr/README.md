# Avid DNxHR / DNxHD on the Raspberry Pi 4: MKV support, QoS fix, 4:2:2 packing

FFmpeg's `avdec_dnxhd` decodes DNxHR LB/SQ/HQ/HQX/444 and DNxHD in software. DNxHR LB and HQ played at 1080p60 on
KMS display planes in our tests.

1. **DNxHR in MKV:** `matroskademux` doesn't map DNxHR's `V_QUICKTIME` track (fourcc `AVdh`). The parser in
   [`../hap/`](../hap/) (`cutepiqtmkvparse`) fixes it, and the frames are byte-identical to the MOV.
2. **Turn QoS off on the decoder:** DNxHR is intra-only. With QoS, a DNxHR HQ clip fell to 1-2 shown frames a second
   with lag growing to 4 s. See [`../docs/intra-only-qos.md`](../docs/intra-only-qos.md).
3. **10-bit/8-bit 4:2:2 to I420 for display planes:** [`../common/yuv-pack-neon/`](../common/yuv-pack-neon/). DNxHR
   went from 20-27 to 60 fps on KMS planes.

```sh
gst-launch-1.0 filesrc location=dnxhr.mov ! qtdemux ! avdec_dnxhd qos=false ! cutepiyuvpack ! videoconvert ! kmssink
```
