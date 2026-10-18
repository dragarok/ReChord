# Ideas from upstream ReChord (brnomt/ReChord, master at 9f98517, 2026-10-17)

This repository forked from brnomt/ReChord at `405ce09`. Since then upstream has 84 commits,
and our 512 MB line has 100+. The two are separate code bases now: upstream's audio path,
input and storage differ from ours. So nothing is merged. This note lists what upstream did
that we had not thought of, or did differently, and what we take from it.

| Upstream | Commits | What we take |
|---|---|---|
| **Media index**: a scanner over the storage that writes slots and records with a commit step; a reader; Rockbox-style Library categories (Artists, Albums, Genres, Years, Labels, Album artists, Composers) and All Tracks over it; "Rebuild index" in the menu; the build reports its own failure; audio kept alive during scans | d1b6d89, 44e3d9c, d87184d, 3190097, 04beb7e, 98c4b3f, 3483e96 | **Yes, next.** Our Library row was deferred. Write a crash-safe index file on the internal volume, built in the background through our storage queue (never stalling audio), and categories as lists in our template. |
| **Undefined policy for tags**, plus year, label, album artist and composer | e36e09c | Yes, with the index: missing tags group under one named bucket, not scattered. |
| **Settings in a crash-safe block record store** on the internal storage, in place, instead of a FAT file | 7dab023, 2c6c12b | Partly. Ours is RECHORD.CFG written through the write-behind queue at pause. A double-buffered record (as RECHO 0.10's RRESUME0/1 with CRC32) would make the settings and the resume point survive a power cut mid-write. |
| **The device creates its own files** under `/.rechord/` at first boot | 90389b6 | Maybe. It keeps the user's root clean (we write ECHOLOG.TXT and RECHORD.CFG at the root). It would change paths users know, so it waits for the settings rework. |
| **Log append and rotate** instead of rewriting | 4cb5829, 2a9af22 | Yes, small: keep the previous session's log when the file reaches its cap, so a crash's lead-up survives. |
| **Click-free track changes**: a fade where the format changes (no gapless join possible), and volume through the CS43131's own ramped PCM_VOLUME | e8beced, 8765f85 | Check. Our gapless joins are bit-exact; a format change re-programs the DAC, which may click on some earphones. A short fade-out and fade-in there costs nothing on a gapless join. |
| **Hold-to-repeat with acceleration**; a millisecond clock for the hold ladder; a stuck-clock detector | 4d15172, 7f68b3a, abed70f, e8c14cf | Already covered: our keys are sampled from SysTick every 10 ms, with gestures and repeat acceleration timed in ms. |
| **32-bit hi-res path, FLAC up to 384 kHz** | 23fd015 | Check our ceiling: the USB DAC and the I2S clocking go to 192 kHz. Whether the CS43131s and the I2S divider can do 352.8/384 kHz on this board is a measurement for the self-test plan. |
| **Core B bring-up phases** (heartbeat, ring, file service, decode on B) | 562eeec..2330903 | Already done differently here: core B runs one decoder image per codec family since 2026-10-15. |
| **Playlist module**, shuffle and repeat settings, i/N in the UI | 295551b, 1b08450 | Already covered: the queue, play modes and n/N. |
| **DFIU invariants** (a rules file checked against the code before a flash) | .dfiu/ | Our equivalent is the QEMU checkers plus mutants. Their idea of recording each hardware-verified rule with its evidence ID is good practice; our device logs and BT_DEVICE_NOTES do part of it. |
