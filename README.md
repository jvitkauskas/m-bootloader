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
- **MIDI mode (FM-1).** Reinstall firmware from a browser installer over USB
  MIDI, including from a phone, with no computer and no firmware running. It's
  in the menu, and starts by itself when there's no valid firmware
  ([specification §11.6](docs/SPEC.md#116-midi-mode-fm-1)). The unit reads the
  package twice, to check it and then to write it, so an installer's progress
  bar fills about twice as fast as the unit's. Wait for the unit.
- **Recovery menu.** Hold **PLAY** (FM-1) or **SEQ PLAY** (SMK-37) while
  switching on. The menu also opens by itself when the firmware cannot start.
  It shows the firmware's state and size, pending updates and the flash chip,
  and offers:
  - **Boot app** and **Download mode**;
  - **Boot anyway**: start firmware that fails its checksum, once;
  - **Wipe user data**: erase the settings areas the firmware declares, and
    nothing else. The menu lists the exact ranges, and you hold the key for
    3 s to confirm.
- **SD card (FM-1).** With an [m-card](https://github.com/jvitkauskas/m-card)
  board and a FAT32 card fitted, the menu gets an **SD card** item at the top.
  It loads and runs `/MBOOT/MENU.BIN` from the card, which can be any program
  built for it ([docs/SPEC.md](docs/SPEC.md) §11.5). The card is only touched
  when the menu is open. This is new and not yet tested on real hardware.
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
| SMK-37 Pro | tested on hardware: start-up (about 1 s), both keys, every menu action, the calibration guard, an official M-VAVE update, Snoop37 and Baud Girl, all loader operations, installation with M-UPGRADE |
| SMK-37 Elite | same build as the Pro, with the same flash layout; installation with M-UPGRADE tested with the Elite firmware on an SMK-37 |
| FM-1 | tested on hardware: start-up, both keys, every menu action, an official M-VAVE update, SLOOP, all loader operations, installation with M-UPGRADE |

Build `1.4-ac26c70`, with the short configuration, on an FM-1 and an SMK-37 Pro:
installing with M-Flasher and M-UPGRADE, stock firmware, putting the stock
bootloader back, and Baud Girl. On the FM-1 also official 015 and 016 updates,
SLOOP, Felucca, ChoralRoot and Melodee. The SMK-37's own official update has not
been run from it. `VDDIO` has not been measured.

**Official M-VAVE updates and Baud Girl's installer put the stock bootloader
back,** with its full configuration. Reinstall m-bootloader afterwards, with
M-Flasher or M-UPGRADE.

**Do not install 1.3 or older over this version.** They need the configuration it
removes, and stop at their menu with `BAD CONFIG` on every start; **Boot app**
still starts the firmware. Melodee's installer offers 1.2 as an option, so leave
that unticked. Reinstalling this version with M-Flasher fixes it.

**`FAILED -5` from Felucca-family firmware.** Melodee's **Return to official V15**,
and M-UPGRADE on an SMK-37 running such firmware, refuse because the boot area
differs from the package's. The boot area and firmware stay as they were. Use
M-Flasher instead.

**Going from FM-1 016 back to 015 leaves garbage presets and wrong pitch,** with
any bootloader: 016 overwrites the presets once it runs, and official updates do
not restore them. Only a backup with the original area does.

**Baud Girl overwrites the SMK-37's stock presets.** Stock firmware installed
afterwards has none, with any bootloader, until a backup with the original area
is restored. Back up before installing it.

**Not tested on the FM-1's newer flash chip.** FM-1s sold from late 2026 have a
UCUN UC25HQ80 flash (JEDEC ID `0xB36014`) instead of the Puya P25Q80H
(`0x856014`). One such unit did not start after m-bootloader was installed, and
the cause is not known yet
([M-Flasher #13](https://github.com/jvitkauskas/m-flasher/issues/13)). Check
**FLASH CHIP** in M-Flasher before installing. With `0xB36014`, whether through
M-Flasher or M-UPGRADE, install it at your own risk.

## Install

Back up your unit with M-Flasher first. Then install the build for your
board, `smk37-uboot.boot` or `fm1-uboot.boot` from the
[releases](https://github.com/jvitkauskas/m-bootloader/releases):

```sh
m-flasher bootloader --file smk37-uboot.boot
```

M-Flasher writes only the boot area. It keeps the chip-key record that firmware installers need and drops the rest of the configuration, which the bootloader does not use, for more room ([specification §5.3](docs/SPEC.md#53-configuration-blob)). If
the bootloader is ever broken, the USB_KEY dongle still forces the chip into
download mode
([specification §4.3](docs/SPEC.md#43-recovery-independent-of-a-valid-boot-image)).

A unit running M-VAVE's firmware enters download mode on a MIDI message,
whichever bootloader it has, so a stock unit can be backed up first: the
SysEx `F0 22 24 35 7D F7` on its first MIDI port
([specification §4.2](docs/SPEC.md#42-entering-rom-usb-download-mode)). On
Linux, find the port with `amidi -l`, then
`amidi -p hw:N,0,0 -S 'F0 22 24 35 7D F7'`. M-Flasher sends it with its
**DOWNLOAD MODE** button, from
[0.5.0](https://github.com/jvitkauskas/m-flasher/releases/tag/v0.5.0).

### With M-VAVE's updater

`fm1-uboot.fwsc`, `smk37-pro-uboot.fwsc` and `smk37-elite-uboot.fwsc` install
it with M-VAVE's M-UPGRADE, over USB MIDI, without download mode. The unit has
to run firmware that M-UPGRADE can update, such as M-VAVE's, and keeps it.

A package holds no firmware, only our installer (`src/installer.c`): the
unit's firmware stores it and the bootloader starts it. It writes only the
bootloader and its boot-area record, shows its progress on the screen, and
restarts the unit. M-UPGRADE then waits for an updater that never comes, and
times out with an error; the installation is done by then. Tested from
M-VAVE's bootloader on an FM-1 (firmware 015) and on an SMK-37 with the Pro and
the Elite firmware (016). This version was tested on an FM-1 and an SMK-37 Pro,
both on firmware 016, and leaves the same boot area as M-Flasher.

The packages are labelled version 099, as M-UPGRADE refuses the version a unit
already has. If it refuses a unit running other firmware, its force upgrade
skips the model and version checks.

## Build

You need Python 3, GNU Make, Rust and a C compiler, and the
[pi32v2 LLVM toolchain](https://github.com/jvitkauskas/pi32v2-llvm) release
`pi32v2-23.1.5-2`. Nothing from JieLi is needed. On Linux x86-64:

```sh
curl -fLo toolchain.tar.xz https://github.com/jvitkauskas/pi32v2-llvm/releases/download/pi32v2-23.1.5-2/pi32v2-llvm-23.1.5-2-linux-x64.tar.xz
echo 'cb896562418a2be93edb9bac26af4bc81a106a32974002a78e6b9fc203c93990  toolchain.tar.xz' | sha256sum -c
mkdir toolchain && tar -xJf toolchain.tar.xz -C toolchain --strip-components=1
cargo install --locked --git https://github.com/exoticorn/upkr --rev a9e56d9d50d1f64cab7590f2c584a12b173e7715 upkr
make
```

Upkr is the packer that stores stage 1. `make UPKR=<path>` uses one installed elsewhere. The release also has packages for Linux arm64 and macOS, with their checksums in
`SHA256SUMS`. `make TOOLCHAIN=<directory>` uses a toolchain unpacked elsewhere.

| Output | What |
|---|---|
| `build/fm1/uboot.boot` | bootloader for the FM-1 |
| `build/smk37/uboot.boot` | bootloader for the SMK-37 Pro / Elite |
| `build/fm1/uboot.fwsc`, `build/smk37/uboot-pro.fwsc`, `uboot-elite.fwsc` | the same bootloaders as M-UPGRADE packages; the Elite's differs from the Pro's only in the model name M-UPGRADE checks |
| `build/loader/mflr-loader-a.bin`, `-b.bin` | flash loader, at RAM `0x01C20000` and `0x01C28000` |

Builds are reproducible: the same commit and toolchain release give the same bytes. The version
shown by the menu and by M-Flasher is `VERSION` plus the git commit, for
example `1.1-e9a25e1`. The build fails if an image outgrows its slot.

`make test` runs the tests: the formats, the loader protocol, and the start-up
and menu of both boards on a simulated chip and flash. `make sanitize` runs them
under ASan and UBSan, and `make reproducible` checks that two builds match.
Some tests also use flash images of real units and M-VAVE's packages in
`testdata/`. Those contain M-VAVE's firmware, so they are not in the
repository, and the tests skip them when absent.

## Source

| Path | What |
|---|---|
| `src/startup.S`, `src/stage0.c` | stage 0: entry, power and clock, holding the key for download mode ([specification §4.4](docs/SPEC.md#44-the-bootloaders-two-stages)) |
| `src/boot.c` | stage 1: start-up decisions, menu |
| `src/midi_update.c`, `src/usb_midi.c` | MIDI mode: the update protocol, and the USB-MIDI device |
| `src/core.c` | flash formats, checksums, ciphers, updater decoding, wipe and calibration rules (portable C, also built for the tests) |
| `src/contract.c` | what the firmware and updaters are handed at start |
| `src/platform.c`, `src/board.c`, `src/display.c` | clock, SPI flash, keys, LEDs, screen |
| `src/loader.c`, `src/loader_target.c` | the flash loader |
| `src/sdboot.c` | finding a card, and loading and running `/MBOOT/MENU.BIN` from it |
| `src/installer.c`, `src/installer_start.S` | the installer in the `.fwsc` packages |
| `tools/` | container and M-UPGRADE package packing, build checks |
| `docs/SPEC.md` | the specification it was written from: hardware, formats and protocols |

## Clean room

m-bootloader was written from scratch, from a specification of the chip, the
flash formats and the protocols ([docs/SPEC.md](docs/SPEC.md)). No code from
JieLi's SDK or from the earlier open bootloader went into it. The people who
studied the original wrote only the specification, and separate sessions that
never saw the original wrote the code.

MIT licence: see [LICENSE](LICENSE).
