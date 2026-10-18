# The HIFI accelerator (RKNanoD, 0x01060000) and ReChord's use of it

ReChord uses the block for one thing: **FLAC's LPC restore** (the prediction filter that turns
residuals back into samples), on core B, the way stock MINIV390's FLAC decoder uses it, with a
self-test, a bounded wait, a check of every result and the decoder's own software behind it.
RECHORD.CFG `hifi=auto|off|verify` (default `auto`). Nothing here has been measured on the device
yet; section 7 lists what the next flash must measure and the log lines that carry it.

Code: `firmware/coreb/b_hifi.c` (the block), `firmware/coreb/b_flac.c` (the decoder's side),
`firmware/player/codecs/dr_flac/dr_flac.h` (the `DRFLAC_RECHORD_LPC` hook). Emulator model and
checker: project repo `qemu/hw/arm/echo_mini.c` ("HIFI accelerator", `dma2_hifi_check`), tests
`tests/cfw/m37_hifi.sh` and `tests/cfw/hifi_checker_mutants.sh`.

## 1. Sources and tags

| Tag | Meaning |
|---|---|
| [SDK] | Rockchip RKNanoD SDK source in `firmware/rockchip/` (`driver/hifi/`, `driver/dma/`) |
| [SDK-bin] | The SDK's prebuilt decoder libraries (armcc objects with symbols and DWARF macro text): `audio/HIFI/flac/RkNanoD_BHFLAC_20160923.lib` (`hifi_flacdec.o`, `hifi_flac_dec.o`), `HIFI/alac/RkNanoD_BHALAC_20160926.lib`, `HIFI/ape/RkNanoD_BHAPE_20160806.lib`, read with `llvm-objdump` and `strings` |
| [code: 0x…] | Stock MINIV390, core-B FLAC image (`work/segs/MINIV390/seg78_code_01000000.bin`, loaded at 0x01000000), unless another image is named (seg79 ALAC, seg80 APE) |
| [trace] | Stock MINIV390 running in our QEMU machine (`tests/bench.py --fw stock`, `-machine hifi-log=on`): CD16 and HR24_96, 2026-10-18 |
| [inferred] | Our reading of the above; not proven |
| [device] | Only the device can tell; not measured |

Stock's FLAC image matches the SDK library instruction for instruction in every HIFI and DMA2 path;
the only differences are extra log strings [code].

## 2. The block

### 2.1 Registers [SDK `driver/hifi/Hw_hifi.h`, `hifi.h`]

| Offset | Name | Use |
|---|---|---|
| 0x00 | ACC_DMACR | [4:0] TX level, bit 8 TX DMA enable, [20:16] RX level, bit 24 RX DMA enable. Stock: **0x01080108** [code: 0x0100532E] |
| 0x04 | ACC_INTCR | bit 0 TX empty int, bit 1 TX underrun int, [8:4] TX threshold, **bit 15 function done**, bit 16 RX full int, bit 17 RX overrun int, [24:20] RX threshold. Stock: **0x00830083** at init [code: 0x0100531A], `\|= 0x8000` before each job [code: 0x010042FE], `&= ~0x8000` after it (the SDK's `Hifi_Clear_FUN_DONE_FLAG`) [code: 0x01004342] |
| 0x08 | ACC_INTSR (read) | bit 0 TX empty, bit 1 TX underrun, **bit 15 done**, bit 16 RX at threshold, bit 17 RX overrun. Stock reads only bit 15 [code: 0x01004330] |
| 0x0C | FUN_CFG | [31:29] type: 0 APE L, 1 APE R, 2 FFT, 3 ALAC, 4 MAC, **5 FLAC**; `count << 15`; bit 0 TX start, bit 1 RX start. The SDK also defines `Data_count 0x1ff << 20`, but every user writes `count << 15` [SDK-bin, code] |
| 0x10 | ACC_CLR | bit 0 TX logic clear, bit 1 RX logic clear. Stock: 3 at init only [code: 0x01005340] |
| 0x20 / 0x24 / 0x28 / 0x2C / 0x30 | APE_CFG / FFT_CFG / ALAC_CFG / **FLAC_CFG** / MAC_CFG | FLAC_CFG = `order << 16 \| shift << 8` [SDK `hifi_flac.c` Flac_Set_CFG; code: 0x01002AD0] |
| 0x34 / 0x38 | MAC_RSL / MAC_RSH | MAC result |
| 0x40 | TX FIFO | written by DMA2 ch1 only [code] |
| 0xA0 | RX FIFO | read by DMA2 ch0 only [code] |
| 0x1000…0xCFFF | RAM windows | FFT 0x1000/0x2000, APE 0x3000–0x8000, ALAC 0x9000/0xA000, **FLAC coefficients 0xB000** ("max=64x32bit"), FLAC initial data 0xC000 (stock never writes it: the warm-up goes through TX) |

### 2.2 Clocks, resets, interrupts [SDK-bin DWARF enums; code: 0x01000228, 0x01000278]

| Item | SDK id | Register (CRU 0x40180000, hiword write mask) | Stock |
|---|---|---|---|
| HCLK_HIFI_GATE | 0x39 | CLKGATE3 (+0x8C) bit 9 | on at decoder open (`0x02000000`), gated at close (`0x02000200`) |
| HCLK_HIGH_DMA_GATE (DMA2) | 0x4B | CLKGATE4 (+0x90) bit 11 | the same (`0x08000000` / `0x08000800`) |
| HIFI_SRST | 0x23 | SOFTRST2 (+0xC8) bit 3 | held `0x00080008`, 1 ms (`DelayMs2(1)`), released `0x00080000`, at open |
| DMA2_SRST | 0x37 | SOFTRST3 (+0xCC) bit 7 | the same with `0x00800080` / `0x00800000` |
| INT_ID_HIFI | 0x32 | core-B IRQ 34 | unused: stock polls INTSR |
| INT_ID_DMA2 | 0x38 | core-B IRQ 40 (NVIC ISER1 bit 8) | stock sleeps (WFI) until ch0's transfer interrupt [code: 0x010052CE–0x010052E6] |

The ROM leaves every gate open (device, rung 1); stock's own start-up gates the HIFI and DMA2
clocks and holds both in reset until its decoder opens them [trace]. ReChord never touched them
before this change, so they stayed as the ROM left them.

The block's clock is core B's HCLK [inferred: the gate is in core B's group; nothing measured]: its
speed follows core B's divider.

### 2.3 DMA2 (0x01070000, DW_ahb_dmac, core B's bus) [code: DmaConfig2 0x01000D5E, table 0x01005C58]

| | TX (memory → FIFO) | RX (FIFO → memory) |
|---|---|---|
| channel | 1 (`HIFI_DMA_TO_CH`, data 0x010220EC) | 0 (`HIFI_DMA_FROM_CH`, 0x010220F0) |
| CTL_L | 0x02109124 \| INT_EN | 0x00A09424 \| INT_EN |
| CFG_L / CFG_H | 0x800 / 0x807 (DST_PER 1) | 0x400 / 0x007 (SRC_PER 0) |
| order | enabled first | enabled second |

These are the SDK's `DMA_CTLL/CFGL/CFGH_HIFI_WR/_RD` macros [SDK-bin macro text], computed and
found byte for byte in stock's table [code]. A transfer longer than **4088 words** (stock
`DMA_MAX_BLOCK_SIZE` = `LLP_BLOCK_SIZE` = 4088, [code: 0x01000D9C]) goes as a chain of 24-byte LLIs
(SAR, DAR, LLP, CTL_L with LLP_SRC_EN \| LLP_DST_EN, size, DSTAT), 4088 words each and the rest in
the last; stock has room for two per channel (0x010224BC). Every address DMA2 gets is a 0x010xxxxx
system address, never core B's 0 alias [code: buffers 0x0103C9A0 / 0x010411A0, LLIs 0x010224BC].

## 3. The FLAC function

### 3.1 What it computes

Stock's FLAC decoder ([code: decode_subframe_lpc 0x0100423E–0x0100436A] = [SDK-bin hifi_flacdec.o
0x5F8–0x716]):

- **Orders 1 and 2** stay in software (`Flac_Decode_Lpc`) [code: 0x010042E8]: a 64-bit `SMULL`/`ADDS`/
  `ADCS` sum and `__aeabi_lasr` [code: 0x01003064; SDK-bin hifi_flac_dec.o 0x33E].
- **Orders 3..32** go whole through the block. The decoder has already written the `order` warm-up
  samples and then the residuals into its `decoded[ch]` buffer; the job sends all `count` = block
  size words through TX and receives `count` words back **in place** [code: 0x01004322].
- The coefficients go to 0xB000 by the CPU, **reversed**: the coefficient of x[n−1] last
  [code: 0x010042C0].
- What comes back, as stock relies on it: the first `order` words unchanged (the warm-up), every
  later one `res[n] + ((Σ c·s) >> shift)` [code: the in-place job leaves the decoder's final samples
  in the buffer]. Stock and RECHO play bit-exact against `flac -d` in our machine with a 64-bit sum
  [trace].
- **Accumulator width, rounding, overflow: unknown** [device]. FLAC needs up to
  bits + precision + log2(order) = 25 + 15 + 5 = 45 bits; stock sends 24-bit files' subframes
  (bits 24/25, shift up to 15) to the block [trace: HR24_96 jobs with `FLAC_CFG` 0x00080F00], so
  either the silicon is wide or stock's 24-bit FLAC is wrong. ReChord tests both classes itself (§5).
- The SDK's software reference for what the block replaces is `Flac_Decode_Lpc` (64-bit sum,
  arithmetic shift, 32-bit add) [SDK-bin]; libFLAC and dr_flac compute the same.

### 3.2 Limits stock never exceeds

| Limit | Value | Evidence |
|---|---|---|
| order | 3..32 | orders 1–2 software [code: 0x010042E8]; FLAC's 5-bit order field |
| shift | 0..15 | a negative shift is an error [code: 0x010042A6] |
| coefficient precision | ≤ 15 bits | precision 16 is an error [code: 0x01004296] |
| count (block size) | ≤ 4608 | `FLAC_MAX_BLOCKSIZE 4608` [SDK flacdec.h]; FLAC_decode_init rejects larger streams [code: 0x01002D5C] |
| channels | ≤ 2 | [code: 0x01002D4A] |
| bits per sample | ≤ 32 (subframe) | [SDK-bin hifi_flacdec.o 0x786] |

### 3.3 Programming sequence (stock)

Decoder open [code: HIFI_DMA_TO_register 0x01000228, FLAC_decode_init 0x01002D00]: both clocks on,
both resets pulsed for 1 ms, IntRegister2(INT_ID_DMA2), then `FUN_CFG = 0xA0000000`, `ACC_CLR = 3`,
`DMACR = 0x01080108`, `INTCR = 0x00830083`.

Per LPC subframe of order ≥ 3 [code: 0x010042F0–0x01004342; trace]:

1. `FUN_CFG = 0xA0000000 | count << 15` (stopped)
2. `INTCR |= 0x8000`
3. `FLAC_CFG = order << 16 | shift << 8`
4. `order` words to 0xB000 (reversed)
5. `FUN_CFG = 0xA0000000 | count << 15 | 3`
6. DMA2 ch1 (TX) then ch0 (RX), `count` words, in place
7. WFI until ch0's transfer interrupt, then poll `INTSR & 0x8000` — **both without a limit**
8. `INTCR &= ~0x8000`

Decoder close [code: 0x01000278]: the DMA2 interrupt off, both clocks gated. No timeouts, retries or
error paths anywhere [code].

### 3.4 What stock exercises on the 512 MB unit

| Function | Stock image | Evidence |
|---|---|---|
| FLAC (type 5) | seg78 | [code], [trace]: CD16 orders 4–8, shifts 9–11; HR24_96 orders 5–8, shifts 12–15; every job 4096 words (the files' block size) |
| ALAC (type 3) | seg79 | `FUN_CFG 0x60000000`, `ALAC_CFG = bps << 24 \| order << 16 \| quant << 8`, coefficients unreversed to 0x9000, only for `quant < 12` [code: seg79 0x01004656–0x010046B0] |
| APE (types 0, 1) | seg80 | per channel, `APE_CFG` from version and level, `APE_Clear` (bit 21) per frame, only for levels > 1000 [code: seg80 0x0100343C, 0x01002502] |
| FFT (2), MAC (4) | none | no caller in any stock image or SDK decoder [SDK-bin, code] |

## 4. What ReChord does (b_hifi.c, b_flac.c)

- **Which subframes.** LPC subframes of order 3..32 with a block of at most 4608 (stock's split and
  limits). Fixed-predictor subframes, orders 1–2 and longer blocks stay in software, as in stock.
- **The decode.** dr_flac decodes the warm-up and the residuals only (`DRFLAC_RECHORD_LPC`) into a
  scratch buffer `s_res`; the block reads it (TX) and writes the samples into dr_flac's buffer (RX).
  **Out of place**, unlike stock: the residuals survive a failed job, so the subframe is redone in
  software from them. Everything else is stock's sequence (§3.3) with the same values, except that
  core B polls DMA2's RawTfr (INT_EN set, the interrupt masked) instead of sleeping on the interrupt:
  the wait has a limit, and it counts as busy time for the load governor (the block runs on B's clock
  [inferred]).
- **Self-test** at the first FLAC track of a boot (`b_hifi_open`): four jobs on synthetic vectors
  (xorshift32, seeds fixed): 4608 words at 16 bits / order 12 / shift 11 (a two-LLI chain, stock's
  longest job), 300 at 16 / 3 / 0, 4096 at 24 / 32 / 15 and 1000 at 25 / 8 / 14, each with one sum
  at the largest its class allows. Each output is checked sample by sample against the software
  restore of the same input (each check on samples already proven, so all pass means the block
  equals the software). Two classes are judged apart: **narrow** (Σ|c| · 2^(bits−1) < 2^31: a 32-bit
  accumulator could not wrap) and **wide**. A failed wide vector leaves the block for narrow
  subframes only; a failed narrow one turns it off.
- **Every job is checked** (`hifi=auto`): the last warm-up sample, the first predicted one, one in
  the middle and the last are recomputed with dr_flac's own arithmetic. A difference: the subframe is
  redone in software, and that class (or the block) is off for the boot. This catches a systematic
  fault (a precision class, a sequence error), not a single flipped sample: `hifi=verify` does that.
- **`hifi=verify`**: every sample recomputed in software; what plays is the software's sample;
  mismatches are counted and the first is logged. This is the device's measure of bit-exactness on
  real music (it costs more than software alone: §6).
- **A bounded wait**: 10 ms plus 8 core-B cycles per tap (order × count). Past it: both DMA2
  channels stopped, both blocks reset and gated, software until power-off.
- **A stalled bus** (no timeout can fire): `hifi_busy` in the shared block is set while B touches the
  block. Core A reloads a core B that stopped ("no heartbeat", "no progress"); the next core B finds
  `hifi_busy` set and marks the block dead for the boot.
- **The state outlives image swaps** (`AS->hifi_state`: untested, ok, narrow only, failed, dead):
  the self-test runs once a boot, and a failed or dead block is not tried again until power-off.
- **`hifi=off`**: the decoder is byte for byte today's software path; B gates the HIFI and DMA2
  clocks (stock's state between tracks).

Core A's part: `hifi=` parsed and written back like `display=` (only when not `auto`), copied to
`AS->hifi_mode` at each play, and core B's lines (`AS->b_msg`, 4 × 128 bytes) logged. Cost: 200
bytes of FW_CODE.

### Log lines (core A's log, ECHOLOG.TXT)

```
hifi: self-test passed (4 jobs, <t> us on the block): FLAC LPC on HIFI[, every sample verified]
hifi: self-test job <v> (<bits>-bit, order <o>, shift <s>, narrow|wide) sample <i>: hifi <x>, software <y>
hifi: self-test failed: FLAC LPC in software
hifi: self-test failed for wide sums (...): FLAC LPC on HIFI for narrow subframes only
hifi: check: job <j> (order <o>, shift <s>, <class>) sample <i>: hifi <x>, software <y>: that class in software from now on
hifi: verify: job <j> (...) sample <i>: hifi <x>, software <y> (counting on)
hifi: job <j> (order <o>, <n> samples) not done after <t> us (RX done|not done): FLAC LPC in software until power-off
hifi: core B was reloaded while it used the block (a stalled bus?): FLAC LPC in software until power-off
flac: track <n>, <bits>-bit: core B <M> M cycles/s, LPC <hw> of <lpc> subframes on HIFI (wait <W> M/s), orders <mask>
hifi: track <n>: hifi=<mode>, block <state>, <jobs> jobs, <m> mismatched, <r> redone in software, longest job <t> us, <p> polls
```

`core B <M> M cycles/s` is core B's time inside the FLAC decoder per second of audio, from SysTick:
the clock FLAC needs. `wait` is the part of it spent waiting for the block. `orders` is a bit mask of
the LPC orders the track used (bit n = order n, bit 0 = order 32).

## 5. The emulator's model and checker

`qemu/hw/arm/echo_mini.c` computes the FLAC function with a 64-bit sum (what the SDK's
`Flac_Decode_Lpc` computes) and raises a `VIOLATION:` for anything stock and the SDK never do: FIFO
traffic from a CPU, any access from core A, an FFT or MAC job, one start bit alone, a start while a
job runs or with results unread, order outside 3..32, shift > 15, a coefficient over 15 bits,
count ≤ order or > 4608, INTCR bit 15 clear or DMACR not 0x01080108 at a start, the configuration
or coefficients written mid-job, a TX word past `count`, an RX read with nothing to read, reads of a
RAM window or the MAC result, writes to INTSR or the FFT/MAC registers or the FLAC initial-data
window, unknown offsets; and through `dma2_hifi_check`: a DMA2 block in core B's 0 alias, longer
than 4088 words, or a HIFI block with other than stock's CTL/CFG values or channels. Access with the
clock gated or the block in reset is the machine's general rule (`clkdeps`). Fault injection:
`-machine hifi-fault=wrong|hang|acc32[@N]`. ALAC and APE jobs (stock's other images) are not
modelled: they never finish (logged under `-d unimp`).

Before this change the model (2026-10-14, "Bench groundwork", from the SDK headers and stock's
decode in QEMU) computed the same FLAC function with no checks.

## 6. Benefit, measured in QEMU

Core B instructions per output frame (`tests/bench.py`, `-icount 2`, `PROF_EXACT`, storage latency
zero, as m3's budget run), the whole file:

| File | hifi=off | hifi=auto | hifi=verify | stock (its HIFI) |
|---|---|---|---|---|
| CD16 (16/44.1, flac -5) | 135 | 101 | 191 | 260 |
| HR24_96 (24/96) | 161 | 112 | 210 | 230 |
| HR24_192 (24/192) | 158 | 111 | 210 | |
| GAPLESS (2 × 16/44.1) | 136 | 101 | 200 | |

`auto` includes the wait loop at the model's DMA speed (200 MB/s, an assumption: the silicon's
speed is unknown), about 9 instructions a frame here; without it, the LPC restore was **33 % of
16-bit FLAC's core-B instructions and 36 % of 24-bit's**. The rest is the bit reader, Rice decoding,
frame parsing, stereo decorrelation and the output. So the block can at most take a third off
FLAC's core-B time.

In clock terms (load governor: double above 60 % load, halve below 20 %, 96/48/24/12 MHz), at one
instruction a cycle: 16/44.1 FLAC needs 6.0 M instructions/s in software and 4.5 M with the block:
both run at 12 MHz, the lowest step. 24/96: 15.5 M → 10.8 M, 48 → 24 MHz. 24/192: 30.3 M → 21.3 M,
96 → 48 MHz. **That holds only if the block is at least as fast as the software it replaces**: if it
takes more than ~8–10 core-B cycles a sample at order 8, it saves nothing (the wait is busy time).
Its speed is the first thing the device must show (§7).

`verify` costs about 40 % more than software alone: a measurement mode, not one to keep.

ALAC and APE are not on the block yet. From the earlier QEMU profile (work/hwdecode/RESEARCH.md §7)
APE's NN filters are 35 % (normal), 57 % (high), 83 % (extra high) and ≈ 96 % (insane) of APE's
core-B cost, about 3.4 instructions a tap: APE extra high 16/44.1 is 105 M instructions/s and insane
≈ 450 M, beyond 96 MHz. That is where the block could matter, not FLAC. What blocks it: the SDK has
no software reference for the APE filters (stock sends every level above 1000 to the block), so the
model's semantics would come from ffmpeg/libdemac only [inferred], and the filter state lives inside
the block across jobs.

## 7. What the next device flash must measure

1. **The self-test** on the silicon: `hifi: self-test passed` (or which vector failed, which sample,
   what the block gave). Narrow and wide apart: the accumulator width question.
2. **Bit-exactness on real music**: one play with `hifi=verify` in RECHORD.CFG, a few albums
   16- and 24-bit: every `hifi: track` line `0 mismatched`; any `hifi: verify:` line is a finding.
3. **The block's speed**: `longest job <t> us` and `wait <W> M/s` against `core B <M> M cycles/s`
   with `hifi=off` and `hifi=auto` on the same tracks: the benefit, or that there is none.
4. **No timeout**: no `not done after` line; `longest job` well below the 10 ms floor.
5. **Clock dependence**: the same tracks at the governor's lower steps (it picks them itself on
   16/44.1): still 0 mismatched.

No step writes anything but the log and RECHORD.CFG; the block is off with `hifi=off`.
