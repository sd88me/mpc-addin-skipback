# MPC Skipback

![MPC Skipback: double-press a button to save the last 30-60 seconds](docs/img/skipback.png)

**Record the past.** Skipback listens to your MPC's main output all the time. When you double-press a button, it
saves the **last 30 to 60 seconds** as a WAV file, so the idea you had a moment ago is already on the card.

An addin for Akai MPC OS standalone devices (MPC Live, One, X, Key and Force, Gen1 or Gen2). It runs inside MPC,
with no extra program to start, and uses the controller's own buttons and LEDs. Tested on a Force and a Gen2 Live
III; see [Status](#status).

## Features

- **Always recording, nothing to arm.** A rolling buffer holds the newest audio. Nothing is written to disk until you
  save.
- **One gesture to save.** On a Force, double-press **Rec Arm**; on the other MPC models, hold **Shift** and press
  **Overdub**. Or any button you pick (`button=learn` lets you just press it). MPC still sees the presses as usual.
- **Light feedback.** The button's LED gives three quick flashes when the gesture registers and three slow ones when the
  WAV has been written (Rec Arm's on a Force, Overdub's on the other models).
- **30 to 60 seconds**, your choice (`window_sec`), saved as a 24-bit stereo WAV with the date and time in its name.
- **Puts the file where you'd look for it.** Saves to a USB drive or SSD if one is plugged in, else the SD card, else
  your Samples folder. Or any folder you choose.
- **Triggerable by anything.** A script, plugin or other tool can start a save by creating a file, and finds out the
  result from another.
- **Light on the system.** The audio thread only copies samples (no locks, no allocation, no system calls); the save runs
  on its own thread. It does nothing in any program but MPC.

## Requirements

- An Akai MPC or Force running MPC OS (Gen1, 32-bit ARM, or Gen2, aarch64; the installer checks each build against
  the device).
- SSH access to the device as root, to install.
- The Force and a Gen2 Live III are the only models it has been tested on (`tested.json`). Other models use a
  default button that has not been checked yet: see [Other models](#other-models-and-learn-mode).

## Install

1. Download `MPC-Skipback-<version>-mpc-armv7.zip` from the
   [Releases](https://github.com/sd88me/mpc-addin-skipback/releases) page and copy it to the device (for example
   `scp` to `/tmp`).
2. On the device, as root:

   ```sh
   cd /tmp && unzip MPC-Skipback-<version>-mpc-armv7.zip && cd MPC-Skipback-<version>
   sh install.sh
   ```

3. **Save your project first.** The installer adds the addin to MPC's `LD_PRELOAD` (keeping everything already in it)
   and **restarts MPC**, which closes the open project. `sh install.sh -n` installs without restarting; the addin starts
   at MPC's next start. The addin lives in `/data/mpc-addins/skipback/` on the internal storage.

Installing a newer version over an older one keeps your `skipback.conf`. You can also install it with the
[MPC Plugin Manager](https://github.com/poloq-instruments/mpc-vst-manager) or `mpc-store.sh` once it is in the
catalog. How the installer works, and what to do if an addin stops MPC from starting, is in the
[addin guide](https://github.com/sd88me/mpc-vst-plugins/blob/main/docs/ADDINS.md).

## Using it

### Saving

- **On a Force: press Rec Arm twice**, within about a third of a second. The first press does what it always does
  (it arms), MPC sees both presses, and the second one starts the save.
- **On the other MPC models (One, Live, X, Key and the rest of that family): hold Shift and press Overdub.** This is the
  default there, taken from TheKikGen's MPC button table (not yet checked on a device: see
  [Other models](#other-models-and-learn-mode)). Shift + Overdub has no function of its own in MPC (checked in the manuals), so nothing else happens when you use it.
- Either can be changed: `button=` takes a note number, a name, a `shift+overdub`-style combination, or `learn`
  (see [Settings](#settings)). Pick a button or combination where an extra press is harmless.
- **Or create the trigger file.** Anything that can create `/tmp/mpc-addin-skipback.trigger` starts a save:

  ```sh
  rm -f /tmp/mpc-addin-skipback.done
  touch /tmp/mpc-addin-skipback.trigger
  cat /tmp/mpc-addin-skipback.done        # a moment later
  ```

The addin deletes the trigger file when it takes it, so two presses are two saves. It saves what played *before* the
trigger, up to `window_sec` seconds; right after MPC starts there may be less than the full window.

### The LED

On a Force the Rec Arm LED flashes; on the other MPC models the Overdub LED. The blink uses the value MPC itself last used to
light that LED, so it follows the model's own scheme (see [Button and LED reference](#button-and-led-reference)); before MPC has
lit it, a default is used. Verified on a Force, not yet on the other models. A controller that is neither a Force nor an MPC
(by its product code and port name) gets no LED messages unless you set `led=1`.

| You see | It means |
|---|---|
| three quick flashes | the double press registered; the save has started |
| three slow flashes | the WAV is written (usually 2 to 5 seconds later, depending on the card) |
| quick flashes only | the save failed; see the `error` line below |

After flashing, the LED goes back to whatever MPC had set it to.

### The result file

`/tmp/mpc-addin-skipback.done` holds one line after every save:

| Line | Meaning |
|---|---|
| `ok <path>` | saved; the path is the new WAV |
| `error <reason>` | nothing saved: no audio yet, disk full, folder not writable, drive removed |

To know a save finished, delete `done` first and wait for it to appear. `skipback.log` (in the addin folder) records
each save and where it went.

### Where the files go

Files are named `Skipback_YYYYMMDD_HHMMSS.wav` and go in a `Skipback` folder. With `output_dir=auto` (the default) the
addin picks, at save time, the first of these that exists and is writable:

1. **a USB drive or SSD**, as `<drive>/Skipback`, or inside the drive's own `Force`, `MPC` or `APC Documents/Samples`
   folder if it has one;
2. **an external SD card**, the same way;
3. **this device's own Samples folder**, found in MPC's settings (the browser shortcut that ends in `/Samples`, else
   the Samples folder beside a recent project's Projects folder, else a `Force`, `MPC` or `APC Documents/Samples` folder
   on the SD card, else `/data/Skipback`).

Plugging in a drive moves new recordings there; unplugging it moves them back. `output_dir=samples` skips the drives.
An absolute path (`output_dir=/media/<drive>/Skipback`) is used as it is: if that drive is not mounted when you save, the
save fails with an `error` instead of going somewhere else. A 30-second WAV is about 8 MB.

### Settings

`/data/mpc-addins/skipback/skipback.conf`, one `key=value` per line, read **when MPC starts** (restart MPC after
editing). `etc/skipback.conf.example` in this repo lists every key with its explanation.

| Key | Default | What it does |
|---|---|---|
| `enabled` | `1` | `0` leaves the library loaded but idle |
| `window_sec` | `30` | seconds saved per trigger, 1 to 60 |
| `button` | `auto` | `auto` = double press of 93 (Rec Arm) on a Force, **Shift + Overdub** on other models; `learn` = the first button you double-press becomes the button (remembered in `button.learned`); a note number or name (`93`, `recarm`) = that button pressed twice; `hold+button` (`49+80`, `shift+overdub`) = that button pressed while the first is held; or `0` for no button, leaving the trigger file. Names: `shift` 49, `overdub` 80, `rec` 73, `recarm` 93, `stop` 81, `play` 82, `playstart` 83 |
| `double_ms` | `350` | two presses within this many milliseconds are a double press |
| `output_dir` | `auto` | `auto` (SSD, else SD card, else Samples folder), `samples` (Samples folder only) or an absolute path |
| `led` | `auto` | `auto` = on for a Force and the MPC models, off for any other controller; `1` forces it on, `0` off |
| `led_button` | `auto` | which LED blinks (a button number); `auto` = the same button |
| `led_fast_blinks`, `led_fast_ms` | `3`, `50` | the quick flashes: how many, and the length of each half in ms (`0` blinks = none) |
| `led_blinks`, `led_ms` | `3`, `250` | the slow flashes |
| `led_on` | `auto` | the LED value of the lit half of a flash: `auto` = the highest value MPC has itself written to that LED (so it follows the model's scheme), else 3 on a Force and 2 elsewhere; or a number 0 to 127 |
| `click` | `0` | `1` also plays a short click through main out when a save finishes (it is not recorded) |
| `trigger`, `done` | `/tmp/mpc-addin-skipback.{trigger,done}` | the two marker files |
| `left`, `right` | `0`, `1` | channels of MPC's output stream that carry main out |
| `tap_card` | `auto` | the codec's ALSA card |
| `poll_ms` | `100` | how often the trigger file is looked for |
| `midi_log` | `0` | `1` logs controller presses and every LED change MPC makes, for finding button numbers and LED values on another model. It writes a lot; turn it off afterwards |
| `log` | `auto` | `skipback.log` in the addin folder; an absolute path, or empty for none |

### Other models and learn mode

On a model other than the Force the default is **Shift + Overdub** (notes 49 and 80), from the shared MPC button
tables below. Only the Force's numbers (Rec Arm 93) have been checked on a real device. **Please help find out what works
on yours.** There are two ways:

**1. Learn mode (easiest).** Lets you pick any button without knowing its number.

1. Edit `/data/mpc-addins/skipback/skipback.conf` on the device and set `button=learn`.
2. Restart MPC (`systemctl restart acvs`, or power-cycle) and play something so the audio starts.
3. **Double-press the button you want to use** (twice, quickly). You hear a short click: it is learned and remembered in
   `/data/mpc-addins/skipback/button.learned`. This press only teaches; it does not save.
4. Double-press it again: that saves a WAV into your `Skipback` folder.

Delete `button.learned` and restart to learn again. Leave `button=learn` in place; it uses what was learned.

**2. Read the numbers yourself.** Set `midi_log=1`, restart, press the button you want and read
`/data/mpc-addins/skipback/skipback.log`: a press shows as `controller in: 90 <note> 7F` (the note is in hex: `50` is 80).
Set `button=<that note, in decimal>`. Set `midi_log=0` again, because it logs a lot.

**What to report** (an issue on this repo, or the MPC Discord thread): your model and MPC version; which button
you found and whether it worked; the `skipback.log` lines that start `controller input hooked` (they name the port, which
is how the model is recognised) and `playback:` (the audio format); whether the WAV sounds right (silent or one-sided
means main out is not channels 0/1: try `left`/`right`); and, if you set `led=1`, whether the button's LED reacted. With
that I can make the right default for each model.

If there is no suitable button, set `button=0` and use the trigger file (for example from a button remap tool).

### Turning it off or removing it

- `enabled=0` in `skipback.conf` (and restart MPC) leaves it loaded but idle.
- `sh /data/mpc-addins/skipback/uninstall.sh` removes it. It also deletes `skipback.conf`; your recordings are kept.

## Button and LED reference

How MPC talks to its own controls, as far as it is known. The main source is TheKikGen's
[MPCLiveXplore-libs](https://github.com/TheKikGen/MPCLiveXplore-libs), which runs inside MPC on the standalone models and the
Force (`src/tkgl_midimapper.h` for the buttons, `tkgl_midimapper.c` and `tmm-plugins/tmm-IamForce2.c` for the rest).

- **Port.** MPC reads its hardware from an ALSA rawmidi port with "Private" in its name: `MPC ... Private` on the MPC models,
  `Akai Pro Force Private` on a Force (TheKikGen finds it with `(MPC|Akai Pro Force|Force 6).*Privat.*`). Press and release
  of a button arrive on it as note-on, channel 1, velocity 127 and 0. This addin hooks reads and writes on that port.
- **Model.** The device tree's product code (`/sys/firmware/devicetree/base/inmusic,product-code`), which this addin logs:
  `ADA2` Force, `ACV5` MPC X, `ACV8` MPC Live, `ACVA` MPC One, `ACVB` MPC Live 2, `ACVM` MPC Keys 61, `ACV5S` MPC XL, `ACVA2`
  MPC One 2.
- **Buttons.** One shared table for the MPC models (`MPC_BT_*`), and a Force table (`FORCE_BT_*`):

  | Button | MPC models | Force |
  |---|---|---|
  | Shift | 49 | 49 |
  | Rec | 73 | 73 |
  | Overdub | 80 | (Save is 36) |
  | Stop / Play | 81 / 82 | 81 / 82 |
  | Play Start | 83 | Launch 116 (Launch 1 to 8: 56 to 63) |
  | Rec Arm | 74 (listed) | 93 |
  | Tap Tempo, Undo | 53, 67 | 53, 67 |
  | Menu, Main | 123, 52 | Menu 2 |

  (The MPC table also lists Rec Arm 74, Read/Write 75, Mute 76, Solo 77 and F-key 78; whether a given model has the physical
  button is not stated.) The [akai_standalone_remap](https://github.com/mmiroshnikov/akai_standalone_remap) README, tried on an
  MPC Live (MPC 3.9.1), uses the same Shift 49, Menu 123, Main 52, + 54, - 55 and Pad Banks 35 to 38.
- **LEDs.** MPC lights a button with a control change on channel 1 written to the same port: controller = the button's note
  number, value = the state. TheKikGen's Force plugin takes the control change the Force app writes, changes only the
  controller number to the matching MPC button and passes the **value through unchanged** to MPC hardware, so the value
  scheme is the Force's. Measured on a Force: values 1 to 4, where 1 and 3 are colour 1 (dim, full) and 2 and 4 colour 2 (the Tap
  Tempo LED alternates 1 and 3 with the tempo), and 0 is off. A one-colour LED (Overdub is thought to be one) may use
  fewer values: the addin therefore lights it with the highest value MPC has itself written to that LED, and only uses a
  default (3 on a Force, 2 elsewhere) before MPC has.
- **Verified on a device:** the Force (Rec Arm 93, the control-change LEDs, values 1 and 3). The standalone MPCs follow the
  same code path in TheKikGen's mapper and are expected to behave the same, but nothing here has been run on one: that is the
  open question this addin asks testers to settle.
- **Shift + Overdub** does nothing natively on the MPC models, according to their manuals, which is why it is the default there.
- **Not found:** an Akai MIDI implementation chart for the standalone MPCs.

## Troubleshooting

- **Nothing happens on a double press.** Open `/data/mpc-addins/skipback/skipback.log`. It should show `active`, then
  `controller input hooked` and `playback: ... Hz`. If it says `button N` check that N is the button you are pressing (or
  use `button=learn`). No `controller input hooked` line means the button logic could not find
  the controller port (use the trigger file meanwhile). If `button 93 pressed twice` appears but nothing is saved, the
  next lines say why.
- **`error no audio yet`.** MPC has not opened its output since it started; play something, then try again.
- **The WAV is silent or one-sided.** Main out may not be channels 0/1 on your model: try other `left`/`right` pairs.
- **It saved somewhere unexpected.** The log line for each save gives the folder, and which drive is picked is explained
  under [Where the files go](#where-the-files-go). Set `output_dir` to choose.
- **MPC won't start after installing.** SSH stays up. Run `sh /data/mpc-addins/skipback/uninstall.sh -y -n` with MPC stopped
  (`systemctl stop acvs`), then start it again; the addin guide has the full recovery steps.
- **With MockbaMod.** MockbaMod diverts the controller, so the button logic may not see presses; use the trigger file. The
  installer also notices an old MockbaMod `run_skipback.sh` add-on and says so: run only one of the two.

## Status

**Experimental.** The releases are marked as pre-releases while the buttons for other models are worked out.

Works on an Akai Force (MPC 3.9.1, 2026-10-10): the double press, the saves (to the SD card, then to a USB drive), the
LED flashes and the settings were all used on a real device. Also works on a Gen2 Live III (MPC OS 3.9.1,
2026-10-23): the aarch64 build loads and runs alongside two other addins with no conflicts (not yet exercised there
beyond loading -- the button/LED/save path on that model wasn't separately re-checked). The other models (Live,
One, X, Key) are untested: the default button there (Shift + Overdub) and the LED values come from documentation of
the MPC button and LED numbers rather than a device (see the [reference](#button-and-led-reference)), and
`button=learn` is there to find what works; see [Other models](#other-models-and-learn-mode). The offline tests
(x86, ASan/UBSan, against a fake audio library and a fake MIDI port) cover everything but the device itself.

Known limits:

- **Memory.** The rolling buffer is sized for the largest window at the highest sample rate and grows to about 48 MB of RAM
  over the first couple of minutes whatever `window_sec` is.
- **MPC sees the presses too.** The addin only looks at the button presses; MPC still gets them. A toggle button pressed twice (Rec Arm) ends where it started, and Shift + Overdub has no function of its own for MPC to carry out.
- **Slow cards.** A save takes 2 to 5 seconds on the Force's SD card; the LED waits for it. The audio is not affected.
- **Failed saves** show quick flashes only; the reason is in `done` and the log.
- **Project name and tempo** are not in the file name (the older Force-only version, in
  [force-audio-jack](https://github.com/sd88me/force-audio-jack), had them).

## How it works

- **Audio.** The addin hooks `snd_pcm_open`, `snd_pcm_hw_params`, `snd_pcm_writei` and `snd_pcm_writen` on the codec's
  playback stream, the technique of [mpc-addin-usb-audio](https://github.com/jacob-sabella/mpc-addin-usb-audio). Two
  channels of every block MPC plays are copied into a ring buffer. A short write is rewound, so a retried write never
  duplicates audio.
- **Button.** MPC reads the controller from a rawmidi port with "Private" in its name (note-on, channel 1). The addin hooks
  `snd_rawmidi_read` on it and only looks at the bytes; nothing is changed, so it works with or without a button remap
  tool such as [akai_standalone_remap](https://github.com/mmiroshnikov/akai_standalone_remap).
- **LED.** MPC sets a button LED by writing a control change on channel 1 to the same port (controller = button, value =
  state), only when it changes. The addin remembers the last value of each and, after a blink, writes that value back.
  (The format is described in TheKikGen's
  [MPCLiveXplore-libs](https://github.com/TheKikGen/MPCLiveXplore-libs).)
- **Save.** A separate thread copies the newest window out of the ring and writes the WAV to `.tmp`, then renames it. It
  starts at the first playback `hw_params`, not in the library constructor, because a thread started that early crashed MPC
  in a sister addin.

## Build and test

```sh
tools/test_host.sh       # x86, ASan + UBSan: unit tests, and the addin in front of a fake libasound / rawmidi
tools/build_armhf.sh     # Docker arm/v7: glibc <= 2.31, no libasound dependency, only the expected exports (Gen1)
tools/build_aarch64.sh   # Docker arm64: glibc <= 2.39, same checks (Gen2 -- Live III, One G2, Key(s) G2)
tools/device_test.sh     # on the device: triggers a save the way a script would and prints the result
```

Package each build with `tools/release_addin.py` from [mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins)
(`--dir build/package` for Gen1, `--dir build/package-aarch64` for Gen2 -- same `--version`/`--repo`/`--license`,
each produces its own `-mpc-armv7.zip`/`-mpc-aarch64.zip`, both go on the same release), then `catalog_check.py
--catalog` on each. The settings file, installer and release layout are described in that repo's `docs/ADDINS.md`.
Device-confirmed on a Gen2 Live III, MPC OS 3.9.1 (2026-10-23; `tested.json`), alongside mpc-addin-remote and
mpc-addin-commander with no conflicts. That device has no persistent `/etc` (a verity rootfs): the LD_PRELOAD entry
lives on `/run` and needs reapplying after a reboot -- `sh /data/mpc-addins/skipback/install.sh -y -n -t
/data/mpc-addins/skipback` redoes it without the release zip (mpc-vst-plugins `docs/ADDINS.md`, "A device with no
persistent /etc"); the USB stick does this for every installed addin automatically.

## Credits

Built by [sd88me](https://github.com/sd88me). The in-process audio hook follows
[mpc-addin-usb-audio](https://github.com/jacob-sabella/mpc-addin-usb-audio) by Jacob Sabella; the Force's LED protocol
is from TheKikGen's [MPCLiveXplore-libs](https://github.com/TheKikGen/MPCLiveXplore-libs). The Skipback idea and the first
Force version come from [force-audio-jack](https://github.com/sd88me/force-audio-jack).

## License

MIT. See [LICENSE](LICENSE).
