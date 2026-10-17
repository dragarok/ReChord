# ReChord roadmap and status (512 MB Echo Mini, branch `echo512`, 2026-10-17)

This is the current picture. `STATUS.md` and `HANDOVER.md` are the history of the earlier
approaches (SDK splice, hybrid fw1/BB) and are kept for reference only.

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
| Settings file `RECHORD.CFG`, log ring flushed to `/Internal/ECHOLOG.TXT` | done | device |
| Screenshot shortcut (VOL+ and VOL- together, then PLAY: `/Internal/SHOTn.BMP`) | done | QEMU |
| Power: backlight PWM, screen off, auto power-off, battery reading and cut-off, charge state | done | device |
| USB mass storage (internal read/write, SD read-only), hot-plug | done | device, QEMU |
| USB DAC: UAC2 high-speed asynchronous, bit-perfect 16/24/32-bit at 44.1-192 kHz, VOL keys, media keys to the host, DAC power modes (battery / power only / charge) | done | device, QEMU |
| USB-C earphones: the player as USB host to a UAC1/UAC2 device | done | QEMU |
| Bluetooth (JieLi module, off by default): scan, connect, reconnect, audio routing, earphone keys, settings from earphone key sequences, USB DAC to Bluetooth earphones | done | device (2026-10-17), QEMU; see [BT_DEVICE_NOTES.md](BT_DEVICE_NOTES.md) |
| Updater: power-cut-safe, both slots, slot repair, ROM USB loader fallback | done | QEMU, maskrom flashing on the device |
| UI | basic: menus, browser, now playing, settings, Bluetooth and power pages; 8x8 ASCII font | device |

## Next

### 1. A proper UI
The current UI is functional but plain. In order:
1. A screenshot test per screen (QEMU `--vshots`), so every UI change is reviewed as images.
2. A real framebuffer and a faster panel push. Today the CPU drives the panel bus pin by
   pin, 56-65 ms a full frame. Next: DMA, with a guard that refuses a push method that ever
   hung, as RECHO 0.10 does.
3. A UTF-8 text engine. Today every non-ASCII byte is shown as `?`, so curly quotes and
   Devanagari titles are unreadable. Next: Latin and Devanagari built in, CJK loaded from
   storage.
4. Screens:
   - now playing with tags, cover art, progress and format
   - the browser by folder and by tag
   - a queue view and reworked settings
   - selectable themes (three are built in, none can be picked yet)

### 2. Reliability and speed
- **Writes without stalls.** Log and config writes block the main loop for up to ~600 ms (one
  sector at a time, each waiting for the card). They should not stall the UI or the audio.
- **Core B clock after a pause.** Core B lowers its clock while paused, and nothing raises it
  again on resume. This is the likely cause of the underrun logged after a pause.
- **Prompts that starve the audio.** Modal prompts, such as the cable question, must keep
  feeding the audio.
- **Resume to an SD folder.** It fails because the card is checked after the resume runs.
- **Key timing** in milliseconds rather than loop passes.

### 3. Features
- A 10-band EQ and ReplayGain applied on core B.
- A real-time clock, a persistent queue across power-off, hold-to-boot, a battery-empty
  shutdown, reboot to the USB loader, and swap up/down.
- A spectrum view.
- Key gestures like the official firmware: long, double and triple press, and seek.
- Settings from a phone or a computer. Open question: the stock Bluetooth module has no data
  channel.

### 4. Bluetooth follow-ups
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
