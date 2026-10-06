# Svalboard QMK + Keybard: first launch

**Make Svalboard your own, from the browser.** Keybard brings visual layout design, programmable key behaviors, pointing controls, and hardware diagnostics together. Svalboard QMK runs your configuration on the keyboard, so your mappings, macros, and pointing settings keep working after you close the editor.

This is a compendium of the first formal launch, rather than a changelog against an earlier public Keybard release. It covers the current Svalboard firmware and matching Sval-capable Keybard. For the implementation inventory and the benefit of each fork change, see [Firmware changes](firmware-changes.md). A shorter [launch announcement](announcement.md) is ready to adapt for a release post.

## The highlights

- **Design visually:** drag keys into place, compare layers, inspect transparent-key behavior, and choose a flat or 3D view.
- **Make every position do more:** tap dances, combos, macros, overrides, repeat mappings, and leader sequences are configurable without compiling firmware.
- **Build around your pointing devices:** independent pointer controls, automatic mouse-layer activation, precision Sniper keys, and faster Boost keys.
- **Experiment with feedback:** choose live updates or queued edits, test the physical matrix, and measure scan timing and LED duty with Scan Lab.
- **Keep your setup:** compatible firmware updates preserve settings; supported shipped Vial configurations migrate automatically; native layout files provide a portable backup.
- **Recognize your board:** a saved name and stable serial distinguish your keyboard and preserve its identity across updates.

## Start here

1. Open Keybard in a desktop browser with WebHID support and connect your Svalboard. Keybard's production deployment is configured for `https://keybard.svalboard.com/`.
2. Grant access to the keyboard when the browser asks. Keybard reads the device's definition, current layers, supported feature counts, and settings.
3. Export a **`.svil`** backup before experimenting. This is the native format for the complete Svalboard setup.
4. Drag a key from a panel onto your layout, or select a position and type its new assignment. With Live Updating enabled, supported edits go straight to the board. With Manual Changes, use **Update** to apply queued edits or **Revert** to discard them.
5. Add a behavior in its editor, tune the pointers, or save a layer in the Layouts panel. Close Keybard when finished: the keyboard runs its saved configuration itself.

Chrome and Edge are practical browser choices where WebHID is available. Keybard detects WebHID support; Firefox and Safari do not provide the required connection API. Layout files can be opened for editing without a connected keyboard, but hardware testing and applying changes require a device. Your operating system's keyboard layout still determines how ordinary HID keycodes become characters.

## The Keybard feature compendium

### A visual workspace for a 16-layer keyboard

| Capability | What you can do | Why it helps |
| --- | --- | --- |
| Drag-and-drop assignment | Place keys from the palettes; move or swap assignments, including between layers. | Try a new arrangement without editing C or memorizing numeric keycodes. |
| Typing Binds a Key | Select a position and press a key, including modifier combinations. Serial assignment can advance through positions. | Enter familiar shortcuts directly and fill an entire cluster efficiently. |
| Key palettes | Choose standard keys, modifiers, function/media/system keys, layer actions, mouse keys, and Svalboard controls. | Discover the available actions in one editor. Some keycode families require firmware support beyond merely appearing in a palette. |
| Multi-layer display | View several layers together; switch between flat and 3D presentations. | Compare related layers and see how a small physical keyboard becomes a larger working space. |
| Transparency visualization | Hide transparent layers or keys and inspect the underlying assignment; hide thumb keys when focusing on fingers. | Understand what a position does through layer fall-through. |
| Layer organization | Rename and color layers; copy, paste, blank, or make a layer transparent through its menu. | Give navigation, symbols, numbers, and application layers recognizable roles. |
| Named behaviors | Give macros and tap dances readable names in the editor and native backup. | Recognize an action by its purpose rather than its slot number. |
| Adjustable workspace | Use a sidebar or bottom panel, adjustable key sizes, and responsive cluster spacing. | Keep the keyboard and its editor usable on different screen sizes. |
| International palettes | Choose among the supplied language and layout palettes. | Pick the key labels and assignments appropriate to your host layout. This does not switch the OS layout for you. |

Layer names and other cosmetic editor metadata are included in native exports. Firmware also provides label storage, but the current Keybard device-loading path does not automatically synchronize those labels; a name in the editor is not a promise that it follows the board to another browser.

### Programmable behaviors without a firmware build

The launch Svalboard definitions provide **256 slots each** for tap dances, combos, macros, key overrides, alternate-repeat mappings, and leader sequences. They share storage with other settings; 256 macro slots does not mean 256 individually unlimited macros.

| Feature | Behavior | Benefit and example |
| --- | --- | --- |
| Tap dance | Configure tap, hold, double-tap, and tap-then-hold outputs, with a per-entry tapping term. | Put related actions on one physical position: a symbol on tap and a different action on double-tap. |
| Combos | Turn a chord of up to four keys into another keycode; enable or disable entries and adjust timing. | Add an ergonomic Escape, Tab, or shortcut without sacrificing a dedicated position. |
| Macros | Edit text and key actions, including press/release actions and delays. Firmware handles extended 16-bit keycodes as well as ordinary keys. | Automate a frequently typed string or a repeatable sequence of keyboard actions. |
| Key overrides | Replace a key under selected modifier and layer conditions, with modifier masks and options. | Make Shift plus a chosen key produce a more useful symbol or shortcut. |
| Alternate repeat | Map a remembered key to an alternate output, with modifier conditions and enabled state. | Build common key pairs and editing patterns around QMK's Repeat/Alternate Repeat system. |
| Leaders | Configure an ordered sequence of up to five keys and an output keycode, with leader timing settings. | Make a mnemonic command sequence; the output can be a macro key. |
| One-shot/mod-tap composer | Combine left/right modifier choices visually, with MEH and HYPER presets, and assign the resulting one-shot or mod-tap key. | Enter modifier chords without holding several keys, or combine a tap action with a modifier hold. |
| Runtime QMK settings | Adjust supported tapping, quick-tap, permissive-hold, hold-on-other-key, retro-tapping, combo, leader, Magic/NKRO, and mouse-key settings. | Tune recognition and comfort to your hands without recompiling and reflashing. |

Settings are discovered from the firmware rather than assumed from a generic QMK feature list. A setting or keycode appearing in a shared palette is not evidence that every build implements it. In particular, this launch does not claim working runtime Flow Tap, Chordal Hold, or one-shot timing/locking changes solely because their fields appear in the settings schema; see the [implementation notes](firmware-changes.md#scope-and-compatibility).

### Pointing that fits the way you work

Svalboard combines the two halves' pointing reports and exposes hardware-specific controls through Keybard's Pointing Devices panel and firmware-defined menus.

| Capability | Benefit |
| --- | --- |
| Independent left/right sensitivity and scroll configuration | Use one device for cursor movement and the other for scrolling, or tune each side to your preference. Available DPI choices depend on the sensor. |
| Per-pointer automouse participation | Choose which pointer activates the mouse layer, rather than having both sides trigger it indiscriminately. |
| Automouse threshold, decay, and timeout controls | Reduce unwanted layer activation and choose how the keyboard returns to typing. |
| Sniper keys, 2×/3×/5×, held or toggled | Slow cursor and scroll movement for precise placement without changing the normal DPI. |
| Boost keys, 2×/3×/5×, held or toggled | Move farther and faster when crossing a large desktop, then return to your normal speed. These are multipliers, not a configurable velocity-based acceleration curve. |
| Scroll hold/toggle, axis lock, and natural-scroll controls | Change between cursor and scroll work, avoid unwanted cross-axis scrolling, and select the direction that feels familiar. |
| Keyboard mouse buttons and movement/wheel keys | Keep clicks and pointer actions on the keyboard, including when a physical pointer is inconvenient. |
| TrackPoint recalibration | Recover from pointer drift on hardware that provides the recalibration operation. |
| Layer colors | Give the board's lighting a visible cue for the active layer. |

The firmware maintains Sniper and Boost hold counts separately from their toggle states. Releasing a held precision key therefore does not accidentally cancel a precision mode you toggled on earlier.

### Layouts you can keep, reuse, and learn

- **Layouts library:** browse bundled layers and your locally saved layers, preview them, search them, and drag a whole layer or an individual key into your working layout.
- **Reusable personal layers:** save a layer from its contextual menu, or import a layout file to use its layers as building blocks. The personal library is stored in this browser; saving or “publishing” there does not upload a public community submission.
- **Native `.svil` files:** export and import layouts, macros, supported dynamic behaviors, QMK settings, custom hardware values, cosmetic metadata, and fragment selections. Legacy `.viable` files remain readable; `.vil` is available for legacy exchange but cannot represent every Sval-specific field.
- **Hardware-aware import:** importing into a connected device preserves device-derived information such as its macro-buffer size rather than trusting a saved file to redefine the hardware.
- **Fragment composition:** select supported finger/thumb cluster fragments in Settings to make the drawing match the board's physical arrangement. Selection changes the represented geometry; it does not add electrical keys to the hardware.
- **Printed layers:** print non-empty layers through the browser, including saving a PDF where the browser offers it. A desk reference makes a new layout easier to learn.
- **Reconnect without another chooser:** Keybard lists matching devices the browser has already permitted, so you can reopen one directly while that permission remains available. Client-session renewal is handled by the connection layer.
- **Build identity:** the deployment badge exposes the running build's branch/commit information, helping you identify which editor you are using when reporting an issue.

Native exports are the portable backup. Browser libraries and presentation preferences depend on browser storage; the keyboard's serial and board name are stored separately on the board.

### Inspect the hardware, then tune it

**Matrix Tester** highlights physical key activity so you can check switches and matrix positions. It requires a connected board with matrix-state reporting, which this firmware enables.

**Scan Lab** measures the optical matrix instead of asking you to guess its settling time. The panel exposes status for the halves, timing probes and sweeps, pre/post-wait controls, measured scan intervals, and LED-on duty. It also exposes the active/light-idle/deep-idle pacing and power controls supported by the firmware.

The benefit is a measurable tradeoff: reduce sensor-LED duty and idle power while choosing how quickly the first input after inactivity should be detected. Current estimates in Scan Lab use a model, rather than an onboard current meter.

Power controls include trackball rest modes where supported, idle RGB dimming, sleeping between paced scans, and selectable 48/24/12 MHz deep-idle clocks with optional longer naps. USB and split-link handling are maintained while the firmware manages those clocks. Longer idle scan periods and sensor rest modes can add first-input latency; choose those settings to suit how quickly you want the board to wake.

On one characterized **single half with a PMW3389 trackball**, measurements were approximately **90 mA active, 40 mA with idle measures, and 24 mA in a deeper low-power configuration**. These are measurements of that setup, not guaranteed power figures for every sensor, both halves together, or the default configuration.

## What the firmware fork adds

| Change | User benefit |
| --- | --- |
| Dynamic Sval feature tables and settings | Configure advanced QMK behaviors from Keybard; no firmware build for routine edits. |
| 256-slot tables and 256 macros | More room for specialized layers, shortcuts, and reusable actions. |
| 128 KiB logical settings store and 32-bit macro addressing | Use a macro buffer larger than the old 16-bit address ceiling, after space for other settings is deducted. |
| Layout stamps instead of build timestamps | Keep a compatible setup when reflashing a build from another day. Incompatible storage geometry still resets the affected region. |
| Versioned keycode translation | Preserve supported older keycodes when upstream renumbers them; the current table handles the steno changes from keycode versions 7/8 to 9. |
| One-time shipped-Vial migration | Carry a supported existing board's layout and settings into Sval automatically. |
| Persistent board identity | Keep the serial across updates and settings resets; choose a board name in Keybard. |
| Wrapped client sessions and sparse table reads | Route replies to the right editor session and avoid reading every unused feature slot. Sessions do not prevent two editors from changing the same setting. |
| Board-specific pointer and scan improvements | Tune both pointing devices, precision/speed modes, scan timing, idle power, and split wake behavior. |
| Bounded writes and retryable migration | Reject invalid macro requests and recover a pending migration after allocation failure or an interrupted copy. |

[Read the complete fork-change catalog →](firmware-changes.md)

## Updating from Vial and choosing firmware

Automatic migration targets **`svalboard/vial-qmk` release `v2025-11-01`, keymap `vial`**, with the expected 16-layer, 10×6 matrix layout. It recognizes that release's stored layout and supported saved-settings versions; it is not a general importer for every Vial build.

Migration carries the core settings, keymap, macros, pointing/layer-color settings, QMK settings, tap dances, combos, overrides, and alternate-repeat entries. The legacy flash store is left intact. A persistent completion flag prevents a later settings wipe from bringing the old setup back; a pending flag permits retry after an interrupted attempt. Already initialized Sval stores are preserved unless an earlier migration is pending.

Export a backup before the upgrade. Other old releases or unrecognized layouts take the normal initialization path. Newly incompatible storage layouts can still require a reset; preserved data is not a promise of lossless upgrades between arbitrary versions.

The launch build matrix includes left and right variants of:

| Target family | Hardware |
| --- | --- |
| `svalboard/{left,right}` | Base board |
| `svalboard/trackball/pmw3360/{left,right}` | PMW3360 trackball |
| `svalboard/trackball/pmw3389/{left,right}` | PMW3389 trackball |
| `svalboard/trackpoint/{left,right}` | TrackPoint |
| `svalboard/azoteq/{left,right}` | Azoteq pointing hardware |

Choose the **sensor family and side** that match the half you are updating. Use the maintained **`sval`** keymap for the regular setup. Base-board **`blank`** builds are also included in the release workflow; **`scanlab`** is a diagnostic keymap, not the normal daily-driver recommendation.

After installing QMK's build tools, an example build is:

```sh
qmk compile -kb svalboard/trackball/pmw3389/right -km sval
```

Enter the RP2040 bootloader by double-tapping reset within 500 ms. The half appears as **RPI-RP2**; copy its matching UF2 to that drive. See the [board README](../../readme.md) for build and handedness details. Match the editor to Sval firmware: the client wrapper and protocol v3 are not interchangeable with an arbitrary Vial/VIA client.

In Keybard's Settings, **Board name** accepts up to 32 characters within the firmware's 64-byte UTF-8 limit. Save it, then restart the keyboard for the computer to show the new USB product name. The serial stays the same. This persistent identity requires the supported 16 MB flash region; it does not live in a layout file.

## Release provenance and claim boundaries

This documentation was checked against firmware **`f2a842b1d3e839cd35c8d85a1f8198014c269361`** and Keybard **`61288ac4490d6c9fb211c765787f8398274d0ad0`**, on 2026-10-04. It describes those sources; it does not assign a release tag or certify that a deployment has completed.

Keybard evidence can be found in the [reviewed source tree](https://github.com/svalboard/keybard/tree/61288ac4490d6c9fb211c765787f8398274d0ad0):

| Claims | Source in that tree |
| --- | --- |
| Visual editing, layers, flat/3D views | `src/layout/EditorControls.tsx`, `src/layout/EditorLayout.tsx`, `src/contexts/KeyBindingContext.tsx`, `src/contexts/LayerContext.tsx` |
| Palettes, behavior editors, pointing controls, modifier composer | `src/layout/Sidebar.tsx`, `src/layout/SecondarySidebar/Panels/`, `src/services/{macro,tapdance,combo,override,vial}.service.ts` |
| Native files and connected-device import | `src/services/file.service.ts`, `src/services/import.service.ts`, `src/layout/SecondarySidebar/Panels/SettingsPanel.tsx` |
| Local library and previews | `src/services/layer-library.service.ts`, `src/layout/SecondarySidebar/Panels/LayoutsPanel.tsx`, `src/pages/ExploreLayoutsPage.tsx` |
| Scan Lab and matrix testing | `src/services/scanlab.service.ts`, `src/layout/SecondarySidebar/Panels/ScanLabPanel.tsx`, `src/components/MatrixTester.tsx` |
| Permitted-device reconnect | `src/components/ConnectKeyboard.tsx`, `src/contexts/VialContext.tsx`, `src/services/usb.service.ts` |
| Name/serial UI and protocol v3 | `src/services/identity.service.ts`, `src/layout/SecondarySidebar/Panels/BoardIdentitySection.tsx`, `src/services/usb.service.ts` |
| Production deployment destination | `.github/workflows/svalboard-deploy.yml`, `.env.svalboard` |

Firmware evidence and the precise upstream comparison are linked in the [fork-change catalog](firmware-changes.md). Existing generic QMK documentation describes many other capabilities; this compendium claims only the integrations supported by the reviewed Svalboard sources. Palette presence alone is not a claim of working audio, MIDI, Bluetooth, or every upstream QMK subsystem on this board.
