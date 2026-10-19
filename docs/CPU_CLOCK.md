# Core B's clock while playing: stock's FLAC point, its self-test, and `cpu_b=`

Branch `clock-stock`. Code: `drivers/clock_rknanoc.c` (the points, the switch, the peripheral clocks),
`services/cpuclk.c` (which tracks, the self-test's core-A half, the record), `audio/audio.c` (the hold
and release of core B, the way back to the UI point), `coreb/b_main.c` (`b_hold`), `coreb/b_selftest.c`,
`audio/selftest_kernel.h`. The emulator side and the tests are in the snowsky_echo_mini repo:
`qemu/hw/arm/echo_mini_clock.inc`, `tests/cfw/m41_clock.sh`, `tests/cfw/clock_mutants.sh`,
`docs/MERGE_NOTES_clock.md`.

Tags: [code] stock MINIV390's disassembly, [trace] stock in QEMU, [sdk] the Rockchip SDK, [device] a
measurement on the user's 512 MB unit, [inferred] reasoning. The research is
`work/coreb_clock/FINDINGS.md` and `work/stock_boot/06_clock_stability.md` on Kai.

## 1. Why

Core B tops out at the UI point's 96 MHz. On the device core B runs ~2.5-2.7 cycles per QEMU
instruction [device, 2026-10-18], so APE extra high (284 M cycles/s), DSD64 and WMA Lossless 16
stutter. Stock never plays at 96 MHz: 96 MHz is its UI point only. At a track's open it moves to a fixed
decode point and holds it for the whole track [trace].

## 2. The points (every value stock's)

| Point | PLL | CON0 / CON1 / CON2 | CLKSEL0 / CLKSEL1 | A / B / pclk | GRF_INTER_CON0 | Source |
|---|---|---|---|---|---|---|
| UI | 96 (VCO 480) | 0x1078 / 0x1146 / 0x08000000 | 0x0000 / 0x0100 | 96 / 96 / 48 | 0x3040 off, 0xC000 off | [trace] stock_points.txt |
| FLAC | 1344 (VCO 1344, integer) | 0x1150 / 0x1046 / 0x08000000 | 0x2005 / 0x0200 | 224 / 448 / 56 | 0x3040 on, 0xC000 on | [trace] stock_CD16, stock_HR24_192; GRF [code] 0x0305235A..92 |

Not used, though stock uses them: 1350 MHz (fractional, stock's APE/ALAC/DSD point), 1392 MHz (lossy,
B 348), 144 MHz (audio init), 192/200 MHz from VCO 400 (boot). One decode point to prove. **Never VCO
400 or 200 MHz**: this unit corrupted data at 200 MHz from VCO 400 under ReChord (root cause open,
snowsky_echo_mini docs/BRINGUP_512M.md); 1344 is far from the VCO floor.

`CLOCK_A_MAX_HZ` is 232 MHz (was 200): stock's own A is 224 at 1344 and 232 at 1392
(FREQSetARMFreq 0x0306872E rounds A's divider down) [code, trace]. `CLOCK_B_MAX_HZ` is 450 MHz,
stock 512 MB's highest B [trace]: `clock_set_core_b_div()` raises any divider until B is at or below
it, and at the FLAC point it refuses every change (B fixed at /3). Core A's divider is fixed at /6 too;
power.c's requests (DARK 50 MHz etc.) are kept and applied when the UI point comes back.

**Strict parity:** a fixed 448 MHz for the whole track, no load scaling of B (stock never scales B by
load: no CRU write until the track ends [trace]). The governor runs only at the UI point.

## 3. The PLL-fed peripheral clocks (step 1)

The ROM leaves I2S0/1, SPI0/1, UART0, 2-5 at PLL/8 and EBC at PLL/16, every gate open [device]; at 1344
that is 168 / 84 MHz. At boot, before its PLL (`clock_periph_init()`, inside BC_CLOCK: a boot that
stops there counts as a clock hang and the next one skips it):

- moved to the crystal: UART0, UART1 (the ROM's value), UART2 (stock's DebugInit value), UART3-5,
  SPI0/1, SFC (the ROM's), I2S0 (the 12 MHz crystal path);
- gated, as stock's CpuClockGateInit (B2h, 0x0307a214): SFC, TIMER0/1 (opened around a switch only),
  I2S0, EBC (no crystal source), SPI0/1, UART2-5, TEST, PVTM, SYNTH, IMDCT, I2C1/2 (pclk);
- kept on, against stock's word: HCLK_CAL_CORE, the HIGH RAMs, HIFI, DMA2 (core B); USB (its own boot
  park and the ROM's maskrom path); SARADC (keys, battery); SDMMC/eMMC (the boot trail from Main's
  first line; both on the crystal); I2S1, I2C0, ACODEC (the DAC driver opens them); PCLK_WDT
  (board_reboot_rockusb writes the watchdog before it opens every gate); UART0 (open in stock's word
  too); PCLK_MAILBOX (core B's wake-up in its hold).

Before every switch to FLAC, `clock_periph_check(1344 MHz)` refuses the point if any open PLL-fed clock
would be above 50 MHz [inferred: the most any ran at on this unit].

## 4. The switch (step 2)

Only at a track's open, as stock. Core B, about to open a track whose `clk` is not `clk_cur`, asks
(`clk_req`) and holds (`b_hold`: SysTick off, PRIMASK set, WFI, woken by the mailbox's A2B channel 0).
Core A's SysTick (`audio_tick`) switches and releases it; B restarts its SysTick at the new clock and
acks. Core A holds B itself to go back to UI when playback stops (stop, end of stream, USB DAC, an
error), not on pause.

`clock_set_point()`, stock's order (FreqSetPrevProcess 0x03068678, SetPllFreq 0x0305222C [code]):
IRQs off; every running DMA channel 0..5 suspended (CH_SUSP, FIFO_EMPTY polled ≤ 800); the cores onto
the crystal, dividers /1; PLL reset, power-down, CON0, CON1, CON2, power-up; GRF memory timing and VOP
TIMING for the new clock (both while on the crystal); LOCK polled (≤ 10 ms on the 24 MHz timer); CLKSEL0,
CLKSEL1; the cores back onto the PLL; DMA resumed; the time base carried across by TIMER0.

**Beyond stock:** the I2S channel (DMA ch 5) is resumed while the PLL locks, with the bus on the 24 MHz
crystal, and suspended again for the divider writes. Stock keeps it suspended throughout; its switches
come before its first sound. ReChord switches at gapless boundaries, where the 16-word I2S FIFO lasts
181 us at 44.1 kHz (42 us at 192 kHz): a lock of ~100 us or more would be a gap. Each suspended window is
now a few register writes on the crystal.

If the PLL does not lock: the old point is programmed again (failed at 3); if that does not lock either,
the cores stay on the crystal.

Re-timed: core A's SysTick (in the switch), core B's SysTick (B, at its release), the VOP's bus timing
(in the switch, `clock_vop_hook`), the DAC's I2C divider (before its next transfer, `i2c_speed`), the
backlight PWM (`backlight_retime`, next main-loop pass), the watchdog's range (`watchdog_retime`).
Not affected (the crystal or the ACODEC PLL): UART1 (BT), eMMC/SD card clocks, SAR-ADC, I2S1's MCLK.

## 5. Which tracks (step 3): `cpu_b=`

- `96` (default): never leaves the UI point. **The measurement build ships with this.**
- `auto`: FLAC when the format's worst measured QEMU instruction count per output frame (HIFI_PLAN.md
  section 1, HIFI.md 6.2 hifi=off) × the rate × 2.6 (the device's cycles per instruction) is above 60 %
  of 96 MHz (the governor's "short of time" line). That gives:

  | At the UI point | At the FLAC point |
  |---|---|
  | WAV/AIFF/PCM (80/frame), FLAC up to 96 kHz (137/163), ALAC 16/44.1 (332), APE fast (394), Vorbis (445), WMA (260), WMA Pro (243, 24/96: 207) | FLAC 176.4/192 kHz, MP3 (665: 320 CBR dense), AAC (1260: HE v2, the profile is not known at queue time), Opus (709), WavPack (595), ALAC 24/96 (424), APE normal and up (616..10080), DSD (486/872/2014), WMA Lossless (2520 / 552) |

- With the EQ on (RECHORD.CFG eq=, coreb/eq.h), `auto` adds its cost (EQ_INSN_FRAME + EQ_INSN_SECTION a
  section, measured) to the format's; core B runs the EQ on a track only if its sections fit what is left
  of 60 % of its clock at the point the track plays at (coreb/b_eq.c room), so with `96` the EQ runs on
  light formats only (16/44.1 FLAC and WAV: all ten bands; ALAC 16/44.1: four; 24/96 FLAC: one).
- `stock`: FLAC for every track (stock never plays at 96 MHz). Stock's own lossy point is 1392 MHz /
  B 348; ReChord uses its one point, 448, for those too.

Never for USB-C earphones (the USB host's DMA on the bus).

## 6. The self-test (step 4)

At the first switch to FLAC of a boot, with core B held, both cores at once (`selftest()` and
`b_selftest()`):

1. A's and B's clock: SysTick cycles against TIMER0 (24 MHz) over 4 ms, within ±0.5 %; LOCK read
   between every step on both cores.
2. The datapath kernel (`st_kernel`: UDIV/SDIV with large operands, the decimal conversion, UMULL/UMLAL,
   SMULL/SMLAL, MLA, CLZ, register shifts and rotates, LDM/STM), its signature equal to the host's.
3. March C- (two backgrounds): A over 0x03048000..0x0304B800, B over its free heap (HIGH RAM, at B's
   clock) and 0x0304B800..0x0304F000 (LOW RAM across the bus); firmware_ap.ld leaves the window free.
4. The code: A's image against its sum at the UI point, B's against core A's sum at its load.

Then the point is admitted for the boot, or the stage is "failed at N" (selftest_kernel.h): back to UI,
off for the build.

**The record** (boot trail sector 1 words 6/7, `bootcrumb_hp`): before the first switch the main loop
writes "in flight" (`cpuclk_arm`); the result clears it. A boot that ends in between leaves it, and
the next boot records "failed at 16". A failure, or that, keeps the point off for every later boot of
the build (a new build starts afresh). The boot itself never changes: the fallback is the UI point.
Without a working boot trail the point is never used; nor before the watchdog runs.

## 7. Log lines (ECHOLOG.TXT)

- `cpuclk: cpu_b=..., FLAC point (...): N boots tried, P passed[; failed at ...]` at boot.
- `cpuclk: switch UI -> FLAC in T us (lock L us), DMA mm suspended, I2S DMA off at most D us; A B pclk`
  then `cpuclk: pll CON0 CON1 CON2 mode MODE grf GRF vop TIMING point`, `cpuclk: sel CLKSEL0..12`,
  `cpuclk: gate CLKGATE0..9`: every clock register after each switch.
- `cpuclk: self-test passed|FAILED at ... in T us: A f MHz (ppm), B f MHz (ppm), lock lost a/b`,
  `cpuclk: self-test kernel a/b (want), march A, B high (KiB), B low, code A ref B ref`.
- `clock: track N: point P, core B at F MHz, X M cycles/s decoding (rate, frames)` per track.
- `audio: queued N ... point P`.

## 8. What the measurement flash must show

With `cpu_b=auto` (or `stock`) in RECHORD.CFG; FINDINGS.md's list:

1. B and A against the 24 MHz timer within ±0.5 % (the self-test line's ppm), LOCK held throughout
   (lock lost 0/0), every clock register after each switch (the `cpuclk:` register lines).
2. The self-test passing on ≥ 20 boots (the record's "boots tried / passed"), at full backlight, low
   battery, warm.
3. B's M cycles/s at 448 vs 96 for APE extra high, DSD64, WMA Lossless 16, FLAC 24/192 (`clock: track`
   lines), 0 underruns (`audio: end of stream ... 0 underruns`).
4. The switch's duration and the longest I2S DMA suspension (the `switch` line), no gap at a gapless
   boundary (listen), no eMMC, DAC or panel errors after it.
5. Battery current at 448 vs stock on the same file (by hand).
6. Optional: the stock-plus-hook image recording the clock registers at the end of stock's PLL routine.

## 9. Open

- The PLL's lock time and the TIMER's count direction on the silicon [inferred]; the mailbox waking a
  holding core B (stock's BB_HOLD uses it [code]; ReChord never has).
- Whether core B's CPI rises with the HIGH RAMs' extra response cycle (0xC000).
- DSD256 needs ~462 M cycles/s at CPI 2.6 (2014 instructions × 88.2 kHz): over 448 even at FLAC.
- A 20-70 ms self-test once a boot; at a gapless boundary into a 192 kHz track the PCM ring holds 43 ms.
