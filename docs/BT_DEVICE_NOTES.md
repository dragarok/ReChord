# Bluetooth on the device: what the module does, what we fixed (2026-10-17)

The JieLi module on UART1 (115200 8N1, power GPIO2.7, audio routed by GPIO2.0 = 1 /
GPIO2.14 = 0) as the 512 MB Echo Mini's own logs show it, and the driver changes that
came from them. Driver: `firmware/drivers/bt_jieli.c`; app glue: `firmware/startup/ap_bt.c`.
Everything here builds on stock's own driver: its frame set, pins and timing came from disassembling the stock firmware (MINIV390); the notes, `08_bluetooth.md`, are not in this repo.

## The module, as logged on the device

Earphones: AirPods Pro. Module hello `CV15` (version 5: `C\x86` / `C\x87` allowed).

| We send | The module answers |
|---|---|
| any frame | `CKKK` about 110 ms later |
| `CQ00` (status) while linked | `CQO`+LL+MAC+name about 110 ms later |
| `CQ00` while unlinked | nothing |
| `CL06`+MAC (connect), earphones there | `CLOK` 1-2 s later, then a `CQO` to the next poll |
| `CL06`, earphones in their case | `CLER` 5-8 s later |
| `CL06` while already linked | the `CKKK` only |
| `CH06`+MAC, 4 s after an unanswered `CL06` | either a bare `CH06` (a real link) or `CH36`+MAC+name, then `CHER` and `CLER` 70 ms later (no link) |
| `CT00` (disconnect) | `CLOK` (never `CTOK`) |
| (nothing: the earphones go away) | an unsolicited `CTOK` |
| `CS00` (scan) | `CS`+LL+MAC+name per device, `CSEN` 13-26 s later |

The module is BR/EDR only (FCC report: no LE test mode) and acts as a source: it is never
discoverable, so a phone or a Mac does not list it. No stock firmware version updates the
module's own firmware.

## Faults found in the logs, and the fixes

| Device symptom | Cause | Fix |
|---|---|---|
| Earphones taken out of the case mid-song never came back | stock's two `CL06` tries 3 s apart, then nothing | tries go on: 10 s, 20 s, then every 30 s until it links; a lost link (unsolicited `CTOK`, `CLOK`, a CQ answer other than O) reconnects, the first try 2 s later |
| Disconnect needed four presses and the audio stayed routed | `CLOK` (the answer to `CT00`) taken as "paired" | `CLOK` while connected (not within 5 s of our own `CL06`) closes the link, as stock's screen parser does; `CT00` unanswered for 3 s unroutes anyway; the user's Disconnect stops reconnecting until a connect or a boot |
| After a 42-minute USB disk session the module went off at once, and the setting with it | the 5-minute idle count used wall time while the disk session held the loop; the idle-off cleared `bt` | only time `bt_poll()` was served counts (at most 1 s a tick); the idle-off keeps the setting, and the next play (`bt_wake()`) powers the module and reconnects |
| Four `CY0` 30/0 pairs in 40 ms at a USB host's rate change | every alternate-setting flip sent | a stop reaches the module once it lasted 300 ms |
| The screen's connect said "connected", nothing was heard anywhere | `CH36` taken as a link, `CHER`/`CLER` after it ignored | a CH link is undone by a `CHER`/`CLER` within 10 s until a `CQO` confirms it: connect failed, audio back on the jack |

## Log lines to look for in ECHOLOG.TXT

- `bt: reconnecting to '...' (link lost)`, `bt: reconnect refused (CLER), try N`
- `bt: CLOK: the link closed`, `bt: no answer to CT00 in 3 s: disconnected anyway`
- `bt: connect failed (CHER after the CH answer)`
- `bt: CQ00 unanswered while connected (N in a row)` and `... answered again after N`
  (two single misses on good links on 2026-10-17, each answered 110 ms later)
- `bt: not served for N ms` (the main loop held BT longer than 1 s: a USB prompt, a DAC entry)
- `bt: the source plays: the module on again`

## Tests

In the project repo (QEMU machine `echo-mini`, `-machine jieli=...`), `tests/cfw/m30_bt.sh`.
QEMU's module has a `profile=device` that answers as the table above. Cases from the
device log: `retry`, `drop`, `disc`, `uacrate`, `chfail`. Each fails on the build before its
fix and passes after it. The checker fails a run that keeps the audio routed to the
module with no link for more than 6 s.

## Device runs, 2026-10-17

The build with the first four fixes in the table above, flashed to both slots:

- boot with a remembered device: linked 7.9 s after power-on
- earphones away mid-song (unsolicited `CTOK`): on the jack at once, tries at +2 s and
  +5 s, linked again 8.6 s after the drop, the song routed back
- Disconnect: one `CT00`, `CLOK`, unrouted, no reconnect; Forget then reboot: no reconnect
- USB DAC with the earphones: 186k packets, none dropped, no underrun, 48 -> 192 -> 48 kHz
- the `CH36` / `CHER` / `CLER` connect above (fixed in the next commit, not yet on the device)

## Open

- Whether the module answers `CQ00` on a link that came from a bare `CH06`.
- The earphones' name differs between the scan (`... - Find My`) and some `CQO` answers.
- What the module says outside our frames: Developer > BT listen (test) (`bt_listen()` in
  `bt_jieli.c`) powers it twice the stock way and sends it nothing; first UART1 at 115200
  (each byte, its time, its line status), then GPIO2.17 as an input (every edge, any baud
  rate). It writes `/BTLISTEN.TXT`; decode it with the project repo's
  `tools/bt_listen_decode.py`. QEMU: `tests/cfw/m35_btlisten.sh` (`jieli=listen=1`: any byte
  to the module, a break, GPIO2.16 not idle-high while powered or a GPIO2.7 pulse fails it).
  Not yet run on the device.
