# Introducing Svalboard-QMK and Keybard

Svalboard-QMK is the new firmware for [Svalboard](https://www.svalboard.com), paired with [Keybard](https://keybard.svalboard.com/), its browser-based configuration tool. This first formal release brings more space for programmable behaviors, settings that survive compatible firmware updates, more pointing controls, and a protocol that lets desktop tools follow your actual layout and layers.

Your keyboard still runs its saved layout itself. Keybard is for configuration; you can close it and keep typing. The optional Keybard Host companion adds a desktop learning overlay.

These notes describe the move from Svalboard's Vial-QMK firmware to Svalboard-QMK. The comparison baseline is **`svalboard/vial-qmk v2025-11-01`, using the `vial` keymap**. Features and limits of other Vial keyboards or custom builds can differ.

## What changes at a glance

| Area | Svalboard Vial-QMK | Svalboard-QMK and Keybard |
| --- | --- | --- |
| Configuration | Vial and its Svalboard controls | Keybard, with visual layers, behavior editors, pointing controls, diagnostics and a Trainer panel |
| Programmable capacity | 50 tap dances, 50 combos, 30 key overrides and 50 macros | 256 entries each for tap dances, combos, key overrides, macros, alternate-repeat mappings and leader sequences |
| Leader sequences | Not part of this Svalboard configuration | Editable sequences of up to five keys, each triggering an action |
| Tap-hold assistance | Achordion | QMK Chordal Hold, enabled by default, plus configurable Flow Tap |
| Firmware updates | Build-date changes could invalidate the saved configuration | Compatible builds preserve it; explicit storage and keycode versions govern migration |
| Board identity | Previous identification scheme | Persistent identity and a user-set board name |
| Pointing | Per-side DPI and scrolling, axis lock, Sniper hold, automouse and TrackPoint recalibration | Those controls carry over, with Sniper toggles, Boost, per-pointer automouse selection, threshold/decay and natural scrolling |
| Layer lighting | Supported in firmware | Layer colors are editable directly in Keybard |
| Learning your layout | Static references and existing third-party tools | Keybard Trainer preview and an optional native desktop overlay that follows the board |
| Backups | Vial `.vil` files | Native `.svil` backups, with legacy `.vil` exchange subject to format and keycode limits |

Tap dances, combos, macros, key overrides, alternate repeat, mod-taps, one-shots and layered layouts are not new inventions in this release. They carry forward from QMK and Svalboard's Vial firmware. The changes are their integration, capacity, controls, persistence and accessibility through Keybard.

## A Svalboard editor in the browser

Open Keybard in **Chrome or Edge**, connect your keyboard, and work with the layout read from the device. Hardware connection requires WebHID; Firefox and Safari do not provide that connection. Files can be opened without a board for offline editing and preview.

- **Visual assignment:** drag keys from palettes, move or swap assignments, or select a position and type its new binding. Serial assignment can advance through positions as you go.
- **Layer views:** compare multiple layers, use flat or 3D presentation, and inspect transparent positions to understand the assignments underneath.
- **Organization:** name layers, macros and tap dances; copy, paste, clear or make layers transparent; choose layer colors.
- **Stored names:** layer, macro and tap-dance names are saved to the keyboard and loaded on connection. Each name allows up to 16 UTF-8 bytes; accented characters and emoji can take more than one byte. Names are also included in native backups.
- **Live or queued edits:** Live Updating applies supported changes as you make them. Manual Changes lets you review a queue, apply it with Update, or discard it with Revert.
- **Workspace choices:** adjust key sizes and panel placement, and choose System, Light or Dark appearance.
- **International palettes:** use labels and assignments appropriate to your operating system's keyboard layout. Choosing a palette does not change the OS layout.
- **Hardware drawing:** saved finger/thumb cluster selections let Keybard draw the arrangement installed on your Svalboard.

Keybard remembers devices the browser has already permitted, allowing reconnection without another chooser while that permission remains available.

## More room for behaviors and macros

The maintained firmware provides **256 slots in each behavior category**. Tap dances offer tap, hold, double-tap and tap-then-hold outputs. Combos combine up to four keys. Key overrides replace an action under selected modifier and layer conditions. Alternate-repeat mappings associate a remembered key with an alternate action. Leader sequences recognize an ordered sequence of up to five keys and execute an output keycode, which can itself invoke a macro.

Macros can contain text, key presses, key releases and delays, including supported layer actions and Svalboard controls. Macro transfers use wider offsets, removing the old 64 KiB addressing boundary. Macro storage is shared rather than a fixed allowance per slot; the firmware reports the available buffer size.

The one-shot/mod-tap composer provides left/right modifier choices and MEH/HYPER presets. You can construct modifier chords and combined tap/hold keys without memorizing their numeric encodings.

## Tune how the keyboard interprets your typing

Supported timing settings are stored on the board, take effect when saved and survive reboot. They include:

- **Tapping term and tap-dance timing:** adjust when a press becomes a hold and the timing of configured dances.
- **Permissive Hold and Hold on Other Key Press:** choose how another key press influences a pending tap/hold decision.
- **Retro Tapping:** allow a held tap/hold key to produce its tap action when released without another qualifying action.
- **Quick Tap:** tune repeated tapping of a dual-role key.
- **Chordal Hold:** use the hand relationship between keys to help distinguish typing rolls from intended modifier holds. It replaces Svalboard Vial's Achordion integration; its behavior is not an exact copy of Achordion.
- **Flow Tap:** favor taps during rapid typing when enabled by a nonzero interval. It is off by default.
- **Combo term:** adjust the window for recognizing a chord, including configured per-combo timing.
- **One-shot timeout and tap-toggle count:** control how long a one-shot remains pending and when repeated taps lock it.
- **Leader timeout and per-key timing:** control the time allowed for a leader sequence.

The following controls also operate at runtime:

| Control | Behavior | Default |
| --- | --- | --- |
| Tapping toggle count | Number of taps needed for `TT(layer)` to toggle. A stored zero is treated as one; holding and releasing still releases the momentary layer. | 5 taps |
| Tap-code delay | Press-to-release delay for generated taps and text macros. Values above 255 ms retain their full width. | 0 ms |
| Tap-hold Caps Lock delay | Duration used for generated Caps Lock taps and Caps Lock tap-hold bindings. | 80 ms |
| Grave Escape overrides | Independently force Escape when Alt, Control, GUI or Shift is held. | All overrides off |

Encoder-map and DIP-switch-map delays remain compile-time settings. Auto Shift is not enabled in the standard firmware. When migrating, check your timing and modifier preferences rather than assuming the new tap-hold engine will feel identical to the old one.

## More control over pointing

Existing Svalboard pointing features carry over: independent left/right sensitivity and scrolling, scroll hold/toggle, axis lock, Sniper hold keys, automouse, and TrackPoint recalibration. Available controls and sensitivity ranges depend on the installed sensor.

New controls include:

- **Sniper toggles:** keep 2×, 3× or 5× slower movement enabled without holding a key.
- **Boost:** hold or toggle 2×, 3× or 5× faster movement for crossing a large desktop.
- **Per-pointer automouse participation:** decide which pointer can activate the mouse layer.
- **Automouse threshold and decay:** tune activation and how accumulated movement subsides, alongside the existing timeout.
- **Natural scrolling:** select the scroll direction you prefer.

Held and toggled speed modes can be used together; releasing a held mode preserves an already-toggled mode. Layer-lighting colors can now be selected in Keybard instead of requiring a firmware edit.

## Learn your layout with Trainer

**Trainer**, below Layouts in Keybard's sidebar, previews the layout and configures an optional desktop overlay. The browser preview is for understanding its appearance. **Keybard Host** supplies the native window above your other applications and reads live state directly from the keyboard.

- Loads the connected board's actual layout and cluster arrangement, and remembers the selected board.
- Follows active and default layers, including base-layout changes, and resolves transparent positions through the underlying layers.
- Uses Keybard's symbols and action icons, including tap/hold and layer commands.
- Updates native character legends for Shift and Caps Lock where host modifier reporting is supported; function keys retain uppercase names.
- Offers independent color, opacity, outline and halo controls, hand selection and scale, so the overlay can remain legible over different backgrounds.
- Starts draggable. Optional click-through leaves a small grip/menu available for moving or hiding the overlay and reopening controls.
- Provides optional held-key highlighting and configurable layer-change highlights: Off, Quick flash or Short fade, with adjustable duration.
- Includes recall practice and familiar-key masking. The overlay returns to reference mode when the temporary practice controls disconnect.

The companion runs in the system tray; closing the browser does not close the overlay. Its keyboard operations are read-only. After editing the board's layout, use Reload layout in the companion to refresh its copy.

The available **Windows preview** is a portable ZIP, linked at the top of Trainer and on the [Keybard Host download page](https://github.com/svalboard/keybard/releases/tag/keybard-host-v0.1.0-preview.1). Extract it and run `Start-Windows.cmd`. First launch needs internet to install its private runtime; no administrator access, system Python installation or PATH change is required. It is a separate download from the firmware and is not a signed installer.

Linux source launch instructions are available, but Wayland window placement, stacking, click-through and modifier reporting have limitations. macOS remains unverified. The Windows preview should not be read as a promise of equivalent validated native packages on all three platforms.

## Keep, reuse and share your setup

**Native `.svil` exports** carry the layout, macros, supported dynamic behaviors, settings, custom hardware values, names, cosmetic metadata and fragment selections. Use them as your portable backup and when moving between computers.

The **Layouts library** lets you browse bundled layers, save personal layers, preview them, search them, and bring a whole layer or an individual key into your working layout. Personal libraries and presentation preferences remain in browser storage; export files to move or share them.

**Printed layers** provide a paper reference or a PDF through the browser's print facilities. Matrix Tester shows which physical keys are registering for troubleshooting.

Legacy `.vil` files remain useful for exchange with the older ecosystem, but they cannot represent every Sval-specific field. Old keycodes may require migration: a legacy backup is not automatically safe to write unchanged to the new firmware. Keybard checks for this rather than silently applying incompatible keycodes.

## Updates, migration and board identity

Svalboard-QMK replaces build-date invalidation with configuration-layout and keycode version checks. Compatible firmware updates preserve saved settings. Supported keycode-number changes can be translated; changes to the storage layout can still require a reset and restore.

Automatic first-boot migration supports **Svalboard Vial-QMK `v2025-11-01` with the `vial` keymap**. It transfers the supported keymap, macros, behavior tables, pointing preferences and settings into the new store, retaining the old Vial store as a migration source. Other old or custom builds are not covered by that promise. Retaining the source does not make switching back a bidirectional synchronization mechanism.

Each board has a persistent identity and can be given a name in Keybard. Save the name and restart the board for the USB device list to display it. Names and serial identity are stored separately from layout backups. This helps distinguish multiple Svalboards and lets compatible updates preserve their identity.

### Moving from Vial-QMK

1. **Export your existing `.vil` backup with Vial before flashing.** Keep it unchanged, and record important modifier swaps, timing preferences and custom macros.
2. Download the Svalboard-QMK image matching the **sensor family and side** of each half. Use the `sval` keymap for the normal setup.
3. Double-tap reset within 500 ms to enter the RP2040 bootloader, then copy the matching UF2 to the `RPI-RP2` drive. Update the other half with its own matching image as needed.
4. Open [Keybard](https://keybard.svalboard.com/) in Chrome or Edge and connect through its normal keyboard chooser. Svalboard-QMK is configured through Keybard, not the Vial or VIA configurator.
5. Check every layer, important macros, modifier preferences, tap-hold behavior and pointing settings. Supported migration transfers substantial configuration, but the checks and limitations below matter.
6. Export a fresh **`.svil` backup** once the new setup is verified. Install Keybard Host separately if you want the desktop overlay.

### Firmware variants

| Family | Release targets |
| --- | --- |
| Base Svalboard | `svalboard/left:sval`, `svalboard/right:sval` |
| PMW3389 trackball | `svalboard/trackball/pmw3389/{left,right}:sval` |
| TrackPoint | `svalboard/trackpoint/{left,right}:sval` |
| Azoteq | `svalboard/azoteq/{left,right}:sval` |
| Blank base-board layout | `svalboard/{left,right}:blank` |
| PMW3360 trackball, deprecated | `svalboard/trackball/pmw3360/{left,right}:sval`; only for units with this sensor |

The release build matrix contains 12 images. Use the side and sensor designation on the image, not just the word “Svalboard.” Historical `default` and other development keymaps are outside that maintained matrix.

## Idle power and diagnostics

Idle settings can reduce scan rates, dim lighting, place supported trackball sensors in rest modes and reduce processor activity. More aggressive deep-idle settings trade power consumption against responsiveness to the first input after inactivity. Savings and wake behavior depend on the installed hardware and settings; this release does not claim a measured battery-life or universal latency improvement.

For firmware development, optional instrumented builds provide an on-board key-event harness. It injects synthetic presses and captures the firmware's reports, allowing timing, behavior and persistence checks on a test board. It is separate from the normal Matrix Tester and ordinary configuration workflow.

## A protocol for companion applications

Sval protocol version 3 exposes the board definition, layout, behavior tables, settings, active/default layers and physical matrix state. Sparse table reads avoid fetching every empty behavior slot; wide indices address the expanded tables, and 32-bit macro offsets address the larger buffer.

A client-ID wrapper distinguishes replies for cooperating applications. Keybard and a read-only companion can use the same board where the OS permits simultaneous HID access. The wrapper retains VIA-format operations for basic keymap access alongside Sval-specific commands; this does **not** imply compatibility with the VIA or Vial configurator.

State is queried through request/reply polling, not a continuous stream of every switch event. Brief taps can fall between matrix snapshots. There is no automatic layout-change notification for an app's cached copy, and client IDs do not prevent competing editors from overwriting settings.

The firmware also contains an **experimental app-context layer contribution**: a companion can select an existing layer independently of manually held layers and saved defaults, with a lease that expires when the companion stops renewing it. This is development functionality, not app-aware automation supplied by the read-only Trainer companion. See the [context-layer notes](../../../../modules/svalboard/core/docs/CONTEXT_LAYERS.md) for its priority rules and current limits.

## Known limitations and upgrade checks

- **Migration:** check modifier swaps, GUI/Caps remapping, NKRO and one-shot choices after upgrading. The current migration's settings initialization can overwrite copied core options. Legacy macros containing unsupported custom actions can acquire incorrect boundaries; verify those macros against the original backup.
- **Interrupted upgrades:** retaining the old migration source and retrying migration does not guarantee recovery at every write boundary. Interrupted in-place keycode-number upgrades have a known recovery defect. Keep the pre-upgrade backup.
- **Reset:** restart the board after a configuration reset before making new edits; otherwise those edits can be discarded at startup.
- **Alternate repeat:** modifier matching and default-alternate behavior have known defects. Verify custom mappings.
- **Held tap dances:** release a tap-dance key before changing its action to avoid leaving the previous output held.
- **Pointing edge cases:** the firmware review records issues involving buffered scrolling, stacked speed factors, disabling an active automouse layer and PS/2 button-state retention. The timing validation does not resolve or validate those paths.
- **Feature-specific settings:** Auto Shift is absent from standard builds. Some mouse-key fields remain stored without runtime consumers; not every field returned by settings discovery is an effective control.
- **Companion previews:** the native overlay has the platform, polling and cached-layout limits described above. Its held-key display is not a record of every resolved tap dance, combo or macro output.

These are documented limitations of the implementation used to prepare these notes. The [firmware review](../reviews/2026-10-04-qmk-fork-review.md) gives the concrete triggers and technical detail; its historical timing findings should be read alongside the later validation below.

## Validation and further reading

The runtime-settings validation built all 12 release targets and a representative non-Sval RP2040 target. **1,106 QMK tests and 17 Svalboard host tests passed.** An instrumented PMW3389-left test board passed **84 checks** for tapping toggle, tap-code delay, Caps Lock delay and Grave Escape overrides, including persistence across reboot. Its original configuration was restored and verified.

Those results establish the behavior covered by those tests. They do not establish physical hardware validation of every sensor variant, every migration path, idle/wake behavior or every feature in this launch overview.

- [Everyday Keybard feature guide](README.md)
- [Firmware additions compared with Vial](firmware-changes.md)
- [Protocol and companion applications](protocol.md)
- [Runtime-settings validation, commands and results](../reviews/2026-10-05-runtime-tap-settings-validation.md)
- [Firmware source and downloads](https://github.com/svalboard/qmk)
- [Keybard source and companion downloads](https://github.com/svalboard/keybard)
