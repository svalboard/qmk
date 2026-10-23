# Draft: release notes for the "last manual update" release

Draft for the first release built with the in-firmware updater
([updater.md](updater.md)). The tag name is not chosen yet: `<TAG>` below.
Fill in the other changes of that release where marked. Wording rules for
Svalboard release notes apply: describe additions relative to Svalboard's
Vial firmware, keep any safety claim to one plain line, and list the files
without naming sensor parts.

---

## <TAG>: the last manual update

This is the last time you need to copy firmware onto your Svalboard by hand.
From this release on, Keybard can install new firmware for you over USB.

### Install this release

1. Download the two `.uf2` files, one ending in `left` and one ending in
   `right`. They work with any pointing module (none, trackball, TrackPoint
   or touchpad), which each half detects when it starts.
2. Put one half into bootloader mode (hold its BOOT button while plugging it
   in, or tap its reset button twice). A drive called `RPI-RP2` appears.
3. Copy that half's `.uf2` onto the drive. The half restarts on its own.
4. Do the same for the other half. **Both halves must run this release:** a
   half on this release cannot talk to a half on an older one.

**Flashing resets your settings.** Each half starts from the default layout
after new firmware. Export your layout in Keybard before you flash, and load
it again afterwards.

### What's new compared with Svalboard's Vial firmware

- **Firmware updates from Keybard.** Keybard sends the new firmware over USB,
  the half checks it, and you approve it on the keyboard itself. Keybard
  updates the half without USB through the split cable first, then the half
  with USB.
- **Approve on the keyboard.** When Keybard asks, hold Index South and Middle
  South together for one second on the half plugged into USB (on the default
  layout, `M` and `,` on the right, `V` and `C` on the left). The LEDs blink
  blue while it waits and flash white when it sees the keys. Then both halves
  show a rainbow while they update (keep the keyboard plugged in) and flash
  green when they are done.
- **Checked before it is installed.** The keyboard checks that the firmware
  arrived intact. Keybard warns you if it was built for the other hand. It does
  not check who built it: only approve updates you started.
- **Version check between halves.** Once both halves run this release or
  later, Keybard tells you when they differ. If they still differ a minute
  after an update, the LEDs blink red for 30 seconds: open Keybard to finish.
- **One firmware for every pointing module.** After attaching or changing a
  module, unplug and replug the keyboard. If you used a trackball, TrackPoint
  or touchpad build before, select the keyboard again in Keybard; on Linux,
  run the udev command Keybard shows.
- **Your own builds work too.** Bootloader mode and a `.uf2` still work for
  any firmware, and the updater takes your own builds as well.
- `<other changes in this release>`

If an update is interrupted, the half either keeps its old firmware or starts
in bootloader mode; then copy its `.uf2` again as above.

### Files

- `*_left_*.uf2`, `*_right_*.uf2`: firmware for each half, for copying by hand.
- `*.svup`: the same firmware, packaged for Keybard's updater. You don't need
  to download these.

---

Notes for whoever publishes this (delete before publishing):

- Check the release's `.uf2` and `.svup` names against the list above before
  posting.
- Settings reset on every new build (D39); the install steps say so. Keep
  that line until Keybard saves and restores the layout around flashing.
- The approval chord and LED colors are from `docs/updater.md` (D4, D24); if
  either changes before the release, update the text.
- The in-Keybard update flow is milestone M4 (Keybard UI). Until it ships, the
  "Firmware updates from Keybard" line describes a capability the firmware has
  and Keybard does not yet offer: hold the notes, or reword that line, until
  M4 is live on keybard.svalboard.com.
- Updates are not signed in this release (`docs/updater.md`, "Security"). Do
  not describe them as signed or as "only Svalboard's firmware".
