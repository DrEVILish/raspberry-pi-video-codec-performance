# FFmpeg: Matroska muxer writes an empty QuickTime sample description for V_QUICKTIME (qtrle unreadable)

Ready to file at https://trac.ffmpeg.org/newticket (component: avformat / matroskaenc).

**Summary:** For a `V_QUICKTIME` track (QuickTime Animation, HAP) FFmpeg's Matroska muxer writes a 90-byte
CodecPrivate whose sample description is all zero after the fourcc: depth, width, height and compressor name
are 0. FFmpeg's own demuxer then gives the qtrle decoder `bits_per_coded_sample = 0` and every frame fails with
"Unsupported colorspace: 0 bits/sample?". The same stream written to MOV has depth 24 (or 32 for ARGB) in its
`stsd`. HAP survives only because its decoder does not need the depth.

**Version:** ffmpeg 7.1.5 (Debian trixie, aarch64; please check master).

**Reproduce:**
```
ffmpeg -f lavfi -i testsrc2=size=64x48:rate=25 -frames:v 10 -c:v qtrle q.mkv
ffmpeg -v error -i q.mkv -f null -          # Unsupported colorspace: 0 bits/sample? (every frame)
ffmpeg -f lavfi -i testsrc2=size=64x48:rate=25 -frames:v 10 -c:v qtrle q.mov
ffmpeg -v error -i q.mov -f null -          # decodes
```
CodecPrivate of q.mkv: `00 00 00 5A 'r' 'l' 'e' ' '` followed by 82 zero bytes (the depth field, 78 bytes after
the fourcc, is 0; in q.mov's `stsd` it is 24, width 64, height 48).

**Expected:** the muxer writes the full sample description (as the mov muxer does), so the file is readable by
FFmpeg and other demuxers. Patching the depth to 24 in the file makes it decode (verified with GStreamer).
