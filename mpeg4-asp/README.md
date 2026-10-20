# MPEG-4 Part 2 (ASP: DivX, Xvid) on the Raspberry Pi 4

Measured, nothing to change:
- FFmpeg's `avdec_mpeg4` (frame-threaded) plays 1080p60 MPEG-4 ASP (41 Mbit/s) and Xvid in AVI with B-frames, at
  about 75% of one core across its threads.

Not tested:
- **DivX "packed bitstream" AVIs** (old DivX 5 with B-frames packed into one AVI chunk). If one stutters, unpack it
  losslessly: `ffmpeg -i in.avi -c copy -bsf:v mpeg4_unpack_bframes out.avi`.
