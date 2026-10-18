# Fewer core-B instructions for every format: the HIFI block where it helps, software where it doesn't

The goal is the same output with fewer computations on core B. That means less busy time, a lower
load-governor step and a longer battery life, on the music people actually play. This document holds
four things. Section 1 profiles every format on core B in QEMU. Section 2 maps each hot spot onto
the HIFI block's five functions, or says why it does not fit. Section 3 lists the software savings
this branch implemented, each bit-exact. Section 4 describes the device probe that answers what the
FFT and MAC functions are. Section 5 gives the ranked plan and the order of work once the probe has
answered.

`docs/HIFI.md` describes the block itself (registers, clocks, DMA2, the FLAC function). This document
does not repeat it.

## 0. Sources and tags

| Tag | Meaning |
|---|---|
| [SDK] | RKNanoD SDK source, `firmware/rockchip/driver/hifi/` (`hifi.c`, `hifi.h`, `Hw_hifi.h`, `hifi_fft.c/h`, `hifi_mac.c/h`) |
| [SDK-bin] | the SDK's prebuilt libraries (`audio/*/*.lib`), read with `strings`: the FFT/MAC macros (`FFT_DATI_ADD`, `FFT_DATR_ADD`) appear only as DWARF macro text, in no code |
| [code] | stock MINIV390's core-B images (as in HIFI.md) |
| [measured] | QEMU, this branch: `tools/hifimap_prof.py` (project repo) with `tests/bench.py`, `-icount 2`, `PROF_EXACT`, and the coreprof plugin's new `hot=1` per-instruction histogram of core B mapped onto the image's ELF (`tools/hotprof_b.py`). One instruction is one QEMU instruction. Device cycles are that count times the device's CPI, which the probe measures (section 4) |
| [inferred] | our reasoning from the above. Not proven |
| [device] | only the device can answer. Not measured |
| [standard] | the codec's own specification or conformance standard |

## 1. Where core B's time goes, per format [measured]

Corpus: the shared one (`work/disks/corpus`, a synthesised music-like signal) plus a dense
worst-case set from `tests/hifimap_corpus.py` (project repo). The dense set is the same signal
with pink noise at about −22 dBFS and hi-hat bursts, so every encoder spends its whole bitrate up to
its band limit. Its encoders are LAME 320/128 CBR, Apple AAC-LC 256k and HE-AAC 64k, Vorbis q10
(ffmpeg's encoder: about 190 kbit/s on this signal, not libvorbis's ~500) and libopus 256k. Its
references come from the host builds of the firmware's own decoders.

Each value is the steady-state number of core-B instructions per output frame. M insn/s is that
value times the rate: the MIPS the format needs at CPI 1. The governor step is the lowest of
12/24/48/96 MHz at which the load stays at or under 60 % (`b_main.c choose_clock` doubles the clock
above 60 % and halves it below 20 %). "over" means more than 96 M insn/s, more than core B's top
clock. The device's CPI is above 1 by an unknown amount (the probe's `cpi` line measures it), so a
format just under a step boundary in QEMU is likely above it on the device.

"Before" is ReChord `round2` 460f900. "After" is this branch. Each "after" value was measured on this
branch's build and is bit-exact against the same reference as "before" (`exact=True` in
`bench/hifimap*.json`).

| Format (file) | Before: insn/frame, M insn/s, step | After | Top functions (before, share of core B's instructions) |
|---|---|---|---|
| FLAC 16/44.1 (CD16, hifi=auto) | 99, 4.4, 12 | 99 | rice decode 49 %, residual loop 16 %, output 13 %, HIFI wait 8 %, memcpy 6 % (the model's block is instant; software alone: 137 = 6.0 M, HIFI.md §6) |
| FLAC 24/96 (HR24_96) | 111, 10.7, 24 | 111 | rice 46 %, residual 21 %, output 12 %, **memcpy 10 %** (software alone: 163 = 15.6 M, 48 MHz) |
| FLAC 24/192 (HR24_192) | 110, 21.0, 48 | 110 | the same (software alone: 158 = 30.3 M, 96 MHz) |
| WAV 16/44.1 (WAV16) | 80, 3.5, 12 | **18, 0.8**, 12 | b_pcm_read 81 % (a byte loop through a 192-byte buffer), memcpy 16 % |
| MP3 LAME −V2 (MP3META) | 474, 20.9, 48 | 465, 20.5, 48 | synth_full 56 %, dct32 14 %, III_decode 10 %, output 7 %, aliasreduce 6 %, imdct_l 4 % |
| MP3 128 CBR, dense | 589, 26.0, 48 | (≈ 577) | synth 45 %, III_decode 15 %, dct32 11 %, imdct_l 11 % |
| MP3 320 CBR, dense | 665, 29.3, **96** | **653, 28.8, 48** (at the line) | synth 40 %, III_decode 18 %, imdct_l 12 %, dct32 10 %, output 5 % |
| AAC-LC 256 (Apple, dense) | 578, 25.5, 48 | **500, 22.1**, 48 | passf4pos (the FFT) 17 %, faad_imdct 10 %, quant_to_spec 9 %, output 8 %, ifilter_bank 8 %, memcpy 7 % |
| AAC-LC 256 (ffmpeg, M4AGAP) | 585, 25.8, 48 | 501, 22.1, 48 | the same |
| HE-AAC 64 (Apple, dense) | 1244, 54.9, 96 | **1075, 47.4**, 96 | dct4_kernel (SBR's DCT-IV) 26 %, hf_generation 14 %, qmf_synthesis_64 12 %, hf_adjustment 10 %, qmf_analysis_32 7 % |
| HE-AAC v2 32 (M4AHEV2) | 1260, 55.6, 96 | 1103, 48.6, 96 | ps_decode 32 %, dct4_kernel 21 %, qmf_synthesis 12 % |
| Vorbis (OGGGAP) | 376, 16.6, 48 | — | mdct_backward 36 %, floor1 8 %, codebook 8 %, window 7 %, ov_read 7 % |
| Vorbis q10, dense | 445, 19.6, 48 | — | mdct_backward 30 %, codebook 20 %, floor1 7 % |
| Opus 128k (OPUSGAP) | 651, 31.2, 96 | — | opus_fft_impl 27 %, comb_filter 14 %, clt_mdct_backward 13 %, deemphasis 6 % |
| Opus 256k, dense | 709, 34.0, 96 | — | the same. **Not bit-exact: core B ends the track at 3.99 s** (section 6) |
| WMA v2 128k | 260, 11.5, 24 | — | FFT pass 23 %, decode_block 20 %, imdct_half 16 %, window 13 %, fft16/fft8 7 % |
| WMA Pro 44.1 | 243, 10.7, 24 | — | decode_frame 27 %, FFT pass 24 %, imdct_half 17 %, fft16/8 8 % |
| WMA Pro 24/96 | 207, 19.9, 48 | — | decode_frame 27 %, pass 25 %, imdct_half 19 %, fft16/8 9 % |
| WMA Lossless 16/44.1 | 4463, **196.8, over** | **2520, 111.1, over** | decode_frame (the LMS cascade) 68 %, **memset 29 %** |
| WMA Lossless 24/48 | 1075, 51.6, 96 | **552, 26.5, 48** | **memset 50 %**, decode_frame 45 % |
| WavPack (WVGAP, normal) | 488, 21.5, 48 | — | unpack_samples (decorrelation) 56 %, get_words 24 %, read_code 15 % |
| WavPack (WVFMTS: -hh, mono, float, hybrid, 24/96 -x) | 595, 26.3, 48 | — | unpack_samples 61 %, get_words 14 % |
| ALAC 16/44.1 | 648, 28.6, 48 | — | dyn_decomp (adaptive Golomb) 68 %, unpc_block (predictor) 27 % |
| ALAC 24/96 | 750, 72.0, 96 | — | dyn_decomp 60 %, unpc_block 27 % |
| APE normal | 607, 26.8, 48 | — | apply_filter_16_11 36 %, predictor 35 %, entropy 25 % |
| APE high | 913, 40.3, 96 | — | predictor 23 %, vector_sp_sub/add 41 %, entropy 17 %, filter_64 16 % |
| APE extra high | 2384, 105.1, over | — | vector_sp_sub/add 69 %, predictor 9 %, filter_256 9 % |
| DSD64 → 176.4k | 567, **99.9, over** | **486, 85.8, 96** | dsd_run 98 % |
| DSD128 → 176.4k | 1066, 188.0, over | **872, 153.8**, over | dsd_run 98 % |
| DSD256 → 88.2k | 2577, 227.3, over (**underruns even in QEMU**) | **2014, 177.6**, over (no underrun in QEMU; exact) | dsd_run 98 % |

ALAC and APE are the `hifi2` agent's work (their HIFI functions). They appear here only for the
ranking.

## 2. The hot spots against the five functions

The functions [SDK `hifi.h`]: 0/1 APE L/R filter, 2 FFT, 3 ALAC, 4 MAC, 5 FLAC. APE, ALAC and FLAC are
fixed decoder stages whose use stock shows (HIFI.md). FFT and MAC have no user anywhere [SDK-bin,
code]. Their format, scaling, rounding, start sequence and speed are unknown until the probe runs
(section 4).

### 2.1 The per-job cost model (for the break-even speeds)

A job's CPU cost is the setup (about 8 register writes and an INTSR poll loop), plus the data. For
FFT that is N words written into each of the two windows and read back. For MAC it is `order` words
into each window and two result registers read. Write the CPU's cost per window word as `w` cycles.
In QEMU `w` is 1.54, the model's virtual time and not a measurement. The probe measures `w` (its
`win ... wcyc rcyc` lines). The planning value used below is **`w = 3`** [inferred: an AHB
peripheral RAM behind core B's bus].

The wait counts as busy time: core B polls, and the block runs on core B's clock [inferred, HIFI.md
§2.2; the probe's 4 clock passes test it]. So a job saves `S − (setup + 4N·w + T)` cycles, where S is
the software it replaces and T is the block's own time. The **break-even block speed** is the T at
which that saving is zero.

### 2.2 FFT: power-of-two IMDCTs (AAC, Vorbis, WMA, WMA Pro)

These decoders compute an N-point IMDCT through an N/4-point complex FFT:
pre-twiddle → FFT → post-twiddle. The twiddles and the windowing/overlap stay on the CPU. Only the
complex FFT moves to the block. The probe's sizes are 64..1024 [SDK `hifi_fft.h` FFT_MODE_64..1024].

| Codec | Transform the block would do | Jobs per second (stereo) | Software it replaces (before) | Per-job overhead (w = 3) | Break-even T | Saving if T is small | Step change? |
|---|---|---|---|---|---|---|---|
| AAC-LC 44.1k | FAAD `cfftb` of 512 points (long blocks); 64 points × 8 (short) | 86 (long) | passf4pos: 4.3 M/s = **49.5k insn per 512-point FFT** | 6.2k | < 43k cycles = **≥ 0.054 butterflies/cycle** (2304 butterflies) | ≈ 3.5 M/s, −14 % of AAC-LC (22.1 → ≈ 18.6 M/s after the -O2 change) | no (48 MHz needs ≤ 14.4) |
| Vorbis 44.1k (blocks 256/2048) | Tremor's `mdct_backward` butterflies replaced by a 512-point (long) / 64-point (short) FFT and twiddles | 86 (long) | mdct_backward 5.9 M/s (≈ 68k per long block; the FFT-replaceable part ≈ 60 %) [inferred] | 6.2k | < 35k cycles | ≈ 3 M/s, −18 % (16.6 → ≈ 13.6) | **48 → 24 MHz** for OGGGAP-like files (13.6 ≤ 14.4, at the line); q10 stays 48 |
| WMA v2 44.1k (frames 2048) | Rockbox `ff_imdct_half`'s N/4 FFT: 512 points | 86 | pass + fft16 + fft8: 3.45 M/s = 40k per FFT | 6.2k | < 34k cycles | ≈ 2.9 M/s, −25 % (11.5 → 8.6) | no (12 MHz needs ≤ 7.2) |
| WMA Pro 24/96 | the same, 512 points | ≈ 94 | 6.7 M/s | 6.2k | < 65k | ≈ 5.7 M/s, −29 % (19.9 → 14.2) | **48 → 24 MHz** (at the line) |

**Exactness:** none of these can be bit-exact against today's output. Each decoder's own
fixed-point FFT rounds in its own way, and so will the block. The standard is the codec's tolerance:
- AAC: the conformance criteria of ISO/IEC 14496-26 (RMS and maximum difference against the
  reference decoder) [standard; the numbers must be taken from the standard before an implementation,
  and checked on its conformance streams]
- Vorbis and WMA: no normative tolerance [standard: the Vorbis I spec defines a float decode]. The
  bound would be ours: today's fixed-point output as the reference, with a stated RMS/max bound
  [inferred].

The conversion depends on the block's scaling, which is unknown. If the block's IFFT is unscaled
int32 (the model's candidate), FAAD's and Rockbox's ranges pass through unchanged. If it scales per
stage or has fewer than 32 bits, the inputs need pre-shifting, and precision is lost (up to 9 bits
at 512 points). The probe's decoder reports the scaling and the residual (section 4).

### 2.3 FFT: what does not fit

- **MP3**: the IMDCT is 36/12-point and the polyphase DCT is 32-point. Neither is a power of two of
  at least 64 [SDK FFT_MODE_64 is the smallest]. The SoC's separate SYNTH/IMDCT36 blocks are not
  HIFI, and RESEARCH/PLAN in `work/hwdecode` rule them out: SYNTH produces 16-bit output, IMDCT36
  is about 11-bit accurate.
- **Opus/CELT**: the MDCT's FFTs are 480/240/120/60 points (mixed radix 2·3·4·5), not powers of
  two. The only route is Bluestein's chirp-z: a 480-point DFT as a 1024-point FFT, a pointwise
  multiply and a 1024-point IFFT. That is 2 jobs and about 2k complex multiplies on the CPU per
  480-point FFT, against `opus_fft_impl`'s ≈ 83k instructions [measured: 8.3 M/s over 100 FFTs/s].
  Estimated saving 40k per FFT, 4 M/s, −13 %, which would take Opus 128k **96 → 48 MHz** (27 ≤ 28.8).
  This is **speculative**: two chained 1024-point transforms need about 24 bits of internal
  precision to pass Opus's conformance comparison (RFC 6716 test vectors, `opus_compare`)
  [standard]. A 1/N-scaled forward FFT would lose 10 bits. It is only worth trying if the probe shows
  the block wide and unscaled.
- **HE-AAC's SBR**: `dct4_kernel` runs 32-point FFTs, below the minimum size. Two of them pair into
  one 64-point FFT: interleave x and y, take Z = FFT64, then X[k] = (Z[k] + Z[k+32])/2 and
  Y[k] = (Z[k] − Z[k+32])·W64^−k / 2, which costs 32 complex multiplies on the CPU per pair. That is
  689 jobs/s for the stereo 64-band synthesis. With a 1k-cycle overhead per job the saving is
  about 3 M/s, −6 % of HE-AAC: no step change. The -O2 build already took −14 % (section 3).

### 2.4 MAC: no decoder hot spot fits

A MAC job yields one dot product (MAC_RSH:MAC_RSL [SDK `hifi_mac.c`]) over operands the CPU must
first place in the block's windows. It pays only for a long dot product over data that stays
resident in the block. No decoder here has one:

| Candidate | Why not |
|---|---|
| MP3 polyphase window (synth_full, 40–56 %) | 16-tap dot products, one per output sample: each job writes 16 + 16 words and reads 2 for 16 MLAs (≈ 32 cycles in software). A loss at any block speed |
| DSD→PCM FIR (dsd_run, 98 %) | The input is 1 bit a tap. The table method sums 8 taps per lookup (3 instructions). A MAC needs one window word per tap: 8× the data. A loss |
| WavPack decorrelation, APE/WMA Lossless LMS, CELT comb filter, AAC TNS | Adaptive or recursive per sample (each output depends on the previous one, and the coefficients change every sample). One job per sample would carry the whole history each time. A loss. APE's own filter function hard-wires APE's adaptation rule, which WMA Lossless's update (update_speed, the >>2/>>1 de-emphasis) does not match [SDK `hifi_ape.c` APE_CFG fields] |
| Resampling, a long EQ | Not in the decode paths today (ReChord plays at the file's rate). A future long FIR is the one use a MAC could have, if the probe shows it fast |

### 2.5 The stock functions (for completeness)

FLAC LPC is on the block in `round2`: one job per subframe, 4096 words. The break-even is about 8–10
core-B cycles a sample at order 8 (HIFI.md §6). The probe now times it against core B's own restore
of the same data, at all four clock steps (its `flac` lines). ALAC and APE belong to `hifi2`.

## 3. Software savings implemented (this branch, every one bit-exact) [measured]

| Change | File | What it does | Saving | Proof |
|---|---|---|---|---|
| WMA Lossless: the per-sample memset | `coreb/wma/lossless/wmalldec.c` (`lms_update`) | FFmpeg zeroes `lms_updates` from `recent + order` to the end of the array, up to 1 KiB, after **every sample of every filter**. Only `[recent + order, top)` can be nonzero, so only that span is zeroed. `top` bounds every nonzero entry (the writes since the last update stay below max(top, 2·order)). After each update the array equals FFmpeg's. The comment in the source gives the argument | 16-bit −24 %, 24-bit −47 % | WMALL, WMALL24 bit-exact (lossless: the original PCM) |
| WMA Lossless: the LMS dot product | the same (`scalarproduct_and_madd_int16/32`) | 4 taps a turn, and a zero residue (mul = 0) skips the adds of 0. The same sums (unsigned, as FFmpeg's) and the same coefficients | a further −26 % at 16-bit | the same |
| | | **Together**: WMALL 16/44.1 197 → 111 M/s (still over 96); WMALL 24/48 51.6 → 26.5 M/s (**96 → 48 MHz**) | | |
| DSD: tables in the stream's bit order, word reads | `coreb/dsd/dsd_dec.c` | LSB-first (DSF) tables are built for their own bit order (tab[k][v] = the MSB-first table at rev8(v)), so no per-byte bit reversal is needed, and the silence fill is 0x96. The window starts at a multiple of the step, so it is read a word (DSD128/256) or a halfword (DSD64) at a time. On core B, one table pointer per byte gives 3 instructions a lookup (inline asm; GCC folds the pointers into 5); the host build uses the C. Locals instead of the struct in the loop. The same integer sums of the same entries | DSD64 −14 %, DSD128 −18 %, DSD256 −22 % | DSD64, DSD128, DSD256, DSD (DSF64 + DFF64 + DSF128) bit-exact; DSD64 99.9 → 85.8 M/s (now under 96); DSD256 no longer underruns in QEMU |
| FAAD's hot files at -O2 | `Makefile` (`COREB_FAAD_O2`) | cfft, mdct, filtbank, specrec, output, sbr_dct, sbr_qmf, sbr_hfgen, sbr_hfadj, ps_dec at -O2 (the rest -Os). The same integer code. aac.bin 113.7 → 119.3 KiB of 128 | AAC-LC −13.5 %, HE-AAC −13.6 %, HE-AACv2 −12.5 % | M4AGAP, AAC_256, AAC_HE, M4AHEV2 bit-exact |
| PCM: straight from the ring | `coreb/b_pcm.c` | 16- and 24-bit little-endian (WAV, AIFC sowt) read from the input ring in place, a few instructions a sample. Other formats keep the byte loop | WAV 16/44.1: 80 → 18 insn/frame (−78 %) | WAV16 bit-exact; PCMFMTS bit-exact |
| MP3: the output loop | `coreb/b_mp3.c` | locals and pointers instead of `m->pos` through memory each sample | −1.8 % (MP3 320: 29.3 → 28.8 M/s, onto the 48 MHz line at CPI 1) | MP3META, MP3_320 bit-exact |

Measured but not done here (the next software steps, in order of value):
1. **WMA Lossless 16-bit is still 111 M/s**, over core B's 96 MHz, so it cannot play in real time on
   the device. What remains is the LMS cascade (87 %). A hand-written Thumb-2 loop could reach about
   5 instructions a tap, against the C's ~6.5 [inferred], which is −20 % more. Only a faster core-B
   clock makes it real-time.
2. **DSD128/256 stay over 96 MHz** with the same filter. A two-stage decimator (a short table FIR,
   then a polyphase half-band), or 88.2k output for DSD128, would cut the cost 2–4× but **changes the
   output**. That is the user's decision, not a bit-exact change.
3. FLAC: the input copy (`memcpy` 10 % at 24-bit: `b_in_read` into dr_flac's buffer). Decoding from
   the ring in place is −10 % for hi-res FLAC.
4. ALAC: dyn_decomp is 60–68 % (an ffmpeg-style Golomb reader; `hifi2`'s area).

## 4. The probe (Developer > HIFI probe (test)) — the device's answers for the next flash

### 4.1 What runs

Core A (`audio/hifi_probe.c`, in the Developer overlay) stops the player. It then runs four passes,
each on a fresh core-B main image (`audio_b_fresh`): the whole probe at PLL / 1 (96 MHz), then the
timing jobs at / 2, / 4 and / 8. It logs every line core B writes and loads a fresh core B at the
end, because the probe leaves the block gated. Core B (`coreb/b_hifi_probe.c`) runs these steps:

1. **cpi**: cycles for 9000 instructions of ALU, loads and stores in core-B RAM, and loads from core
   A's SRAM (the input ring). These turn every QEMU count in section 1 into device cycles.
2. **flac**: `b_hifi_open` (the self-test, as for playback), then 6 jobs (orders 3/8/12/32, 16/24-bit,
   256..4608 samples) through `b_hifi_flac`, each timed against core B's own restore of the same
   residuals (a 32-bit and a 64-bit loop) and checked sample by sample.
3. **win**: 1024 words written to and read back from each FFT window, to show whether they read
   back at all, and the CPU's cycles per word (`w` in §2.1).
4. **fft**: every size 64..1024, FFT and IFFT, 5 vectors (impulse at 0 and at 1, random ±2^14,
   random ±2^22, DC 32767). Each job logs INTSR at 4 points (to show when done rises), its data-in,
   run and data-out cycles, and a CRC-32 of the output. Full dumps (12 words a line, runs
   compressed) go out for every 64-point job, for 512-point random ±2^14, and for the impulse-0 and
   DC vectors.
5. **mac**: windows DATR/DATI/ALAC-coef/FLAC-coef filled with a·(i+1), a = 1, 3, 7, 15, so every
   window pair gives a different sum. Orders 1..1024 identify the operands; random 32-bit operands
   at orders 1/8/1024 give the width and signedness; order 8 twice without a reload shows whether
   the block accumulates. The done source is logged (INTSR bit 15, MAC_CFG bit 5 = the SDK's
   MAC_DONE, or both).

The sequence is only what the SDK shows [SDK]. The open is stock's: both clocks on, both resets for
1 ms, FUN_CFG, ACC_CLR 3, DMACR 0x01080108, INTCR 0x00830083. Then Hifi_Set_ACC_XFER_Disable
(FUN_CFG = type | count << 15), Hifi_Enable_FUN_DONE_FLAG, FFT_Set_CFG or MAC_Set_CFG, the data by
CPU word copies (HIFITranData), and the start. No firmware shows the start, so it has two variants.
**A** is Hifi_Set_ACC_XFER_Start (| 3, how every other function starts). **B** leaves the transfers
off; that is the reading of Hifi_Set_ACC_XFER_Disable's partly lost GBK comment that FFT and MAC read
the RAM directly. B is tried only if A never finishes the first job. Results are read with
MAC_Get_result's order: RSH, then RSL. Then Hifi_Clear_FUN_DONE_FLAG. DMA2 is not used for FFT or
MAC.

Safety: every wait is bounded (10 ms + 32 cycles a butterfly-point or tap). On a timeout the block is
reset and gated (stock's close) and opened again for the next job. Core B sets `hifi_busy` while it
touches the block, so a stalled bus turns FLAC-on-HIFI off until power-off (HIFI.md §4). Core A ends
a pass on a fault, on a core B with no heartbeat for 1 s, or after 20 s, and names the step
(`AS->probe_step`). Nothing touches boot, the maskrom path or any storage but ECHOLOG.TXT. FFT and
MAC never run in a decode path: the QEMU checker raises a VIOLATION for them unless the machine runs
with `hifi-probe=on`, which only m39 and the mutants use.

Cost: core B main image +6.7 KiB (115.8 of 128). Core A resident +156 B (the `audio_b_fresh` API).
Developer overlay +1384 B.

### 4.2 The log and its decoder

Lines (`hfp: ...`), about 400 for the four passes:
```
hfp: start (4 passes: the probe at B / 1, timing at / 2, / 4, / 8)
hfp: v1 arg <0|1> b_hz <hz> div <d> rvr <cycles per 10 ms>
hfp: cpi alu <c> ldr <c> str <c> ldra <c> (9000 instructions each)
hfp: flac o <order> s <shift> n <n> b <bits> hw <c> sw32 <c> sw64 <c> st ok|bad|timeout
hfp: win off <window> n 1024 bad <k> first <i> w <x> r <y> or <diff bits> wcyc <c> rcyc <c>
hfp: fft n <N> f|i v <vec> var A|B st done|timeout sr <intsr x4> in <c> run <c> out <c> crc <crc32> o <4 values>
hfp: d f<N><f|i><vec><r|i> <first index>: <hex words, w*k = k repeats>
hfp: mac o <order> set <0|1|2> var A|B st done by intsr|cfg|both|timeout sr <a,b> cfg <a,b> run <c> rsh <x> rsl <x>
hfp: end jobs <n> timeouts <n> us <total> lost <bytes> block gated
hfp: core B <faulted|stopped (no heartbeat)|took over 20 s> in step <step> (...)      (only on a stall)
hfp: done|stopped, <n> lines
```
`tools/hifi_probe_decode.py ECHOLOG.TXT` (project repo) regenerates every input from the probe's
xorshift32 seeds. Per FFT job it searches a family of candidates for one that reproduces the block
bit for bit: scale 2^−k for k = 0..10, e^∓, natural or bit-reversed order, rounding nearest, floor,
toward zero or to even. Dumped jobs are compared word for word and the others by CRC. When nothing
matches, it reports the best complex gain (log2, phase) and the residual's RMS, mean (bias: the
rounding) and maximum, which is what a per-stage rounding leaves. For MAC it reports the operand
pair, the width (s32×s32 to 64, u32, low 16, low 24, a 32-bit sum, a high-word sum) and whether
results accumulate. For FLAC and FFT it reports cycles per sample, tap and butterfly; for windows,
cycles per word; plus CPI per instruction class, and whether the block's cycles stay constant
across the four clocks (it runs on core B's clock).

### 4.3 The QEMU model, checker, faults and mutants (project repo)

`qemu/hw/arm/echo_mini_hifi_fm.inc`, included by `echo_mini.c`, with one-line hooks in its HIFI
section:
- Without `hifi-probe=on`, every FFT/MAC access stays the VIOLATION it was.
- With it, the model computes **one candidate semantics, [inferred]**. It exists so the decoder has
  something definite to recover:
  - FFT in place in the windows, started by variant A: X = DFT/N forward and an unscaled inverse,
    round half away from zero, saturating, natural order.
  - MAC: Σ (int64) DATR·DATI.
  - Variant B starts nothing.
- **VIOLATION** for:
  - FFT_CFG other than the SDK's flags and size, or MAC_CFG other than order << 18 (1..1024)
  - an FFT start whose count is not FFT_CFG's N, or a MAC start whose count is not the order
  - INTCR bit 15 clear, DMACR not the SDK's value, a configuration not written since the reset, or
    windows not written as far as the job reads
  - configuration, windows or a start while a job runs
  - a window word read before it was written, or while a job runs
  - RSH/RSL read before a MAC job is done, or RSL without RSH just before it
  - a FLAC job after an FFT/MAC job with no HIFI_SRST between
- **Faults** (`hifi-fm-fault=`), each hitting one job:
  - `fft-hang@N`, `mac-hang@N`
  - `fft-wrong@N`, `mac-wrong@N`
  - `win-zero` (the windows do not read back)
  - `stall@N` (core B stops at the N-th FFT start)

`tests/cfw/m39_hifi_probe.sh` runs nine cases. **probe**: the decoder recovers the candidate exactly
for all 50 FFT jobs and all 12 MAC jobs, FLAC timing is ok, and CD16 is bit-exact on the block
afterwards. **ffthang**: one timeout, the rest matched. **fftnone**: A and B both time out and MAC
still runs. **fftwrong** and **macwrong**: the decoder singles out exactly the bad job. **machang**.
**winzero**: "do not read back", nothing matches. **stall**: "no heartbeat in step 20402", a fresh
core B, the block off, CD16 bit-exact in software. **off**: the VIOLATION that guards every decode
path.

`tests/cfw/hifi_probe_mutants.sh` has 12 mutants, each of which must be caught:
- nobound: no wait limit
- noreset: no reset after a timeout
- badcfg: a wrong FFT flag
- readhung: results read on a timeout
- rslfirst
- nodone: INTCR bit 15 not set
- count: the FUN_CFG count
- noungate: block_up without the clocks
- noreload: core A does not load a fresh core B
- nobusy: no hifi_busy
- nostall: no heartbeat check
- decodepath: the probe called from the FLAC decoder

### 4.4 What the next flash must measure

One run of Developer > HIFI probe (test), then the log through the decoder. No setting changes.

| Question | Where in the log | Decides |
|---|---|---|
| Core B's CPI per instruction class at 96/48/24/12 MHz | `hfp: cpi` | every number in section 1 in device cycles: whether MP3 320 really sits at 48 MHz, and whether DSD64 (85.8 M insn/s) and APE high fit |
| The FLAC function's speed against core B's own restore; constant in B-cycles across the clocks? | `hfp: flac` × 4 passes | whether FLAC-on-HIFI saves anything (HIFI.md §6), and whether the block runs on B's clock |
| Do the FFT windows read back? CPU cycles per window word (`w`) | `hfp: win` | the per-job overhead in §2.1; without readback the FFT is useless |
| Which start works (A or B); does done rise on INTSR bit 15? | `hfp: fft ... var .. sr` | whether the FFT is usable at all |
| The FFT's transform: direction, order, scaling, rounding, overflow (DC), bits (±2^22) | the decoder's `fft` verdict, or its best gain and residual | the conversion in §2.2 and whether AAC/Vorbis/WMA stay within tolerance; Bluestein for Opus only if wide and unscaled |
| The FFT's speed per size, and its data-in/out cost | `run`, `in`, `out` cycles | the break-even in §2.2 (AAC: ≥ 0.054 butterflies a cycle) |
| MAC's operands, width, accumulation, done flag, speed | `hfp: mac` | MAC's future use (no decode path today: §2.4) |
| No stall, no timeout | `hfp: end ... timeouts 0` × 4, `hfp: done` | the block's safety under these sequences |

## 5. The ranked plan (benefit to the user: battery and governor steps on real music)

The order assumes the probe answers favourably where marked. "Now" means done on this branch. Rows
are ordered by how common the format is on a DAP, then by the size of the step.

| # | Format | Cost now (after) | Best path | Expected saving | Unknown |
|---|---|---|---|---|---|
| 1 | FLAC 16/44.1 (the most common) | 99 insn/frame, 12 MHz (software 137) | HIFI LPC (round2) + the ring-in-place input (§3, next step 3) | busy −30 % with the block; no step (already the lowest) | the block's speed [device] |
| 2 | FLAC 24/96, 24/192 | 111/110 (software 163/158) | HIFI LPC (round2) + input in place | **48 → 24 MHz, 96 → 48 MHz** with the block; −10 % more | the block's speed, the wide accumulator [device] |
| 3 | MP3 (V2, 128, 320) | 465–653, 48 MHz (320: at the line) | software (done: the output loop); synth stays full-accuracy | 320k: **96 → 48 MHz** at CPI 1 (now) | the device's CPI may put 320k back on 96 [device: cpi] |
| 4 | AAC-LC 256 / M4A | 500, 48 MHz (was 578) | -O2 (now); then the FFT for the 512-point IMDCT | −13.5 % now; −14 % more with the FFT; no step | FFT semantics and speed [device]; ISO 14496-26 tolerance [standard] |
| 5 | HE-AAC v1/v2 | 1075/1103, 96 MHz (was 1244/1260) | -O2 (now); paired 32-point DCT-IV kernels on a 64-point FFT | −14 % now; −6 % more | the same |
| 6 | DSD64 / DSD128 / DSD256 | 486 / 872 / 2014 (was 567 / 1066 / 2577) | the exact table restructure (now); a new decimator (changes the output: the user's call) | DSD64 now under 96 MHz at CPI 1; DSD128/256 need the decimator (2–4×) | the CPI (85.8 M insn/s is close to 96) [device] |
| 7 | WAV/PCM | 18 (was 80) | in-place reads (now) | −78 % busy | — |
| 8 | ALAC, APE | (`hifi2`) | their HIFI functions; ALAC's Golomb in software | (theirs) | (theirs) |
| 9 | Vorbis | 376–445, 48 MHz | the FFT for Tremor's IMDCT | −18 %, **48 → 24 MHz** for typical files (at the line) | FFT [device]; a tolerance of our own |
| 10 | Opus | 651–709, 96 MHz | Bluestein on 1024-point FFT/IFFT (speculative); first fix the 256k truncation (§6) | −13 %, **96 → 48 MHz** if the block is wide and unscaled | FFT precision [device]; RFC 6716 conformance [standard] |
| 11 | WMA Lossless | 16-bit 2520 (over 96 MHz), 24-bit 552 (48 MHz) (were 4463/1075) | the exact memset and LMS changes (now); a hand LMS loop next | 24-bit **96 → 48 MHz** now; 16-bit still over: −20 % possible, still over | — |
| 12 | WMA v2 / WMA Pro | 260 / 207–243, 24–48 MHz | the FFT for `ff_imdct_half` | −25..29 %; Pro 24/96 **48 → 24 MHz** | FFT [device] |
| 13 | WavPack | 488–595, 48 MHz | software (the decorrelation loop); nothing on the block fits | — | — |

**Order to implement after the probe answers:**
1. Read the probe's `cpi` and `flac` lines. They settle FLAC-on-HIFI (keep, or `hifi=off` by default)
   and convert this table to device cycles. Re-rank on CPI.
2. If the FFT works (it reads back, a start finishes, a transform is recovered): one shared core-B
   module `b_hifi_fft.c` (its own section, apart from `b_hifi.c`) holding the job, a bounded wait,
   the reset, and a self-test of one job per size against the decoder's own FFT within a stated
   tolerance. Wire it first into **WMA Pro / WMA** (the largest share, Rockbox's own FFT as the
   self-test reference), then **Vorbis**, then **AAC-LC** (with the 14496-26 check on conformance
   streams). Each one gets a hifi=off|auto|verify switch like FLAC's, a QEMU model of the semantics
   the device showed (replacing the candidate), and the checker and its mutants extended to the
   decoder's sequence.
3. Only if the FFT is wide (≥ 24 bits) and unscaled: Opus via Bluestein, behind its own switch and
   the RFC 6716 vectors.
4. MAC: nothing to wire. Record its semantics in HIFI.md for a future long FIR.
5. In parallel, independent of the probe: the software next steps of §3 (WMALL LMS loop, the FLAC
   input in place) and the DSD decimator question to the user.

## 6. Findings on the way

- **Opus 256k (libopus, dense signal) stops at 3.99 s on core B** [measured]. Core B decodes the
  first 191 488 frames and then ends the track: "end of stream", 0 underruns, B err 0. The host
  `opusref`, built from the same libopus/opusfile sources, decodes all 576 000 frames. The stop falls
  at the first large packet (1156 bytes; the file has packets up to 1276). Root cause not found
  here. Suspects: `op_read_stereo`'s error return being taken as the end (`b_opus.c`, r ≤ 0), or a
  stack/arena limit with VAR_ARRAYS at large packets.
- **DSD256 underran even in QEMU** before this branch (227 M insn/s at a 250 MIPS virtual core); it
  no longer does (177.6 M). On the device it stays over 96 MHz.
- WMA Lossless 16/44.1 and APE extra high are over 96 M insn/s at CPI 1: neither plays in real time
  at core B's top clock on the device [measured; device CPI ≥ 1].
