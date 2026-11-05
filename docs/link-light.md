# Link light

A half that was paired with the other half in this power session, and then
lost it without being told to stop, blinks amber (2 Hz, on and off, no layer
colors in between) until the pairing comes back. Code: `keyboards/svalboard/link_light.c`.

## When it blinks

| Half | Pairs (arms the light) | Loses the pairing (blinks) | Clears |
|---|---|---|---|
| With USB (master) | the first presence ping the other half answers | the split transport counts as disconnected: failures for 200 ms (`SPLIT_MIN_DISCONNECT_MS`) | the first good exchange after it |
| Without USB (sub) | the first presence ping it gets | no ping for 1.5 s (`SVAL_LINK_SUB_LOST_MS`: pings come a little over every 500 ms, so two missed) | the next ping |

- A half on its own from power-up never pairs, so it never blinks.
- A power cycle or restart starts unarmed.
- A restart on purpose is a stop, not a loss. When the half with USB restarts
  or goes to its bootloader from firmware (`shutdown_kb`), its last ping says
  so (`link_quiet` 2) and the sub stops. When it restarts the sub on purpose
  (a side write with the restart flag, Scan Lab's reboot), it stops itself
  first. It pairs again once the link has gone down and up, or after 5 s
  (`SVAL_LINK_REARM_MS`) if it never went down.
- A hardware reset of the half with USB (its RESET button into BOOTSEL) gives
  the firmware no chance to say so: the sub blinks until it is back.
- The sub is powered over the split cable, so it blinks only when the data
  lines fail and power stays.

## When it stays dark

- The updater's lights always win, and the link light stays dark while an
  update runs: an update restarts halves on purpose.
- In USB suspend. The master checks its own USB, and the sub reads the master's
  state from the presence ping, in its `link_quiet` byte. That byte is appended
  to `presence_rpc_t` like the other synced settings: an older master sends a
  shorter ping, and the sub then reads 0. If the link breaks while the computer
  sleeps, the sub keeps the last "suspended" it heard, so after the computer
  wakes only the half with USB blinks.
- In builds without the updater, which has the only LED driver for status
  lights.

Amber means only this. Red stays for failures (see the light table in
[updater.md](updater.md)).

## Testing

Dev builds with `SVAL_LINK_TEST=yes` (never in a release; needs
`SVAL_UPDATER=yes`) add raw HID protocol `0xFA` in the client wrapper:

| Op | Does |
|---|---|
| 0 STATUS | both halves' state: armed, lost, light now, ms since the loss, the sub's silence when it called its last loss, the last loss's length, counts |
| 1 PAUSE ms | the master stops the split exchange and the pings for that long without telling the sub, as a broken link looks to the sub |
| 2 SUB REBOOT | the sub restarts, which the master sees as a broken link |
| 3 BOOT AS SUB | this half restarts once as the sub whatever its USB says, so a pair with both halves on USB can be tested over the split cable. With no master answering, the split watchdog restarts it normally within a few seconds |

Host test: `tests/sval_link/test_link_light.c` (in `util/updater_test/run.sh`).
