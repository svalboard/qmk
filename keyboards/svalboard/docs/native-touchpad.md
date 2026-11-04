# Native touchpad

With an Azoteq trackpad on either hand, Svalboard presents its pads to the
computer as one Precision Touchpad on its own USB interface. Windows, and Linux
with libinput, then handle every gesture themselves: pointer, tap and
tap-and-drag, two-finger scroll with inertia, pinch, two-finger right click,
and three- and four-finger swipes. No app runs on the computer.

## What the user gets

- **Windows and Linux** switch the touchpad interface to touchpad mode (HID
  Input Mode 3) when it connects. From then on the operating system's
  touchpad settings apply to the pads, and Keybard's pointer settings (DPI,
  scroll mode, natural scroll, axis lock) do not apply to a pad hand, except
  while a scroll key drives the firmware's scroll. The pointer keys still
  work: sniper, boost and the scroll keys (see below).
  The firmware reports this per hand in the Sval pointing identity (0x2C):
  flag `SVAL_POINTER_OS_TOUCHPAD` (0x10), so Keybard can show it.
- **macOS, a BIOS, or any host that never asks for touchpad mode** gets the
  pads as today's mouse, with the trackpad chip's own tap, scroll and drag.
- **A trackball or TrackPoint on the other hand** works exactly as before: only
  a hand whose module is an Azoteq pad is ever handed to the touchpad.
- **No pad on either hand:** the interface is still there, but it never
  reports and nothing else changes.
- **Linux:** libinput turns tap-to-click off by default for touchpads with
  buttons; turn it on in the desktop's touchpad settings.

The two pads, one per hand, make one area: the left pad, a 10 mm unreachable
gap, then the right pad, 96 × 40 mm in all (4572 × 1792 counts). A board with
one pad uses the same area, with the pad in its hand's part. The surface the
operating system sees is seven times that area's width and height, 672 × 280 mm
(32004 × 12544 counts), at the same counts per mm, with the pads' area in its
center (`SVAL_PTP_SURFACE_TIMES` in `azoteq/ptp_descriptor.h`). Pointer speed
and gesture distances are those of the pads; the room around them is where
boost's scaled travel goes (below).

## How it works

| Piece | File |
|---|---|
| Make flags | `azoteq/multitouch.mk` (included by `pointing_auto/pointing_auto.mk` and `azoteq/rules.mk`) |
| Build settings | `azoteq/ptp_config.h` |
| Report descriptor | `azoteq/ptp_descriptor.h` |
| Contacts, frames, feature reports | `azoteq/ptp.c` |
| Pad reads, the other half's pad, the dev command | `azoteq/multitouch.c` |
| Read timing | `azoteq/pad_schedule.h` |
| Contact read in the driver | `drivers/sensors/azoteq_iqs5xx.c` (behind `SVAL_AZOTEQ_MULTITOUCH`) |
| USB interface | `tmk_core/protocol/usb_ptp.h`, `usb_descriptor.c/.h`, `chibios/usb_main.c`, `chibios/usb_endpoints.c/.h` (behind `SVAL_NATIVE_PTP`) |

**Interface.** The touchpad is the last USB interface, so every other
interface keeps its number, with one 64-byte interrupt IN endpoint polled every
1 ms. Its feature reports (the 257-byte certification blob, Input Mode,
Surface and Button Switch, latency) are answered by `ptp.c` through one hook in
`usb_main.c`. The build sets bcdDevice to 0x0F03 so Windows reads the new
interface set instead of a cached one.

**Pads.** Each half reads its pad at the chip's 8 ms report period, over I2C at
400 kHz, in one read: the 10 base bytes and the 7-byte contact slots the last
read needed (up to the highest slot with a contact, since a contact keeps its
slot until it lifts). Only when the chip's count says more contacts are down
than that read found does a second read, in the same communication window,
fetch the remaining slots; so a read grows with the contacts down, and costs
two reads only as a finger lands. The USB half feeds its own pad to
`ptp.c` at once. The other half keeps its last sample; the USB half fetches it
over the split link on the Scan Lab RPC with its own request op (0xA7, 32-byte
reply), so the split message table is the same as in every other build. The
other half marks its split pointing report with a counter that moves only when
its pad has news, and only once the USB half has asked for it; the USB half
fetches on news, every 10 ms while that pad has contacts, and every 250 ms
while it is quiet.

**Read timing.** A read that starts before the chip's window opens is held by
clock stretching, and the main loop with it. `pad_schedule.h` times the next
read from the end of the last one, one period later less a lead that adapts so
that reads land just after the window opens. The bare transfer time it
subtracts is kept per read length (0 to 5 slots), so a longer read is not
taken for a stretch. A pad that stops answering is read less and less often,
down to about once a second.

**Contacts.** Contact IDs, the 5-contact limit (the most a Windows Precision
Touchpad allows), arming on an all-up sample, palm, faults and lifts are in
`ptp.c`. A finger beyond the five is left out of the touchpad for its
lifetime, but `POINTING_CONTACTS` (0x2F) still shows it, up to 6 fingers in
all, so a host app draws three on each pad. A pad whose contacts get no valid
sample for 50 ms is lifted; so is the other pad while an update runs. With
contacts on both pads, a frame waits up to one period for the second pad's
sample, and every frame is stamped with its newest sample's read time.

**The other pad over a flaky link.** A failed split transaction blocks the
USB half for QMK's split timeout (20 ms), and on the touchpad test board they
come in bursts while both pads are touched. Windows ends a pinch as soon as
either contact lifts, so the other half's contacts are held at their last
position for up to 150 ms without a new sample (`SVAL_PTP_REMOTE_STALE_MS`)
before they are lifted, and a reply with no sample from a half that has been
sending them counts as a failed fetch until three come in a row (QMK's RPC
reply carries no checksum). A pad lifted because its samples stopped arms
again on its next valid sample, so a finger still down is reported again as a
new contact instead of staying unseen until it lifts; a chip reset or palm
still waits for an all-up sample. The dev command's status (v3) counts these
lifts per pad.

While Velocity Scroll is on (`SV_VELOCITY_SCROLL` held, or
`SV_VELOCITY_SCROLL_TOGGLE` tapped on; `velocity_scroll.h`), the firmware takes
the pads from the operating system: reports stop after a
release of every contact, and `ptp.c` follows one finger per pad for the
scroll. A finger already down is followed at once; after it lifts, only a
finger that lands later is, so one resting on a pad never takes over. When
it turns off, the pads arm again on an all-up sample, so a finger still
down never moves the pointer.

**Pointer keys.** While the operating system owns a pad (Windows or Linux in
touchpad mode), the keys a trackball hand uses work on the pads too
(`keymaps/keymap_support.c`):

- **Scroll toggle and scroll hold** take the pads from the operating system
  (only while it owns them: touchpad mode, its touchpad switched on), as
  Velocity Scroll does: the OS sees every contact lift with Confidence
  clear (so a finger that just landed is never a tap), and one finger per pad
  drives the firmware's own scroll, at the hand's DPI and with its scroll
  settings, exactly where a trackball's counts would enter
  (`pad_follow.c`). A finger already down is followed at once. When the key
  lets go, a finger still down comes straight back to the operating system as
  a new contact where it is, so scrolling turns back into pointing mid-stroke;
  if it lifts within 300 ms it lifts with Confidence clear, never a tap.
- **Sniper and boost** leave the pads to the operating system and scale the
  travel of every contact by the factor in force (`ptp.c`,
  `sval_ptp_set_scale`). Pressing or releasing the key re-anchors every
  contact where it was just reported, so the speed changes from the next
  frame with no jump, no stall and no lift. A finger that lands while another
  is down enters where the first finger's map puts it, so the fingers'
  reported spacing stays consistent for gestures; with no finger down it
  lands at its real position. Positions stay inside the surface, and an axis
  that reaches its edge slides along it, so the other axis keeps moving and
  turning back moves at once.

On a Mac, with built-in gestures, or with the touchpad off, the pad is the
chip's mouse and these keys work on it as they always have. `POINTING_MOTION`
(`0x2D`, bytes 53-55, `docs/protocol.md`) reports the sniper divisor and boost
multiplier in force, and `POINTING_CONTACTS` (`0x2F`) gives every contact's
real position on its pad, never the scaled one.

## The split link's receive

The half with USB receives the other half's replies (its pad snapshot, its
matrix, its pointing report) by DMA from the PIO receive FIFO into a ring
(`SERIAL_PIO_RX_DMA`, `platforms/chibios/drivers/vendor/RP/RP2040/serial_vendor.c`).
Before, an interrupt and a thread wake drained the 8-byte FIFO as bytes
arrived, and an interrupt handler running longer than 8 byte times (80 us at 1
Mbaud; USB's handler was measured up to about 300 us) overflowed it: the PIO
state machine stalled, missed bits, and the transaction failed, often only at
the 20 ms receive timeout, with the main loop blocked meanwhile. On a touchpad
on the half without USB that showed as pauses of 25 to 50 ms in the pointer;
on a trackball there, as lost motion.

- The DMA channel runs only while a transaction does: the updater refuses to
  program or commit flash while any DMA channel is busy.
- A framing error fails the receive at once.
- The wait for a reply spins rather than sleeps. On a trackball pair, idle
  with the default power saving, that keeps the core out of WFI 5.3 % of the
  time (0.5 % when sleeping), while sleeping costs about 4 % of the scan
  rate. `SERIAL_PIO_RX_DMA_POLL_US` above 0 makes it sleep.
- The link counts as disconnected (QMK then releases the other half's held
  keys) once failures have gone on for 200 ms (`SPLIT_MIN_DISCONNECT_MS`,
  `quantum/split_common/split_connection.h`), not after ten failed scans,
  which the short deadline would pass in a few tens of ms.
- QMK's per-scan transactions and the pad snapshot fetch get a 3 ms deadline
  (`SERIAL_USART_SHORT_TIMEOUT`); the other RPCs, the updater's relay among
  them, keep the 20 ms one, because their other end may take time.
- The half without USB keeps the interrupt receive: a dedicated high-priority
  thread drains it, and it only receives short requests. It waits 3 ms for
  the buffer that follows a handshake (the master sends it at once), so the
  bytes of a transaction the master gave up on are never taken for the next.

Dev builds (`SVAL_TOUCH_DEV`) count failures by stage on both halves (and,
on the half with USB, their cost) over the dev command (0xFC op 0x42 action 7).

## Builds

| Flag | Where | Effect |
|---|---|---|
| `SVAL_NATIVE_PTP` | on in the auto builds (`svalboard/left`, `svalboard/right`), off in `svalboard/azoteq/*` | The native touchpad. `SVAL_NATIVE_PTP=no` builds without it |
| `SVAL_PTP_KEY_BUTTONS=yes` | with the touchpad | MS_BTN1 and MS_BTN2 are the touchpad's buttons while it reports |
| `SVAL_PTP_MOUSE_BEFORE_MODE3=no` | with the touchpad | The pads stay silent until the host asks for touchpad mode |
| `SVAL_TOUCH_DEV=yes` | dev builds only | Raw HID 0xFC op 0x42: owner (native or mouse) and status, for `tools/multitouch/ptp_owner.py`; action 2 holds Velocity Scroll with no key, for `tools/multitouch/velocity_hold.py` |
| `SVAL_PERF_STATS=yes` | dev builds only | Timing counters over raw HID 0xFC op 0x43, for `tools/multitouch/perf.py`; with or without the touchpad. They include the slowest scan gap split into exclusive parts: matrix row reads, the split exchange of every scan, the pad driver call, the other pad's split fetch, USB IN sends, the counters' own split reads, and everything else |

Release builds refuse `SVAL_TOUCH_DEV` and `SVAL_PERF_STATS`, and
`check_release_elf.py` rejects their symbols. Neither 0xFC command is a
released protocol.

## Tests

- `util/test_native_ptp.sh`: `ptp.c`, `multitouch.c` with `ptp.c` (split link,
  hints, per-hand gating, updates), `pad_schedule.h` against a simulated chip
  and main loop, the descriptor (cross-checked with hid-tools when installed)
  and every report the C tests sent, decoded with it.
- `make test:pointing/azoteq_multitouch`: the driver's contact read.
- `tools/multitouch/ptp_desc_check.py ELF`: the descriptor and interface layout
  inside a built image.

## Measured

On a keyless test board with a pad on each half: the auto build with the
blank keymap, the real matrix scan running on both halves (unpaced, turbo 0,
90/90 us). A keyless board reads its unconnected column inputs as four keys
held, so the scan settles into one unchanging frame; key events themselves
were not exercised.

**Idle**, two 30 s windows each, with and without the touchpad (the same
commit, `SVAL_NATIVE_PTP=no`):

| | Without the touchpad | With it |
|---|---|---|
| USB half: matrix scans per second | 573, 571 | 483, 484 |
| USB half: longest scan gap | 3.3, 3.2 ms | 5.3, 5.2 ms (3 to 4 gaps a second over 4 ms) |
| Other half: matrix scans per second | 762, 772 | 750, 751 |
| Other half: longest scan gap | 6.2, 5.6 ms | 2.6, 2.6 ms |
| Pad reads per second, each half | 96 to 99 | 123 |
| Pad read | 0.64 ms USB half, 0.87 to 0.98 ms other half (100 kHz, 10 bytes, with the end of session) | 0.59 ms USB half, 0.55 ms other half (400 kHz, 10 bytes, the read alone) |
| Other pad fetched over the split link | | 4 a second, 0.81 ms each |

On the USB half the touchpad takes 11% of the time in the pad driver call
(read, end of session and the sample) and 4% in its task. Its slowest idle
gap is one loop pass with a pad read held by the chip (2.5 ms), the matrix
(1.0 ms) and the 4-a-second split fetch (0.8 ms).

**With fingers**, by hand on the USB half, 10 s each, on the build before reads
were sized to the contacts (every slot read while a finger was down):

| | Idle | One finger | Two | Three |
|---|---|---|---|---|
| Pad read | 0.6 ms | 1.65 ms | 2.6 ms | 4.5 ms |
| Matrix scans per second | 483 | 330 | 300 | 252 |

The longest scan gap was up to 9 ms, and once 24.6 ms with one finger on each
pad, with no failed pad read or split fetch.

## Not covered yet

- Linux on hardware.
- Timing while typing (needs a keyed board).
- A pad that holds the I2C clock line low (the back-off is host-tested only).
