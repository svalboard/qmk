# Building Svalboard firmware yourself

Linux first. This builds the same image the release workflow builds and wraps
it in the pair UF2 that Keybard installs ([pair-uf2.md](../../../docs/pair-uf2.md)).
Windows and macOS work the same way inside WSL or a Linux container.

## 1. Get the source

Clone the `svalboard` branch. You do not need a fork for this:

```
git clone -b svalboard https://github.com/svalboard/qmk.git
cd qmk
git submodule update --init --recursive
```

If you want your changes on GitHub and you already have a fork of
`qmk/qmk` (GitHub allows one fork per fork network, so it will not offer a
second one), push to that fork instead:

```
git remote add svalboard https://github.com/svalboard/qmk.git
git fetch svalboard
git push origin svalboard/svalboard:refs/heads/my-svalboard
```

## 2. Toolchain and Python

Release images are built and tested with the Arm GNU Toolchain 13.2.Rel1
(`arm-none-eabi-gcc 13.2.1`). Install it next to your distribution's compiler;
no need to replace it:

```
T=arm-gnu-toolchain-13.2.rel1-x86_64-arm-none-eabi
U=https://developer.arm.com/-/media/Files/downloads/gnu/13.2.rel1/binrel
curl -LO $U/$T.tar.xz
mkdir -p ~/arm-toolchain
tar -xJf $T.tar.xz -C ~/arm-toolchain
export PATH=~/arm-toolchain/arm-gnu-toolchain-13.2.Rel1-x86_64-arm-none-eabi/bin:$PATH
arm-none-eabi-gcc -dumpversion
```

GCC 15 (Arm GNU Toolchain 15.2) also builds every combination in this file
and passes the release checks. A distribution's own `arm-none-eabi-gcc` can
differ from Arm's build (different newlib, different defaults); if one fails,
use the 13.2 tarball above and open an issue with the first error line.

Python 3.9 or newer, in a virtual environment:

```
python3 -m venv .venv
. .venv/bin/activate
pip install qmk -r requirements.txt
```

`qmk compile -kb svalboard/either -km blank` works, and so does `make`.
The commands below use `make`.

## 3. Build

The target is `svalboard/<side>:<keymap>`. `either` is the one image for both
halves; `sval` is the default keymap and `blank` has no key bindings.

| Build | Command | Output |
|---|---|---|
| Plain image, no updater | `make svalboard/either:sval` | `svalboard_either_sval.uf2` |
| Updater build (what Keybard can replace later) | `make svalboard/either:sval SVAL_UPDATER=yes` | `svalboard_either_sval.uf2` |
| Release build (what the release workflow makes) | `make svalboard/either:sval SVAL_UPDATER=yes SVAL_UPDATE_RELEASE=yes SVAL_FW_VERSION_STRING=mybuild1` | `svalboard_either_sval.uf2`, `.build/svalboard_either_sval.elf` |

The `.uf2` appears in the repository root. A build that carries
`SVAL_UPDATER=yes` can be installed over the updater (Keybard) later. A plain
build has no updater: once it is on a board, only BOOTSEL installs the next
firmware.

### Flags for your own builds

| Flag | What it does | Use it? |
|---|---|---|
| `SVAL_UPDATER=yes` | Includes the in-firmware updater | Yes, always |
| `SVAL_UPDATE_RELEASE=yes` | Release updater build: refuses every flag below, and sets the release bit that `verify-pair --build release` checks | For a release-style image |
| `SVAL_FW_VERSION_STRING=text` | Up to 16 of `A-Z a-z 0-9 . _ + -`, shown by Keybard | Optional |
| `SVAL_TOUCH_DEV=yes` | Adds a raw HID command (0xFC op 0x42) that reports who owns the touchpad, for the `tools/multitouch` test scripts | Dev only |
| `SVAL_PERF_STATS=yes` | Adds scan timing counters (0xFC op 0x43) | Dev only |
| `SVAL_UPDATE_NO_CHORD=yes`, `SVAL_DEV_KEEP_SETTINGS=yes`, `SVAL_UPDATE_TEST_HOOKS=yes`, `SVAL_TEST_WIPE=yes` | Skip the install confirmation, keep settings across builds, add test halts, wipe settings | Firmware developers only, never on a board you use daily |

`SVAL_TOUCH_DEV` and `SVAL_PERF_STATS` are not part of any released protocol.
They add code and a little work to every scan, and the release build refuses
both, so an image that has them is never a release image. Leave them off unless
you are running the matching test script.

Installing a dev image replaces the release on that board, and every new
build resets stored settings on its first boot, so export your layout first. A board on a
release build refuses only images marked DIAGNOSTIC (Scan Lab, keytest and
host bootloader builds); an ordinary dev image installs the usual way, with the
chord.

## 4. Make the pair UF2

`make_update.py pair` turns the build into the one file that installs both
halves, then `verify-pair` checks it. Run it from the repository root:

```
MU=keyboards/svalboard/tools/make_update.py
python3 -I $MU pair svalboard_either_sval.uf2 -o pair.uf2 \
  --kb svalboard/either --keymap sval --release
python3 -I $MU verify-pair pair.uf2 --build release
```

For a dev build (any of the dev flags, no `SVAL_UPDATE_RELEASE`), leave out
`--release` and verify as plain:

```
python3 -I $MU pair svalboard_either_sval.uf2 -o pair.uf2 \
  --kb svalboard/either --keymap sval
python3 -I $MU verify-pair pair.uf2 --build plain
```

Only `svalboard/either` can be paired; a `left` or `right` build is for one
half and the tool refuses it.

Before the pair step, a release build should also pass the RAM-code check the
release workflow runs (it needs `arm-none-eabi-objdump` on `PATH`):

```
python3 -I keyboards/svalboard/tools/check_ram_funcs.py \
  .build/svalboard_either_sval.elf
```

## 5. Install

- **Keybard:** Firmware, "Install by hand", choose `pair.uf2`. Confirm with the
  chord on the half with USB: hold Index South and Middle South for one
  second (right half `M` and `,`, left half `V` and `C` on the default
  keymap). Keybard installs both halves.
- **BOOTSEL:** hold BOOTSEL while plugging a half in (or tap reset twice, 150
  to 500 ms apart) and copy `pair.uf2` onto `RPI-RP2`. That half installs and
  restarts, then installs the other half over the split cable if that half
  runs an updater build. Copy the file onto the second half too if it does not
  (it is harmless if it does).

## What the workflow does

`.github/workflows/release.yml` runs on a tag named `v<N>` or `x<N>`
(`x3`, `v3.1`). `build-firmware.yml` then builds four release updater
builds in a pinned container with the 13.2 toolchain: `either` with the `sval`
and `blank` keymaps as pair UF2s (`make_update.py pair --release`, then
`verify-pair --build release`), and `sval` for the left and right halves as
plain UF2s. A lint job checks each file against its ELF. Branch pushes and pull
requests build nothing.

### On a fork

The workflows use only the built-in `GITHUB_TOKEN`; no secrets are needed.
Forks have Actions disabled until you enable it (Actions tab), and
`release.yml` skips every job unless the repository is `svalboard/qmk`
(`if: github.repository == 'svalboard/qmk'`, so a fork never publishes
firmware by accident). To run it on your fork, change that line to your
repository, make sure your fork has a `svalboard` branch containing the tagged
commit, and push a tag such as `x99`. The result is a
prerelease on your fork with the four files. Building locally, as above, makes
the same files in a few minutes and is simpler.
