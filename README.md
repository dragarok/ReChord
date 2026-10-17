# ReChord

> **Re-Chord**: re-harmonize the Echo Mini. A free, from-source custom firmware for
> the SNOWSKY (FiiO) Echo Mini, a "Rockbox for the Echo Mini".

## Objective

ReChord is a complete custom firmware, written from source: not a patch and not a
byte-mod of the stock binary. All of the firmware source is available and modifiable,
on both halves of the device:

- **UI and system** (core A): menus, screens, storage, USB, Bluetooth, power.
- **Audio** (core B): decoders, gapless playback, DSP.

Everything compiles from source and installs as a normal firmware image.

## Architecture

The RKNanoD SoC has two Cortex-M3 cores. ReChord runs its own code on both:

- **Core A**, our `fw1`, is clocked by power state (96 MHz in the UI, 48 MHz idle). It does:
  - boot, clocks and power
  - the UI and keys
  - storage: eMMC / SD NAND, microSD, FAT/exFAT
  - USB: mass storage, the USB DAC (UAC2), and USB host for USB-C earphones
  - the Bluetooth module
  - feeding core B's input ring
- **Core B** runs one decoder image per codec family, loaded from the firmware slot when a
  track needs it. It decodes into a PCM ring that core A's DMA streams to I2S and the two
  CS43131 DACs, gapless across tracks and sample-rate changes.
- **The mask ROM** (`0x00000000-0x02FFFFFF`) is the fixed boot ROM. It is not firmware we
  replace.

## Status (October 2026, 512 MB board, branch `echo512`)

> **512 MB board only, so far.** This work targets the 512 MB Echo Mini (`MINIV*.IMG`
> firmware). The differences from the 8 GB board come from comparing the stock 512 MB and
> 8 GB firmware images. They are written up in [`docs/BOARD_DIFF_512M.md`](docs/BOARD_DIFF_512M.md)
> and selected at build time (`BOARD=512m | 8g`, `firmware/drivers/board.h`). Everything
> marked "device" was tested on one 512 MB unit, the only one we have. The 8 GB build is
> untested on hardware.

The 512 MB Echo Mini runs ReChord as its daily firmware:
- it plays music gapless
- it works as a USB disk and a USB DAC
- it plays to Bluetooth earphones

The full table, with what's verified on the device and what only in QEMU, is in
[`docs/ROADMAP.md`](docs/ROADMAP.md).

| Area | State |
|---|---|
| Boot, 512 MB board port, boot guard, watchdog | ✅ device |
| Dual-core audio, gapless (bit-exact), DAC volume | ✅ device |
| FLAC, WAV/AIFF, MP3, AAC/HE-AAC, ALAC, Vorbis, Opus, WavPack, APE, WMA (v1/v2, Pro, Lossless), DSD | ✅ QEMU (bit-exact), FLAC on the device |
| Storage: eMMC, SD NAND, microSD, FAT16/32/exFAT, FAT32 writes | ✅ device |
| Tags, play queue, sorted browser, resume, settings file, log file | ✅ device |
| Power: backlight, screen off, auto power-off, battery | ✅ device |
| USB disk and hot-plug | ✅ device |
| USB DAC (UAC2, bit-perfect 16/24/32-bit, 44.1-192 kHz, media keys) | ✅ device |
| USB-C earphones (the player as USB audio host) | ✅ QEMU |
| Bluetooth (JieLi module): connect, reconnect, routing, earphone keys | ✅ device, see [`docs/BT_DEVICE_NOTES.md`](docs/BT_DEVICE_NOTES.md) |
| Power-cut-safe updater, both slots, ROM USB loader fallback | ✅ QEMU |
| UI | 🟡 functional but basic (8x8 ASCII font); the rebuild is next |

ReChord is experimental. Keep the stock firmware image at hand.

## Roadmap

Next, in order:
1. **A proper UI:**
   - a screenshot test for every screen
   - a framebuffer with DMA panel push
   - UTF-8 text (Latin, Devanagari, CJK)
   - new now-playing, browser, queue and settings screens
   - themes
2. **Reliability:**
   - log and config writes that don't stall the main loop
   - core B's clock after a pause
   - prompts that keep feeding the audio
3. **Features:**
   - EQ and ReplayGain on core B
   - a real-time clock and a persistent queue
   - key gestures
   - a spectrum view

Details: [`docs/ROADMAP.md`](docs/ROADMAP.md).

## Build

```bash
export PATH=<arm-gnu-toolchain>/bin:$PATH
make cfw-ap -j8                                   # core A: build/cfw/rechord_ap.elf
make cfw-img FONT_IMG=<stock MINIV390.IMG>        # + core-B codec images, stock bitmap fonts
# -> build/MINIV900.IMG (512 MB board); BOARD=8g -> build/HIFIEC90.IMG
```

Kai's exFAT has coarse timestamps, so make can miss an edit there. When in doubt, build
from clean (`rm -rf build/cfw build/coreb`).

## Install

- **Updater:** copy the update file (`MINIV900.IMG` on the 512 MB board) to the player's
  root, eject, and confirm on the player.
- **Maskrom** (recovery, or a known-good write): `rkdeveloptool` writes the image, padded to
  512-byte sectors, to both firmware slots. On the 512 MB board these are LBA `0` and
  `0x11000`. Read both slots back and compare them. See
  [`docs/DEBRICK-LINUX.md`](docs/DEBRICK-LINUX.md).

Stock is always restorable the same way.

## Repository layout

```
firmware/
  startup/      core A: startup, main loop and UI (ap_ui.c), Bluetooth app (ap_bt.c)
  ui/ theme/    screens, themes
  drivers/      clocks, panel, eMMC/SD, USB (MSC, UAC2 DAC, host), CS43131, JieLi BT
  services/     config, log, input, key sequences, FAT/exFAT, tags, queue, updater, BT protocol
  audio/        core A's audio engine and container parsers (MP4, ASF, APE)
  coreb/        core B: runtime and decoders (libmad, FAAD2, Tremor, Opus, WavPack,
                demac, ALAC, WMA, DSD)
  rockchip/     Rockchip RKNanoD SDK (reference)
tools/          image packing and analysis
docs/           ROADMAP (start here), BT_DEVICE_NOTES, hardware and RE notes, history
```

## Sources and acknowledgements

**The stock firmware is the main source.** We disassembled and decompiled the official
firmware images and traced them in QEMU to learn the hardware:
- the boot path
- the 512 MB vs 8 GB board differences
- the panel, keys and storage controllers
- the DACs and their power order
- the USB PHY
- **all of the Bluetooth support**: the module, its pins, its frames and its timing

The Rockchip RKNanoD SDK, the chip datasheets and the FCC filing photos settled what the
code didn't.

**RECHO is the second reference.** [RECHO](https://recho.atzkey.workers.dev), the
closed-source custom firmware for the Echo Mini (earlier released as RefCFW), showed first
what this hardware can do. As part of this work we decompiled RECHO (0.9.2 and 0.10.0) and
traced it in QEMU to learn from it.

Several parts of ReChord copy what RECHO does, re-written in our own C:
- the PLL/clock sequence
- the I2S/DMA programming
- backlight and power behaviour
- the battery reading
- the volume-key shortcuts
- the settings format and its defaults

These places say "RECHO" in their source comments.

**Please support RECHO's developers.** Their About screen links
<https://ko-fi.com/miauk>.

Also used:
- Rockbox (libdemac, libwma, libwmapro)
- FFmpeg (WMA Lossless)
- libmad
- FAAD2
- Tremor and libogg
- libopus and opusfile
- WavPack

Their licenses are in their directories under `firmware/coreb/` (as license files, or
in the source headers for WMA).

## Disclaimer

ReChord is an independent project. It is not affiliated with FiiO, SNOWSKY, Rockchip, or
the RECHO developers. Firmware images (stock or RECHO) are not distributed here; get
official firmware from FiiO. Flashing modified firmware carries risk: use at your own risk.

## Alternatives

[RECHO](https://recho.atzkey.workers.dev): a mature closed-source custom firmware for the
Echo Mini (8 GB board).
