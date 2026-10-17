# Merge notes: Bluetooth (JieLi module)

Two branches carry this work:
- **ReChord `echo512-bt`**: 4 BT commits on echo512 `f3760b0`, plus this note.
- **Project `qemu-bt`**: 1 commit on master `e3024fd` (the QEMU JieLi fake, the m30 test, the checker mutants), plus this note.

The pre-port state of the old BT worktree, exactly as it was (an old bring-up snapshot with BT mixed in), is saved as ReChord branch **`echo512-bt-snapshot`** (`5aa0e93`). Nothing from it is lost. The bring-up hunks in it are superseded by fa1be80 and later commits. Every BT hunk is in echo512-bt; the diff check is below.

The source for every BT value is `work/stock_boot/08_bluetooth.md` (stock MINIV390 disassembly). Each value is cited in the code by its stock address.

## (a) Changes by file

### ReChord (`echo512-bt`)

| Commit | File | What, and why |
|---|---|---|
| `baaf574` | `firmware/services/bt_proto.c/.h` (new) | The module's UART protocol: an encoder for the 3 TX forms stock sends, and a byte-stream framer for every RX form stock parses (08 §2.2/§2.3). Unlike stock's 100-byte buffer refilled from offset 0, the framer handles split bursts, several frames per burst, garbage and stalls. Pure code. |
| | `firmware/services/keyseq.c/.h` (new) | A timed key-sequence matcher (single/double/triple told apart by the gap). It is generic so that future on-device gestures can share it. |
| | `firmware/services/bt_keys.c/.h` (new) | Earphone key frames → keyseq → actions. Stock's one-event map: CP pause, CC play, CA prev, CB next. CD/CU/CM are only logged (stock ignores them). |
| | `firmware/services/tests/test_bt_proto.c`, `test_keyseq.c` (new), `run_tests.sh` | Host tests. |
| | `firmware/ap.mk` | Adds the 3 service sources. |
| `101efc3` | `firmware/drivers/bt_jieli.c/.h` (new) | The UART1 driver. Link: 0x400B0000, 115200 8N1, IRQ 25, GPIO2.16/2.17 func1. Power: GPIO2.7. Then the CV1x hello (5 s), scan, connect (CL06, then CH06 at +4 s), status poll (3 s on the BT screen, 5 s elsewhere), auto-reconnect, 5 min idle off, and the analog route (GPIO2.0 = 1 / 2.14 = 0 while connected). Non-blocking. |
| | `firmware/drivers/board_rknanoc.c` | `bt_early_pins()` in `board_early_init`. It drives only stock Main step 1's "module off" levels: GPIO2.7, 2.16 and 2.17 low. Stock also sets 2.14 = 1 / 2.0 = 0 at that point; we leave those to the DAC driver, as in the build proven on the device. `bt_board_off()` in `board_power_off_pins` runs stock's shutdown sequence (ram:0306745c) **only if the module was powered this boot**. |
| | `firmware/drivers/dac_cs43131.c` | At unmute, if `bt_routed()`, keep GPIO2.0 = 1 / 2.14 = 0 (stock seg33 0x03087688). Otherwise the unmute is unchanged. |
| | `firmware/startup/ap_startup.c` | A weak `UART1_IRQHandler` at vector 25. |
| | `firmware/ap.mk` | Adds `bt_jieli.c`. |
| `298ce3e` | `firmware/services/config.c/.h` | New RECHORD.CFG keys: `bt=off\|on` (default **off**; this is stock's +0x34b, which only the BT menu turns on), `bt_volume`, and `bt_mac`/`bt_name` (the remembered device). |
| | `firmware/startup/ap_bt.c` (new) | The "Bluetooth" screen: on/off, the remembered device, Scan, found devices, Disconnect, Forget. Also the player glue (stock's key gates, the separate BT volume while routed, CY0 on play/pause) and the boot auto-on with reconnect when `bt=on`. `bt_app_tick()` **returns at once while BT is off and not on its screen**. Without that early return it cost core A 3–4 instructions per audio frame and failed m3's budget (see (b)). |
| | `firmware/startup/ap_ui.c` | 8 lines: the externs, `open_bt`, the "Bluetooth" row **appended as the last row** (index 16, after "USB detect", so no existing row moves), `bt_app_init()` after `power_init()`, and `bt_app_tick()` before the draw in the main loop. Nothing else in ap_ui.c changed. Both of echo512's `usb_dwc2_idle_tick()` and the detect-test code are kept. |
| | `firmware/ap.mk` | Adds `ap_bt.c`. |
| `b08b0cd` | `keyseq.c/.h` | New `KEYSEQ_ALSO` rules. They are matched over their own history, so they never hold back or replace the keys' own actions; their action comes on top of them. |
| | `bt_keys.c/.h`, `ap_bt.c` | Settings by key sequence. Each sequence is 4 next/prev presses at most 2 s apart, and leaves the player on the track it started on: `next prev next prev` → the next play mode (`play=`); `next next prev prev` → BT volume +3 dB; `next prev prev next` → BT volume −3 dB (`bt_volume=`). Single keys keep exactly stock's behaviour, with no added latency. |
| | `test_keyseq.c` | 127 checks: ALSO rules next to hold-back rules, each sequence, too slow, interrupted, counted once, CD/CU never forming a sequence. |

Untracked and **not committed**: `stock/3.7.0/` (the stock HIFIEC37.IMG that `make cfw-img` wraps) and `community/refcfw/HIFIEC90.IMG`. They are reference binaries, the same as in the main worktree.

Port check: `git diff fa1be80 9faf17a` (the port on fa1be80) and `git diff f3760b0 echo512-bt` have identical +/- lines, apart from the later `bt_app_tick` early return.

### Project (`qemu-bt`)

| File | What, and why |
|---|---|
| `qemu/hw/arm/echo_mini_bt.inc` (new, `#include`d by echo_mini.c) | With `-machine jieli=SCRIPT`, UART1 becomes a DesignWare APB UART (IRQ 25) with a scripted JieLi module behind it. The script sets: hello CV15/CV10/none, scan devices, connect ok/err/silent, key frames at virtual times, `keyform=2`, `drop=`, and `split=`/`noise=` replies. The checker logs `jieli: VIOLATION:` for: TX while the module is off, TX with 2.16 not on the UART, a line not at 8N1/115200 ±3 %, a frame stock never sends, C\x86/C\x87 to a non-CV15 module, UART pins on the UART or high for 20 ms while the module is off, the audio routed with no link for more than 6 s, and RX overrun. **Without `jieli=`, UART1 is the old 16550 and nothing changes.** |
| `qemu/hw/arm/echo_mini.c` | Includes the .inc. Three hooks: in `regfile_write`, GRF 0x50010048 → `jieli_grf()`; in `board_gpio_write` port 2 → `jieli_gpio2()`; and the UART loop maps `jieli_new()` instead of `serial_mm_init` for i == 1 when the property is set. Also the `jieli` property. The rebase onto master kept both sides at the 3 conflict points: `usb_phy_cfg_written`, the DAC rail block, and the `dac-rail` property. |
| `qemu/build.sh` | Also copies `echo_mini_bt.inc`. |
| `tests/cfw/m30_bt.sh` (new) | 5 cases: `off`, `absent`, `pair`, `keys`, `faults` (see (b)). Skips itself if `$QEMU` has no `jieli=`. Disks come from `make_disk.py --add`, which is pure Python (no hdiutil). |
| `tests/cfw/bt_checker_mutants.sh` (new) | 4 firmware mutants. The checker must catch each one. |

## (b) Test status (final firmware `b08b0cd`; QEMU = master a24c6bc + `jieli`, a private binary at `work/build/qemu_bt/qemu-system-arm`)

- **Host:** `make host-tests` passes. test_settings 123, test_log 49, test_input 187, test_fs 26, test_bt_proto 4065, test_keyseq 127, test_tags 488, display 69, panel_init 204, font_ppm 358, all with 0 failures. The fs_write power-cut sweep and fw_powercut (131 cut points) are OK. Rerun after every rebase.
- **m30_bt: 5/5 pass**, on both builds:
  - **off:** never powered, UART1 silent, a track plays.
  - **absent:** 5 s waiting for CV1x, then off again, `bt=off` saved, the track still plays.
  - **pair:** screen → scan → found → CL06/CLOK/CQO → routed; `bt=on`, `bt_mac` and `bt_name` saved.
  - **keys:** reconnects at boot; 4 next + 4 prev; `play=shuffle` and `bt_volume=-37` saved by sequence; `volume=-40` (the jack) untouched; CP/CC send CY0 0/30; the last second of audio is still track 1's 410 Hz tone.
  - **faults:** split and garbage replies, a 2-byte key frame, and a drop that is noticed and unrouted.
- **bt_checker_mutants: 4/4 caught:**
  - `pinsleft`: "GPIO2.16 on the UART for 20 ms while the module is off".
  - `nogate`: C\x86 to CV10.
  - `noroute`: "routed … with no link up".
  - `baud`: "125000 baud".
- **The suite** (master's tests in the qemu-bt worktree, every run path rewritten into a private dir, `--jobs 2`):
  - Run 2 on b08b0cd: 19 of 25 pass.
  - Of the 6 failures, 5 were rerun at a lower load and pass: m0, m14 (with master's e3024fd test), m25, m27 and m6.
  - **m5 was not completed**: the coordinator's stop arrived mid-run.
  - **Skipped on the coordinator's order: m9_queue, m10_sd, m21_twin.** They need diskutil/hdiutil, which hung at a Touch ID prompt (DiskArbitration) during my run 1. None of them touches BT.

| Failure | Root cause | Evidence |
|---|---|---|
| m3 (run 1, before the fix) | **BT, fixed.** `bt_app_tick()` did its full work every 10 ms tick while off. | Core A insn/frame: CD16 45.47 / GAPLESS 46.30 vs the base f3760b0's 42.38 / 42.06 (budget < 45). After the early return, m3 passes in run 2. |
| m0 (run 2) | Harness: m0 uses host-time `--keys`; at load ~14 two presses were lost and the cursor ended on "USB storage". **Not the menu:** the Bluetooth row is row 17, off the first page, and the first page is pixel-identical. | Run 2 screen `work/qemu_runs/btsuite2/test_m0/screen_11.5.png` (deleted with the run dir) vs `tests/cfw/ref/m0_internal.png`. The rerun at load ~4 passes against the **unchanged** ref. The ref was not regenerated. |
| m14 (runs 1–2) | Test false alarm: master a24c6bc logs "DAC supply on (GPIO2.2)" on the channel m14 greps for supply pins. | Fixed by master e3024fd; passes with it. |
| m25 diskidle, m27 dacthendisk, m6 (run 2) | Harness: `emu_run.py`'s HMP socket `recv` timed out under load, so the emulator never produced fwlog.txt. | m25 `diskidle/stderr.txt`: `TimeoutError: timed out` in `_read_until_prompt`; m27 "fwlog.txt: No such file". All pass on rerun. |
| m5 (runs 1–2) | **Open.** Run 2: the same harness timeout (`FileNotFoundError` on the wav: no audio was produced). Run 1 was on the pre-fix build; its log is lost. m5 checks bit-exactness and core B insn/frame, and BT runs on core A. | Needs one clean run. |
| m1, m12, m17, m18 (run 1) | Environment: the worktree lacked the ignored `firmware/official/*.IMG` / `custom/HIFIEC90.IMG`. | I symlinked them in; all 4 pass in run 2. |

## (c) What might conflict with the infra agent's work (`qemu-infra`)

- **`qemu/hw/arm/echo_mini.c`** (both edit it): my change is small.
  - The `#include "echo_mini_bt.inc"` before RegFile.
  - 3 lines in `regfile_write` (GRF 0x50010048).
  - 1 line in `board_gpio_write` port 2.
  - The `i == 1 && m->jieli` branch in the UART loop.
  - The `jieli` property.
  
  The fake maps its own region with `memory_region_init_io` (`.valid` 1..4, `.impl` 4) rather than through infra's `strict_map`. It should go through `strict_map` once that lands, or get `blk = -1` / a `clkdeps[]` row. Infra's notes say the UARTs are not in `clkdeps[]` and that UART1's PCLK is gate id 137. The driver ungates CLKGATE8 bit 9 (= 0x89 = 137) and CLKGATE1 bit 13 before use, so a gate check would pass.
- **Widths and timing:**
  - The driver does only 32-bit accesses: UART regs, GRF 0x50010048/0x500100BC, CRU 0xC0/0x84/0xA0/0x38, and GPIO2 DR/DDR read-modify-write.
  - No DMA, no I2C and no storage access.
  - TX polls USR.TFNF with a 5 ms timeout, and the fake never blocks TX.
  - CRU SOFTRST0 bit 8 is pulsed for 1 ms: stock's UART1 reset. If infra's SOFTRST model maps bit 8 to anything other than UART1, check it.
  - A chip reset zeroing GPIO outputs is fine: `bt_early_pins` re-drives them at boot.
- **Pins and rails:** BT touches only GPIO2.7, 2.16 and 2.17. It touches 2.0 and 2.14 only while routed, and at power-off only if the module was used. It never touches **GPIO2.2** (the DAC rail), GPIO0.14 / GPIO2.1 (the USB/charge rails) or GPIO2.6 (power hold). With BT off (the default), the firmware's only extra writes are 2.7/2.16/2.17 = 0 at boot.
- **Menu rows:** the Bluetooth row is **appended last** (index 16), and the menu clamps rather than wraps. So m0, m6, m8–m16 (×3 to Files), m15 (×13 to the probe), m27 (×14 to the USB-C probe), m25 and the mutants scripts are all unaffected. Only m30's own `BTROW` (16 downs) depends on the row's position. If a row is ever inserted above it, m30's `BTROW` count must change.
- **Test infra:**
  - m30 and the mutants use fixed `M30_DIR`/`MUT_DIR` defaults under `work/qemu_runs` and their own disks. They need porting to infra's `$QEMU_RUNS`/`$DISKS` and `--snap`.
  - They use `--vseconds`/`--vkeys` with `--icount 2`, so infra's exact virtual-time stop applies.
  - `bt_checker_mutants.sh` calls `make` directly, not `cfw.sh build`, and symlinks `stock/` and `community/` into its worktree.
  - I did not edit `tools/cfw_test.sh`. My isolated runner was a scratch script, not committed.
- **QEMU binary:** the shared `sdk/qemu` was not reinstalled. My binary was built in the shared build dir without `ninja install`, and the QEMU source tree's echo_mini.c was restored to master's afterwards. The next `qemu/build.sh` rebuilds it from master, or from qemu-bt once merged.

## (d) Open questions, and what is unverified on the device

1. **m5** needs one clean run on b08b0cd.
2. **Pairing on the device:** the reply timing and the reply set of the real module are unknown. That covers: when CV1x arrives, whether CLOK precedes CQO, what CQ00 returns while idle or linked, and whether CH06 is needed. The fake's delays (hello 80 ms, CLOK 200 ms, link 500 ms) are guesses. The driver logs every frame with its time (`bt: rx/tx`).
3. **Key letters:** the CP/CC/CA/CB meanings are stock's mapping, but what a given earphone sends for its gestures, and whether it sends CD/CU for volume, is unknown. The settings sequences assume the earphone can produce next and previous. The log line `bt: key Cx at N ms` gives the real gaps.
4. The idle-off and reconnect timings use stock's tick constants ×10 ms (300 s idle, 3 s reconnect), as the previous agent measured stock in QEMU. The doc's own §5.1 says 30 s / 300 ms. **Not settled.**
5. The 512M boot auto-on: stock skips the CV handshake (so no 0x86/0x87 until the screen runs it); ours waits for CV1x. If the real module does not send CV1x at power-on, auto-on reports "no module" and turns `bt=off`.
6. No headphone-jack sense: stock lets the jack win over BT (GPIO2.8/GPIO0.13, polarity unverified). `bt_jack_in()` is a weak 0.
7. The UI placement: the Bluetooth row sits last on the debug-style main menu. Stock has it in its menu, so moving it later needs m30's `BTROW` updated.

**Device test order** (after the main session's merge and flash):
1. Boot with no RECHORD.CFG: no change from echo512 (USB DAC, disk, playback). ECHOLOG has no `bt:` lines.
2. Main menu → Bluetooth (last row) → PLAY on "Bluetooth": `bt: power on`, then `module up (CV1x)` or `no module`.
3. With an earphone in pairing mode: it appears under Scan → PLAY on it → it shows as connected; music plays through it.
4. Earphone keys: pause, play, next, prev.
5. Sequence `next prev next prev` (≤ 2 s gaps): the play mode changes (Now Playing / Settings; `play=` in RECHORD.CFG).
6. Sequence `next next prev prev`: the BT volume goes up 3 dB (`bt_volume=`).
7. Reboot: it reconnects by itself. Then Disconnect, then Forget.
8. Send ECHOLOG.TXT back: it holds the frame timings for items 2–4 of the open questions.
