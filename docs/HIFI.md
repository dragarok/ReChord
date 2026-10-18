# The HIFI accelerator (RKNanoD, 0x01060000) and ReChord's use of it

ReChord uses the block for the three things stock MINIV390 uses it for, on core B, each the way
stock's decoder for it does: **FLAC's LPC restore** (§3), **APE's NN filters** (§3b) and **ALAC's
adaptive LPC** (§3c), each with a self-test, a bounded wait, a check of its jobs against the
decoder's own software and that software behind it. RECHORD.CFG `hifi=auto|off|verify` (default
`auto`) covers all three. Nothing here has been measured on the device yet; section 7 lists what
the next flash must measure and the log lines that carry it.

Code: `firmware/coreb/b_hifi.c` (the block, FLAC), `b_hifi_fn.c` / `b_hifi_fn.h` (what APE and ALAC
share), `b_hifi_ape.c`, `b_hifi_alac.c`; the decoders' side `b_flac.c`, `b_ape.c` (+ libdemac's
`demac/decoder.c` hook), `b_alac.c` (+ `alac/alac_dec.c`'s), `firmware/player/codecs/dr_flac/dr_flac.h`
(the `DRFLAC_RECHORD_LPC` hook). Emulator model and checker: project repo `qemu/hw/arm/echo_mini.c`
("HIFI accelerator", "APE and ALAC", `dma2_hifi_check`), tests `tests/cfw/m37_hifi.sh`,
`m39_hifi_ape_alac.sh`, `hifi_checker_mutants.sh`, `hifi2_mutants.sh`.

## 1. Sources and tags

| Tag | Meaning |
|---|---|
| [SDK] | Rockchip RKNanoD SDK source in `firmware/rockchip/` (`driver/hifi/`, `driver/dma/`) |
| [SDK-bin] | The SDK's prebuilt decoder libraries (armcc objects with symbols and DWARF macro text): `audio/HIFI/flac/RkNanoD_BHFLAC_20160923.lib` (`hifi_flacdec.o`, `hifi_flac_dec.o`), `HIFI/alac/RkNanoD_BHALAC_20160926.lib`, `HIFI/ape/RkNanoD_BHAPE_20160806.lib`, read with `llvm-objdump` and `strings` |
| [code: 0x…] | Stock MINIV390, core-B FLAC image (`work/segs/MINIV390/seg78_code_01000000.bin`, loaded at 0x01000000), unless another image is named (seg79 ALAC, seg80 APE) |
| [trace] | Stock MINIV390 running in our QEMU machine (`tests/bench.py --fw stock`, `-machine hifi-log=on`): CD16 and HR24_96 (FLAC); APE16, APE16H, APE16X, APE24, ALAC16, ALAC24 (APE, ALAC, branch hifi2); 2026-10-18 |
| [inferred] | Our reading of the above; not proven |
| [device] | Only the device can tell; not measured |

Stock's FLAC image matches the SDK library instruction for instruction in every HIFI and DMA2 path;
the only differences are extra log strings [code]. Its ALAC image's `lpc_prediction` and its APE
image's `ape_apply_filters` / frame start match theirs the same way, but for one detail of
`APE_Set_CFG` (a register re-read, §3b.2) [code vs SDK-bin].

## 2. The block

### 2.1 Registers [SDK `driver/hifi/Hw_hifi.h`, `hifi.h`]

| Offset | Name | Use |
|---|---|---|
| 0x00 | ACC_DMACR | [4:0] TX level, bit 8 TX DMA enable, [20:16] RX level, bit 24 RX DMA enable. Stock: **0x01080108** [code: 0x0100532E] |
| 0x04 | ACC_INTCR | bit 0 TX empty int, bit 1 TX underrun int, [8:4] TX threshold, **bit 15 function done**, bit 16 RX full int, bit 17 RX overrun int, [24:20] RX threshold. Stock: **0x00830083** at init [code: 0x0100531A], `\|= 0x8000` before each job [code: 0x010042FE], `&= ~0x8000` after it (the SDK's `Hifi_Clear_FUN_DONE_FLAG`) [code: 0x01004342] |
| 0x08 | ACC_INTSR (read) | bit 0 TX empty, bit 1 TX underrun, **bit 15 done**, bit 16 RX at threshold, bit 17 RX overrun. Stock reads only bit 15 [code: 0x01004330] |
| 0x0C | FUN_CFG | [31:29] type: 0 APE L, 1 APE R, 2 FFT, 3 ALAC, 4 MAC, **5 FLAC**; `count << 15`; bit 0 TX start, bit 1 RX start. The SDK also defines `Data_count 0x1ff << 20`, but every user writes `count << 15` [SDK-bin, code] |
| 0x10 | ACC_CLR | bit 0 TX logic clear, bit 1 RX logic clear. Stock: 3 at init only [code: 0x01005340] |
| 0x20 / 0x24 / 0x28 / 0x2C / 0x30 | **APE_CFG** / FFT_CFG / **ALAC_CFG** / **FLAC_CFG** / MAC_CFG | FLAC_CFG = `order << 16 \| shift << 8` [SDK `hifi_flac.c` Flac_Set_CFG; code: 0x01002AD0]; ALAC_CFG = `bps << 24 \| order << 16 \| quant << 8` [SDK `hifi_alac.c` Alac_Set_CFG; code: seg79 0x01002470]; APE_CFG: version bit 31, level bits, clear bit 21 (§3b.2) |
| 0x34 / 0x38 | MAC_RSL / MAC_RSH | MAC result |
| 0x40 | TX FIFO | written by DMA2 ch1 only [code] |
| 0xA0 | RX FIFO | read by DMA2 ch0 only [code] |
| 0x1000…0xCFFF | RAM windows | FFT 0x1000/0x2000; APE 0x3000–0x8FFF (L/R coefficients, adapt values, history: the block's own state, stock never touches them); **ALAC coefficients 0x9000** ("max=64x32bit", by the CPU), ALAC initial data 0xA000 (never written); **FLAC coefficients 0xB000** ("max=64x32bit"), FLAC initial data 0xC000 (stock never writes it: the warm-up goes through TX) |

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
  in the buffer]. With a 64-bit sum in the model, stock's output in our machine equals `flac -d` on
  every frame it plays (CD16 and HR24_96; it drops the last 2868 / 3076 frames, as before) [trace].
- **Accumulator width, rounding, overflow: unknown** [device]. FLAC needs up to
  bits + precision + log2(order) = 25 + 15 + 5 = 45 bits; stock sends 24-bit files' subframes
  (bits 24/25, shift up to 15) to the block [trace: HR24_96 jobs with `FLAC_CFG` 0x00080F00], so
  either the silicon is wide or stock's 24-bit FLAC is wrong. ReChord tests both classes itself (§4).
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

## 3b. The APE function (types 0 = left, 1 = right)

### 3b.1 What it computes

Monkey's Audio decodes in three stages: the range decoder (the residuals), the NN filters (one to
three adaptive sign-LMS filters a channel, by level) and the predictor (a short adaptive predictor
and, in stereo, the channels' decorrelation). Stock sends each channel's **NN-filter stage** to the
block and keeps the other two in software [code: seg80 ape_apply_filters 0x0100341E, ape_unpack_mono
0x010034A6, ape_unpack_stereo 0x01003508, ape_decode_frame 0x0100368E] = [SDK-bin hifi_ape_apedec.o
0x6C6, 0x74E, 0x7B0, 0x934]. The library is ffmpeg's APE decoder (its function names, `APESIGN`,
`ape_filter_orders`, `ape_filter_fracbits`) [SDK-bin].

- **A job** is one channel's filters over `n` blocks, **in place** (TX from the decoded buffer, RX
  back into it), every filter of the level in turn (normal 16 taps; high 64; extra high 32, then 256;
  insane 16, 256, then 1280) [code: one job a channel per call; SDK-bin ape_filter_orders].
- **The filter state lives inside the block** from job to job, one set for the left channel and one
  for the right (the SDK's windows: coefficients 0x3000 / 0x4000, adapt values 0x5000 / 0x6000,
  history 0x7000 / 0x8000 [SDK Hw_hifi.h]): the coefficients, the saturated history, the adapt
  values and the running average. **APE_CFG bit 21 (`APE_Clear`) zeroes it**; stock clears at every
  frame's start [code: Get_Frame_Info 0x01003926], as libdemac's and ffmpeg's `init_filter` do.
  Stock never reads or writes the windows [code: no literal in 0x01063000–0x01068FFF].
- **The arithmetic** [inferred]: the SDK has no software twin (every level above 1000 goes to the
  block; the lib has no filter code [SDK-bin]). What stock's ffmpeg-derived decoder expects of the
  block is ffmpeg's `do_apply_filter` = libdemac's `do_apply_filter_3980` / `_3970`, for each
  sample x of the channel and each filter (order N, fraction bits F):

      dot = Σ c[i]·d[i] over the last N saturated outputs d   (32-bit, wrapping)
      c[i] -= a[i] if x > 0, += a[i] if x < 0                   (16-bit coefficients, wrapping)
      y = x + ((dot + 2^(F−1)) >> F);  d ← sat16(y)
      3.98+: a ← ±32 / ±16 / ±8 / 0 from |y| against the running average (> 3·avg, > 4/3·avg, > 0),
             avg += (|y| − avg) / 16, then a[−1], a[−2], a[−8] halved
      before 3.98: a ← ±4 (0 for y = 0), a[−4], a[−8] halved

  and y goes on to the next filter. **[trace]**: with that model, stock's APE decoding in our
  machine equals the reference on every frame it plays (normal, high and extra high at 16/44.1,
  normal at 24/96; 0 VIOLATION): what stock relies on the block for is this filter. What the
  silicon does where stock's use does not pin it down — the dot product's width (Monkey's Audio
  and libdemac wrap at 32 bits), the rounding add — is [device]: the self-test drives the dot
  product past 32 bits on purpose (§4.2).

### 3b.2 APE_CFG [code: APE_Set_CFG 0x01002502; SDK hifi_ape.c, hifi_ape.h]

| Level | Filters (taps, fraction bits) | APE_CFG, version ≥ 3.98 | Before 3.98 |
|---|---|---|---|
| 1000 fast | none: no job | — | — |
| 2000 normal | 16, 11 | 0x840B0000 | 0x040B0000 |
| 3000 high | 64, 11 | 0x900B0000 | 0x100B0000 |
| 4000 extra high | 32, 10; 256, 13 | 0xA80AD000 | 0x280AD000 |
| 5000 insane | 16, 11; 256, 13; 1280, 15 | 0xC40BDF00 | 0x440BDF00 |

Bit 31 = version ≥ 3980 (`APE_VERSION_EQUAL_OR_GREAT_3980`), bit 21 = `APE_CLC` [SDK]. Read as
[inferred]: [28:22] the first filter's order, [30:29] how many follow (256, then 1280), [19:16] /
[15:12] / [11:8] their fraction bits. `APE_Set_CFG` writes the register twice: the version bit
alone, then with the level's bits (for normal from a register; for the others OR-ed into a re-read
of the register) [code; trace: `+0x020 <- 80000000`, `<- 840b0000`].

### 3b.3 Limits stock never exceeds

| Limit | Value | Evidence |
|---|---|---|
| levels on the block | above 1000 (fast has no filters) | `cmp #1000` before every job [code: 0x0100342C, 0x01003928] |
| blocks a job | ≤ 4096 | `blockstodecode = min(left, 4096)` [code: ape_decode_frame 0x010036A8] |
| APE_CFG | the 8 values above | [code: 0x01002502] |
| types | 0 (left; mono and pseudo-stereo too), 1 (right) | [code: 0x01003472, ape_unpack_mono] |
| the clear's start | type 0, 4096, no data after it | [code: 0x0100392E–0x01003964; trace] |
| INTCR bit 15 | never set; INTSR never read | the image holds `Hifi_Enable_FUN_DONE_FLAG` (0x01004CCA) and `Hifi_Get_ACC_Intsr` (0x01004D10), nothing calls them [code]; hifi_ape_apedec.o imports neither [SDK-bin]; [trace] |
| the windows 0x3000–0x8FFF | never touched by the CPU | no address of them in the image, the HIFI base only with register offsets [code]; [trace] |

### 3b.4 Programming sequence (stock) [code; trace: stock APE16, `-machine hifi-log=on`]

Decoder open: the same clocks-and-resets open as FLAC's (`HIFI_DMA_TO_register`).

Each frame's start, levels above 1000 (Get_Frame_Info 0x01003926–0x01003964):

1. `FUN_CFG = 0 | 4096 << 15` (stopped: 0x08000000)
2. `APE_CFG = version`, `APE_CFG = cfg` (APE_Set_CFG)
3. `APE_CFG |= 0x00200000` (APE_Clear: a read, then 0x842B0000 at normal)
4. `ACC_CLR = 3`, `DMACR = 0x01080108`, `INTCR = 0x00830083`
5. `FUN_CFG = 0 | 4096 << 15 | 3` (0x08000003) — and no data: the next write is the first job's
   stopped `FUN_CFG`

Each chunk of up to 4096 blocks, per channel (ape_apply_filters 0x0100341E):

1. `FUN_CFG = type << 29 | n << 15` (stopped)
2. `APE_CFG = version`, `APE_CFG = cfg` (bit 21 off again)
3. `FUN_CFG = type << 29 | n << 15 | 3`
4. DMA2 ch1 (TX) then ch0 (RX), `n` words, in place (HIFI_DMA_TO_ACC 0x01004EA6)
5. WFI until ch0's transfer interrupt (0x01004F28) — **without a limit**; nothing else

Where in that clear sequence the silicon resets the state (the bit-21 write, ACC_CLR or the
data-less start) is not known [device]; ReChord replays the whole sequence.

Each track: the decoder's open (seg80 0x01002594…0x0100261A) calls HIFI_DMA_TO_register
(0x01000228: HCLK_HIFI and HCLK_HIGH_DMA on, DMA2_SRST and HIFI_SRST held 1 ms, released, DMA2's
interrupt registered) [code], and its close (0x0100274A) APE_free then HIFI_DMA_TO_Unregister
(0x01000278: the interrupt off, both clocks gated) [code]. **So stock's block runs one APE
configuration (one level, one version class) from a reset to the next**: one file per decoder open.
[trace, 2026-10-18, `-machine hifi-log=on`]: stock's APE16, APE16X, APE24 (24/96 normal: the same
APE_CFG 0x840B0000 and sequence as 16-bit, 4096-word jobs; nothing kept off the block for 24-bit),
APE24X: `hifi-srst` (HIFI_SRST asserted) at the decoder open, then the same writes as above in every
frame, 0 VIOLATION (the model's new "configuration since the reset" rule included; one file per run:
the bench does not drive stock through a playlist, so the per-track reset is shown by the code). Between the clear start and the frame's first
job stock entropy-decodes the first chunk: **14.96–15.51 ms** at 16/44.1, **19.2–19.4 ms** at 24/96
(virtual time, `-icount shift=2`) [trace].

### 3b.5 What the device showed (2026-10-18) and why

ECHOLOG_20261018_round3 (round3, one boot, bape loaded once, so the block reset once at the first
APE open: round3 reset it only when `!b_hifi_is_up()`) [device]:

| Order | Block use since that reset | Result [device] |
|---|---|---|
| 1 | level 2000 self-test, then 2000 track (16-bit) | passed; 46 jobs checked, 0 mismatched |
| 2 | level 3000 self-test, then 3000 track | passed; 46 checked, 0 mismatched |
| 3 | level 4000 self-test | `frame 0 job 0 channel 0 sample 2: hifi -193, software -151` |
| 4 | level 2000, 24/96 track (APE_HR24_96) | `APE job 0 (level 2000, channel 0, frame 1) sample 3: hifi -12387, software -12481` |

Where ReChord differed from stock [code, trace]: (a) **the configuration changed with no reset in
between** (rows 2–4; stock resets at every decoder open, 3b.4); (b) the clear start to first job gap:
0.020–0.028 ms in the self-test, 0.15–0.20 ms when decoding, against stock's ~15 ms [trace: ReChord
APE16/16X/24/24X, `hifi-log=on`]; (c) 256-word jobs against stock's 4096. Every register value and
write order is stock's (APE_CFG 0xA80AD000 / 0x840B0000 written version-bit-first then OR-ed into the
read-back, the clear sequence, DMACR, INTCR, FUN_CFG) [trace, both]; ReChord sends every level and
both bit depths to the block, as stock does.

The two values, reproduced (a host simulation of libdemac's filters with knobs, on the self-test's
own vectors: residuals 1561, 1342, -198, 0, … for frame 0 channel 0, and APE_HR24_96's frame 1
channel 0 job 0 input 0, -15557, -5993, -12387, …, dumped from aperef's decoder) [inferred]:

- Level 4000 at sample 2 is the first sample any filter changes (the first adaption needs two
  samples): the 32/10 filter adds `(32·1342 + 512) >> 10 = 42`, the 256/13 one `(32·1342 + 4096) >> 13
  = 5`, so libdemac gives -198 + 42 + 5 = **-151** (the device's "software"). The device's **-193** =
  -198 + 5: **the 256/13 filter alone; the 32-tap filter contributed nothing.** Of the candidates tried
  (both filters in either order, either one alone, fraction bits swapped, either filter or both with
  3.97 adaption, unsaturated history, adaption from the input, both filters on the input in parallel
  or chained), only "one 13-bit filter, the 32/10 one absent" (the 256/13 alone, or 32 taps at 13 bits)
  gives -193 while passing levels 2000 and 3000; "the 32/10 filter alone with 3.97 adaption" gives
  -193 too but fails 2000 and 3000, which the device passed.
- Level 2000 at 24/96: samples 0–2 pass through every filter unchanged (the input's first sample is
  0, so the first adaption value is 0); at sample 3 libdemac adds `(32·(-5993) + 1024) >> 11 = -94`
  (**-12481**); the device's **-12387** is the input itself: **the 16-tap filter contributed nothing.**
  No arithmetic of the filter does that from a cleared state with these small (16-bit) values; the
  device passed the same level with clean state (rows 1). Nothing here is 24-bit specific.
- One rule fits all four rows: **after a configuration change with no reset, the first filter slot
  (the 16/64/32-tap one) is inert where its order shrank or its fraction bits changed (64 → 32 taps,
  11 → 10 bits; 32 → 16, 10 → 11), and works where it only grew (16 → 64 at 11 bits, row 2); a slot
  used for the first time since the reset (the 256-tap one, row 3) works.** It reproduces -193 and
  -12387 exactly and the passes; the records do not tell "order shrank" from "fraction bits changed"
  [inferred]. A mechanism that would give it: the clear zeroes the state but not a slot's internal
  ring position/configuration latch, which only the reset sets [inferred].
- The other difference, the clear-to-job gap (b), does not explain row 4 (2000 passed with a 0.028 ms
  gap in row 1 and failed with 0.2 ms in row 4); it is measured anyway (4.2, retry).

The fix (branch ape-fix): stock's decoder open and close at every APE track (b_hifi_ape_open:
`b_hifi_power()` unconditionally, so the track's level, self-test included, runs from a reset;
b_hifi_ape_close: `b_hifi_gate()`). The model (§5) reproduces rows 3 and 4 with round3's firmware,
names the cause with a VIOLATION, and passes the fixed firmware on the same playlist.

## 3c. The ALAC function (type 3)

### 3c.1 What it computes

Stock's ALAC decoder is ffmpeg's alac.c [SDK-bin hifi_alac_alac.o: `lpc_prediction`,
`decode_element`, `rice_decompress`]. Its `lpc_prediction` [code: seg79 0x0100459A, inlined again
in decode_element at 0x01004B40] = [SDK-bin 0x1D6]:

- **order 0** copies, **order 31** is ALAC's first-order filter: both in software [code: 0x010045BE].
- The warm-up `out[1..order] = sign_extend(out[i−1] + err[i], bps)` by the CPU, **written back into
  `err[]`** as well (what the block takes) [code: 0x01004620].
- **quant ≥ 12**: software (`Alac_Decode_Lpc`, the SDK's own twin of the block) [code: 0x0100462E];
  quant < 12: the block, out of place (`err[]` → TX, RX → `out[]`), all `n` words [code: 0x01004656–
  0x010046B0].
- The coefficients go to 0x9000 by the CPU, in ffmpeg's array order — the bitstream's reversed
  (decode_element reads them last first, [SDK-bin 0x484–0x4A0]): word j multiplies out[i−order+j].
  Apple's `unpc_block` (ReChord's software) keeps them in bitstream order: coefs[k] ↔ lag k+1.
- What comes back [SDK-bin Alac_Decode_Lpc 0xFA, = ffmpeg]: out[0..order] = in[0..order]; then, with
  d = out[i−order−1],

      val = (Σ c[j]·(out[i−order+j] − d) + 2^(quant−1)) >> quant      (32-bit)
      out[i] = sign_extend(val + d + in[i], bps)
      if in[i] ≠ 0: for j = 0.. while in[i]'s remainder keeps its sign: c[j] −= sign(d − out[i−order+j])·sign(in[i]),
                    the remainder −= ((that sign·(d − out[…])) >> quant)·(j + 1)

  — the coefficients adapt inside the job; nothing is kept between jobs. Apple's `unpc_block`
  computes the same (its 16-bit coefficients wrap where the SDK's 32-bit ones would not: only past
  ±32767, never in practice [inferred]). [trace]: stock's ALAC decoding (16/44.1, 24/96) through the
  model equals the reference on every frame it plays.

### 3c.2 Limits stock never exceeds

| Limit | Value | Evidence |
|---|---|---|
| order on the block | 1..30 | 0 and 31 in software [code: 0x010045BE–0x010045CC] |
| quant (denshift) | < 12 | [code: 0x0100462E] |
| bps (ALAC_CFG [31:24]) | ffmpeg's sample size − shift + channels − 1, ≤ 32 | [SDK-bin decode_element 0x3BC–0x3CE] |
| words a job | ≥ 2; the frame length, ≤ 4096 with stock's buffers | `nb_samples > 1` [code: 0x010045B4]; six frame-length buffers from a 0x18400-byte pool [SDK-bin allocate_buffers 0xAB4] [inferred] |
| ALAC_CFG bits 7..0 | 0 (the SDK's `ALAC_INPUT_END_FLAG`, bit 5, unused) | [SDK hifi_alac.h; code] |
| ALAC data window 0xA000 | never written (the warm-up goes through TX) | [code] |

### 3c.3 Programming sequence (stock) [code: seg79; trace]

Decoder open (Alac_decode_init 0x010025CC): `FUN_CFG = 0x60000000`, `ACC_CLR = 3`,
`DMACR = 0x01080108`, `INTCR = 0x00830083` (after the clocks-and-resets open).

Each channel of each frame with order 1..30 and quant < 12 (0x01004656–0x010046B0):

1. `FUN_CFG = 0x60000000 | n << 15` (stopped)
2. `INTCR |= 0x8000`
3. `ALAC_CFG = bps << 24 | order << 16 | quant << 8`
4. `order` words to 0x9000 (`HIFITranData`)
5. `FUN_CFG = 0x60000000 | n << 15 | 3`
6. DMA2 ch1 (TX, `err[]`) then ch0 (RX, `out[]`), `n` words
7. WFI until ch0's transfer, then poll `INTSR & 0x8000` — **without a limit**
8. `INTCR &= ~0x8000`

### 3.4 What stock exercises on the 512 MB unit

| Function | Stock image | Evidence |
|---|---|---|
| FLAC (type 5) | seg78 | [code], [trace]: CD16 orders 4–8, shifts 9–11; HR24_96 orders 5–8, shifts 12–15; every job 4096 words (the files' block size) |
| ALAC (type 3) | seg79 | §3c: orders 1..30 with `quant < 12`, coefficients in ffmpeg's order to 0x9000 [code: seg79 0x01004656–0x010046B0]; [trace]: ALAC16 and ALAC24 (both Apple-style: orders 4 and 8, quant 9, bps 17, 4096 words) |
| APE (types 0, 1) | seg80 | §3b: per channel, `APE_CFG` from version and level, `APE_Clear` (bit 21) per frame, only for levels > 1000 [code: seg80 0x0100343C, 0x01002502]; [trace]: normal, high, extra high (0x840B0000, 0x900B0000, 0xA80AD000), 4096 words a job |
| FFT (2), MAC (4) | none | no caller in any stock image or SDK decoder [SDK-bin, code] |

## 4. What ReChord does

### 4.1 FLAC (b_hifi.c, b_flac.c)

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

### 4.2 APE (b_hifi_ape.c, b_ape.c, demac/decoder.c)

- **Which jobs.** Levels above fast (stock's split), both versions libdemac reads (3.97–3.99), any
  bit depth, mono and stereo: each channel of each 256-block chunk (b_ape.c's `CHUNK`; stock sends up
  to 4096) is one job, in place, stock's sequence and values (§3b.4), including APE_Set_CFG's two
  writes and the frame-start clear with its data-less start. libdemac's filters are a function of
  their own (`demac_filters_init`, `demac_filters_sw`, ReChord changes in decoder.c) behind the hook
  `demac_rechord_filters`; with the hook declining, decode_chunk is libdemac's.
- **Self-test, per level and version class**, before the level's first use in a boot: two frames
  (8 and 2 jobs a channel, both channels interleaved as the decoder sends them, a clear before
  each), synthetic residuals (xorshift32, fixed seeds: ±2^11, every 7th 0, every 61st ±2^18, and
  in the first frame 200 samples of a constant 30000 that drive the dot product past 32 bits), every
  output against libdemac's filters on the same input from the same reset. All equal: the block
  computes libdemac's filters for that level (and its clear works: the second frame). Cost: 20 jobs;
  in software about 0.5 M (normal) to 24 M (insane) instructions, once.
- **Checks** (`hifi=auto`): the filter state is in the block, so a job can only be checked by
  software that has run the frame from its reset. Each frame's first job per channel is checked:
  its first 64 samples (`APE_CHECK_PART`; the rest only if they differ); in a track's first frame,
  whole, its first 4096 / taps jobs (16 at normal and high, 14 at extra high, 2 at insane: what
  shows only once the block's history has filled). A difference: the software's samples play, the
  rest of that frame goes on in software (exactly: the software has the frame's state) and the level
  is off until power-off. A fault that starts later in a frame is caught at the next frame's start;
  the samples between are the block's (m39 g: bounded to the frame). `hifi=verify` catches it at once.
- **`hifi=verify`**: libdemac runs every job alongside; its samples play; mismatches are counted.
- **A bounded wait**: 10 ms plus 8 core-B cycles a tap (taps × blocks); stock waits for the RX
  transfer only (INTCR bit 15 is never set for APE), and so does ReChord. Past it: the block off
  until power-off (both channels stopped, reset, gated). A checked job is redone from its copy,
  exactly; any other: the filter state of the rest of that frame went with the block, and those
  blocks play as **silence** (counted, "silenced"); the next frame decodes in software.
- **Costs nothing when off**: `hifi=off` (or fast, or a failed level) is one call that returns 0
  per channel and chunk, and the block is never touched (m39 k: no access at all).
- **A reset at every track (ape-fix)**: stock's decoder open (clocks on, the block and DMA2 through a
  1 ms reset) at every APE track that uses the block, before its level's self-test, and stock's close
  (the clocks gated) at its end: the block runs one configuration from a reset, as stock's (§3b.4,
  §3b.5). Costs 1 ms of core B a track.
- **What a failure logs (measurement only)**: a failing self-test or check also logs the job's
  hifi − software differences at samples s..s+11 (from the first bad one) and 32..39 (where the
  candidates of §3b.5 differ: sample 3 on tells one 13-bit filter from a 3.97-style 10-bit one, 35 on
  the 256-tap filter from a 32-tap one). A failing self-test runs once more after another reset with
  20 ms after each clear start (stock's gap, §3b.4) and logs the result; the level stays in software
  either way.

### 4.3 ALAC (b_hifi_alac.c, b_alac.c, alac/alac_dec.c)

- **Which jobs.** Stock's: order 1..30 and quant < 12, plus quant ≥ 1 (Apple's code shifts by −1 at
  0) and frames with something to predict (n > order + 1); ≤ 4096 words; out of place, Apple's
  residual buffer → TX, the channel buffer ← RX. The CPU computes the warm-up (Apple's first loop)
  and writes it over the residuals 1..order, as stock does (kept aside and restored after the job).
  The coefficients go to 0x9000 reversed from Apple's array (= ffmpeg's order, §3c.1). Stock's open
  (FUN_CFG 0x60000000, ACC_CLR, DMACR, INTCR) at every ALAC track.
- **Self-test** at the first ALAC track of a boot: 5 jobs: 4096 words at order 8 / quant 9 / 17 bits
  (Apple's encoder's own settings; a two-LLI chain), 1500 at 4 / 9 / 16, 1000 at 30 / 11 / 25,
  700 at 1 / 1 / 32 (sums past 32 bits), 900 at 13 / 5 / 20; every output against `unpc_block`.
- **Checks**: the coefficients adapt inside the job, so a sample can only be recomputed by running
  the job from its start: `unpc_block` over the warm-up and the first 32 predicted samples
  (`ALAC_CHECK`). A difference: the channel redone by `unpc_block` from the residuals, ALAC off until
  power-off. `hifi=verify`: the whole channel, the software's samples play.
- **A bounded wait**: 10 ms plus 16 cycles a tap; RX transfer and INTSR bit 15, as stock. Past it:
  the block off until power-off, the channel redone from the residuals, exactly.

### 4.4 Both: state, switches, logs

- `hifi=auto|off|verify` covers all three functions; each has its own state in the shared block
  (`hifi_state` FLAC's; `hifi_ape` a bit per level and version class, passed and failed;
  `hifi_alac`), kept across core-B image swaps (ape ↔ main) until power-off. A dead block (a
  timeout or a stalled bus: `hifi_busy` set when core B was reloaded) stops all three.
- Core A: nothing new (the switch and the log drain are FLAC's); FW_CODE unchanged (139352 of
  161824 B). Core B: `main` 107080 → 110948 B (of 131072), `ape` 20952 → 26124 B.

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

APE and ALAC (b_hifi_ape.c, b_hifi_alac.c, b_hifi_fn.c):

```
hifi: APE self-test passed (level <L>, version >= 3980|< 3980, 20 jobs, <t> us): NN filters on HIFI[, every sample verified]
hifi: APE self-test[ retry] (level <L>, version ...) frame <f> job <j> channel <c> sample <i>: hifi <x>, software <y>
hifi: APE diff hifi-sw from sample <i>: <d_i> ... <d_i+11>
hifi: APE diff hifi-sw at samples <k+32>..<k+39>: <d> x8
hifi: APE self-test retry (another reset, 20 ms after each clear start): passed|failed|timed out
hifi: APE self-test failed: level <L> in software
hifi: ALAC self-test passed (5 jobs, <t> us on the block): its LPC on HIFI[, every sample verified]
hifi: ALAC self-test job <v> (order <o>, quant <q>, <bps>-bit) sample <i>: hifi <x>, software <y>
hifi: ALAC self-test failed: its LPC in software
hifi: check: APE job <j> (level <L>, channel <c>, frame <f>) sample <i>: hifi <x>, software <y>: level in software
hifi: check: ALAC job <j> (order <o>, quant <q>, <bps>-bit) sample <i>: hifi <x>, software <y>: ALAC's LPC in software from now on
hifi: verify: APE|ALAC job <j> (...) sample <i>: hifi <x>, software <y> (counting on)
hifi: APE|ALAC job <j> (<n> samples) not done after <t> us (RX done|not done): the block off until power-off
hifi: core B was reloaded while it used the block (a stalled bus?): the block off until power-off
ape: track <n>, level <L>, <bits>-bit: core B <M> M cycles/s, NN filters on HIFI|in software (<jobs> jobs, wait <W> M/s)
alac: track <n>, <bits>-bit: core B <M> M cycles/s, LPC <hw> of <n> channels on HIFI (wait <W> M/s), orders <mask>
hifi: track <n>: APE|ALAC hifi=<mode>, <state>, <jobs> jobs, <c> checked, <m> mismatched, <r> redone, <s> silenced, max <t> us, <p> polls
```

`<state>`: `ok|failed|untested|dead` (APE: the track's level and version class; dead: the block,
for all three functions); `max`: the longest job's wait. `silenced`: APE blocks played as silence after a failure mid-frame (§4.2). `channels`: ALAC's
channel-frames with an LPC (orders 1..31); `orders` a bit mask (bit n = order n).

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
`-machine hifi-fault=wrong|hang|acc32[@N]` (FLAC); APE's and ALAC's below.

Before this change the model (2026-10-14, "Bench groundwork", from the SDK headers and stock's
decode in QEMU) computed the same FLAC function with no checks.

### APE and ALAC (branch hifi2)

The same section models stock's other two functions ("APE and ALAC" in echo_mini.c):

- **APE**: per channel and filter level, the state stock relies on inside the block (16-bit
  coefficients, the saturated history, the adapt values, the average), zeroed by a start with
  APE_CFG bit 21 set (both channels); each TX word runs through the level's filters with libdemac's
  arithmetic (§3b.1, [inferred]). No output word is held back: the RX FIFO has each result at once.
- **ALAC**: the coefficients from 0x9000 at the start, adapting in the job; words 0..order pass
  through; the rest per ffmpeg's `lpc_prediction` / the SDK's `Alac_Decode_Lpc` (§3c.1).
- **VIOLATION**: an APE_CFG other than stock's 8 values; an APE data job with no clear since the
  block's reset, with another APE_CFG than the clear's, with the clear bit still set, with INTCR
  bit 15 set, of 0 or more than 4096 words; a clear start other than type 0 with 4096, or TX data in
  it; ALAC_CFG with order outside 1..30, quant over 11, bps outside 1..32 or other bits; an ALAC
  job of under 2 or over 4096 words, with INTCR bit 15 clear, or a coefficient outside 16 bits; CPU
  writes to the APE windows, past the 64 ALAC coefficients, or to the ALAC data window.
- **Faults** (`-machine hifi-fault=KIND[@N]`, N counting that function's jobs from the boot):
  `ape-wrong` (every output + 1), `ape-hang` (no output, the RX transfer never ends), `ape-noclear`
  (the clear does nothing), `ape-drift` (+1 from the 1025th sample after a clear),
  `alac-wrong` (every predicted sample + 1), `alac-hang`, `alac-noadapt` (no coefficient adaption).
  A hung job's RX transfer now never completes (`dma_block_done`: the RX request never comes), as
  on the silicon; before, the model's DMA read zeros out of the empty FIFO and finished.
- `hifi-ape:` / `hifi-alac:` lines (under `-d unimp`) name each APE_CFG and ALAC order the first time.
- **Configuration changes without a reset (ape-fix)**: VIOLATION for an APE clear with another
  APE_CFG than the first since the block's reset (stock: one configuration per decoder open, §3b.4);
  and, as the device does (§3b.5, [inferred]), a filter slot whose order shrank or whose fraction bits
  changed since the reset passes its input through (`hifi-ape: APE_CFG … filter <i> … passes its input
  through`). With round3's firmware on APELEVELS the model gives the device's two lines value for value.
- **Stock on the model** [trace, 2026-10-18]: APE16, APE16H, APE16X, APE24, ALAC16, ALAC24: 0
  VIOLATION; every frame stock plays equals the reference, with no event. (Stock leaves out the first
  12288 frames of an APE track and 4096 of an ALAC one, silence in their place: the capture is all
  zeros up to there; its slower runs end at the bench's 240 s before the track does; it drops the
  last 2868 frames as with FLAC.)

## 6. Benefit, measured in QEMU

### 6.1 FLAC

Core B instructions per output frame (`tests/bench.py`, `-icount 2`, `PROF_EXACT`, storage latency
zero, as m3's budget run; the steady-state window), and the wait loop's share (its turns, logged as
`polls`, times its 19 instructions):

| File | hifi=off | hifi=auto | of which waiting | auto without the wait | LPC's share of software FLAC | hifi=verify |
|---|---|---|---|---|---|---|
| CD16 (16/44.1, flac -5) | 137 | 101 | 6.9 | 94 | 31 % | 191 |
| HR24_96 (24/96) | 163 | 112 | 6.7 | 105 | 35 % | 210 |
| HR24_192 (24/192) | 158 | 111 | 6.4 | 105 | 34 % | 210 |
| GAPLESS (2 × 16/44.1) | 136 | 101 | | | | 200 |

The wait is the model's (DMA2 at 200 MB/s, the block instant): an assumption, the silicon's speed is
unknown. **The LPC restore is about a third of FLAC's core-B instructions** (31 % at 16 bits, 35 % at
24); the rest is the bit reader, Rice decoding, frame parsing, stereo decorrelation and the output.
So the block can take at most a third off FLAC. For comparison, ReChord's software FLAC already ran
on 1.6–1.8× fewer core-B instructions than stock and RECHO with their HIFI (`docs/PARITY_PLAN.md` in
the project repo).

In clock terms (the load governor doubles B's clock above 60 % load and halves it below 20 %:
96/48/24/12 MHz), at one instruction a cycle: 16/44.1 needs 6.0 M instructions/s in software and
4.5 M with the block: both run at 12 MHz, the lowest step, so **no change for CD-quality FLAC**.
24/96: 15.6 M → 10.8 M, **48 → 24 MHz**. 24/192: 30.3 M → 21.3 M, **96 → 48 MHz**. That holds only
if the block is at least as fast as the software it replaces: if it takes more than about 8–10 core-B
cycles a sample at order 8, it saves nothing (the wait is busy time). Its speed is the first thing
the device must show (§7).

`verify` costs about 40 % more than software alone: a measurement mode, not one to keep.

### 6.2 APE and ALAC (branch hifi2)

Core B instructions per output frame, as for FLAC (`tests/bench.py`, `-icount 2`, `PROF_EXACT`,
storage latency zero, the steady-state window; work/hifi2 `measure.sh`, `table.py`); "waiting" is
the wait loop's turns (`polls` in the track line) times its 25 instructions; insane through a
measurement build that lifts core A's cost gate (`audio.c` `k_ape_cost`: core A skips insane), its
`off` and `verify` runs at `-icount 0` (4× core B's instruction rate: insane is beyond 96 MHz in
software). Every run bit-exact (verify at insane: what played before the bench's 120 s ended),
0 VIOLATION. Build `997f27a`.

| File | rate | hifi=off | hifi=auto | of which waiting | auto without the wait | saved | hifi=verify |
|---|---|---|---|---|---|---|---|
| APE fast (16/44.1) | 44.1 k | 394 | 396 | 0 | 396 | (no filters) | 394 |
| APE normal | 44.1 k | 616 | 408 | 10.5 | 397 | 35 % | 632 |
| APE high | 44.1 k | 920 | 407 | 10.7 | 396 | 57 % | 954 |
| APE extra high | 44.1 k | 2390 | 409 | 10.7 | 398 | 83 % | 2390 |
| APE insane | 44.1 k | 10080 | 406 | 10.7 | 395 | 96 % | 10149 |
| APE normal 24/96 | 96 k | 642 | 429 | 10.7 | 418 | 35 % | 675 |
| APE extra high 24/96 | 96 k | 2480 | 428 | 10.5 | 417 | 83 % | 2509 |
| APE mono, high | 44.1 k | 417 | 157 | 5.3 | 151 | 64 % | 437 |
| APE 8-bit, normal | 44.1 k | 546 | 350 | 10.5 | 339 | 38 % | 576 |
| APE gapless pair, normal | 44.1 k | 620 | 406 | 10.5 | 395 | 36 % | 651 |
| ALAC 16/44.1 | 44.1 k | 332 | 169 | 10.7 | 159 | 52 % | 359 |
| ALAC 24/96 | 96 k | 424 | 233 | 10.1 | 223 | 47 % | 446 |

- **APE: the block takes the whole NN-filter stage**: every level costs what fast costs (≈ 396),
  i.e. what is left is the range decoder (≈ 153 a frame) and the predictor (≈ 215), libdemac's,
  plus ≈ 30 for the output and the runtime [QEMU hotblocks, `tools/hotprof.py`]. The jobs' own
  cost (set-up, DMA, the checks) is 1–3 instructions a frame at 256 blocks a job; larger jobs would
  save at most that (not worth b_ape.c's input bound, `IN_KEEP`). The first frame's longer check
  costs about 0.07 s of software at extra high, once a track (outside the steady window).
- **ALAC**: with the CLZ change below, the LPC is half of ALAC's software (52 % / 47 %); the Rice
  decoder `dyn_decomp` is most of the rest (≈ 120 a frame), then the element parsing and the
  output copy.
- **`hifi=off` costs nothing**: off is the software (APE fast with the block on: 396 against 394;
  the differences between `off` here and before the branch, ±1 %, are the window's run-to-run
  spread), and the block is not touched (m39 k).
- **`verify`** costs a few per cent more than software alone: a measurement mode.

**Found while profiling, taken: ALAC's `lead()`** (Apple's ag_dec.c: a bit loop counting leading
zeros, up to 32 turns a call, ~90 instructions a sample in `lg3a` alone; Apple's comment asks for
CLZ) is `__builtin_clz` (Cortex-M3 CLZ): the same result for every input (32 for 0), **ALAC
software 648 → 332 (16/44.1) and 744 → 424 (24/96)**, m5 and m39 bit-exact.

**In clock terms** (the governor: up above 60 % load, down below 20 %; 96/48/24/12 MHz), at one
instruction a cycle [inferred: CPI 1], counting the block's time at an assumed 1 tap a core-B
cycle [device: its speed is unknown]:

| File | software M/s → MHz | block path (CPU + block at 1 tap/cycle) M/s → MHz |
|---|---|---|
| APE normal 16/44.1 | 27.2 → 48 | 17.5 + 1.4 = 18.9 → 48 (no change) |
| APE high 16/44.1 | 40.6 → 96 | 17.5 + 5.6 = 23.1 → **48** |
| APE extra high 16/44.1 | 105 → **beyond 96** | 17.6 + 25.4 = 43.0 → **96** (48 from 2.3 taps a cycle) |
| APE insane 16/44.1 | 445 → beyond 96 (core A skips it) | 17.4 + 137 = 154 → beyond 96: real time needs **≥ 1.75 taps a cycle** at 96 MHz |
| APE normal 24/96 | 61.6 → 96 | 40.1 + 3.1 = 43.2 → 96 (no change) |
| APE extra high 24/96 | 238 → beyond 96 | 40.0 + 55.3 = 95.3 → 96 at 99 %: **needs ≥ 1 tap a cycle**, 2 for 70 % |
| ALAC 16/44.1 | 14.6 → 48 | 7.0 + 0.5 = 7.5 → **24** |
| ALAC 24/96 | 40.7 → 96 | 21.4 + 1.2 = 22.6 → **48** |

(ALAC's "tap": order × channels × rate, orders 4 and 8 in the corpus.) Extra high at 44.1 kHz is
past 96 MHz in software today: core A admits it (its gate assumes 240 M/s), so on the device it
cannot keep up without the block [inferred from the instruction count; device: never measured].

**Break-even** (the block saves nothing when its time per tap is the software's per tap, the wait
being busy time): saved instructions per channel-sample ÷ taps:

| Function | taps a channel-sample | saved a channel-sample | the block must beat |
|---|---|---|---|
| APE normal | 16 | 110 | 6.8 core-B cycles a tap |
| APE high | 64 | 262 | 4.1 cycles a tap |
| APE extra high | 288 | 996 | 3.5 cycles a tap |
| APE insane | 1552 | 4843 | 3.1 cycles a tap |
| ALAC (orders 4–8) | 4–8 | 87 (16-bit), 100 (24-bit) | about 87 cycles a sample (11–22 a tap) |
| FLAC (§6.1: (137 − 94) / 2, (163 − 105) / 2) | 3–32 | 21 (16-bit), 29 (24-bit) | about 21–29 cycles a sample |

The NN filter is a multiply-accumulate and an adapt add a tap: a block doing one tap a cycle is
3–7× under break-even; one at 1/4 tap a cycle saves nothing at extra high and insane.

**Proposals (not shipped)**:
- **Overlap the job with the CPU's work.** Stock sleeps while a job runs; nothing in the block's use
  stops core B decoding meanwhile (the next chunk's residuals for APE, the other channel's Rice
  decoding for ALAC). Same jobs, same values; core B's time becomes max(CPU, block) instead of the
  sum: at 1 tap a cycle APE extra high 43 → 25 M/s (48 MHz), insane real time from 1.43 taps a cycle.
  QEMU cannot show it (its block is instant); it needs the device's speed (§7.8) and pipelining in
  libdemac's decode_chunk.
- **Admit insane** (core A's `k_ape_cost` gate) when the APE self-test for 5000 has passed — only
  once the device shows the block at ≥ 1.75 taps a cycle (§7.10).
- **The software that is left**: libdemac's predictor (≈ 215 a frame, more than half of APE with
  the block) keeps its state in a struct through every sample; ALAC's output is decoded into a
  buffer and copied (≈ 11 a frame). Not HIFI work; candidates for the next profiling round.

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
6. **APE and ALAC self-tests** on the silicon (the first APE track of each level, the first ALAC
   track): `hifi: APE self-test passed (level <L>, version >= 3980, 20 jobs, <t> us on the block)` /
   `hifi: ALAC self-test passed (5 jobs, <t> us ...)`, or the failing frame / job / channel / sample
   with both values. A failure at the APE self-test's first frame after sample 600 (the constant
   run) points at the dot product's width; one in its second frame at the clear.
7. **APE and ALAC bit-exactness on real music**: one session with `hifi=verify`, APE normal, high
   and extra high (16- and 24-bit), ALAC 16 and 24: every `hifi: track <n>: APE|ALAC hifi=verify, …`
   line `0 mismatched`; a `hifi: verify: APE job …` line is a finding (frame, channel, sample, both
   values).
8. **The block's speed per function** (the break-even, §6): `hifi=off` and `hifi=auto` on the same
   tracks: `ape: track <n>, level <L>, …: core B <M> M cycles/s, NN filters on HIFI (<jobs> jobs, wait
   <W> M/s)`, `alac: track …`, and each `max <t> us`. Cycles a tap = W × 10^6 / (taps × channels
   × rate); the block saves only below 6.8 (normal), 4.1 (high), 3.5 (extra high), 3.1 (insane)
   cycles a tap, and below about 87 cycles a sample for ALAC (§6.2).
9. **No timeout, no silence**: no `not done after`, every APE line `0 silenced`.
10. **Insane**: core A still skips it (`audio: APE level 5000 at 44100 Hz is too slow for core B`);
    whether to admit it with the block on depends on item 8 (§6.2: real time at 96 MHz needs the
    block at 1.75 taps a cycle or more; 1.43 with the overlap proposal). A proposal, not shipped.

11. **APE after ape-fix** (the same files as 2026-10-18, in the same order: APE_CD16_C2000, C3000,
    C4000, APE_HR24_96, APE_HR24_X, one boot, `hifi=auto`): every level's `hifi: APE self-test passed`
    (4000 included), every `hifi: track <n>: APE …, ok, … 0 mismatched`, extra high `NN filters on HIFI`
    with `core B` well under 96 M cycles/s and no underrun. That confirms §3b.5's cause (a configuration
    change with no reset). If the 4000 self-test still fails right after its reset: the `hifi: APE diff`
    lines tell the candidates apart (the 256/13 filter alone gives diffs from sample 2 of -42 21 -1 15 713
    -62 …; a 3.97-style 32/10 filter alone -42 19 0 10 812 …; 256 vs 32 taps differ from sample 35), and
    the `retry` line tells whether stock's ~15 ms between the clear and the first job matters (passed:
    the clear runs on after its start; the decode path then needs that gap too).

No step writes anything but the log and RECHORD.CFG; the block is off with `hifi=off` (all three
functions). APE extra high 16/44.1 is a good first test of the benefit: in software it needs more
than 96 MHz (§6.2), so with `hifi=off` it is expected to underrun on the device, and with the block
on to play.
