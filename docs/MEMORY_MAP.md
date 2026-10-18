# Core A's memory: the map, the code budget, and overlays (2026-10-18)

Core A (UI, storage, USB, the audio feed) runs from its 640 KiB of SRAM at 0x03000000. Its
resident code is what the boot ROM loads and runs in place, and it cannot grow: when it filled up
(161612 of 161824 B, 212 B free) the answer was a structure, not a squeeze. Rarely used, mutually
exclusive modes now run as **overlays**, read on demand into a window of otherwise unused SRAM, as
stock's own firmware does with its modules m00..m78 (snowsky_echo_mini `docs/RESEARCH.md`,
"Overlay modules ... load into a code window (CRC-checked)"; `docs/IMAGE_FORMAT.md`: core-A
overlays in windows at 0x0307A008, 0x0307BF08, 0x0308B638). Sources: `firmware/firmware_ap.ld`,
`firmware/services/ovl.{h,c}`, `firmware/startup/ap_ovl.c`, `tools/ap_size.py`, `tools/ovl_pack.py`.

## The map

| Range | Size | What | Source |
|---|---|---|---|
| 0x03000000..0x03004000 | 16 KiB | main stack (MSP 0x03004000), nothing else ever | firmware_ap.ld; 2026-08-25 brick cause B |
| 0x03004000..0x030040EC | 236 B | .data (the ROM enters through `ap_ram_vectors` at its start) | firmware_ap.ld, verdict #19 |
| 0x030040EC..0x030283D8 | 148204 B | .bss (g_scratch 32 KiB, s_band 20 KiB, s_pool 16 KiB, s_q / s_ent 9.6 KiB each, s_wq_buf 8 KiB) | `build/cfw/ap_size.txt` |
| 0x030283D8..0x0302FFC0 | 31 KiB | free (the heap, symbolically) | |
| 0x0302FFC0..0x03030000 | 64 B | .noinit: the crash record `g_bootguard_rec`, the display trial's marker | firmware_ap.ld |
| 0x03030000..0x03040000 | 64 KiB | the audio input ring (A writes, B reads) | audio/audio_shared.h AS_IN_RING |
| **0x03040000..0x03048000** | **32 KiB** | **the overlay window (OVL_WIN)**: one overlay at a time | this change |
| 0x03048000..0x0304F000 | 28 KiB | free: room for a second window | |
| 0x0304F298.. | | the loaded image (IMG offset 0; the payload from 0x0304F490) | docs/HANDOVER.md "PHASE A AUDIT RESULT" |
| 0x03050000..0x03077820 | 161824 B | FW_CODE: vectors, code, read-only data, the .data load image; executes in place | firmware_ap.ld |
| 0x03077900..0x0308FF00 | 99840 B | the fonts window ("fonts" module, 92472 B, loaded at boot) | drivers/text_load.c |
| 0x0308FF00..0x0308FFE0 | 224 B | the audio LLI (8 x 28 B) | audio_shared.h AS_LLI |
| 0x0308FFE8..0x0308FFF8 | 16 B | the boot ROM's own record (written at every power-on; never ours) | QEMU echo_mini.c ROMREC_BASE, rung 1 |
| 0x03090000..0x030A0000 | 64 KiB | the PCM ring (8 blocks of 8 KiB) | audio_shared.h AS_PCM_RING |

What the ROM writes at reset is unchanged by overlays: its row-0 copy of the code block up by 0xB68
and its 4-byte poke at 0x03078CD0 (tools/pack_fw1.py rows 0 and 2) land in FW_CODE and the fonts
window before our code runs; the loader's own data sits at 0x0304A000 (SDK BuildAll.sct
LOADER_DATA_BASE, docs/community.md 4.2) only while it loads us. The window is filled only by our
code, long after.

## The code budget

Every link runs `tools/ap_size.py` and the build log keeps its headline:

```
core A: code 138900/161824 B (22924 free, keeps 4096), RAM 148440/180160 B (31228 free)
core A overlays (window 32768 B at 0x03040000): usbdac 6972, usbout 8372, dev 7012 (largest 24396 free)
core A fonts window: 92472/99840 B (7368 free)
```

"code" is what the ROM loads into FW_CODE. `build/cfw/ap_size.txt` has the whole breakdown: by
subsystem and by object, code / rodata / data / bss, resident and per overlay (strings the linker
merges are shared out in proportion to each object's own). **The build fails with less than
`AP_MIN_FREE` (4096 B) free** (`firmware/ap.mk`): over main's last 60 commits (2026-10-15..18)
resident code grew 96.9 KB, a median commit by 880 B, the 75th percentile by 2552 B, the 90th by
6956 B. 4 KiB lets any ordinary change and an urgent resident fix land; a feature-sized one has to
go in an overlay (or free code) first, before the space is gone rather than after.

## Overlays

| Overlay | Module | What | Entered from |
|---|---|---|---|
| usbdac | ov_dac | the USB DAC (UAC2): `drivers/usb_uac_rknanoc.c`, the session in `startup/ap_ui.c` | the cable's mode, Home > USB > USB DAC |
| usbout | ov_uout | USB-C earphones, the player as USB host: `drivers/usb_out_rknanoc.c` | Settings (usb_out=on), every pass while no cable |
| dev | ov_dev | the Developer tests: DAC test, USB console (`drivers/usb_cdc_rknanoc.c`), USB-C probe screen, USB detect, BT listen (`bt_jieli.c` bt_listen), HIFI probe (`audio/hifi_probe.c`; core B's side `coreb/b_hifi_probe.c`, docs/HIFI_PLAN.md) | Settings > System > Developer |

How it works:

- **One link.** Each overlay is an output section of the core-A link at the window's address
  (`OVERLAY` in firmware_ap.ld), so it calls resident code directly and the linker resolves and
  checks every call. Its .data and .bss stay resident (the window holds code and constants only).
  The Makefile strips the overlays from what `tools/make_fw1.py` packs (`rechord_ap_res.elf`: the
  ROM's image holds only resident code) and `tools/ovl_pack.py` writes each as a module
  (`build/cfw/ov_*.bin`) into the image's module table, beside core B's images and the fonts.
- **Resident code never names an overlay's symbol.** `NOCROSSREFS_TO` in firmware_ap.ld fails the
  link if it does ("prohibited cross reference from .text to `usb_dac_session' in .ovl_usbdac"),
  and `NOCROSSREFS` keeps the overlays from naming each other. The way in is a **gate** in
  `startup/ap_ovl.c`: `ovl_get(id)` loads the overlay (if it is not in the window) and returns its
  table of entry points from its header. Callers keep the drivers' names: the link wraps them
  (`AP_OVL_WRAP` in ap.mk: `-Wl,--wrap=usb_out_start` sends every call from another object to
  `__wrap_usb_out_start`), and the table holds the real ones (`__real_usb_out_start`).
- **Checked before anything in it runs** (`services/ovl.c`): the module's word sum
  (`services/modules.c`), then the header's magic, its id, its length, the link's ABI (`__ovl_abi`:
  a hash of every core-A object and the linker script, so a resident image and overlays from
  different builds never pair up) and a CRC-32 of every byte, the header's included (the
  updater's, poly 0x04C10DB7; `tools/ovl_pack.py` computes it with the crc field as 0). A
  copy that is damaged or unreadable is read again from the second firmware slot. Anything still
  wrong is a **refusal**: the window marked empty, `ovl: <module> refused: <why> (<n>)` once per
  boot (never read again), a toast naming the feature, never a jump into what was read.
- **One at a time.** Loading one replaces the one in the window: its `stop` hook leaves its mode
  first (usbout: host off and the music back to the jack, what `usb_out_off()` does before a USB
  device mode), the window is checked unchanged (`ovl: <module> changed while loaded` otherwise),
  and any hook still pointing into it (the USB interrupt's `usb_irq_hook`) is cleared and logged.
  While code in the window runs (a gate holds it: `ovl_hold`), no other overlay is loaded; such a
  request is refused as "busy".
- **Load time** is logged: `ovl: <module> <bytes> B <ms> ms`. It holds the main loop as a core-B
  image load does (the device: 99688 B in 9..17 ms, ECHOLOG 2026-10-16/17 "B up with bmain"; an
  overlay is under a tenth of that), with the 64 KiB input ring and the PCM ring still feeding the
  output. The only load that can happen while music plays is usbout (USB-C earphones switched on).

## Putting a feature in an overlay

1. Write it in its own `.c` file (or files) under its subsystem, as if it were resident; add it to
   `AP_SRCS` in `firmware/ap.mk`.
2. Pick its overlay: an existing one whose mode it shares (the Developer tests: `dev`), or a new one:
   an id in `services/ovl.h` (`OVL_*`, before `OVL_COUNT`), its section in firmware_ap.ld's
   `OVERLAY` (header first: `KEEP(*(.ovl_hdr.<name>)) *(.ovl.<name>.*)`, then
   `*your_file.o(.text* .rodata*)`), a `NOCROSSREFS_TO(.ovl_<name> .vectors .text .data)` and a
   size `ASSERT` beside the others, `<name>:<module>` in `AP_OVLS` (ap.mk; module names are at most
   8 characters), its id in `tools/ovl_pack.py` `IDS`, and a descriptor in `ovl_desc[]`
   (`startup/ap_ovl.c`: module, what the toast calls it, its stop hook).
3. In `startup/ap_ovl.c`: its table of entry points (`OVL_API(<name>)`) and header
   (`OVL_HEADER(<name>, OVL_<ID>, &table)`), and a gate for each way in: `ovl_get` for the user's
   action that enters the mode (a menu row: toast on refusal), `ovl_if` for anything that only
   matters while the mode runs (a status, a poll). Callers elsewhere that use the feature's own
   names: add those names to `AP_OVL_WRAP` and name the gate `__wrap_<name>`, the table entry
   `__real_<name>`. Hold the window across each call (`GATE`/`GATE_V`, or `ovl_run`).
4. Build. A resident caller you missed fails the link (`prohibited cross reference`); fix it by
   going through the gate. A static helper the compiler shares with resident code stays resident
   (it is only listed where it is used alone); a string section shared with a resident table must
   stay out of the overlay (the link says which, as it did for `.rodata.usb_probe_menu.str1.1`).
5. If the mode leaves anything behind that points into the window (an interrupt hook, a callback
   kept by resident code), its stop hook must undo it, and the hook word goes in `ovl_hooks[]`.
6. Test it: its own milestone, then `tests/cfw/m38_overlay.sh` (switching, every damage, Recovery with
   every overlay damaged) and `tests/cfw/ovl_mutants.sh` if the loader changed.

**Never in an overlay**: anything the recovery path needs, which must work with every overlay in
the slot damaged: boot, the boot guard and the crash record, Recovery (VOL+ at power-on) and its USB
disk session (`usb_msc_rknanoc.c`), the updater and slot repair (`fw_update.c`), the way back to
stock, the rockusb route (VOL- held), the storage and display drivers, the fonts loader, the main
loop and anything it calls every pass, core B's loader, interrupt handlers' resident parts. m38 runs
m17 and m18 on an image with every overlay damaged to prove it.

## gc-sections and LTO

The link uses `--gc-sections` (ap.mk). The vector-table rule R-a2d2b19d3577 is cited in the linker
script, but its text is in no doc or `.dfiu` revision; the written reason against gc was
`docs/HANDOVER.md` section 4, about the SDK-era BB link, where it "silently drops nearly everything
(stubs look dead)" because that link had no explicit roots. This link has them: `ENTRY`, `KEEP` for
both vector tables, `.noinit` and the overlay headers. At its introduction it dropped 99 sections,
all unreferenced: 2776 B of code (the `__aeabi_mem*` aliases, `printf`/`puts`, the weak
`fopen`..`fclose` and settings stubs of `ap_main.c`, `ui_run`/`ui_pop_screen`, the unused theme,
replaygain and icon helpers, `panel_init_run_variant`); every symbol a test or tool reads
(`g_bootguard_rec`, `log_ring*`, `s_unsaved`) is still there. LTO was measured too: 6196 B
smaller, but it recompiles every function across files (every path the device has proven,
the boot and recovery code included, would be new code), it needs the naked-asm files built
without it (`ap_startup.c`, `watchdog.c`: "offset out of range" otherwise), and its partitions do
not keep object names, which the overlays' placement by object needs. Not used.
