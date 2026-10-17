# 512 MB vs 8 GB board: what the V3.9.0 firmware tells us

Method (all offline, reproducible):
1. `tools/ghidra/run_headless.sh IMG` imports each image into Ghidra (on Kai) with the real runtime memory map, rebuilt from the scatter table by `LoadRKnanoFW.java`, and exports per-function fingerprints with `ExportFunctions.java`.
2. `tools/match_functions.py` pairs functions across builds by fingerprint, first within the same block, then across blocks.
3. `DecompileFunctions.java` produces a full decompile of both builds (`work/functions/full_*.c`, ~164k lines each). `tools/diff_decomp.py` diffs them after normalising names.
4. `tools/gpio_calls.py` lists every literal GPIO operation per pin.

## Numbers
- 2771 (8G) vs 2757 (512M) functions.
- **2666 have identical logic**; 27 have the same shape with different constants (mostly relocated string/resource IDs).
- 78 / 64 functions (~33 KB) are build-specific.
- All ten core-B codec images (segments 73–82) are **byte-identical**, so audio decoding is the same.
- Fonts are identical. Resource 2 differs.

## GPIO: the boards are almost the same electrically
Rockchip DW GPIO accessors (same in both builds, with different addresses):

| Function | 8G address | 512M address | Register |
|---|---|---|---|
| iomux(port,pin,func) | 0x03052d70 | 0x03052d70 | GRF write-mask |
| dir(port,pin,out) | 0x03056a94 | 0x03056a60 | DDR +0x04 |
| write(port,pin,level) | 0x03056ace | 0x03056a9a | DR +0x00 |
| read(port,pin) | 0x03056aee | 0x03056aba | EXT +0x50 |

About 50 pins are used. Usage is identical except:

| Pin | 8 GB | 512 MB |
|---|---|---|
| **GPIO0.14** | peripheral mux only (func 1/2) | also driven as a **GPIO output** (high at boot, toggled in the USB-mode handler) |
| **GPIO2.1** | output low (8 sites) | output, controlled together with GPIO0.14 (14 sites) |

Both pins are switched in the USB-connect handler (string IDs 0xDE/0xDF = USB DAC vs data). The pattern depends on the `0x66666666` board ID (GPIO2.9 strap low) and stock's "UAC charging" setting at state+0x35D (decoded 2026-10-15 from MINIV390 disassembly of every write site):
- **GPIO0.14 = charge enable, 1 = charge** [inferred, strong]. High from reset, set by the charge screen, low at power-off.
- **GPIO2.1 = a VBUS → system supply path, 1 = run from VBUS** [inferred, medium].
- Setting values (non-0x666 board, DAC chosen): **0 (default) "Turn off charging power supply": both 0**; 1 "Power only, no charging": GPIO0.14 = 0, GPIO2.1 = 1 below 4000 mV battery, 0 above; 2 "Turn on charging power supply": GPIO0.14 = 1, GPIO2.1 = 0 (the normal state).
- On the 0x666 board both stay 1 everywhere and stock offers no DAC choice there.
- ReChord: `board_usb_supply()` and `dac_charge=off|on` (default off); test `tests/cfw/m14_dac_supply.sh`.

All of these are shared: the panel 8080 bus (GPIO0.16–0.23, 0.24/0.25), keys (GPIO0.11/0.13 reads + ADC), power (GPIO2.5/2.6), mute (GPIO2.0), backlight (GPIO2.4) and panel reset (GPIO2.15).

## Module-level differences
- **Boot/system init (`Reset_Handler`, 930 B → 1352 B):** the same sequence, plus GPIO0.14 high, GPIO2.1 handling, and a final GPIO2.1 decision keyed on `0x66666666`. The RTC default date is set to 2026-05-01 12:00 if invalid (both builds).
- **Updater (seg08):** looks for `ECHOMINI.IMG`/`HIFIECnn` on 8G and `MINIMINI.IMG`/`MINIVnnn` on 512M.
- **8G seg06** is a **Chinese lunar-calendar** module (1901–2100 tables, 12/10 stem-branch cycles, leap months, digit drawing). It has no counterpart in 512M.
  - The normal clock and date exist on both. The user confirmed this on their 512 MB unit (2026-10-14), and the date-setting code in seg32 is present in both builds.
  - 512M reuses the seg06 slot for new code (GPIO0.14 plus power/USB).
- **seg21:** the 512M build references `UNICODEXDIC` (a dictionary/font index resource).
- **seg09, seg32, seg01, seg11, seg25–31:** changed. seg32 is a large settings/UI module; seg09 is the USB-connect handler. *To do:* go through `work/functions/decomp_8G_vs_512M.diff`.

## Storage
Both builds link both the eMMC and SFC drivers. The storage-related functions show no build-specific differences in this pass, which fits the 512 MB board also using eMMC (a small part) and detecting it at runtime. *To confirm:* read the IDB "NANO" system size. That needs the device, so it waits for the user's OK.

## What this means for porting
The 8 GB-board knowledge (ReChord, RECHO) mostly transfers. Two areas need care:
1. GPIO0.14 / GPIO2.1. Mirror stock's 512M behaviour exactly; never treat GPIO0.14 as its 8G peripheral function.
2. Image naming and the layout constants (`MINIVnnn.IMG`, IDB/system size).

## Bluetooth (both boards)
- BT is a separate **JieLi** module. It sits **after the DACs**: it re-digitises the analog output and sends SBC. The firmware cannot improve that path, so it is a dead end for audio quality (community finding, 2026-10-14).
- Control is a simple text protocol (about 4 commands) over **UART at 115200 8N1**: UART config 3, TX GPIO2.16 func1, RX GPIO2.17. It is opened by `FUN_030538da` (512M) with the RX interrupt enabled. GPIO2.16/2.17 are also driven low as GPIOs in many places, probably to power the module down or hold it in reset.
- `uart_init` (`FUN_03056f16`) is a pin-mux table. Its config 6 uses GPIO0.14 func1, which is why the 8G build only ever "muxes" GPIO0.14: an unused table entry. **On 512M, GPIO0.14 is a plain GPIO output**, toggled with GPIO2.1 in the USB-mode handler.

## Goal: USB audio out (UAC host) to USB-C earphones
The SoC already does USB device mode (MSC + UAC "USB DAC"). The DW OTG core can in principle act as a host. Open questions:
1. Does the board supply **VBUS 5 V out** on USB-C, through a boost or OTG switch? Check whether GPIO0.14 / GPIO2.1 on 512M is a USB power/path control.
2. CC / ID detection for host role.
3. RAM budget for a minimal host stack (enumeration + isochronous OUT to one UAC1/UAC2 endpoint).
4. Decode on core B → I2S today; route PCM to USB isochronous instead.
