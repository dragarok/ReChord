# ReChord roadmap and status (512 MB Echo Mini, branch `echo512`, 2026-10-17)

This is the current picture. `STATUS.md` and `HANDOVER.md` are the history of the earlier
approaches (SDK splice, hybrid fw1/BB) and are kept for reference only.

**512 MB only, so far.** This line targets the 512 MB Echo Mini (`MINIV*.IMG` firmware).
The differences from the 8 GB board are documented in [BOARD_DIFF_512M.md](BOARD_DIFF_512M.md),
from a comparison of the stock 512 MB and 8 GB firmware, and selected at build time
(`BOARD=512m | 8g`). All device testing was done on a single 512 MB unit, the only one we
have. The 8 GB build has not been tested on hardware.

How each part was learned is in [Sources](#sources): mainly the stock firmware, with RECHO as
a second reference for some parts.

## Where it stands

"Device" means verified on a real 512 MB unit and recorded in its own log (`ECHOLOG.TXT`).
"QEMU" means covered by the milestone suite on the `echo-mini` machine (strict hardware
models with checkers). That suite and the machine live in a separate repository, not
published yet.

| Area | State | Verified |
|---|---|---|
| Boot: ROM -> our reset -> our main loop, 512 MB board port, boot guard, boot trail, watchdog | done | device, QEMU |
| Clocks: 200 MHz PLL, SysTick time base, WFI idle, power states (UI / IDLE / DAC) | done | device, QEMU |
| Storage: eMMC and SD NAND (byte/sector addressed, 8/4/1-bit), microSD, FAT16/32/exFAT, FAT32 writes on the internal volume | done | device, QEMU |
| Audio engine: dual core (core B decodes, DMA to I2S), gapless (bit-exact across track boundaries and sample-rate changes), CS43131 volume | done | device, QEMU |
| Codecs on core B, one image per family loaded from the firmware slot: FLAC, WAV/AIFF/AIFC (int and float), MP3, AAC (LC, HE v1/v2), ALAC, Ogg Vorbis, Opus, WavPack, APE, WMA v1/v2, WMA Pro, WMA Lossless, DSD (DSF/DFF) | done | QEMU (bit-exact against reference decoders), FLAC on the device |
| Damaged files never stop the player (bounds at open, decoder abort, core-B supervisor) | done | QEMU |
| Tags: ID3v2.2-2.4, ID3v1, APEv2, Vorbis comments, RIFF INFO, M4A, Ogg | done | QEMU, host tests |
| Player: play queue, sorted browser, resume, play a whole folder | done | device |
| Library: background index per volume (incremental, A/B commit, resumes after a power cut), Songs / Albums / Artists / Genres / Favourites / Playlists, playlists made and edited on the device (M3U8 on the SD card or Internal) | done | QEMU (round6) |
| EQ on core B: stock's presets, a 10-band Custom, crossfaded changes, bit-exact when off | done | QEMU (round6) |
| Settings file `RECHORD.CFG`, log ring flushed to `/Internal/ECHOLOG.TXT` | done | device |
| Screenshot shortcut (VOL+ and VOL- together, then PLAY: `/Internal/SHOTn.BMP`) | done | QEMU |
| Power: backlight PWM, screen off, auto power-off, battery reading and cut-off, charge state | done | device |
| USB mass storage (internal and SD read/write), hot-plug | done (SD writes: QEMU, sd-rw) | device, QEMU |
| USB DAC: UAC2 high-speed asynchronous, bit-perfect 16/24/32-bit at 44.1-192 kHz, VOL keys, media keys to the host, DAC power modes (battery / power only / charge) | done | device, QEMU |
| USB-C earphones: the player as USB host to a UAC1/UAC2 device | done | QEMU |
| Bluetooth (JieLi module, off by default): scan, connect, reconnect, audio routing, earphone keys, settings from earphone key sequences, USB DAC to Bluetooth earphones | done | device (2026-10-17), QEMU; see [BT_DEVICE_NOTES.md](BT_DEVICE_NOTES.md) |
| Updater: power-cut-safe, both slots, slot repair, ROM USB loader fallback | done | QEMU, maskrom flashing on the device |
| UI: no key bar, a status header (play state, volume, Bluetooth icon, battery), 7-row lists, Now Playing on the full height, options, queue, track info, settings, themes, questions that keep the music going, toasts, a hold line; Noto Sans in five sizes with Devanagari; key gestures in milliseconds (tap, hold, 2x, 3x, holds after taps) | device (2026-10-18): a full screen 13 ms after its key is handled on DMA (19-21 on GPIO); a tap's release takes ~50 ms to confirm |
| Display: display list in 16-row bands, dirty rectangles with column windows; VOP + DMA path by default behind a trial guard (stock's bus timing), GPIO writer as the fallback (display=gpio) | device (DMA trial passed every boot, 2026-10-18), QEMU |
| No stalls: storage writes queued behind the audio, settings saved at pause, core B at full speed on resume | device |

## The recovery path comes first

The way back from any bad image is the boot ROM's maskrom:
1. Unplug USB.
2. Press RST with a paperclip.
3. Hold every key except power.
4. Plug USB in.

`rkdeveloptool` can then write any image. The ROM decides this before any of our code runs.
Nothing in ReChord may change what the ROM sees on that path:
- **PMU_SYS_REG3** is the ROM's boot-mode register. The ROM leaves `0x18DF` there at every
  power-on and, as stock's code implies, reads it at the next reset. ReChord never writes it,
  except for the stock-style USB-loader reboot (`0x18AF`). QEMU fails any other write as a
  VIOLATION.
- No other mechanism into the loader will be added, such as RECHO 0.10's `MASKROM.REQ` file.

## Next

### 1. Display speed and seek
- **Tearing:** a screen change shows for one refresh as part old, part new, split on a slant: the
  writes cross the panel's scan (MADCTL 0x60) and nothing syncs them to it. The bus now runs at
  stock's timing (a shorter slant); a tear-free change needs the panel's TE line (Developer >
  Panel TE (test) asks the device whether it is wired) and writes in the scan's order.
- **Tap latency:** a release is confirmed ~50 ms after the finger lets go (the voltage's rise,
  then 20 ms of stability and two samples); stock takes it at the first idle reading. The gesture
  lines' rise= and blips= say on the device whether that is safe.
- **Seek:** VOL+ or VOL- twice, then held, seeks forward or back in the track (codec-side seek on
  core B for each format).

### 2. Features
- Library follow-ups: Years and Composers views, an edit merged into the index instead of a
  rebuild, very large folders scanned in more than one slice.
- Settings and the resume point in a double-buffered, checksummed record that survives a power
  cut mid-write; the log appended and rotated.
- ReplayGain applied on core B.
- A real-time clock, a persistent queue across power-off, hold-to-boot, a battery-empty
  shutdown, and swap up/down.
- A spectrum view.
- Settings from a phone or a computer. Open question: the stock Bluetooth module has no data
  channel.

### 3. Bluetooth follow-ups
- Whether the module answers status polls on a link that came from `CH06`.
- A listen-only capture of the module's own output.

## Sources

**The stock firmware is the main source.** We disassembled and decompiled the official
firmware images (MINIV390 and the other 3.x versions, 512 MB and 8 GB) and traced them in
QEMU. That is how we learned:
- the boot path and the ROM entry
- the board differences between 512 MB and 8 GB
- the panel and the keys
- the storage controllers
- the CS43131 DACs and their power order
- the USB PHY tuning and hot-plug behaviour
- **all of the Bluetooth support**: the module's UART link, its power and audio-routing
  pins, every frame stock sends and parses, and its timing

Where the hardware was unclear, the Rockchip RKNanoD SDK, the chip datasheets and the FCC
filing photos settled it.

**RECHO is the second reference.** RECHO is the closed-source custom firmware for the Echo
Mini, at <https://recho.atzkey.workers.dev>; it was earlier released as RefCFW. Its
developers proved first what this hardware can do: gapless, decoding on core B, 200 MHz.
As part of this work we decompiled RECHO 0.9.2 and 0.10.0 and traced them in QEMU to learn
from them.

Several parts of ReChord copy what RECHO does, re-written in our own C:
- the PLL and clock sequence
- the I2S/DMA programming
- the backlight PWM and power behaviour
- the battery reading
- the volume-key shortcuts
- the settings file format and its defaults

Each such place says "RECHO" in a source comment.

Please support RECHO's developers. RECHO's About screen links <https://ko-fi.com/miauk>.
