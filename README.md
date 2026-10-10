# MPC Skipback

An addin for Akai MPC OS standalone devices (MPC Live, One, X, Key and Force) that records the main output
all the time and, when you ask, saves the **last 30 seconds** as a WAV file: the take you didn't know you wanted
until it was over.

> **Status: works on a Force (MPC OS 5.0.17, 2026-10-10).** A double press of Rec Arm saved a 30 s WAV and the LED
> blinked when it was done. Host tests (x86, ASan/UBSan, fake libasound and rawmidi) pass. Other models are untested.
> See [Not verified yet](#not-verified-yet).

## What it does

- Keeps a rolling copy of main out (the first two channels of MPC's playback stream) in memory. 30 seconds is about
  10 MB; nothing is written to disk until you trigger a save.
- On a trigger (a double press of a controller button, or a file), writes the newest `window_sec` seconds (1 to 60, default 30) as a 24-bit stereo WAV named
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

**On a Force: press Rec Arm twice** (within 350 ms). The first press reaches MPC as usual (it arms), the second starts
the save, and a few seconds later the Rec Arm LED blinks three times to say the WAV is written. `button`, `double_ms`
and the `led_*` settings change this; `button=0` turns the button off.

**From anything else**, create the trigger file and the addin saves, deletes the file, and writes the result:

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

Anything can create the file: a script, a plugin, or a hardware-button remap. The button needs no remap tool.

Files go to a `Skipback` folder inside the device's own Samples folder (see [Where things live](#where-things-live)),
so they show up in the sample browser. Set `output_dir` to an absolute path to put them elsewhere.

`tools/device_test.sh` (copy it to the device) does the steps above and prints the file's size.

## Where things live

- **The addin itself** is always installed on the internal storage, in `/data/mpc-addins/skipback/` (the library,
  `skipback.conf` and `skipback.log`). It is loaded into MPC as it starts, before the SD card or a USB drive may be
  mounted, and the system disk is read-only, so the SD card and USB drives are not options for the install.
- **The recordings** go wherever `output_dir` says. With `auto` the addin asks MPC where *this* device keeps its
  samples, each time it saves, from `MPC.settings`: the browser shortcut that ends in `/Samples` (on the Force used for
  testing: `/sdcard/Force Documents/Samples`), else the Samples folder next to the Projects folder of a recent
  project, else a `Force`, `MPC` or `APC Documents/Samples` folder on the SD card, else `/data/Skipback`. A WAV is
  about 8 MB for 30 s. To record onto a USB drive or SSD, set `output_dir` to a path on it, for example
  `/media/<drive id>/Skipback`: if the drive is not mounted when you save, the save fails with an `error` in the
  `done` file rather than going somewhere else.
- Only the Force has been checked; the `auto` rules for the MPC models are untested.

## Settings

`/data/mpc-addins/skipback/skipback.conf`, read when MPC starts. `etc/skipback.conf.example` documents every key.

| Key | Default | |
|---|---|---|
| `enabled` | `1` | `0` leaves the library loaded but idle |
| `window_sec` | `30` | seconds saved per trigger, 1 to 60 |
| `output_dir` | `auto` | where the WAVs go: `auto` = `Skipback` inside this device's Samples folder, or an absolute path |
| `trigger` / `done` | `/tmp/mpc-addin-skipback.{trigger,done}` | the two marker files |
| `button` | `93` | controller button (channel-1 note) whose double press saves; 93 is Rec Arm on a Force, 73 is Rec; `0` = off |
| `double_ms` | `350` | two presses within this are a double press |
| `led`, `led_button`, `led_on`, `led_blinks`, `led_ms` | `1`, `auto`, `3`, `3`, `120` | the done blink: which LED (auto = the button), the lit value, how many times, how long each half |
| `midi_log` | `0` | log controller presses and every LED change MPC makes, to find LED values on another model |
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

- Models other than the Force: the playback stream, the main-out channels, the button numbers and the LED values.
- On the Force: that a save during heavy playback doesn't disturb the audio, and that the LED goes back to the state
  MPC had set (Rec Arm's own LED was not written by MPC during the test, so it is restored to off).
- The default folder outside a Force (`/sdcard/Skipback` is a guess).
- Project name and tempo in the file name (the old Force version had them).

## Build and test

```sh
tools/test_host.sh       # x86, ASan + UBSan: unit tests, and the addin in front of a fake libasound
tools/build_armhf.sh     # Docker arm/v7: glibc <= 2.31, no libasound dependency, only the expected exports
```

Package with `tools/release_addin.py` from [mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins), then
`catalog_check.py --catalog`.

## How the button and LED work

MPC reads the Force's buttons from a rawmidi port with "Private" in its name (note-on, channel 1; the same port the
[button remap](https://github.com/mmiroshnikov/akai_standalone_remap) shim hooks). The addin hooks `snd_rawmidi_read`
on it and only looks at the bytes: nothing is changed, so it works with or without a remap tool. MPC sets button LEDs
by writing a control change on channel 1 to the same port (controller = button, value = state); the addin remembers
the last value of each and, for the done blink, writes the blink and then that value back.

## How it works

The audio thread copies two channels of MPC's `snd_pcm_writei`/`writen` data into a lock-free ring: no lock, no
allocation, no system call. A separate thread polls for the trigger, copies the newest window out and writes the WAV
(to `.tmp`, then renamed). That thread starts at the first playback `hw_params`, not in the library constructor,
because a thread started that early crashed MPC in a sister addin. A short write is rewound so a retried write never
duplicates audio.

## License

MIT. See [LICENSE](LICENSE).
