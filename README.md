# Hexfin

Hexfin is a native [Jellyfin](https://jellyfin.org) client for the Sony PSP, written in C with PSPSDK. The PSP-1000 is the primary target. The Jellyfin server does the expensive work of transcoding; the PSP streams the result and decodes it on its hardware Media Engine.

> **Status: early (0.2.0).** It signs in with Quick Connect, lists your libraries and Continue Watching, and plays what you pick, with pause, stop, seeking and resuming. Your progress is reported to the server, so Continue Watching and "watched" stay up to date across your other Jellyfin apps.

The interface is text only, on purpose: cover images would compete with the video decoder for the PSP's memory.

## What works on hardware

Tested on a PSP-1000 running 6.61 with ARK custom firmware, against Jellyfin 12.1:

| Piece | Status |
|---|---|
| Wi-Fi (the first connection saved on the PSP) and HTTP streaming | Works |
| Fragmented-MP4 parsing (the format Jellyfin transcodes to) | Works |
| AAC audio decoding and playback | Works |
| H.264 video decoding on the Media Engine, including a server that encodes with VAAPI | Works |
| Audio and video in sync, in real time | Works |
| Quick Connect sign-in, kept between runs | Works |
| Playing an item from a Jellyfin library | Works |
| A whole film or episode from start to finish | Works |
| Library lists, pause and stop, seeking, settings, About screen | Works |
| Keeping the screen on; clock and battery display | Works |
| Volume display | Works |
| Resuming, reporting progress to the server | Built; tested in the emulator only |

## Requirements

**To run it:**
- A PSP running **6.61 with ARK** custom firmware. Video decoding needs a small kernel module (`hexfin_me.prx`), which ARK is able to load.
- A Wi-Fi connection saved in the PSP's own network settings. The app uses the first one. See [Connecting the PSP to Wi-Fi](#connecting-the-psp-to-wi-fi) below.
- A **Jellyfin server** (developed against 12.1) that:
  - has **Quick Connect** turned on (Dashboard > General);
  - can be reached over **plain HTTP**, which usually means its LAN address, such as `http://192.168.1.10:8096`. The PSP has no HTTPS;
  - is allowed to **transcode** for your user. The app asks for a stream the PSP can decode (H.264 Constrained Baseline, AAC-LC stereo at 44.1 kHz, at most 480x272, around 600 kbps), and the server makes it. Nothing needs setting up on the server for that.

### Connecting the PSP to Wi-Fi

The PSP's radio is from 2004: it speaks only 802.11b and older Wi-Fi security, and many current routers no longer offer either. The recommended way to connect it is a small travel router set up just for the PSP. The **GL.iNet Mango (GL-MT300N-V2)** wireless mini router is the one recommended here: connect it to your home network, give it a Wi-Fi network the PSP can join, and save that network in the PSP's settings. The router then carries the PSP's traffic to your Jellyfin server.

**To build it:**
- **Docker**, for the PSP toolchain. The image is built from [docker/Dockerfile](docker/Dockerfile) on top of `pspdev/pspdev`.
- **Windows with Visual Studio's C++ workload**, for the host tests only. They run under AddressSanitizer.

## Building

```sh
scripts/build.sh                 # the EBOOT: build/EBOOT.PBP
scripts/build.sh -C src/kbridge  # the kernel module: src/kbridge/hexfin_me.prx
scripts/test.sh                  # host tests: JSON, HTTP, MP4, H.264 and AAC parsing, sync, text
```

The server address built into the EBOOT is the developer's. Point it at yours either in `jellyfin.cfg` (see below) or at build time:

```sh
scripts/build.sh 'PROBE_DEFS=-DPROBE_JF_SERVER=\"http://192.168.1.10:8096\"'
```

### The probe

The same EBOOT can also be built as the **probe**: instead of the app, it plays one fixed source and reports on screen how far it got. It exists to tell a decoding problem apart from a network one. Switches go in `PROBE_DEFS`, for example `scripts/build.sh 'PROBE_DEFS=-DPROBE_MODE=1 -DPROBE_BUILD_NAME=\"A3\"'`:

| Switch | What it does |
|---|---|
| `PROBE_MODE` | `0` the app (default); `1` the probe |
| `PROBE_BUILD_NAME` | names the build on screen and in its log file |
| `PROBE_SOURCE_HTTP` | `0` reads `probe360.mp4` from the memory stick; `1` streams `PROBE_HTTP_URL` |
| `PROBE_JELLYFIN` / `PROBE_JF_ITEM` | signs in and plays that one library item |
| `PROBE_SHOW_ALL` | shows every decoded frame, ignoring timing (for checking the picture) |
| `PROBE_ME_RUNTIME` / `PROBE_ME_BOOT_MODE` | how the Media Engine is prepared; leave at `1` / `4` |
| `PROBE_SKIP_AUDIO` / `PROBE_SKIP_VIDEO` | leaves one decoder out, to isolate a fault |

The test clips are not in the repository.

## Installing and using it

Copy the two files into one folder under `PSP/GAME/` on the memory stick:

```
PSP/GAME/Hexfin/EBOOT.PBP
PSP/GAME/Hexfin/hexfin_me.prx
```

Launch **Hexfin** from the PSP menu. It connects to Wi-Fi, then signs in. The first time, it shows a **Quick Connect code**: enter it in any Jellyfin app you are already signed in to, under Settings > Quick Connect. The sign-in is saved, so this happens once.

The home screen lists **Continue Watching** and your video libraries; music, book and photo libraries are left out. Playing something you stopped partway through asks whether to **resume** from where you were or start from the beginning.

While you watch, the app tells the server how far you are: when playback starts, every 10 seconds, when you pause or resume, and when you stop. Continue Watching, the resume position and "watched" then stay in step with your other Jellyfin apps.

| Button | In the lists | During playback |
|---|---|---|
| Up / Down | move | |
| Left / Right | move a screen at a time | |
| L / R | previous / next page (50 items a page) | back / ahead 10 s (hold to go further) |
| X | open a folder, play an item | pause and resume |
| O | back | stop, back to the list |
| Triangle | reload the list | |
| START | Settings | pause and resume |
| HOME | quit | quit |

**Settings** has **Clock** and **Battery** (show or hide each in the top corner and on the pause screen), shows the server and the signed-in user, and has **Sign out** and **About** (version and author).

When you press the volume buttons, the app shows the new level: over the picture during playback, and on the notice line in the lists. The PSP's firmware keeps the volume in kernel memory, so reading it uses ARK's kernel bridge. If a firmware does not allow it, the log says so and no volume is shown.

The screen stays on, and the PSP does not go to sleep, for as long as the app is running. Use the POWER/HOLD switch to turn the screen off.

### jellyfin.cfg

The app keeps its settings in `jellyfin.cfg`, next to `EBOOT.PBP`. It is plain text:

```
device_id=psp-...
user_id=...
token=...
server=http://192.168.1.10:8096
clock=on
battery=on
```

- `server=` is optional and overrides the built-in address. To use it on the first run, create the file with just that line before launching.
- `clock=` and `battery=` are `on` or `off`, the same as the two settings.
- `token=` is your sign-in. Treat the file like a password.
- Delete the file, or use Settings > Sign out, to sign in again.

### The log

Every run writes a log to the root of the memory stick: `ms0:/probe-app.log` for the app, `ms0:/probe-<build name>.log` for a probe. It records Wi-Fi, sign-in, every step of starting the decoders, and one summary line per playback: frames shown and dropped, decode time and audio gaps. It is the first thing to look at when something goes wrong.

## Known limitations

- If you leave with HOME during playback, the saved position is the last one reported, up to 10 seconds earlier. Stop with O first to save the exact spot.
- Each seek starts a new stream on the server, so the picture takes a second or two to come back.
- A very long pause may let the server drop the stream.
- Names are shown in plain ASCII: accented letters lose their accents ("Pokemon"), and scripts such as Japanese show as `?`. The PSP's built-in font has nothing else.
- The server picks the audio track, and burns any subtitles into the picture.
- Only the first Wi-Fi connection saved on the PSP is used.

## The emulator

[scripts/run-emu.sh](scripts/run-emu.sh) runs the build in a portable copy of PPSSPP. That shows the program starts, signs in, browses, streams, parses and exits cleanly. It shows nothing about decoding: **PPSSPP does not implement the Media Engine call this project depends on** (`sceMpegGetAvcNalAu`), so video always fails there, and several bugs in this project only ever showed on real hardware. Treat every decode result from the emulator as unverified.

## How it plays video

The PSP's official video API expects Sony's own container format. This project instead feeds raw H.264 to the Media Engine through an undocumented firmware call, `sceMpegGetAvcNalAu`. Getting that working on 6.61 took three things beyond the obvious:

1. **Restarting the Media Engine in mode 4** from a tiny kernel module ([src/kbridge/](src/kbridge/)), and loading the PSP's own `mpeg_vsh.prx` before the game-side MPEG library ([src/platform/me_runtime.c](src/platform/me_runtime.c)).
2. **Decoding to no destination, then converting colours in a second step** with `sceMpegAvcDecodeDetail2` and `sceMpegBaseCscAvc` ([src/media/video_psp.c](src/media/video_psp.c)).
3. **Rewriting one construct in VAAPI-encoded streams** that the Media Engine refuses: a redundant reference-marking command in every P-frame ([src/media/h264_mmco.c](src/media/h264_mmco.c)). The rewrite is verified to decode to pixel-identical frames.

## Repository layout

```
src/main.c          start-up and teardown; the probe and its report
src/app/            the app: Wi-Fi, sign-in, lists, settings, About
src/player/         playing one stream: read-ahead, decoders, A/V sync, pause
src/jellyfin/       sign-in, library lists, PlaybackInfo, jellyfin.cfg, a small JSON reader
src/media/          MP4 parsing, H.264 and AAC handling, the decoders, A/V sync
src/net/            Wi-Fi and a small HTTP client
src/platform/       firmware imports, the Media Engine runtime, buttons, keep-awake, the log
src/kbridge/        hexfin_me.prx, the kernel module that boots the Media Engine
src/ui/             the display, text and menu helpers, the statistics overlay
tests/              host tests
PROMPT.md           the full project brief
```

## Author

**hexplus**: [github.com/hexplus](https://github.com/hexplus/)

## License

Hexfin is licensed under the [Apache License, Version 2.0](LICENSE). Copyright 2026 Fran Ramírez (hexplus).

See [NOTICE](NOTICE) for the attribution notice that goes with it. In short: Hexfin is an independent, third-party client compatible with Jellyfin. It is not an official Jellyfin project, and it is not affiliated with or endorsed by The Jellyfin Project.
