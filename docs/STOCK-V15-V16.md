# M-VAVE's FM-1 V15 and V16 bootloaders

What changes between the boot areas of M-VAVE's FM-1 firmware 015 and 016, measured on an FM-1 and read
from the packages. This records facts for a possible setting that starts the FM-1 as either version does.
It is not part of the specification yet.

## Sources

**Packages.** Boot areas and boot configurations were read from these packages:

| Package | SHA-256 | Where it comes from |
|---|---|---|
| FM-1 015 | `db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a` | M-VAVE's fm1_open repository, `sdk/firmware/FM-1_015.fwsc` |
| FM-1 016 | `d017fb65e70aca97ee29d944470c3515a74581df5c0576bf70de917c7b2b61d5` | `https://yms-file-store.oss-cn-hongkong.aliyuncs.com/software/firmware/FM-1.fwsc`, last modified 2026-10-21 |
| SMK-37 Pro 016 | `2d98ce72530e71d617384963423820536f094501299e2cfec0a8d3c3fb6bcfb0` | `…/software/firmware/SMK-37Pro/SMK-37%20Pro_016.fwsc`, last modified 2026-08-12 |
| SMK-37 Elite 016 | `b2ee38abfef3ae922e4eecdb8a551e14345300ae6f270265feacd33b6ef99cb6` | M-VAVE's update for the Elite, `SMK-37Elite_016.fwsc` |

**Hardware tests.** These ran on 2026-11-21 on one FM-1 (flash chip `0x856014`, chip key `0x980F`):
- **A voltage probe:** a RAM app that reads the P33 voltage registers `0x05`, `0x06` and `0x09`, started with
  M-Flasher's `run-ram`. Run that way, the app starts after the ROM's download mode. The tests reached
  download mode in two ways:
  - **from firmware:** the running firmware is sent the download-mode message, and resets straight into the
    ROM;
  - **from power-on:** the unit is switched on with REC held, under m-bootloader, so only the bootloader runs
    first.
- **A boot probe:** a program installed as the application, so that the installed bootloader starts it. It
  records:
  - the registers at entry;
  - the boot object at `r0`;
  - the clock, PLL and flash-controller registers;
  - instruction timings.

  It then requests download mode, and the record is read back from RAM. The records under the 015 bootloader
  and m-bootloader 2.3 were made on 2026-11-14 on the same unit.

## The bootloaders

| | FM-1 015 | FM-1 016 | SMK-37 Pro and Elite 016 |
|---|---|---|---|
| `uboot.boot` | 14,384 bytes, `730e54f0a439f58d147be4364ad21e19566945ada9d3a7bbc8371dce5068d3ef`: JieLi's AC79 SDK 1.1.9 bootloader | 14,488 bytes, `b88747cb72afc554d6ca078594a89f4fdcea5a74d26345b633f8e2524e7e3415`: JieLi's AC79 SDK 1.2.13 (2026-04-20) bootloader, the same file as `sdk/tools/pack/uboot.boot` in fm1_open | the same file as FM-1 015's |
| Updater (`ota.bin`) | same in all three | same | same |
| Other package members | — | `farg.cfg` (5 bytes), purpose unknown | — |

## The boot configurations

`isd_config.ini` in the boot area, after its 34-byte chip-key record:

| Key | FM-1 015 | FM-1 016 | SMK-37 016 |
|---|---|---|---|
| `SYS_CLK` | 360,000,000 | **320,000,000** | 360,000,000 |
| `HSB_DIV`, `LSB_DIV` | 1, 2 | 1, 2 | 1, 2 |
| `OSC`, `OSC_FREQ`, `SPI`, `RESET` | `OSC0`, 24 MHz, `4_1_0_0`, `PB01_08_0` | same | same |
| `VDDIO` | absent | **4** | absent |
| `DVDD`, `DCDC14` | absent | **11, 3** | absent |
| `VLVD`, `UTBD` | 4, 1,000,000 | absent | 4, 1,000,000 |
| `SDRAM_TEST_SIZE`, `SDRAM_CONFIG_VAL` | absent | 4096, 0 | absent |
| `ENABLE_SDRAM` and the other `SDRAM_*` keys | 0, same values | same | same |

The SMK-37 016 configuration is byte-identical to FM-1 015's after the chip-key record. So the SMK-37 has only
V15 behaviour; there is no V16 behaviour on it to follow.

## What the bootloaders hand to the application

From the boot probe and the voltage probe. Each row is a register the probe read; rows not listed read the
same under all three bootloaders.

| | 015 bootloader | 016 bootloader | m-bootloader (2.3 clock; 2.4 voltages) |
|---|---|---|---|
| System clock select, `0x10014` low byte | 6: from the second PLL, 360 MHz | **2: from the 480 MHz main PLL, 320 MHz** | 6 |
| Second PLL, `0x119A8` / `0x119AC` | `0x0940022B` / `0x0750310C` | **0 / 0** (left off) | as 015 |
| Main PLL, `0x119A0` / `0x119A4` | `0x45400203` / `0x3F503026` | same | same |
| Bus dividers, `0x10008` | `0x00010200`: HSB = system/2, LSB = HSB/3 | same, so **HSB 160 MHz, LSB 53.3 MHz** | same |
| Flash controller control, `0x40200` | `0x809803B5`: chip-select-high field (bits 23:20) 9 | **`0x808803B5`: 8**, as SPEC §3.6's rule gives for HSB 160 MHz | as 015 |
| Flash baud, base, read mode | 1, `0x4000`, quad output | same | same |
| I/O voltage, P33 `0x05` bits 2:0 | not written | **4 (3.2 V)** | 2.4: 4; since PR #38: not written |
| Core voltage, P33 `0x09` bits 3:0 | 11 (1.26 V) | 11 | 11 |
| Domain voltage, P33 `0x06` bits 2:0 | 3 (1.40 V) | 3 | 3 |
| `r0`, the boot object, `r1` (entry `0x02000120`) | as SPEC | identical | identical |
| `psr`, `icfg`, `EMU_CON`, `DBG_EN`, PC limits, cipher control | as SPEC | identical | identical |
| CPU cycles for each timed block, from flash cold and warm | — | identical to 015's | identical |

**Read differently, not explained:** ANA `0x11978` (`0xFC3A39CD` under 015, `0x00020081` under 016),
ANA `0x119DC` (`0x00001D00`, `0x0000F000`) and the flash controller's `+8` code register (`0x965D`,
`0x9E59`). m-bootloader never writes any of them, yet under 2.3 they read as under 015. They probably report
state that follows the clock set-up, but that is not established.

**Retained RAM** from `0x01C7FDD8` up, including the vector area, holds what the previously running program
left. Under 016 it held what look like 016 firmware code addresses. No bootloader is known to write it.

## The voltages while firmware runs

| Measured | I/O, `0x05` | Core, `0x09` | Domain, `0x06` | P33 `0x11` |
|---|---|---|---|---|
| Power-on, nothing writes `0x05` (m-bootloader from PR #38, REC held) | 7 (3.5 V) | bootloader's 11 | 3 | `0x05` |
| Under FM-1 015 firmware, any bootloader | 7 (3.5 V) | **9 (1.20 V)** | 3 | `0x39` |
| Under FM-1 016 firmware, 016 bootloader | 7 (3.5 V) | **9 (1.20 V)** | 3 | `0x39` |

- **The chip powers up with the I/O rail at 3.5 V.**
- **Both firmwares set their own voltages while they run,** with the same results.
- **016's 3.2 V lasts only from start-up until its firmware starts.**
- **P33 settings survive a software reset,** including the reset into download mode. Under m-bootloader 2.4 with
  REC held, the probe saw that bootloader's own writes: I/O code 4 and core code 11.

## What a "V16" start would mean for m-bootloader

Only from the measurements above:
- system clock select 2, with the second PLL not started;
- the flash chip-select-high field at 8;
- I/O voltage code 4;
- every timing that is worked out from LSB reworked for 53.3 MHz. That covers Timer 0's delays, SPI1's clock
  for the panel and the SD card, and the MIDI mode's timing. They now assume a whole-number 60 MHz.
- 320 MHz as the clock passed to the application.

## Not known

- **The clock the firmware sets for itself.** Neither probe sees it: one records the bootloader's set-up, and
  the other runs after the download-mode loader has set its own clock.
- **Changes in the 1.2.13 bootloader that leave no trace in these registers,** such as the order of the
  start-up steps, delays, flash set-up or the new `USB_UPDATE_CUSTOM` key.
- **What `farg.cfg` is for.**
- **Why M-VAVE changed the clock and the I/O voltage.**
