# MPC Skipback

An addin for Akai MPC OS standalone devices (MPC Live, One, X, Key and Force) that records the main output
all the time and, when you ask, saves the **last 30 seconds** as a WAV file: the take you didn't know you wanted
until it was over.

> **Status: tested offline only.** The code builds for the device and passes host tests (x86, ASan/UBSan, against a
> fake audio library), but it has not yet run on a real device. Expect to be among the first. See
> [Not verified yet](#not-verified-yet).

## What it does

- Keeps a rolling copy of main out (the first two channels of MPC's playback stream) in memory. 30 seconds is about
  10 MB; nothing is written to disk until you trigger a save.
- On a trigger, writes the newest `window_sec` seconds (1 to 60, default 30) as a 24-bit stereo WAV named
  `Skipback_YYYYMMDD_HHMMSS.wav`, then reports the result in a small file so something else (a button remap, a
  script) can show feedback.
- Runs inside MPC as an `LD_PRELOAD` library, so there is no extra process to start and nothing to keep running. It
  does nothing in any program that isn't `MPC`.

It is the in-process successor of the Skipback in [force-audio-jack](https://github.com/sd88me/force-audio-jack), and
uses the audio-hook technique of [mpc-addin-usb-audio](https://github.com/jacob-sabella/mpc-addin-usb-audio).

## Install

Needs SSH access to the device (root). Download the release zip, copy it over, then on the device:

```sh
unzip MPC-Skipback-<version>-mpc-armv7.zip && cd MPC-Skipback-<version>
sh install.sh
```

This puts the files in `/data/mpc-addins/skipback/`, adds the library to MPC's `LD_PRELOAD` (keeping everything
already in it) and **restarts MPC**, which closes the open project. `sh install.sh -n` installs without restarting;
the addin starts at MPC's next start. Installing again upgrades it and keeps your `skipback.conf`.

Remove it with `sh /data/mpc-addins/skipback/uninstall.sh` (this also deletes `skipback.conf`; recordings are kept).
How the installer works, and what to do if an addin stops MPC from starting, is in the
[addin guide](https://github.com/sd88me/mpc-vst-plugins/blob/main/docs/ADDINS.md).

## Use

Create the trigger file and the addin saves, deletes the file, and writes the result:

```sh
rm -f /tmp/mpc-addin-skipback.done
touch /tmp/mpc-addin-skipback.trigger
cat /tmp/mpc-addin-skipback.done        # a moment later
```

`/tmp/mpc-addin-skipback.done` holds one line:

| Line | Meaning |
|---|---|
| `ok <path>` | saved; the path is the new WAV |
| `error <reason>` | nothing saved: no audio yet, disk full, folder not writable |

Each trigger is one save; two presses make two files. To know a save finished, delete `done` first and wait for it to
appear. Setting `click=1` also plays a short click through main out when a save succeeds (it is not recorded).

Anything can create the file: a script, a plugin, or a hardware-button remap. The planned remap is **double press of
Record** (rule in `akai_standalone_remap`, which doesn't have this action yet), flashing a button LED when `done` says
`ok`.

Files go to `/sdcard/Force Documents/Samples/Skipback` when `/sdcard/Force Documents` exists, otherwise
`/sdcard/Skipback`. Set `output_dir` to change it.

`tools/device_test.sh` (copy it to the device) does the steps above and prints the file's size.

## Settings

`/data/mpc-addins/skipback/skipback.conf`, read when MPC starts. `etc/skipback.conf.example` documents every key.

| Key | Default | |
|---|---|---|
| `enabled` | `1` | `0` leaves the library loaded but idle |
| `window_sec` | `30` | seconds saved per trigger, 1 to 60 |
| `output_dir` | `auto` | where the WAVs go (absolute path) |
| `trigger` / `done` | `/tmp/mpc-addin-skipback.{trigger,done}` | the two marker files |
| `click` | `0` | click through main out after a save |
| `left` / `right` | `0` / `1` | channels of MPC's playback stream that carry main out |
| `tap_card` | `auto` | the codec's ALSA card |
| `poll_ms` | `100` | how often the trigger is looked for |
| `log` | `auto` | `skipback.log` in the addin folder; empty for none |

## Troubleshooting

- **Nothing happens on trigger:** read `skipback.log`. It should say `active`, then `playback: 4 ch, format …, 44100 Hz`.
  No `playback` line means MPC hasn't opened its output yet (play something) or the stream isn't what the addin
  expects; the log says what it saw.
- **`error no audio yet`:** the output stream hasn't started since MPC started.
- **WAV sounds wrong or silent:** main out may not be channels 0/1 on your model; try other `left`/`right` pairs.
- **MPC won't start after installing:** SSH is still up. See the addin guide linked above, or
  `sh /data/mpc-addins/skipback/uninstall.sh -y -n` with MPC stopped (`systemctl stop acvs`).

## Not verified yet

- Which playback stream MPC opens on each model, and whether main out is always channels 0/1.
- That a save during heavy playback doesn't disturb the audio.
- The default folder outside a Force (`/sdcard/Skipback` is a guess).
- Project name and tempo in the file name (the old Force version had them).

## Build and test

```sh
tools/test_host.sh       # x86, ASan + UBSan: unit tests, and the addin in front of a fake libasound
tools/build_armhf.sh     # Docker arm/v7: glibc <= 2.31, no libasound dependency, only the expected exports
```

Package with `tools/release_addin.py` from [mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins), then
`catalog_check.py --catalog`.

## How it works

The audio thread copies two channels of MPC's `snd_pcm_writei`/`writen` data into a lock-free ring: no lock, no
allocation, no system call. A separate thread polls for the trigger, copies the newest window out and writes the WAV
(to `.tmp`, then renamed). That thread starts at the first playback `hw_params`, not in the library constructor,
because a thread started that early crashed MPC in a sister addin. A short write is rewound so a retried write never
duplicates audio.

## License

MIT. See [LICENSE](LICENSE).
