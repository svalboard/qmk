# Vendored: SHA-512 from Monocypher 4.0.2

SHA-512 for the in-firmware updater: the manifest hash the chord confirms, and
the image hash checked at verify and again at commit. Compiled only when the
build sets `SVAL_UPDATER=yes` (`keyboards/svalboard/updater/updater.mk`).

- **Upstream:** https://github.com/LoupVaillant/Monocypher
- **Release tarball:** https://github.com/LoupVaillant/Monocypher/releases/download/4.0.2/monocypher-4.0.2.tar.gz
- **Tarball SHA-256:** `38d07179738c0c90677dba3ceb7a7b8496bcfea758ba1a53e803fed30ae0879c`
- **License:** dual BSD-2-Clause / CC0-1.0, your choice (`LICENCE.md`, copied from the tarball). Not GPL.

Only the SHA-512 code is kept. The two files are cut from the tarball's
`src/` without other changes; each says at its top which lines it holds.

| File | Cut from | SHA-256 |
|---|---|---|
| `sha512.c` | `src/optional/monocypher-ed25519.c` lines 1-52 (license) and 60-310 (utilities, SHA 512); `crypto_wipe` from `src/monocypher.c` lines 163-167 | `c504c22bee90077de47eb295c8791a78bc7bc55b655a6008697c7a78d63211df` |
| `sha512.h` | `src/optional/monocypher-ed25519.h` lines 1-52, 71-76 (`crypto_sha512_ctx`) and 84-91 (SHA 512); `src/monocypher.h` line 77 (`crypto_wipe`) | `7c88358cad4465d945947fe72725da7490fd7836f857a1688ed1e47c1291397b` |
| `LICENCE.md` | `LICENCE.md`, byte for byte | `5f8360e4c06ddcc584bdb4b210c6af824c4bb301e6a9a521869b6d90795ca4b3` |

The source files they were cut from, for checking against the tarball:

| Tarball file | SHA-256 |
|---|---|
| `src/monocypher.c` | `afe2b098c8569577a84488e0b98d276d1fba6506adea68bb9241a52111734c59` |
| `src/monocypher.h` | `f78bb31255cfb7beba66afd2137f5194c8a025cf40488b6cc1e295234d43f374` |
| `src/optional/monocypher-ed25519.c` | `7c9b16056cbd27521919e8a6f56a228808b9e718afc42e3d33f28c08e5abdee2` |
| `src/optional/monocypher-ed25519.h` | `bd546edcd468d64e28caa3dbf4b1d6bfad7435c0ce994723fd81aae26405121b` |

## Use

- `#include "sha512.h"` (this directory is on the include path: `EXTRAINCDIRS`
  in `updater.mk`), then `crypto_sha512` or `crypto_sha512_init` / `_update` /
  `_final`. This is standard SHA-512 (FIPS 180-4).
- The host tests (`util/updater_test/run.sh`) check the FIPS 180-2 vectors and
  that the incremental and one-shot forms agree.
