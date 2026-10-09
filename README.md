# mpc-addin-skipback

An addin for Akai MPC OS standalone devices (MPC Live/One/X/Key, Force) that keeps a rolling record of main out and,
on a trigger, saves the last seconds as a 24-bit stereo WAV: audio from *before* you decided to save it.

**Status: host-tested only (x86, ASan/UBSan, fake libasound). Not yet run on a device.**

It is the in-process successor of the Skipback in [force-audio-jack](https://github.com/sd88me/force-audio-jack): there a
shared-memory ring fed a second process; here the whole thing runs inside MPC, hooking `snd_pcm_writei`/`writen` on the
codec's playback stream, the technique of [mpc-addin-usb-audio](https://github.com/jacob-sabella/mpc-addin-usb-audio).
Installed by the shared addin installer of [mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins) (`docs/ADDINS.md`).

## How it works

- The audio thread copies two channels of MPC's playback stream (`left`/`right`, default 0/1 = main out) into a ring
  buffer. It takes no lock, allocates nothing and makes no syscall.
- A thread polls for the trigger file. When it appears it is deleted, the newest `window_sec` seconds are copied out, and
  written to `output_dir/Skipback_YYYYMMDD_HHMMSS.wav` (staged as `.tmp`, then renamed).
- The thread starts at the first playback `hw_params`, not in the library constructor (a thread started that early
  crashed MPC in a sister addin).
- Nothing happens unless the process is `MPC`. `enabled=0` leaves the library loaded but idle.

## Trigger and feedback contract

Anything can start a save by creating `trigger` (`/tmp/mpc-addin-skipback.trigger`); the addin consumes it, so two
presses are two saves. When the save ends the addin atomically replaces `done` (`/tmp/mpc-addin-skipback.done`) with one line:

    ok /sdcard/Force Documents/Samples/Skipback/Skipback_20261009_150941.wav
    error <reason>                     (no audio yet, disk full, ...)

A caller that wants feedback deletes `done` before triggering and waits for it to appear (or watches its mtime).
`click=1` also mixes a short click into main out after a successful save (not recorded).

The planned caller is the button remap in [akai_standalone_remap](https://github.com/sd88me/akai_standalone_remap):
a rule that touches the trigger on double press of Record, and flashes a button LED when `done` says `ok`. Those remap
actions are not written yet.

## Settings

`skipback.conf` in the addin's folder (`/data/mpc-addins/skipback/`), read when MPC starts; `etc/skipback.conf.example`
lists every key. An upgrade keeps your copy.

## Build and test

    tools/test_host.sh        # x86, ASan + UBSan: unit tests and the addin in front of a fake libasound
    tools/build_armhf.sh      # Docker arm/v7; checks glibc <= 2.31, no libasound dependency, exports; writes build/package/

Package: `tools/release_addin.py` from mpc-vst-plugins (`--dir build/package`), then `catalog_check.py --catalog`.

## Not done / unverified

- On a device: which playback PCM MPC opens on each model, channel layout (is main out always 0/1?), sample format,
  that a save under load doesn't glitch, and the default output folder off a Force (`/sdcard/Skipback` is a guess).
- Project name and tempo in the file name (the Force version read them from MockbaMod's `CurrentProject`).
- The remap side (trigger action, LED on `ok`).
