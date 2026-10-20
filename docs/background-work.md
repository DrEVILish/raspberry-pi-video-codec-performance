# Background work next to video playback: thumbnails, imports and transcodes that don't make playback stutter

On a 4-core board like the Pi 4, a media app's background jobs compete with the decoders. These jobs are things like
ffprobe at import, a playability check, thumbnail grabs, waveform analysis and transcodes. A thumbnail grab with
default settings took all four cores for a second or more, and video already playing fell to 12-45 fps for
seconds. Three measured changes:

## 1. Run tools at the lowest priority

```sh
nice -n 19 ionice -c 2 -n 7 -- ffmpeg ...
```

- **Lowest CPU priority, and the lowest *best-effort* I/O level.** Don't use the idle I/O class (`-c 3`): a probe
  that the user waits for can then stall behind playback's reads.
- **`nice` and `ionice` exec the tool in place:** the PID is the tool's, so a timeout's kill reaches it, and every
  thread it starts inherits the priority.

**Priority isn't enough on its own.** A niced ffmpeg still runs flat out on any idle core, and shares the memory
bus and the 1 MB L2 cache with the decoders. Also limit its threads while something plays:

```sh
nice -n 19 ionice -c 2 -n 7 -- ffmpeg -threads 1 -filter_threads 1 ...
```

## 2. Thumbnails from the nearest keyframe, decoded alone

```sh
ffmpeg -threads 1 -skip_frame nokey -noaccurate_seek -ss 5 -i in.mkv -frames:v 1 -vf scale=640:-2 thumb.jpg
```

An exact seek (`-ss` before `-i` with the default accurate seek) decodes every frame from the previous keyframe up to
the target. `-skip_frame nokey -noaccurate_seek` decodes just the keyframe at or before it.

| Codec (1080p60 test file) | CPU per thumbnail, exact seek | keyframe only |
|---|---|---|
| AV1 | 6.2 s | **0.4 s** |
| H.264 | 6.4 s | **0.4 s** |
| HEVC, MPEG-2 | - | 0.4 s |
| ProRes 4444 | 1.2 s | 0.6 s |
| VP9 | 3.1 s | 2.9 s (its decoder ignores `skip_frame`) |

- **Use `-threads 1`.** FFmpeg's Theora decoder rejects `skip_frame` with frame threads ("Operation not
  permitted"), and a single keyframe gains nothing from threads.
- **Keep the exact seek as a fallback** for any decoder that gives nothing.

## 3. Keep GPU/browser work away from playback start

Starting a heavy GL process (for example a WebKit page preload) just as a video starts made the video stall at its
start. Background jobs like that should:
- start only after playback has been quiet for a while (30 s here);
- be killed the moment a cue starts to *build*, not when it's on screen. Use a process group
  (`setpgid`, `kill(-pgid, SIGKILL)`), so the browser's helper processes die with it;
- back off when they keep failing (1, 2, 4 ... minutes, up to an hour), rather than retrying every minute.

Measured: the WebKit child and its four helper processes were gone 91 ms after play was pressed.
