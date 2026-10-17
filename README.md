# m-bootloader

An open-source (MIT) bootloader for the M-VAVE **FM-1** and **SMK-37 Pro /
Elite** synthesizers (JieLi AC791N), plus the small flash loader that
[M-Flasher](https://github.com/jvitkauskas/m-flasher) runs on the chip to read
and write its flash.

It replaces the bootloader M-VAVE ships, adding a way back into USB download
mode and a recovery menu, so a unit with broken or unwanted firmware can be
reflashed without opening it. M-VAVE's stock firmware, Snoop37 and Baud Girl
run with it.

## Features

- **Download mode from the keys.** Hold **REC** (FM-1) or **SEQ REC** (SMK-37)
  while switching on. The unit appears on USB in the chip's download mode,
  ready for M-Flasher.
- **Recovery menu.** Hold **PLAY** (FM-1) or **SEQ PLAY** (SMK-37) while
  switching on. The menu also opens by itself when the firmware cannot start.
  It shows the firmware's state and size, pending updates and the flash chip,
  and offers:
  - **Boot app** and **Download mode**;
  - **Boot anyway**: start firmware that fails its checksum, once;
  - **Wipe user data**: erase the settings areas the firmware declares, and
    nothing else. The menu lists the exact ranges, and you hold the key for
    3 s to confirm.
- **Checked start.** Every firmware file's checksum is verified before the
  firmware starts.
- **Firmware updates.** M-VAVE's own updater and other firmwares' updaters run
  as they do with the stock bootloader.
- **Calibration guard (SMK-37).** Keeps a copy of the factory calibration and
  puts it back if it is erased.
- **Flash loader for M-Flasher.** Fast reads (about 860 KB/s), an on-chip CRC32
  check, checked writes, the flash chip's unique ID, and progress on the
  unit's own screen.

## Status

| | |
|---|---|
| SMK-37 Pro | tested on hardware: start-up (about 1 s), both keys, every menu action, the calibration guard, an official M-VAVE update, Snoop37 and Baud Girl, all loader operations |
| SMK-37 Elite | same build as the Pro; not yet tested |
| FM-1 | builds and passes the simulated tests; not yet tested on hardware |

**Official M-VAVE updates and Baud Girl's installer put the stock bootloader
back.** Reinstall m-bootloader with M-Flasher afterwards.

## Install

Back up your unit with M-Flasher first. Then install the build for your
board, `smk37-uboot.boot` or `fm1-uboot.boot` from the
[releases](https://github.com/jvitkauskas/m-bootloader/releases):

```sh
m-flasher bootloader --file smk37-uboot.boot
```

M-Flasher writes only the bootloader and keeps the rest of the boot area. If
the bootloader is ever broken, the USB_KEY dongle still forces the chip into
download mode
([specification §4.3](docs/SPEC.md#43-recovery-independent-of-a-valid-boot-image)).

## Build

You need Linux x86-64, Python 3, GNU Make and a C compiler, and JieLi's
toolchain 20250324.1:

```sh
curl -fLo toolchain.tar.xz https://jl-update.oss-cn-shenzhen.aliyuncs.com/jieli-linux-toolchains-20250324.1.tar.xz
echo 'f686586bcfb45e0f0bb27fd2b39c7a7f313cb4f0e88a66a14da621ffa8225958  toolchain.tar.xz' | sha256sum -c
mkdir toolchain && tar -xJf toolchain.tar.xz -C toolchain --strip-components=1
make
```

| Output | What |
|---|---|
| `build/fm1/uboot.boot` | bootloader for the FM-1 |
| `build/smk37/uboot.boot` | bootloader for the SMK-37 Pro / Elite |
| `build/loader/mflr-loader-a.bin`, `-b.bin` | flash loader, at RAM `0x01C20000` and `0x01C28000` |

Builds are reproducible: the same commit gives the same bytes. The version
shown by the menu and by M-Flasher is `VERSION` plus the git commit, for
example `1.0-73370d4`. The build fails if an image outgrows its slot.

`make test` runs the tests: the formats, the loader protocol, and the start-up
and menu of both boards on a simulated chip and flash. `make sanitize` runs them
under ASan and UBSan, and `make reproducible` checks that two builds match.
Some tests also use flash images of real units in `testdata/`. Those contain
M-VAVE's firmware, so they are not in the repository, and the tests skip
them when absent.

## Source

| Path | What |
|---|---|
| `src/startup.S`, `src/boot.c` | entry, start-up decisions, menu |
| `src/core.c` | flash formats, checksums, ciphers, updater decoding, wipe and calibration rules (portable C, also built for the tests) |
| `src/contract.c` | what the firmware and updaters are handed at start |
| `src/platform.c`, `src/board.c`, `src/display.c` | clock, SPI flash, keys, LEDs, screen |
| `src/loader.c`, `src/loader_target.c` | the flash loader |
| `tools/` | container packing and build checks |
| `docs/SPEC.md` | the specification it was written from: hardware, formats and protocols |

## Clean room

m-bootloader was written from scratch, from a specification of the chip, the
flash formats and the protocols ([docs/SPEC.md](docs/SPEC.md)). No code from
JieLi's SDK or from the earlier open bootloader went into it. The people who
studied the original wrote only the specification, and separate sessions that
never saw the original wrote the code.

MIT licence: see [LICENSE](LICENSE).
