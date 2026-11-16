# Settings storage

Svalboard keeps its settings (keymap, macros, Sval tables, pointing settings) in QMK's wear-leveling store: 128 KB of logical EEPROM, held in two identical 512 KB copies directly after the firmware.

| Flash | Contents |
|---|---|
| `0x000000`–`0x15FFFF` | Firmware (a linker check keeps it below `0x160000`) |
| `0x160000`–`0x1DFFFF` | Settings, copy 0 |
| `0x1E0000`–`0x25FFFF` | Settings, copy 1 |
| `0x260000`–`0xFFCFFF` | Free (the updater's staging slot is part of it) |
| `0xFFD000`–`0xFFDFFF` | Build reset record |
| `0xFFE000`–`0xFFFFFF` | Board identity: serial and name |

Installing a different firmware build resets every setting to that build's defaults, on each half. Before flashing, export your layout file in Keybard; load it again afterwards. Flashing the same file again keeps your settings.

## Keyboard settings: the settings area

Every keyboard setting (pointer, Auto Mouse, layer colors, scan timing, touchpad, scroll speed) is a record in the settings area: 8,192 bytes at a fixed place in the 128 KB store, directly below the combo names. It is counted from the end of the store, so the tables in front of it can change size without moving it. The macro buffer ends below it.

| Logical address | Bytes | Contents |
|---|---|---|
| `0x00000` | 37 | QMK eeconfig |
| `0x00025` | 22,344 | Sval tables: tap dances, combos, key overrides, Alt Repeat, leader, QMK settings, fragments, labels |
| `0x0576D` | 5 | VIA magic, keycode version, layout options |
| `0x05772` | 100 | VIA custom config: retired, never read or written |
| `0x057D6` | 1,920 | Keymap, 16 layers |
| `0x05F56` | 94,370 | Macro buffer |
| `0x1CFF8` | 8,192 | Settings area |
| `0x1EFF8` | 4,104 | Combo names |

Records (`settings_area.h`): an 8-byte header, then entries of id, capacity and two slots. A save writes the slot that does not hold the current value and gives it the next sequence number last, so a power cut during a save leaves the previous value: each setting reads as its old or its new value. A missing record, or one with no valid slot, reads as the setting's default. Ids from newer firmware are skipped.

Adding a setting:

1. A line in `settings_ids.h`: the next unused id (never reuse one) and the payload size.
2. Load it with `settings_area_get()` over its default, save it with `settings_area_put()`.
3. For Keybard: a VIA custom value id and a GET_INFO feature bit.

No magic, stamp or protocol version changes. A record can grow later by appending fields: a shorter stored record fills the front, and the new fields keep their defaults. The build fails if the records can overflow the area, and `tools/settings_budget.py` (run by `tools/pre_pr.sh`) warns once they could fill half of it. Code that uses the retired custom config fails the same check.

Installing a different build resets every setting (above), so moving the settings here needed no migration: the first boot writes the defaults into the area.

## Settings on a new build

At boot the firmware hashes its own image in flash and compares the hash with the build reset record at `0xFFD000` (`build_reset.c`). If the record holds another hash, or no valid record (Vial, vLaunch2 and older never write one), the board resets every setting to the build's defaults, through the same path as a new `EECONFIG_MAGIC_NUMBER`, then records the hash. GET_INFO byte 18 bit 2 tells the host about the reset until it sends `0x2A`.

- The board identity (serial and name) and the updater's own state are kept.
- On a flash chip smaller than 16 MB the record has nowhere to go, so the board keeps its settings; an incompatible layout is still reset by the magic below.
- If the record can't be written, the next boot resets again, and the board reports that changes won't survive a restart (byte 18 bit 1).
- A firmware without the record in between doesn't count: flashing build A, then vLaunch2, then the same build A again keeps what vLaunch2 left.

## Upgrading from earlier firmware

Settings that firmware before the launch release left at `0x160000` don't carry this firmware's `EECONFIG_MAGIC_NUMBER` (config.h), so on the first boot QMK resets every setting to defaults (the build reset record above resets them anyway). That boot takes a few seconds longer than usual. Load your layout file to restore your setup. Data earlier firmware left elsewhere in flash is never read, and that space stays free. When a later change needs every stored setting reset again, decrement the magic.

## What it protects against

Some boards running the Vial firmware reset to the default layout at random. The cause is in QMK's wear leveling: at boot, a checksum mismatch on the stored image was treated as a brand-new board, so one bad flash read discarded every setting, and the defaults written afterwards made the loss permanent.

Now:

- **A bad read** is retried, rereading the flash rather than its cache.
- **A damaged copy** is read from the other copy, then repaired.
- **Power lost during consolidation** leaves a complete copy: consolidation erases and rewrites one copy at a time, and the next boot repairs the other.
- **Power lost while a change is saved** can lose only that change, which was never confirmed. A log entry cut short at the end of a copy is ignored, and a copy whose log stops at an invalid entry keeps the entries before it; if the other copy reads completely, it is used instead.
- **Both copies unreadable**: the board starts from defaults and remembers that it did. Keybard tells you on its next connection to reload your layout file, and `SV_OUTPUT_STATUS` prints the same notice.
- **A change that doesn't reach flash** is reported instead of passing silently. Every program and erase is read back, and the boot-time check that the flash chip is large enough is retried. If the store can't be opened, or a write or erase doesn't read back, the board keeps working from memory, so the change still shows, but it reports that changes won't survive a restart (GET_INFO byte 18, bit 1) until it is restarted. Keybard warns you, and `SV_OUTPUT_STATUS` prints the same notice.

A single wrong write log entry that still reads as valid is not detected; QMK's log entries carry no checksum of their own. Your layout file remains the recovery for anything the board cannot read.

## Wear

Each copy wears at QMK's normal rate: a small change appends a few bytes to the write log, and each copy is erased only when its log fills. The second copy doubles the bytes written, not how often any sector is erased.

A consolidation happens when the log fills, about once per 98,000 key changes. It erases each copy one 64 KB block at a time, skipping blocks that are already erased and servicing USB between blocks, then rewrites both copies. The keyboard pauses for a few seconds while it does; on a test board the whole pause, including the host noticing it, measured about 4.5 s. A fresh board's first-boot format erases nothing, since its region is already blank.

## Tests

```
make test:wear_leveling_general test:wear_leveling_2byte test:wear_leveling_2byte_optimized_writes \
     test:wear_leveling_4byte test:wear_leveling_8byte test:wear_leveling_mirror test:wear_leveling_mirror_large
```

`wear_leveling_mirror` covers a damaged or erased copy, a copy missing the last entry, both copies damaged, a retried bad read, and power loss at every write and erase of a consolidating write. `wear_leveling_mirror_large` runs the same tests with a store large enough for multi-word log entries, plus an entry cut off at the end of a copy and copies whose logs stop at an invalid entry.
