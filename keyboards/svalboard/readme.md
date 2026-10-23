# Svalboard RP2040

![Svalboard](https://www.svalboard.com)

Svalboard - Datahand Forever!

Keyboard Maintainer: [morganvenable](https://github.com/svalboard/vial-qmk) [@_claussen] on discord
Hardware Supported: RP2040, Svalboard Alpha, Svalboard Lightly
Hardware Availability: [Store](https://www.svalboard.com)

See the [release notes](https://github.com/svalboard/qmk/releases/latest) for what's new and how to move from Vial, and the [Keybard guide](https://github.com/svalboard/keybard/blob/main/docs/guide.md) for configuring the board.

Build examples for the maintained Sval keymap (after setting up your build environment):
```
make svalboard/right:sval
make svalboard/left:sval
```
or
```
make svalboard/trackpoint/right:sval
make svalboard/trackpoint/left:sval
```

--------------------------------------------------------------

For vanilla QMK (not maintained):

```
qmk compile -kb svalboard -km default
```
You can then copy the UF2 file from your qmk_firmware root directory to the device's drive letter.

Or you can flash directly with:
```
qmk flash --keyboard svalboard --keymap default
```

The one-image targets (`svalboard/either`, `svalboard/trackball/pmw3389/either`) run the same image on both halves: each half takes its side from a thumb strap or its stored side record, which a host sets with Sval `0x33` ([protocol notes](docs/protocol.md), "Side"), and a half that ran a per-hand build keeps that side. The per-hand targets below work as before.

The per-hand targets use the EE_HANDS flag to set handedness, so the MCU is either Left or Right, permanently. If for some reason you need to reflash this parameter to be the other hand, you must add the appropriate side bootloader parameter to program the eeprom -- this cannot be done by drag-and-drop of a UF2 file:
```
qmk flash --keyboard svalboard --keymap default -bl uf2-split-left
qmk flash --keyboard svalboard --keymap default -bl uf2-split-right
```

See the [build environment setup](https://docs.qmk.fm/#/getting_started_build_tools) and the [make instructions](https://docs.qmk.fm/#/getting_started_make_guide) for more information. Brand new to QMK? Start with our [Complete Newbs Guide](https://docs.qmk.fm/#/newbs).

## Bootloader
To enter bootloader mode, simply double tap reset in 500ms or less.  The device will come up as a mass storage device titled RPI-RP2.  Drag the desired UF2 file to the drive and it will immediately reboot and enumerate as Svalboard.

## Svalboard-specific firmware features
See the [release notes](https://github.com/svalboard/qmk/releases/latest) and, for app developers, the [protocol notes](docs/protocol.md).

For automated action and persistence checks without physical keypresses, see [on-device key-event tests](docs/keytest.md).
