# Clean-room specification: m-bootloader and the M-Flasher RAM flash loader

Version 1.0, 2026-10-16, for the M-VAVE FM-1 and SMK-37 Pro / Elite, JieLi AC791N / WL82, pi32v2. This document describes interfaces, hardware transactions, data formats and observable behavior. It supplies no implementation source, disassembly, vendor routine names or prescribed source organization. The replacement implementation is to be independently written in C and pi32v2 assembly and released under MIT.

This specification merges two drafts written independently on the dirty side from the same brief: one by OpenAI Codex (GPT-6.1 Sol, high effort), which is the base of the text below, and one by Claude (Opus 5.5), whose clean-room procedure, read-command summary, suggested menu wording, font, source lists and test-data notes were added. Where the drafts disagreed, the facts were rechecked against the sources and the images; the decisions are in §13.2. See `CLEANROOM.md` for the process record.

## 0. Clean-room procedure

This section binds the implementer (the "clean side"). Everything else in the document is what the clean side gets to know.

### 0.1 What the implementer may and may not use

* Write all code new, under the **MIT** licence.
* MAY read: this specification and its amendments (§0.3); public datasheets (the AC791N datasheets in the SDK's `doc/` folder, the ST7789V datasheet, datasheets for the two flash chips, `0x85 60 14` and `0xEB 60 14`, and a Winbond W25Q-series datasheet for the common command set); JieLi's public documentation (doc.zh-jieli.com); register names, addresses and structure layouts in the AC79 SDK's **header** files; the public notes and tools listed in Appendix B; M-Flasher's host code (`src/` of the M-Flasher repository, MIT); the USB_KEY dongle's code (E9); the blanked test images (Appendix C).
* MUST NOT read or use: the previous open bootloader (`fm1-bootloader`, evidence E1/E2), its reconstruction and annotations (E6), JieLi's bootloader binaries (`uboot.boot`, also inside stock flash images and inside M-VAVE `.fwsc` packages) or any disassembly of them, JieLi's flash loader binary, M-Flasher's current `loader/` directory (E4), or the SDK's `.c` source files that implement the same jobs (clock setup, flash drivers, update handling, LZ4). The evidence catalog in §1.2 names these paths as provenance only.
* MUST NOT copy code from any source, public ones included, unless its licence is compatible with MIT and it is credited; public material is for facts.
* MUST NOT link libraries shipped with JieLi's toolchain (`libc.a`, `libcompiler-rt.a`): build freestanding and write the few helpers needed (memory copy/fill/compare, and any compiler helper the code generator calls).
* Keep a work log in the repository (`LOG.md`): decisions, questions asked, answers received, and every public source consulted for a fact.

### 0.2 Asking the dirty side

When this specification is incomplete, unclear or wrong (including when hardware behaves differently from what it says), do not guess around it and do not look for the original:

1. Write the question in `QUESTIONS.md` in the repository: a number, what is unclear, why it matters, and what was observed (for hardware: exact steps, register values, symptoms).
2. The maintainer takes it to a dirty-side session (one with access to the sources of §1.2).
3. The answer comes back as facts only: register values and ordering, formats, timings, observed behavior. It contains no code, no names and no structure from the dirty sources.
4. Each answer is added to this specification as a numbered amendment (Appendix D) and, where it changes a section, in that section too. The specification stays the single record of what the implementer was told.

### 0.3 Test data

Blanked copies of real flash images are provided for offline tests (Appendix C). The bytes of the stock bootloader (flash `0xA0`…`0x38CF`) are overwritten with `0xFF` in them. They contain M-VAVE's firmware and one unit's calibration: never commit them to a public repository.

## 1. Scope, requirements and evidence

Two independently deployable programs are required:

* **A: m-bootloader.** A RAM-executed second-stage boot image stored in the first 16 KiB of flash. The mask ROM loads it. It starts the installed application, honors staged update requests, provides a recovery key and a display menu, and protects specified unit data during its own writes.
* **B: download-mode flash loader.** A RAM program uploaded by the PC through the mask ROM. It registers a command handler in the ROM's existing USB mass-storage implementation. It gives the unchanged M-Flasher host access to raw SPI flash and a few display and identification commands.

**MUST** denotes an external compatibility or safety requirement of the replacement. **SHOULD** denotes a recommended engineering property. **MAY** denotes a free design choice. Requirements explicitly described as replacement policy are deliberate improvements or constraints, not claims about all existing implementations. No binary identity, instruction sequence identity, source layout, internal address layout, private helper interface, or particular font is required.

Numbers with `0x`, opcode/byte strings, compact address ranges and address columns containing values such as `C1000` are hexadecimal. Ordinary byte counts, structure offsets written as 0/2/4/8/12/16, dimensions and timing counts are decimal unless marked otherwise. Memory and flash ranges are half-open `[start,end)` unless inclusive endpoints are explicitly stated. Source-level hardware register labels are used only as hardware identifiers.

The devices have 1 MiB SPI NOR flash, no external SDRAM/PSRAM, a 24 MHz oscillator, and a 240 × 240 ST7789-family color display. A MUST boot compatible stock M-VAVE images, FM-1 Felucca and SLOOP images, and SMK-37 Snoop37 images without altering them. Baud Girl is a valuable SMK-37 compatibility case because its declared storage layout differs. Firmware's own startup shortcuts MUST remain available. B MUST work with the host protocol in §10 without requiring host changes. [E1, E2, E3, E4, E8; H/S/O as defined below]

### 1.1 Evidence convention

Each table or paragraph's trailing evidence reference applies to all its factual entries unless overridden. A normative requirement derived from a fact has the same evidence; a newly imposed policy is labeled **policy**. These confidence classes are deliberately different:

| Class | Meaning |
|---|---|
| **H** | High confidence: directly checked SDK hardware/interface declaration, host serialization, or independently decoded bytes/checksums in supplied artifacts. It does not mean hardware was exercised in this investigation. |
| **S** | Strong source evidence: consistent current code and/or stock instruction analysis, without an independent electrical measurement. |
| **O** | A hardware observation reported in repository documentation; not repeated by the author of this specification. Applies only to the device/condition reported. |
| **I** | Inference from a flow, register accesses, or related chip documentation. Must be validated where consequential. |
| **U** | Unverified or unresolved. No implementation should silently assume it has been established. |

No hardware was accessed in preparing either draft; source directories were read only, and each draft was written without seeing the other. Hardware facts marked O come from earlier documented tests.

### 1.2 Evidence catalog

Paths are provenance for the dirty-side audit, **not dependencies that the clean implementer needs to open**. Of these, the implementer MUST NOT open E1, E2, E4 and E6, nor the boot areas or packages of E8 (§0.1); E3, the SDK headers of E5, E7 and E9 are public or MIT material the implementer may read. All required numerical interfaces are reproduced below. Filenames identify evidence only and do not prescribe the replacement's modules.

| ID | Inspected material |
|---|---|
| E1 | `/root/fm1-bootloader`, revision `d16c403`: `README.md`, `docs/recovery-menu.md`, board descriptions and board pin transactions, image writer, linker/build constraints. README has an older blanket “not tested” notice; the more specific recovery-menu hardware reports take precedence only for their stated cases. |
| E2 | Same checkout's startup, boot interface, boot decisions, configuration, clock, flash, filesystem, efuse, cache, timer, power, GPIO, menu, display and calibration guard sources. These establish current behavior, not independent hardware proof. |
| E3 | `/root/fm-flasher`, revision `9d96274`: `src/jl.rs`, `formats.rs`, `boards.rs`, `sectormap.rs`, `ops.rs`, `tests.rs`, `scsi.rs`, `usb.rs`; host wire serialization and image recognition are authoritative for host compatibility. |
| E4 | Same checkout's `loader/README.md`, loader entry/command behavior, register accesses, display behavior, linker and build files. |
| E5 | `/root/fm1-toolchain/fw-AC79_AIoT_SDK`, revision `d179b448`: `include_lib/driver/cpu/wl82/asm/WL82.h`, `csfr.h`, `spi.h`, `p33.h`, `clock.h`; `include_lib/system/boot.h`; `include_lib/update/update.h`; `cpu/wl82/tools/isd_config_rule.c`; `cpu/wl82/sdk_ld_sfc.c`. SDK version differences are called out below. |
| E6 | `/root/fm1-toolchain/bootloader-rebuild/ANNOTATIONS.md`, relevant portions of annotated stock instructions and C reconstruction: entry, efuse, clock, update, flash and application handoff. The annotation prose contains some contradictions resolved below against instructions and SDK declarations. |
| E7 | `/root/fm1-toolchain/references/jl-uboot-tool/jltech/uboot.py`; `jl-misctools/firmware/fwunpack_newfw.py`, `jltech/chipkeybin.py`, `jltech/cipher.py`; local jielie notes on BANKCB, JLFS, ENC, SPI and SFC. General chip-family descriptions are weaker than WL82-specific evidence. |
| E8 | Independent read-only Python decoding during this investigation of the four `/root/flasher-testdata/*.bin` images and two `.fwsc` packages. Checked table CRCs, both application file CRCs in each image, boot encryption boundaries, plain configuration records, identical boot areas and staged updater decoding. Reference results are in §12. |
| E9 | `/root/usbkey-pico/main.py` and `/root/smk37-backup/README.md`: USB_KEY wiring/protocol and the reported real SMK-37 backup/recovery observations. The backup is unit-private and MUST NOT be distributed as a generic image. |

### 1.3 Deliberately excluded support

External RAM initialization/training, SPI3 PSRAM, SDRAM application images, flash larger than 16 MiB / four-byte flash addressing, SD/NAND/OTP boot targets, UART updating, RF provisioning, efuse programming, chip erase, a new USB stack in A, firmware installation from A's menu, and general settings UI are outside this specification. These devices do not require them; B MUST NOT expose irreversible efuse programming or chip erase. A MAY omit unused oscillator and flash read modes as described in §3, but MUST support the supplied stock configuration and firmware interfaces. Dual-bank directory selection is specified because the existing boot contract supports it, although no supplied image exercises it. [E1–E7; S, with dual-bank hardware U]

## 2. Machine, address spaces and peripheral register facts

### 2.1 CPU and memory

The CPU target is pi32v2, toolchain CPU `r3`, little endian, 32-bit pointers. Use the target compiler's C calling convention: the first two argument words are in `r0`, `r1`, the result is in `r0`; normal calls must preserve the ABI's saved registers. The CPU has a main stack and a system/exception stack, and exception return state separate from normal return state. A's entry is exceptional; B's entry is a normal ROM call. [E1, E2, E4–E6; H/S]

| Address/range | Meaning and constraint | Evidence/confidence |
|---|---|---|
| `0x00010000` onward | Low-speed peripherals; these are CPU addresses, not flash offsets. | E5 H |
| `0x00040000` onward | High-speed peripherals, including SFC. | E5 H |
| `0x00050000` onward | GPIO and pin mux. | E5 H |
| `0x01C00000` through below `0x01C80000` | Internal RAM address region used by SDK link layouts; this is not permission to overwrite all of it while ROM runs. | E5 H; physical full extent I |
| `0x01C02000` | A payload load and entry address. | E1, E6, E8 H/S |
| `[0x01C02000,0x01C0A800)` | A's entire live code/data/stacks/workspace MUST fit here while a stock updater can execute. | E1, E2 S/O |
| `0x01C0A800` | Supplied updater's decompressed load/entry. | E8 H |
| `0x01C20000`, `0x01C28000` | B's two link/entry locations. Each image and all private RAM MUST fit within its own `0x8000`-byte slot. | E3, E4 H |
| `0x01C7FD80` onward | Retained boot/update/application communication area; exact layouts in §6. MUST survive A's own initialization. | E2, E5, E6 H/S |
| `0x01EEE008` | Cache control. | E5 H |
| `0x01F00000` etc. | Cache internal arrays, cleared only as specified in §3.6. | E2, E6 S |
| `0x02000000` | Decrypted, cached SFC logical flash window. Application logical address zero is mapped here. | E2, E5–E8 H/S |
| `0x02000120` | Entry for all supplied XIP application images. | E8 H |
| `0x04000000` | External RAM window, absent on these boards. Reject applications requiring it. | E1, E5 S |

The complete mask-ROM address map, its private RAM allocations, full CPU control-state reset values, interrupt vector arrangement and all ABI subtleties have not been established here. B MUST restrict itself to its slots and the supplied ROM callbacks rather than guessing private ROM addresses. [U; policy]

### 2.2 Register directory

MMIO register slots below are four bytes apart. Where the SDK declares an 8- or 16-bit register, unused bytes in the slot are reserved. Current implementations use word accesses to many such slots; byte-lane equivalence is not independently measured. Read/modify/write MUST preserve unrelated bits where only a field change is specified. `bit n` means `1 << n`. [E2, E4, E5; H/S]

| Block | CPU base/address | Registers/offsets needed |
|---|---|---|
| Power | `0x10000` | Control; bit 4 requests core software reset. |
| Clock dividers | `0x10008` | HSB divisor minus one: bits 17:16; LSB divisor minus one: bits 10:8. |
| Clock controls | `0x1000C`, `0x10014` | Control 0; control 2 respectively. Details §3.2. |
| Chip stepping ID | `0x10200` | Low byte selects efuse read width. This is different from the 16-bit encryption key. |
| Timer 0 | `0x10400` | `+0` control, `+4` count, `+8` period, `+0xC` PWM. |
| Main PLL / secondary PLL | `0x119A0`, `0x119A4`; `0x119A8`, `0x119AC` | First/second control words of each PLL. |
| SPI0 (flash) | `0x11C00` | `+0` control, `+4` baud divisor, `+8` byte buffer, `+0xC` DMA RAM address, `+0x10` DMA count. |
| SPI1 (panel) | `0x11D00` | Same five register offsets. |
| Peripheral ENC | `0x13000` | `+0` control (SDK u8), `+4` key (u16), `+8` cipher address. |
| CRC16 | `0x13500` | `+0` input FIFO, `+4` result/initial value. |
| P33 serial bridge | `0x13E00` | `+0` PMU control, `+4` secondary serial/RTC control, `+8` main serial control, `+0xC` serial byte data. |
| SMK LED commons | `0x16A0C` | High-speed USB PHY control; bits in §8.3. |
| SFC | `0x40200` | `+0` control, `+4` baud, `+8` code, `+0xC` physical flash base. |
| XIP ENC | `0x40300` | `+0` control (u8), `+4` key (u16); further exception-range registers exist but are not needed by the inspected boot flow. |
| GPIO A–H | `0x50000 + 0x40 × port`, A=0…H=7 | `+0` output, `+4` input, `+8` direction (1 input), `+0xC` digital-input enable, `+0x10` pull-up, `+0x14` pull-down, `+0x18` drive option 0, `+0x1C` drive option, `+0x20` second digital-input enable. |
| Mux controls | `0x5101C`, `0x51020` | First and second mux control words. |
| Cache control | `0x01EEE008` | Bits 8 and 9 enable read-only flash and read/write cache; bit 14 indicates idle. |

For SPI control, bit 0 enables, bit 1 selects slave (0 master), bit 3 selects separate transmit/receive lines/full duplex, bits 5:4 determine update/sample edges, bits 11:10 select DMA width (00 single, 01 dual receive, 10 quad), bit 12 is direction (0 TX, 1 RX), bit 13 is interrupt enable, bit 14 clears completion and bit 15 reports completion. Some SDK symbolic names for bits 10/11 are confusing; the actual TX/RX usage matters. Writing DMA count starts the transfer; count is 16-bit. A zero count MUST NOT be used to wait for a display transfer. Baud behavior used here is source clock divided by `(baud + 1)`. [E2, E4, E5, E7; H/S; frequency relationship E7 I corroborated by E2 timing comments]

## 3. Hardware initialization and transactions

### 3.1 P33 power domain and reset

The main P33 bridge performs a three-byte transaction. Assert bridge serial-control bit 0 for the entire transaction. For each byte: set serial-control bits 3:2 to `11`, write the data byte, set bit 4 to start, wait for bit 1 to clear, then read the returned data if needed. The three bytes are (1) operation OR address bits 9:8, (2) address bits 7:0, (3) data or read dummy `0x5E`. Operations are write `0x00`, OR `0x20`, AND `0x40`, read `0x80`. Clear bridge bit 0 after the third byte. These are P33 register addresses, not CPU addresses. [E2, E4–E6; S]

| P33 address | Required purpose |
|---|---|
| `0x05` bits 2:0 | Main I/O voltage selection: code 0…7 corresponds to 2.8…3.5 V, 0.1 V increments. |
| `0x06` bits 2:0 | 1.4 V domain selection: 1.25 V + code × 0.05 V. Default code 3 = 1.40 V. |
| `0x09` bits 3:0 | Digital core voltage: 0.93 V + code × 0.03 V. Default code 11 = 1.26 V. |
| `0x80` bit 4 | Watchdog enable; clear for A startup. |
| `0xAD`, `0xAE`, `0xAF` | P33/other/USB pin latch controls; write zero to release at startup. |
| `0xB0`, `0xB1`, `0xB2` | Efuse bank selection, row/byte/read strobe, returned byte. |
| `0xD0…0xD7` | A…H pin latches, pins 0…7. |
| `0xD8…0xDF` | A…H pin latches, pins 8…15. |

Voltage mappings are SDK declarations [E5 H]; the initialization and latch behavior are stock/current transactions [E2, E6 S]. A MUST set core/domain voltage before increasing clock frequency. With the supplied configuration, which has no voltage records, use core code 11 and domain code 3; leave I/O voltage unchanged. Existing behavior treats zero voltage values as unset, so zero does not request the minimum voltage. Preserve that meaning for compatibility. [E2, E8 S/H]

Apply configured GPIO input/output states, wait at least the source's short settling interval, clear the sixteen A–H latch bytes and the three additional latch registers, and allow settling again before switching clocks. Existing settling intervals are two uncalibrated busy-loop iterations at each side; no electrical minimum was established. A MAY use a measured conservative delay. [E2, E6 S; minimum U]

Before a software reset, latch every pin explicitly named by the configured output policy: set its bit in the appropriate low/high P33 latch byte without changing its output level. Then set power-control bit 4. This keeps named outputs across reset; it does not encode their levels in the latch byte. Normal startup releases those latches after reapplying outputs. [E2, E6 S]

### 3.2 Clock contract for A

For the supplied configuration, A MUST establish nominal system 360 MHz, HSB 180 MHz and LSB 60 MHz. SPI0, SPI1 and Timer 0 use LSB timing in the inspected implementation. It is unsafe to interpret the configured divider values as divisors directly: add one. The UART clock, if queried by a compatible updater, is treated as 48 MHz. No UART service is required. [E2, E6, E8 S/H]

The following are hardware field-setting requirements distilled from the working flow, not a source-code template. Unnamed fields in secondary-PLL controls are preserved. Delay counts here are **uncalibrated source-loop counts**, not microseconds. A MUST allow PLL settling; actual minima and whether a lock status can replace delays are U. [E2, E6 S]

1. Clear clock-control-0 bit 1, settle for the equivalent of two short loop iterations; clear bit 8 and settle again. **Leave bits 5:4 as found.** The stock code intends to clear them for OSC0, but because of the oscillator-tag defect described in §5.3 it never does on these devices: every working unit, with the stock and the previous open bootloader alike, has run with bits 5:4 untouched. Clearing them is untested (U) and MUST NOT be done without a hardware test and an amendment. The source does not establish additional distinct 24 MHz hardware programming for OSC1 or OSC_RTC.
2. Select the required system source/division in clock-control-2's low byte. The supplied 360 MHz case uses source nibble 6 and both divider-index fields 5:4 and 7:6 zero: low byte `0x06`.
3. For the 24 MHz oscillator and main 480 MHz PLL, program first PLL control initially `0x45400200`, second control `0x3F503026`, enable first-control bit 0, settle ten loop iterations, then enable bit 1. The resulting first control is `0x45400203`. These values follow a 12 MHz reference divider of 2 and multiplier field 38.
4. The 360 MHz system source needs the secondary PLL. Disable its control-0 bits 0 and 1. Set control-0 fields: bits 6:2 = 10, bit 7 = 0, bits 9:8 = 2, bit 10 = 0, bits 23:22 = 1, bits 25:24 = 1, bits 27:26 = 2. Set control-1 fields: bits 11:0 = 268, bits 14:12 = 3, bits 18:15 = 0, bits 21:20 = 1, bits 23:22 = 1, bits 26:24 = 7. This is a 2 MHz reference and nominal 540 MHz PLL target used by system source 6. Enable control-0 bit 0, settle ten iterations, then bit 1. With other bits zero, final controls would be `0x0940022B` and `0x0750310C`; do not use those whole-word examples to destroy unknown retained bits.
5. Allow the equivalent of ninety more loop iterations. Set clock-control-0 bits 6 and 7. Set system-divider bits 17:16 = 1, bits 10:8 = 2. Set clock-control-0 bit 8 to select the prepared clock path.

A MAY support the following other stock-derived fixed system settings. In this table the three columns give clock-control-2 fields 3:0, 5:4 and 7:6. Exact electrical validation beyond the supplied 360 MHz case is U. [E2, E6 S]

| System MHz | Source | Field 5:4 | Field 7:6 |
|---|---:|---:|---:|
| 320 | 2 | 0 | 0 |
| 240 | 3 | 0 | 1 |
| 192 | 0 | 0 | 0 |
| 160 | 3 | 1 | 0 |
| 120 | 3 | 0 | 2 |
| 96 | 0 | 0 | 1 |
| 48 | 0 | 0 | 2 |
| 40 | 2 | 0 | 3 |
| 24 | 0 | 0 | 3 |

For optional arbitrary clock settings the source uses secondary-PLL system selection 5 below 160 MHz (multiplier factor 4), 6 through 384 MHz (factor 3), otherwise 7 (factor 2); the PLL target is `system MHz × factor / 2`. Reference prescaler is oscillator Hz / 2 MHz minus 2; multiplier field is PLL target Hz / 2 MHz minus 2. Supporting arbitrary unvalidated frequencies is not necessary for these devices. [E2, E6 S]

For optional non-24 MHz main-PLL support, choose a 12 MHz reference when oscillator frequency is divisible by 12 MHz, otherwise a 2 MHz reference when legal. First-control base `0x01400200` plus prescaler `(oscillator/reference − 2)` in bits 6:2, reference-type bits 27:26 (2 for 2 MHz, 1 for 12 MHz), and bits 31:30 = 1; second-control base `0x3F503000` plus `(480 MHz/reference − 2)` in its multiplier field. Validate divisibility and field bounds first. HTC is treated by the source as a preexisting 12 MHz PLL-derived reference and skips main-PLL reprogramming. Those paths MAY be rejected with an explicit recovery reason rather than guessed. [E2, E6 S/U]

The unused 12 MHz OSC_RTC path uses a second P33 bridge channel: hold secondary-control bit 0; send `0xA0 + register` then dummy zero for read, or `0x20 + register` then value for write, using the shared serial data register. After each byte, synchronize the core and wait for secondary-control bit 1 set; release bit 0 at the end. It sets target register 0 bit 0, and target register 6 bit 4 according to high-current oscillator selection while clearing bit 5. The physical target of this channel is unresolved; this is **not** established as the SDK's normal RTC-address-space access. This option is unnecessary for the supplied 24 MHz boards and MAY be excluded. [E2, E6 S for transactions; U for hardware target]

### 3.3 Delays, timeouts and CRC peripheral

Timer 0 provides calibrated delays after clock setup: stop/clear completion with control bit 14, set count zero, set period to `(LSB Hz / 1,000,000) × requested microseconds`, start with control bit 0, wait for bit 15, then clear/stop with bit 14. Split long waits to avoid period overflow. Source recovery keys sample every 20 ms; display LED refresh uses 1 ms. [E2 S]

The CRC peripheral computes CRC16/XMODEM when result is seeded zero and each byte is written to its FIFO. Synchronize before reading the result. SPI0 DMA also feeds this CRC unit; software CRC MUST be used for calculations that overlap such DMA, or the peripheral must be isolated. See the algorithm in §5.1. [E2, E6, E7 S]

Replacement policy: malformed flash contents, missing SPI completion, stuck flash busy, or failed screen initialization MUST NOT cause memory corruption or an infinite silent startup hang before recovery is offered. Use bounded waits and a recoverable failure state. Exact timeout values are design choices and require hardware validation. B's valid commands SHOULD complete inside the host's 10-second command timeout, especially erase commands. [Policy; host timeout E3 H]

### 3.4 SPI0 flash access

The proven WL82 transactions use software CS on PD0. Mux control 1 bit 1 and mux control 0 bit 5 are cleared. Configure PD4/PD5 as outputs, enable the PD4 drive-strength bit, PD1 as input. For quad mode, PD2 and PD3 are outputs initially high. The inferred remaining signal roles are PD4 clock, PD5 output/IO0, PD1 input/IO1, PD2/PD3 additional quad lines. The roles beyond CS are inferred from accesses; SDK configuration comments describe another pin mapping and MUST NOT override these WL82-specific accesses without measurement. [E2, E4 S; individual role assignment I]

Disable SFC before claiming SPI0. A's initial SPI control is `0x28` before enable/clear; B uses the same value. Set desired baud, clear pending with bit 14, enable with bit 0, deassert CS and allow a short settling interval. For a byte TX, clear direction bit 12, write the byte to the byte buffer, wait completion, clear pending. For byte RX, set direction and clock a dummy `0xFF` byte. Source implementations synchronize before polling and delay after CS changes (A ten busy iterations, B forty); electrical minimum CS high/setup time is not independently known. [E2, E4, E6 S]

All supported NOR commands use MSB-first 24-bit byte addresses. Flash command facts:

| Opcode | Operation / data |
|---|---|
| `9F` | Read JEDEC manufacturer, memory-type and capacity-code bytes. |
| `03` | Single-bit ordinary read, address then data, no dummy byte. Used by B. |
| `0B` | Single-bit fast read, address then one dummy byte. |
| `3B` | Dual-output read. Address is single-bit, then 8 dummy clock cycles, then dual data. |
| `6B` | Quad-output read. Address is single-bit, then 8 dummy clock cycles, then quad data. A's supplied configuration uses this. |
| `BB` | Dual I/O, with address/mode packet; optional, see caveat below. |
| `EB` | Quad I/O, with quad address/mode packet, then dummy clocks. |
| `05`, `35` | Read status registers 1 and 2. |
| `06` | Write enable; separate CS transaction before each write/erase/status write. |
| `01` | Write status 1 and, for compatible parts, status 2 in the same transaction. |
| `31` | Write status 2 alone; used as a quad-enable fallback. |
| `02` | Page program, address then data, maximum 256-byte page, MUST split at page boundaries. |
| `20` | Erase 4 KiB sector. |
| `D8` | Erase aligned 64 KiB block. |
| `4B` | Factory unique ID: four dummy bytes after opcode, then eight ID bytes. B only. |

[E2, E4 S; not a claim of universal support on all NOR brands]

For raw DMA read, hold CS active for opcode/address/data, set peripheral ENC control to 2 (pass through), direction RX and width fields, set DMA RAM address then nonzero count. On completion clear pending, turn peripheral ENC off, and release CS. Never reuse the DMA destination until complete. For decoded top-level headers, key `0xFFFF`, cipher address zero, and ENC control `0x83` restart the stream for each independent read. Off afterward is zero. Software decryption MAY replace this path when its exact restart boundaries are preserved. [E2, E4, E6 S]

Summary of the read command chosen from the configured width and read mode (mode digit 0 → output, 1 → I/O, 2 → continuous I/O):

| Width | Output (mode 4) | I/O (mode 8) | Continuous I/O (mode 12) |
|---|---|---|---|
| 1 | `0B`, one dummy byte, single-bit data | `0B` | `0B` |
| 2 | `3B`: command/address single-bit, two dummy bytes received dual, data dual | `BB`: address/mode packet (address, `00`), data dual | `BB` with mode byte `A0`, opcode only on the first transaction |
| 4 | `6B`: command/address single-bit, four dummy bytes received quad, data quad (**the supplied devices**) | `EB`: address/mode packet quad, two dummy bytes quad, data quad | `EB` with mode byte `A0`, opcode only on the first transaction |

A MUST support width 4 output reads and a single-bit read for its safe initial phase; the other cells MAY be omitted with a documented fallback (§5.3, §13.1).

Quad receive uses width bits `10`, bit 3 clear; dual receive uses `01`, bit 3 clear. Restore width zero and separate-line mode after the multi-bit data phase for subsequent commands. At 6B, four dummy **bytes received in quad width** give the eight dummy cycles; at 3B, two dual bytes do the same. At EB, send a four-byte address/mode packet in quad width, then receive two quad dummy bytes. Mode byte zero is ordinary I/O; `0xA0` requests continuous read, where the initial opcode is omitted after the first transaction. The current BB path sends address/mode single-bit and has no explicit extra dummy phase. Whether real dual-I/O NOR accepts that is unresolved; MUST NOT claim that path validated. [E2, E6 S; BB hardware U]

To use quad access, read both status bytes; keep existing status bits except clear busy/write-enable bits in the status-1 value to be written, and set status-2 QE bit 1. Try the combined `01` status write, poll status-1 WIP bit 0 until zero (source A waits 100 μs between polls), read back QE. Source permits initial attempt plus up to three retries using separate status-1 `01` and status-2 `31` writes. Failure to establish QE MUST fall back to a validated narrower path or recovery; do not proceed blindly. [E2, E6 S; final failure policy]

Before any erase/program that needs it, clear status-1 mask `0x7C` (BP/TB/SEC protection) and status-2 mask `0x40` (CMP), preserving QE and unrelated bits. This Winbond-style status model is used on the observed `0x856014` and `0xEB6014` parts; no general SFDP support is required. Exit a previously enabled continuous-read mode first; source sends two `FF` bytes under CS and resets its continuous-read tracking. Then WREN, command/address/data, release CS, and wait WIP clear. [E2, E4 S; brand generalization U]

Capacity is derived from the JEDEC capacity exponent: the observed last byte `0x14` means `2^20 = 0x100000` bytes. Reject implausible exponents rather than overflowing a shift. The flash header's `0xFF000` size is filesystem space and excludes the final reservation; MUST NOT mistake it for physical chip capacity. B operates on raw bytes with no application encryption/decryption. [E2–E4, E8 H/S; rejection policy]

### 3.5 Efuse and flash key fallback

Efuse is read only. Byte width per row is four when chip-stepping ID low byte is nonzero, otherwise three. For bank selector `s`, write P33 `0xB0 = s << 6`. For row `r` and byte `j`, write `0xB1 = (r << 6) | (j << 2) | 2`, wait at least the source's 50 μs interval, read `0xB2`, clear `0xB1`, and assemble bytes little endian. B's current source substitutes an uncalibrated loop for the delay; a replacement SHOULD use a defensible calibrated delay. [E2, E4, E6 S]

The encryption key is bank 1, row 3, bits 23:8, truncated to 16 bits. A uses it unless it is `0xFFFF`; then read the last 256 physical flash bytes. In that page, a valid fallback has CRC16 of bytes 0…7 equal to the little-endian halfword at +8, and those first eight bytes are not all zero with zero CRC. Use bits 23:8 of the little-endian word at +4, i.e. page bytes +5/+6, as key. All observed devices/packages use `0x980F`, but A and B MUST read the unit's key; hardcoding it is inappropriate. B's FC09 reports physical efuse key, not A's fallback. [E2–E4, E6, E8 H/S]

For A's 16-bit trim export, bank 0 rows 2 and 3 are read. There is a significant documentation discrepancy: the annotated prose describes selecting row-2 bits with `0x000DC703` and row-3 bits with `0xFFF238FC`. Actual stock instructions and current code retain the row-2 accumulated bits when row 3 is added. Consequently, if independently read row values are R2/R3, the stock result is **`R2 OR (R3 AND 0xFFF238FC)`**, of which only the low 16 bits are handed to the app. A MUST preserve this stock-visible result unless a hardware-backed correction is explicitly adopted. Simply implementing the prose's disjoint merge is not established equivalent. Low trim byte feeds the bandgap trim: magnitude bits 6:2, sign bit 7; other fields are not needed here. [E6 instructions at container offsets `0x1206…0x128A`, E2 S; analog meaning partly E5/E6 I]

The eight Bluetooth address bytes are copied without transformation from physical `capacity − 192`, i.e. `0xFFF40` on 1 MiB. They represent six address bytes plus a little-endian CRC16 according to SDK interfaces. Existing A does not validate them. All-FF data is allowed to reach firmware, which decides how to handle it. Last-page reads are absolute for the supported base-zero layouts; do not propagate a source latent bug that adds a nonzero filesystem base beyond chip capacity. [E2, E5, E6 S/H]

### 3.6 Cache and XIP setup

Before executing an application, A MUST disable SPI0 ownership, select the SFC pin route (mux-control-0 bit 16 cleared), reset SFC control by writing `0x00F00000` then zero, program baud from the configured SPI divider, write the mode word below, and set the physical app-area base in `0x4020C`. Program XIP key with the selected unit key. [E2, E6 S]

| Data width | Output read mode | I/O read mode | Continuous I/O mode |
|---|---:|---:|---:|
| Quad | `0x00280381` | `0x00260581` | `0x00260781` |
| Dual | `0x00280281` | `0x00240481` | `0x00240681` |
| Single | `0x00200089` | `0x00280189` | not established |

The source internal mode numbers 4/8/12 correspond to the three columns, not flash opcodes. After writing the chosen mode word, replace bits 23:20 with `floor(floor(HSB Hz / 1,000,000) × 45 / 1000) + 1`, for roughly 45 ns chip-select-high timing. At HSB 180 MHz this is 9. The generic community SFC notes speculate a different meaning for this field; the WL82 boot flow and SDK analysis establish this timing use more strongly. Exact fields behind the rest of each mode word are not all named; mode words are the required hardware facts. [E2, E6 S; optional modes hardware U]

Invalidate caches by clearing enable bits 8/9 and clearing these half-open internal ranges with word writes: `[0x01F00000,0x01F04000)`, `[0x01F08000,0x01F0A000)`, `[0x01F0A000,0x01F0A200)`, `[0x01F0B000,0x01F0B200)`. Restore enable bits. Disable interrupts, wait cache-control bit 14 idle, enable XIP ENC with control 1 and ensure cache bits 8/9 set before reading/checking or calling decrypted XIP code. This clearing operation is established by stock/current accesses, not a complete description of the cache organization. [E2, E5, E6 S]

If the CRC check or the application call fails/returns, release XIP flash ownership before menu SPI reads. After a failed XIP attempt, a subsequent boot choice MUST restart through reset rather than directly attempting XIP setup a second time; the direct retry failed on a documented SMK-37 Pro. [E1 O; E2 S]

## 4. Mask ROM boot and recovery interfaces

### 4.1 Finding and loading A

For these devices the ROM's boot filesystem base is physical flash zero. Its first 32 bytes are a flash header; 32-byte directory records start at +`0x20`. The first record denotes the boot container, stored at physical `0xA0`. The container is decoded with fixed key `0xFFFF`; its header and payload restart encryption separately. The supplied first-record attribute is zero, meaning an uncompressed boot image. Multi-bank BANKCB and ROM-dictionary compressed boot images are not required. [E2, E7, E8 H/S]

An independently produced A MUST use this 16-byte single-bank BANKCB container, all numeric fields little endian:

| Container offset | Size | Meaning / required value |
|---:|---:|---|
| `0` | 2 | Bank count, 1. |
| `2` | 2 | Payload length in bytes, excludes this header. |
| `4` | 4 | Load address `0x01C02000`. |
| `8` | 4 | Payload file offset, 16. |
| `12` | 2 | CRC16/XMODEM of plain payload. |
| `14` | 2 | CRC16/XMODEM of plain bytes 0…13. |

The ROM loads the payload, not the container header, and transfers control to its first byte. A bare release `.boot` file MUST be a plain container; flash installation applies encryption and updates the outer filesystem entry. Header/payload CRC behavior is established by tools, stock images and successful existing flows. The precise order of the ROM's checks, which outer filesystem CRCs it enforces, its response to each malformed header, and its complete search algorithm are not verified from a WL82 ROM dump. Do not advertise those inferred details as a proved ROM state machine. [E1–E3, E6–E8 H/S/I]

At A entry, `r0` points to the ROM boot-device descriptor:

| Descriptor offset | Size | Meaning |
|---:|---:|---|
| `0` | 4 | Pointer to device parameter storage. Its byte +1 is the read-mode byte. |
| `4` | 4 | Flash/filesystem base offset, zero for the supported images. |
| `8` | 4 | Device-online value; exact semantics unused/unverified. |
| `12` | 4 | Read callback address. |

The callback receives device parameter pointer in `r0`, destination pointer in `r1`, absolute storage byte offset in the next argument, and byte length in the fourth. Before invoking it, set the parameter's byte +1 to the requested mode: metadata reads use 1, plain configuration data uses 0. The current flow reads the configuration descriptor at relative +64 in mode 1, then its data in mode 0. A read of +96 exists but its result is unused and is not a requirement. Exact callback return conventions and mode behavior beyond these calls are U. [E2, E6 S]

A starts in the ROM's exception context. It MUST preserve the incoming descriptor while establishing its own initialized state, use a valid exception return to enter normal execution (stock sets exception-return state to zero and interrupt-return target to its continuation), and establish its own system and main stacks. Initialized data already resides at its linked RAM addresses. Clear only A-owned uninitialized storage, never all RAM or the retained area. Stack sizes/layout are design choices; existing A uses 1 KiB exception and 4 KiB main stack. The required effect is correct ABI execution, not copying the original entry instructions. [E2, E6 S]

### 4.2 Entering ROM USB download mode

A MUST write the 16 bytes `usb_update_mode` followed by NUL starting at `0x01C7FD80`, then latch configured output pins as in §3.1 and request software reset. The string intentionally occupies more than the SDK's nominal eight-byte status reservation and overlaps the start of the update request. The ROM recognizes it and exposes its download-mode disk. This mechanism is observed for key/menu recovery on SMK-37 Pro; FM-1 use is established by existing firmware/source descriptions but not retested here. Do not replace the reset with a guessed direct ROM jump. [E1, E2, E9 S/O]

The ROM owns USB enumeration and bulk-only mass storage. Host-visible identity is USB VID/PID `4C4A:8057`, SCSI vendor `WL82`, product `UBOOT1.00`; reported USB descriptive string may be `WL80UBOOT1.00`. Normal power cycling exits. A key-triggered entry does not initialize the screen and gives no deliberate LED confirmation. Menu-triggered entry leaves a visible download-mode screen that the panel retains through reset. [E1, E3, E9 H/O]

### 4.3 Recovery independent of a valid boot image

The USB_KEY dongle can force the mask ROM into download mode even when A itself is invalid. It sends `0x16EF`, 16 bits MSB first, with USB D+ as clock and D− sampled on the rising edge. The local Pico implementation uses open-drain lines, nominal 10 μs half periods (50 kHz), 150 μs gaps, and detects acknowledgement as both lines held low for roughly 1–2 ms. It requires seeing the released lines high after power-on before accepting low as acknowledgement; an unpowered target also clamps them low. After acknowledgement, release both signals and move the cable to the PC. [E9 S/O]

Documented test wiring: Pico GP2 → D+, GP3 → D−, common ground, 2.2 kΩ pull-ups from 3.3 V to each signal, no breakout VBUS connection; the SMK runs on its battery. The hardware recovery procedure was reported successful on SMK-37 Pro. A broken A cannot provide its own key; the dongle must remain the way back during acceptance tests. [E9 O; electrical timing tolerance U]

## 5. Flash formats, encryption and layout

### 5.1 Checksum and cipher algorithms

All format CRC16s below are **CRC16/XMODEM**: width 16, polynomial `0x1021`, initial zero, no input/output reflection, no final XOR, check value for ASCII `123456789` is `0x31C3`. Streaming continuation carries the previous 16-bit remainder. CRC32 is IEEE reflected, reversed polynomial `0xEDB88320`, initial `0xFFFFFFFF`, final XOR `0xFFFFFFFF`, check value `0xCBF43926`. These are checksums, not authentication. [E2–E4, E7, E8 H]

The ENC transform is its own inverse. Keep a 16-bit state initialized to the key. For each byte, XOR it with the state's low byte, then advance the state one bit left, retaining 16 bits, XORing `0x1021` if its previous top bit was set. There is **one state step per data byte**, not eight. Distinguish this from the eight steps per byte in CRC16. [E2, E3, E7, E8 H]

Encryption boundaries MUST be respected:

| Object | Starting state / reset boundary |
|---|---|
| Flash header | ENC `0xFFFF`, restart for its 32 bytes. |
| Each top-level directory record | ENC `0xFFFF`, restart independently per 32-byte record. |
| Boot container header | ENC `0xFFFF`, restart for 16 bytes. |
| Boot container payload | ENC `0xFFFF`, restart for the whole payload, with no 32-byte resets. |
| Configuration blob | **Plain bytes** on flash, including its encoded chip-key prefix. Do not apply ENC to the blob. |
| Application area | For each 32-byte aligned relative offset `q`, ENC state = `unit key XOR ((q >> 2) modulo 65536)`, restart every 32 bytes. Offset is relative to the selected app area, not absolute flash. |
| VM, BTIF, calibration, presets and resource regions | Treat as raw bytes unless their own firmware format is explicitly being interpreted. A's wipe/guard and B need no generic decryption here. |

[E2, E3, E7, E8 H/S; this corrects imprecise claims that the entire boot area is encrypted]

For an unaligned application read, expand to surrounding 32-byte blocks, decrypt each with its own aligned seed, then select the requested bytes. The offset-dependent seed repeats every `0x40000` bytes; the data stream still restarts each line. The SFC hardware implements the matching XIP behavior. [E2, E3, E7, E8 H/S]

USB loader upload and chip-key replies use a **different** CRC-based stream transform. Start a CRC16 accumulator at the low half of a 32-bit key; feed the high half as two little-endian bytes using CRC16 continuation. For each byte index i, feed one byte from the cyclic constant sequence below into that CRC, then XOR the data byte with the remainder's low byte. For this protocol the key is always `0xFFFFFFFF`. Reset for each uploaded block, not once for the entire loader. The 16 entropy bytes are: `C3 CF C0 E8 CE D2 B0 AE C4 E3 A3 AC D3 F1 C1 D6`. This algorithm and constant are interoperability data. [E3, E4, E7 H]

Useful generated test vectors, input 32 zero bytes:

| Transform/key | Output hex |
|---|---|
| ENC `FFFF` | `ffdf9f1f1f3e7cf8f0c1a367cebd5b970f1e3c78d183274e9c1913264c983060` |
| ENC `980F` | `0f3f7efcd9b264c8b162c48810012346ad5a950b162c79d3a66dfbf6ecf9d3a6` |
| USB CRC stream `FFFFFFFF` | `2fa481cc4ec15deaeea2b6a6298ad4f70384ebe1cf8a8a14bde6dd1cd0f8463b` |

[E8 H, independently computed from the specified algorithms]

### 5.2 Flash header and records

The decrypted flash header is 32 bytes, little endian:

| Offset | Size | Meaning |
|---:|---:|---|
| 0 | 2 | Header CRC16 over bytes 2…31. |
| 2 | 2 | Burner-size field; preserve, not interpreted for application boot. |
| 4 | 4 | Version/vendor ID bytes. |
| 8 | 4 | Filesystem flash-size field. Supplied images: `0xFF000`. |
| 12 | 1 | Filesystem version; supplied decoded value `0x10`. |
| 13 | 1 | Block alignment in 256-byte units; supplied value 16. |
| 14 | 1 | Reserved. |
| 15 | 1 | Special options; preserve. |
| 16 | 16 | Product ID bytes. |

For validation, decrypt **all 32 bytes** and compute CRC. Raw stored version/product bytes can look like plain text (`0.01`, `AC791N_STORY` plus FF padding) even though after ENC they are not text. This packager convention is real; do not reject the valid decoded header for unreadable ID strings, and do not substitute raw ID fields into a CRC-checked decoded copy. All four supplied boot areas have decoded header CRC `0xDCA1`, burner field `0x0220`. [E5, E7, E8 H]

Every ordinary file/directory/region record is 32 bytes:

| Offset | Size | Meaning |
|---:|---:|---|
| 0 | 2 | CRC16 of bytes 2…31. |
| 2 | 2 | File data CRC16, or `0xFFFF` where not applicable. |
| 4 | 4 | Address; interpretation depends on record kind. |
| 8 | 4 | Length; `0xFFFFFFFF` may denote an unspecified/banked directory length. |
| 12 | 1 | Attribute byte. |
| 13 | 1 | Reserved/opaque. |
| 14 | 2 | Index/end marker; preserve. Not reliable as a universal entry-count rule. |
| 16 | 16 | Name, NUL-terminated when shorter than 16; comparisons MUST be bounded. |

Top-level addresses are filesystem-base-relative; here that base is zero. Top-level records are decoded with fixed ENC key. A MUST check record CRC before using its address/length and bound traversal before data or chip end; unknown general attribute semantics MUST NOT be inferred just from their bit patterns. [E2, E3, E7 H/S; bounded traversal policy]

The common top-level layout is:

| Physical location | Record/object | Address field | Length | Attribute |
|---:|---|---:|---:|---:|
| `0x00000` | Flash header | — | 32 | — |
| `0x00020` | `uboot.boot` | `0x000A0` | stock `0x3830` | `00` |
| `0x00040` | `isd_config.ini` | `0x038D0` | `0x2BB` = 699 | `02` |
| `0x00060` | `app_dir_head` | `0x04000` | `FFFFFFFF` | `81` |
| `0x00080` | `key_mac` | `0xFF000` | `0x1000` | `12` |
| `0x000A0` | Boot container | — | up to `0x3830` | — |
| `0x038D0` | Plain configuration blob | — | 699 in supplied images | — |
| `0x04000` | Encrypted application area | — | firmware dependent | — |

All four supplied image boot areas are byte-identical. The stock boot container's 16 plain header bytes are `01 00 20 38 00 20 C0 01 10 00 00 00 A2 81 F5 BC`; its payload is `0x3820` bytes, payload CRC `0x81A2`, container CRC `0x71C8`. These are format fixtures, not payload bytes to incorporate in the replacement. [E8 H]

### 5.3 Configuration blob

At blob offsets 0…31 is an encoded chip-key record; +32 is its little-endian CRC16. A ignores this prefix for runtime key selection. PC tools decode it as follows: sum the first sixteen bytes modulo 256; clamp sums ≥`0xE0` to `0xAA`, sums ≤`0x10` to `0x55`. For each bit i=0…15, set that key bit iff `(byte[16+i] XOR byte[15−i])` is less than the adjusted sum. Supplied prefix decodes to `0x980F`, CRC `0x1C1D`. This is format documentation, not permission to use this record in place of physical efuse. [E6–E8 H/S]

Starting at +34, each record consists of one **value length byte**, a NUL-terminated ASCII key name, then exactly that many raw value bytes. The length excludes key and NUL. A length byte 0 or FF ends records; exact exhaustion of the descriptor length after a complete record is also valid. In fact the supplied 699-byte blobs contain 39 records ending exactly at byte 699, with no terminator byte. Numeric values are little-endian four-byte integers. Strings need not contain a trailing NUL within their value. A MUST parse within the descriptor length, reject missing NUL/truncated values/oversized destinations, and terminate its own string copies. Blob larger than 4096 bytes is unsupported; recover safely. Unknown keys MAY be ignored. The configuration file entry's data CRC, where checked, covers these plain blob bytes. [E2, E6, E8 H/S; parser bounds policy]

Runtime keys and supplied values:

| Key | Value / interpretation | Required handling |
|---|---|---|
| `SPI` | Seven plain bytes `4_1_0_0`; width_divider_mode_port | MUST support quad, divider 1, output read. Mode digit 0→output, 1→I/O, 2→continuous I/O. Fourth digit is not used by the inspected WL82 flow. |
| `OSC` | Four bytes `OSC0` | 24 MHz board oscillator; see oscillator-tag caveat below. |
| `OSC_FREQ` | 24,000,000 | MUST establish correct timing. |
| `OSC_HC_EN` | 0 | Optional oscillator drive setting; unused in 24 MHz path. |
| `OSC_1PIN_EN` | 0 | Stored in handoff metadata; no identified hardware effect in current flow. |
| `SYS_CLK` | 360,000,000 | MUST support this system frequency. |
| `HSB_DIV`, `LSB_DIV` | 1, 2 | Encoded divisor minus one. |
| `RESET` | Nine bytes `PB01_08_0` | Pin PB1, configured hold time 8 seconds, active level 0, represented in updater metadata. No actual long-press controller programming is established by A's current code. |
| `BOOT_DELAY` | Absent | Optional startup delay, milliseconds nominally; old implementation is uncalibrated before clock setup. |
| `PORT_INPUT` | Absent | Optional `PXnn_ud`: pull-up/down flags u/d. Direction input. |
| `PORT_OUTPUT` | Absent | Optional `PXnn_v`, or H/L followed by two-hex-digit `(port×16 + pin)` codes, with subsequent H/L groups allowed. Used also for reset latching. |
| `ERR_PORT_OUTPUT` | Absent | Optional `PXnn_LTd`: output level L, toggle flag T, hex half-period d in 50 ms units (zero means 50 ms). Source drives low when parsing. May be omitted because its fatal-memory-error feature is excluded. |
| `UTRX` | Absent | Optional updater metadata RX pin string. |
| `DVDD`, `DCDC14`, `VDDIO` | Absent | Optional voltage selectors in §3.1; zero treated as unset. |
| `ENABLE_SDRAM` | 0 | MUST remain no external RAM. Nonzero unsupported on these boards. |
| `SDRAM_*` | Present but unused | Ignore because external RAM is excluded; do not initialize nonexistent RAM. |
| `UTBD`, `VLVD`, `UPDATE_JUMP` | 1,000,000; 4; 0 | Present but not queried by the stock/current non-debug boot flow. They impose no new UART/brownout/jump behavior here. |
| `UTTX` | Absent | Not read by the current non-debug flow; keep TX metadata empty unless deliberately supporting it. |

[E2, E5, E6, E8 H/S]

For an otherwise readable configuration with a missing SPI record, the existing flow chooses dual width, divider 3 and output-read mode. This is a source-established fallback, not proof that arbitrary missing clock records are safe; the replacement must validate the whole chosen recovery profile. [E2 S]

GPIO parsing MUST accept only actual A–H pins 0…15 for this task. Some legacy parser aliases for charge/USB/RTC pins use another chip family's numbering and can address unintended WL82 pins. They are not required. Validate output groups, pin numbers, lengths, numeric widths and supported clock values before applying them. [E2, E6 S; restrictions policy]

**Oscillator-tag caveat:** both stock and current A reuse an unterminated SPI string buffer when reading OSC; with the supplied bytes, the comparison sees residual characters and leaves the oscillator tag at 0 rather than intended OSC0 tag 1. This is source-proven, not a requirement to reproduce unsafe parsing. The 24 MHz PLL path still works. A MUST parse safely, but its effects MUST match what the units have always run: clock-control-0 bits 5:4 untouched (§3.2) and oscillator tag **0** in the updater metadata (§6.4), the value every tested official update has seen. Reporting tag 1 instead is U and needs an official-update hardware test and an amendment. Numeric tags in the legacy contract are RTC=0, OSC0=1, OSC1=2, HTC=3, PLL-derived 12 MHz=4. [E2, E6 S; corrected tag compatibility U]

Recovery MUST remain usable if the app, application table or config is damaged. The existing “ignore oversized config and use defaults” path does not establish safe complete clock defaults: several fields start zero. Replacement policy is to use a specifically validated recovery clock/pin configuration, or test recovery keys while still using a known ROM clock, before acting on malformed configuration. No requirement mandates reproducing divide-by-zero or invalid PLL programming on broken config. [E2 S; policy; ROM clock on A entry not fully measured]

### 5.4 Application directory and bank selection

Find the top-level `app_dir_head` by bounded name lookup. For a supported single-bank image its length is zero or `FFFFFFFF`, and its address selects the encrypted app area. The area's first decrypted record has attribute `83`, name `app_area_head`, **address is the CPU entry address**, and length describes the area extent. Subsequent `82` records are CRC-bearing files (`app.bin`, `cfg_tool.bin`), with addresses **relative to the selected area**. `12` / `92` region descriptors (`VM`, `PRCT`, `BTIF`, `USR`, etc.) use **absolute physical flash offsets** and do not carry file-data CRCs. Do not add the area base to them. [E2, E3, E8 H/S]

Supplied tables occupy at most nine 32-byte records (area header plus eight children) before code at relative `0x120`. A MUST validate all CRC-bearing application files, including `cfg_tool.bin`, over their **plain/decrypted** data, and validate overflow-safe bounds against physical capacity minus selected area base. The area header's own data CRC is not the per-file acceptance check. Require a nonempty `app.bin` and a plausible XIP entry inside its supported mapped area; an absent/invalid directory must not accidentally count as an intact application. Boot-anyway may bypass file-data CRC refusal, not invalid pointers or out-of-bounds reads. [E2, E3, E8 H/S; strict empty/bounds/entry policy]

File-table traversal in current code terminates at first bad record CRC, with limits of eight children for A and sixteen records for host inspection. Do not read instructions at +`0x120` as an unbounded directory. Index/end values in the supplied region tables vary (1 at the last region), whereas top-level key reservation has `FFFF`; CRC plus layout bounds is the established safe termination basis. [E2, E3, E8 H/S]

A SHOULD preserve legacy dual-bank selection; a build advertising this support MUST implement the following contract. A build omitting it must list that limitation rather than silently selecting a random bank. If the primary directory length is neither zero nor `FFFFFFFF`, it is interpreted as the location of an application-info record. If a top-level `app_dir_head2` exists, its length similarly addresses its bank info. These info records are read with default fixed-key metadata decoding. Each is 32 bytes, little endian:

| Offset | Size | Meaning |
|---:|---:|---|
| 0 | 2 | CRC16 over bytes 2…31. |
| 2 | 2 | Data CRC field, not the bank selection criterion. |
| 4 | 2 | Encoded-data CRC field, not the selection criterion. |
| 6 | 2 | Unsigned version. |
| 8 | 4 | Length. |
| 12 | 4 | Reserved. |
| 16 | 16 | Name. |

If primary info is valid, use secondary only if its info is valid and its unsigned version is strictly greater. Ties use primary; this is an ordinary comparison, not a wrap-aware version counter. If primary info is invalid and secondary exists, the old flow selects secondary even without validating its info first, then validates its first decrypted app record. A SHOULD require valid selected app metadata regardless. With no secondary, retain primary and validate it. Bank info's length field interpretation and update lifecycle beyond this selection are unverified on hardware. There is no automatic switch to an older bank merely because a selected bank's later **data** CRC fails in the current behavior. [E2, E6 S; dual-bank fixture/hardware U]

Before parsing top-level metadata set both hardware ENC keys to `FFFF`. After bank selection set both to the selected unit key and decode the first app record; the normal SPI path is then returned to raw mode. Alternatively software decrypt all reads with the same seeds. A's SFC base is filesystem base plus selected directory address. [E2, E6 S]

### 5.5 Device data regions

These are reference layouts, half-open ranges; A MUST derive wipe targets from the installed table, not blindly use this table. A MUST protect the fixed ranges even when a corrupt/foreign table disagrees. [E1–E3, E8 H/S/O]

| Region | FM-1 stock | SMK-37 stock / Snoop37 | Baud Girl on SMK-37 |
|---|---|---|---|
| Boot area | `[00000,04000)` | same | same |
| Firmware/protected-code extent | `[04000,93000)` | `[04000,9D000)` | `[04000,C0000)` per PRCT |
| VM | `[93000,E9000)` | `[9D000,C0000)` | `[D9000,E9000)` |
| BTIF | `[E9000,EA000)` | `[C0000,C1000)` | `[C0000,C1000)` |
| USRTRIM | none | `[C1000,C2000)` | not declared; physical calibration still protected |
| USRFLASH | none declared | `[C2000,EB000)` | not declared |
| USR resources/sounds | `[EA000,FC000)` | `[F4000,FE000)` | `[EA000,FC000)` |
| Final unit reservation | `[FF000,100000)` | same | same |

PRCT describes a protection range starting at physical zero; it is not another file to checksum or erase. Unlisted gaps are not necessarily disposable: third-party firmware may use them. FM-1 Felucca/SLOOP can put projects/samples within declared VM while also keeping settings or sample data elsewhere. A's “wipe user data” is exactly a VM/BTIF wipe, not a promise to remove every third-party setting or sample. [E1, E3 S/O]

SMK-37's first `0x22C` calibration bytes start at `0xC1000`, with eight-byte marker `trimdat` plus NUL at +`0x224`. Unit wheel values also reside in the presets header around physical `0xC2060` and `0xC2080` and in presets themselves. Those values are unit-specific; do not copy the example unit's calibration into any replacement. Reported experiments show stock firmware resets presets and erases USRTRIM/BTIF when USRFLASH is blank, and default presets do not recreate wheel calibration. Keeping USRTRIM during the wipe alone is insufficient to guarantee that the app will keep it later. [E1 O; marker E8 H]

### 5.6 Package format context

A and B do **not** need a `.fwsc` parser. M-Flasher parses packages and sends raw bytes to B. This context explains what must remain compatible and supplies test extraction rules. [E3 H]

A package is a UFW-like image with identity bytes inserted in an initial prefix of 48-byte blocks: byte 47 of prefix block i carries `(identity character + i + 1) modulo 256`, followed by `0x7D` sentinel bytes after the identity. Host removes the 48th byte from at least the first twenty blocks, continuing while the next block's marker is `7D`; it concatenates their 47 retained bytes and appends the untouched remainder. Identity is decoded from prefix markers until first `7D`. Supplied FM-1 identity is `FM-1_015` with 20 prefix blocks; SMK identity is `SMK-37 Pro_016` with 36. Do not assume the insertion applies throughout the complete file. [E3, E8 H]

The resulting image's first 64 bytes are ENC FFFF as a single object. Its LE fields include CRC16 at +0 over decoded bytes 2…63, file-list CRC at +2, total image size at +4, count u16 at +8. File list starts +64 and consists of 80-byte records, each independently ENC FFFF. The list checksum is over **stored/encrypted list bytes**, not decrypted entries. Each entry has type u16 +0, index u16 +2, member CRC u16 +4, offset u32 +8, size u32 +12, name[16] +64. Offsets address the image after removing marker bytes. Other fields are opaque. [E3, E8 H]

Type-0 `flash.bin` is a raw flash image starting at zero, already encrypted as required, with member CRC over those raw bytes. FM-1 package supplies `0x93000` bytes and SMK package `0x9D000`. Other members include `ota.bin` type `0x64` and `USR` type `0x32`. CRCs for some non-flash members do not equal the raw stored-byte CRC; the encryption/checksum conventions for all of those members have not been established here and MUST NOT be generalized from `flash.bin`. The current M-Flasher firmware install does not install the separate USR member. [E3, E8 H]

## 6. Retained RAM and executable handoffs

All structures in this section are byte layouts with little-endian integers and 32-bit pointers. They do not require any particular C type names. Use explicit layout assertions; do not rely on host-compiler bitfield packing. [E2, E5, E6 H/S]

### 6.1 Retained area map

Let R = `0x01C7FD80`.

| Address | Size | Contents |
|---|---:|---|
| R+0 = `0x01C7FD80` | nominally 8 | Boot status; some ROM strings extend beyond it. |
| R+8 = `0x01C7FD88` | 80 | Application's update request. |
| R+136 = `0x01C7FE08` | 24 | Application boot information. |
| R+160 = `0x01C7FE20` | 32 | Copy of decoded flash header. |
| R+216 = `0x01C7FE58` | 8 | Newly trained external-memory configuration; MUST be zero on these devices. |
| R+224 = `0x01C7FE60` | 4 | Extra application area base; MUST be zero. |

Do not infer that gaps are free for arbitrary use. A MUST avoid clearing retained request/status before examining them, and MUST populate stable copies before calling the app. The +80/+88 offsets mentioned by newer SDK applications are relative to the application boot-info pointer at R+136, hence R+216/R+224, not relative to R itself. [E2, E5, E6 H/S]

### 6.2 Update request format and interpretation

| Request offset | Size | Meaning |
|---:|---:|---|
| 0 | 2 | CRC16 over bytes 2…79. |
| 2 | 2 | Update type; zero is invalid. |
| 4 | 2 | Update result. |
| 6 | 2 | Flash-record magic `0x5441`; not required by the current RAM validity predicate. |
| 8 | 32 | File/path or descriptive name. |
| 40 | 32 | Opaque private arguments. |
| 72 | 4 | Absolute flash address of staged updater's outer file record. |
| 76 | 2 | Extension-argument length. |
| 78 | 2 | Extension-argument CRC. |

A RAM request is valid when computed CRC is nonzero, matches +0, and type is nonzero. A MUST preserve all 80 bytes for interpreting and executing a valid request. No extra whitelist of types is established by the current behavior. SDK type series starts at `0x5A00` (USB), followed by SD0, SD1, PC, UART, Bluetooth, BLE app, SPP, dual-bank, BLE test, NOR, user NOR UFW, and network UFW; the real supplied SMK flash record uses `0x5A0D`. Result series also starts `0x5A00` (none), then ready, success, parameter/device/key errors. These symbolic meanings are context; the staged-image boot path primarily tests nonzero type and CRC. [E2, E5, E8 H/S]

If RAM request is valid, A MUST use its private-argument address (the **original retained** request +40, not a pointer into an overwritten staging buffer) in the updater's private/memory-info pointer field. It remains an opaque 32-byte request-specific region; do not assume it always contains the complete 60-byte default memory-info object below. [E2, E6 S]

With no valid RAM request and a successfully mounted application, do not scan flash for stale update records during normal boot. With failed mount, scan raw flash downward: step = header alignment ×256 if alignment is 1…16, otherwise 4096. Candidate boundary starts `capacity − step`, continues while boundary >4096, decreases by step; candidate's 80 bytes begin `boundary − 256`. Accept the first magic `5441` and valid CRC record, then apply the RAM-style nonzero CRC/type validity rule. This exact placement excludes the last sector's own tail for step 4096. Do not “simplify” it to a different end-of-sector scan. A menu status may count all matching records regardless of mount success. [E2, E6 S; scan numeric addresses E8 H]

Before invoking a staged updater, clear the halfword at **R+2**, which is boot status storage, not request+2. The old flow falls through toward app boot if the updater fails or returns; if nothing bootable remains, show recovery. Invalid request/address/compression MUST never cause an unchecked jump. [E2, E6 S; bounds policy]

`UARTUPDATE` and `UART_UPDATE_CUSTOM` boot-status requests are unsupported: current A clears the first status word/halfword and resets. A MUST not enter a perpetual updater loop for these requests. It MAY explain unsupported mode in recovery before resetting/clearing it. [E2, E6 S; UI policy]

### 6.3 Staged updater format and LZ4

At the request's absolute updater address, read an ordinary 32-byte outer file record. Its address field is an **absolute** flash address for reads; its length bounds the staged stream. In the supplied staged example the address points to the beginning of that same record and length includes it. The first 32 bytes of the stream are an inner/file record which the boot path skips. At stream +32 is this compression header:

| Offset within compression header | Size | Meaning |
|---:|---:|---|
| 0 | 2 | CRC16 of bytes 2…31. |
| 2 | 2 | CRC16 of complete decompressed updater. |
| 4 | 4 | Decompressed byte count. |
| 8 | 4 | RAM load and entry address. |
| 12 | 4 | Argument field in file, **replaced** at execution with absolute stream end. |
| 16 | 16 | Descriptive name. |

Blocks begin at stream +64. Each is `[compressed length u32 LE][output length u32 LE][raw LZ4 bytes]`. Each compressed block must fit a 4096-byte input staging limit; output length must be nonzero. The decoder carries prior contiguous output as LZ4 history, so treating every block as an independent LZ4 stream is wrong. There is no standard LZ4 frame header or per-block frame checksum. The final produced byte count and CRC MUST match the compression header before execution. [E2, E6, E8 H/S]

Independent decoder contract: each token's upper nibble is literal length, with extended bytes adding until one is <255 when the nibble is 15. Copy those literal bytes. If input remains, read a nonzero little-endian 16-bit backward distance, then match length is low nibble +4, extending similarly when the nibble is 15. Copy the match with overlap semantics from already produced output, including preceding blocks. Retain up to 64 KiB history, or use the still-contiguous complete output. Bound all reads, writes, length additions and backreferences. Last input is a literal tail; reject malformed/truncated blocks. No vendor-specific decoder over-copy behavior is required. [E2/E6 format S, independent algorithm statement]

Replacement policy: validate the outer record and header bounds, stream length at least 64, each `8 + compressed length` within remaining stream, each output within the declared total and a safe RAM range, executable address alignment/range, and no overlap with A's live workspace, retained structures or ROM runtime reservations. The source lacks several such checks. A malformed “negative” u32 length is simply huge and MUST be rejected unsigned. The supported stock updater destination is `0x01C0A800`; other destinations require explicit memory-allocation validation rather than arbitrary writes. Exact ROM RAM reservations for arbitrary updater destinations remain U. [Policy; E1, E2, E8 S/H for supported case]

The supplied SMK stock flash has a valid request at `0xBEF00`, type `5A0D`, result `5A01`, magic `5441`, name `ota-SMK-37 Pro_013`, pointing at `0xB9000`. Its staged stream length is 19,969 bytes. Compression header: header CRC `569C`, output CRC `B295`, output size 23,324 (`0x5B1C`), load address `0x01C0A800`. Six `(compressed,output)` lengths are `(3213,4096)`, `(3544,4096)`, `(3575,4096)`, `(3616,4096)`, `(3467,4096)`, `(2442,2844)`; they consume the stream exactly and independently decode to CRC `B295`. Both supplied packages contain the same updater payload with their package-specific outer wrapping. [E8 H]

After successful verification, call the updater with `r0` = pointer to the 24-byte updater context below and `r1` = **absolute file data address + file length**. The updater depends on A's context objects remaining alive below its load address. It is a normal call, not the application handoff or a reset. Watchdog is off, flash access is in the SPI/raw stage; no prior application XIP transfer is required. Further updater assumptions about every CPU register have not been proved; preserve the target ABI and supplied context. [E2, E6 S; official SMK update E1 O]

### 6.4 Updater context layouts

The 24-byte context MUST have:

| Offset | Size | Meaning |
|---:|---:|---|
| 0 | 4 | Selected application's CPU entry, or zero when unavailable. |
| 4 | 4 | Selected application's physical flash area base, or zero when unavailable. |
| 8 | 4 | Pointer to 52-byte clock/pin configuration object. |
| 12 | 2 | Selected encryption key. |
| 14 | 2 | Reserved, zero. |
| 16 | 4 | Reserved, zero. |
| 20 | 4 | Pointer to default memory/power object or retained request private bytes as §6.2. |

The clock/pin object is:

| Offset | Size | Meaning |
|---:|---:|---|
| 0 | 4 | Packed flags: bits 7:0 reserved zero; bits 11:8 oscillator tag; bit 12 high-current flag; bit 13 one-pin flag; bits 17:14 LSB divider encoding; remaining bits zero. |
| 4 | 4 | Oscillator frequency Hz. |
| 8 | 4 | System frequency Hz. |
| 12 | 8 | TX pin name, bounded NUL string; empty in supplied non-debug flow. |
| 20 | 8 | RX pin name. |
| 28 | 4 | UART baud metadata; zero in supplied non-debug flow despite UTBD record. |
| 32 | 8 | Configured output pin string. |
| 40 | 8 | Configured input pin string. |
| 48 | 1 | Encoded reset pin, port×16+pin. Supplied PB1 = 17. |
| 49 | 1 | Reset active level. |
| 50 | 1 | Reset hold time number. |
| 51 | 1 | Reserved zero. |

The default memory/power object occupies 60 bytes. For no external RAM, keep unused fields zero, but preserve these placements: +8 byte external-RAM enable =0; +12 low nibble HSB-divider encoding; +20/24/28/32/36 external-memory size/test/config/refresh data =0; +40/+44 timing words =0; +48 packed external-memory mode/timing flags =0; +52 bits 15:0 external delays =0, bits 19:16 core voltage selector, bits 23:20 I/O voltage selector, bits 27:24 domain voltage selector, high nibble zero; +56 halfword optional encoded error output; +58 padding zero. +0/+4/+16 and +9…11 are reserved zero. Voltage metadata reflects parsed values (zero can mean use hardware default), not necessarily the default code actually applied. [E2, E6 S; exact bitfield layout derived from target layout/instructions]

Optional error-output halfword: pin in bits 3:0, port in 7:4, level in bit 8, toggle in bit 9, half-period digit in 13:10, valid bit 15. External-RAM-specific fields are defined only as reserved zero here because their hardware is excluded; a new implementation must not derive a memory controller from them. [E2, E6 S]

### 6.5 Application entry contract

At `0x01C7FE08` the 24-byte app boot object MUST contain:

| Offset | Size | Meaning |
|---:|---:|---|
| 0 | 4 | Pointer `0x01C7FE20` to the stable decoded 32-byte flash header. |
| 4 | 4 | Physical flash base of selected application area, normally `0x4000`. |
| 8 | 4 | CPU mapped application base, `0x02000000`. |
| 12 | 2 | Encryption key. |
| 14 | 2 | Low 16 bits of compatible efuse trim result (§3.5). |
| 16 | 8 | Unmodified MAC/CRC bytes from last-page location. |

This **+4 physical / +8 logical** ordering is established by SDK `boot.h`, current source, and stock stores. Some annotation prose reverses the two; do not follow that prose. The SDK 1.2.x extra locations at +80 and +88 relative to this pointer MUST be initialized to zero as §6.1, despite older stock leaving them unspecified. [E2, E5 H; E6 instructions S]

Before the call, establish the required clocks/voltages and XIP state (§3), copy the decoded flash header and app object to their fixed locations, finish all flash DMA, validate the app files unless a one-shot bypass is authorized, disable interrupts, and pass the object pointer in `r0`. Call the area-header entry (normally `0x02000120`). The application runs from flash and initializes its own runtime; no loading of the entire app into internal RAM is required. A's stacks must still be valid at the call. No requirement establishes zero contents of other general registers or A-owned RAM as an app ABI. An optional configured TX pin is restored to plain input with digital input enabled and no pulls before app entry; it is empty in these images. Restore board key/LED temporary settings before handoff. [E2, E5, E6 S/H]

If the application returns, the boot is failed and MUST enter the recovery menu; it must not fall through into unrelated RAM or repeat a stale update indefinitely. An app that hangs without returning cannot be diagnosed automatically by A's CRC check: the recovery key or USB_KEY is still necessary. [E1, E2 S/O; failure policy]

## 7. A: required boot decisions and failure behavior

The following decision order is externally significant; internal organization is free. [E1, E2, E6 S, unless policy noted]

1. Establish safe entry execution, retained-state access and watchdog policy. Consume any one-shot boot-anyway marker before it can survive another failure. Initialize A-owned storage. Preserve the ROM descriptor.
2. Obtain and validate configuration, prepare voltages/pins/clocks, and test board recovery **before any application filesystem mounting, CRC scan, update scan, calibration write or display initialization** can stall boot. A MAY make recovery even earlier using validated ROM timing. Malformed configuration must not prevent the recovery key (§5.3 policy).
3. A continuously held recovery key for approximately one second requests ROM download mode. Recovery takes priority over the menu when both keys are held. A key released early boots normally. Restore temporary scan pins if the key check ends without reset.
4. Test the menu key for approximately one second. Record the request; mount/probe flash before opening the menu because status/actions need flash access. Do not steal OCT− / OCT+ firmware shortcuts.
5. Establish safe initial flash reads (the existing flow uses dual-output mode), discover JEDEC/capacity and MAC, read physical key and possible fallback, then mount selected app directory with configured bus mode. When width is the legacy probe value 3, try quad before dual and, within each, continuous I/O, I/O, output read. Optional modes must be validated; do not claim a successful mount on invalid CRC. On failure clear the selected app base.
6. If the LSB flash source exceeds 66 MHz, the existing probe temporarily raises the SPI divider by one for mounting, then restores the configured divider for XIP. The supplied 60 MHz LSB does not need this. Preserve safe bus timing; exact need outside the supplied configuration is I.
7. Run the SMK calibration guard after SPI setup and before app/menu activity, subject to safe capacity/address checks. It may run even when application mounting failed; damaged application metadata must not give it arbitrary write addresses.
8. If menu was requested, show it. An accepted boot choice continues through normal update selection; it does not promise to ignore a valid pending updater. Recovery/download and wipe take their explicit actions.
9. Check retained update request first. If valid, forward its private bytes and attempt the verified staged updater. Otherwise, only if mounting failed, look for saved flash requests. If mounted with no request, proceed to app.
10. Handle unsupported retained UART requests without looping. If a staged updater fails/returns, attempt the supported app if possible, otherwise recovery. Record/show an intelligible reason for invalid updater/address/CRC instead of executing it.
11. Establish XIP, check file data CRCs unless one-shot bypass applies, populate the fixed app ABI, and call the app.
12. No usable app, CRC failure, unsupported external-RAM entry or app return leads to the recovery menu. A display-less diagnostic build MAY enter ROM mode directly. Once an XIP boot has been attempted, later menu boot choices reset before retrying.

Normal successful boot MUST NOT erase or rewrite user firmware/settings. The SMK guard's first backup and recovery of invalid calibration are explicit exceptions. Normal boot MUST not require the recovery display to be initialized. A does not need to reproduce the stock bootloader's silent reset loops, unsafe parsers, stale scratch bytes, vestigial storage reads or unused timestamps. [E1, E2 S; replacement policy]

## 8. A: boards, keys, indicators and panel

### 8.1 Board selection and image identity

A MUST have separate FM-1 and SMK-37 build targets. SMK-37 Pro and Elite share the same target; evidence for equivalent hardware is firmware pin/peripheral behavior reported in E1, not an Elite hardware test in this investigation. A MUST trust the installed board target for the recovery key rather than drive candidate boards' pins in an autodetection procedure. The installer checks model: an FM-1 key row is an analog input on SMK and an SMK output-enable pin is an FM-1 key row. Wrong-board installation can make recovery ineffective. [E1, E2 S; Elite physical equivalence I]

M-Flasher recognizes the following 48-byte identity at **payload offset `0x40`**, hence container offset `0x50`, loaded RAM `0x01C02040`:

| Offset within identity | Size | Required contents |
|---:|---:|---|
| 0 | 8 | ASCII `AC79BLID`, exactly eight bytes. |
| 8 | 8 | NUL-padded lowercase board tag: `fm1` or `smk37`. |
| 16 | 32 | ASCII version/build description, bounded and NUL-terminated. |

The entry path MUST branch around this data. Initial instructions before offset 40 hex MUST fit without overlapping it. Version text can be independently chosen and SHOULD identify the new implementation distinctly. A valid BANKCB plus the tag lets the host display an open bootloader and reject the wrong board. Do not change the magic/tag/offset merely to rename the project. [E1–E3 H]

Board identity is an installer decision, not a guarantee supplied by a flash chip model. Current host evidence rules:

* Strong SMK evidence: `trimdat`+NUL at physical `0xC1224`, `TRIMCOPY` at `0xFF000`, or an installed valid tagged SMK bootloader. Tagged FM-1 is likewise strong FM-1 evidence.
* Weak layout evidence: declared USRTRIM at C1000 suggests SMK; VM at 93000 and USR at EA000 suggests FM-1. Weak JEDEC hints: `856014` FM-1 and `EB6014` SMK. Firmware can be changed and parts can vary.
* One strong model with no opposing weak evidence is considered certain. With no strong evidence, two agreeing weak signals and no competing model are considered certain. Conflicting strong evidence leaves it unresolved. The loader's display is enabled only after certain identification or explicit host board selection.

[E3 H; this is more nuanced than the simplified identification summary in the bootloader README]

### 8.2 Keys and scan timing

| Function | FM-1 | SMK-37 Pro / Elite |
|---|---|---|
| Recovery at startup | REC, 1 s | SEQ REC, 1 s |
| Menu at startup | PLAY, 1 s | SEQ PLAY, 1 s |
| Move previous / next | OCT− / OCT+ | `<` / `>` above octave buttons |
| Select | PLAY | SEQ PLAY |
| Menu LEDs | OCT−, OCT+, PLAY | `<`, `>`, SEQ PLAY |

Firmware shortcuts that MUST pass through: OCT− + OCT+ calibration on Felucca/SLOOP/Snoop37; OCT− alone for SLOOP/Snoop37 USB rescue. SEQ PLAY/REC here are sequencer controls, not the octave controls. [E1, E2 S/O]

The current hold logic polls every 20 ms for 1000 ms after an initially detected press. One missing sample is tolerated; two consecutive missing samples cancel. Menu presses become stable only after two equal polls and trigger on the newly pressed edge, with no automatic repeat. A MAY improve debounce while preserving approximate hold duration, release behavior and priority. Save and restore every modified GPIO/mux/PHY state needed by the application; deselect scan columns on exit. [E2 S; debounce details MAY]

**FM-1 scan facts:** two cascaded shift registers, data PA4, clock PA3, latch PA1. Shift sixteen bits MSB first, pulse clock high then low for each, latch high then low afterward. Idle/deselected word FFFF. Low selected-column bits. OCT− reads PA8 with word `F7FE` (bits 0/11 clear); OCT+ PA8 with `EFFD` (bits 1/12 clear); PLAY PA6 with `FBFF` (bit 10 clear); REC PA6 with `FF7F` (bit 7 clear). Enable digital input/pull-ups and remove pull-downs on PA6/PA8; closed key reads low. Allow 10 μs after changing column before sampling. Use GPIO read/modify/write edges with real bus access time; documentation reports overly short edges selecting an adjacent column. [E2 S; inherited FM-1 observations via Felucca, no new hardware test]

FM-1 indicator lines are PA9, PA10, PH6, PH9 for matrix rows PA5…PA8; high lights a selected-column LED. The menu needs PH9 for octave keys and PA10 for PLAY. Keep those lines dark while changing/reading columns. During waits cycle OCT− column, OCT+ column and PLAY column, one per millisecond, with only the relevant LED line high, giving approximately one-third duty. Restore saved state before app. This exact menu multiplexing is documented **untested on FM-1 hardware**. [E1, E2 S/U]

**SMK scan facts:** four shift registers, data PA4, clock PA3, latch PA1, output enable PA0 active low. A 32-bit logical word is sent **least significant byte first, MSB first within each byte** (same stream as the firmware's DMA of a little-endian word). Do not shift the whole 32-bit number MSB first. Initialize output-enable/clock/latch/data low and outputs enabled. Lower sixteen logical bits contain active-low LED lines except bits 5:3 used for analog mux addressing. Upper sixteen select columns low: columns 0…7 correspond to bits 24…31, columns 8…15 to bits 16…23. All-off/deselected word FFFFFFFF. [E2 S]

For the recovery/menu controls, sample PH8 with digital input and pull-up enabled, pull-down disabled, after 10 μs settling:

| SMK control | Selected logical word with LEDs off | Column word index | Firmware key-code context |
|---|---:|---:|---:|
| `<` | `FFFEFFFF` | 8 | `96` hex |
| `>` | `EFFFFFFF` | 4 | `56` hex |
| SEQ PLAY | `FEFFFFFF` | 0 | `16` hex |
| SEQ REC | `F7FFFFFF` | 3 | `46` hex |

Firmware codes name a column one scan tick later, so their code's column number is one greater than the logical selected column word index. Closed key is PH8 low. Other matrix row pins are PA2, PA5, PA6, PA7, PA9, PH6 and PH9, but A need not scan them. [E2 S; recovery/menu observed on Pro E1 O]

### 8.3 SMK LED commons

The LED groups share the high-speed USB PHY's D+ and D− pads, separate from the ROM download USB handling assumptions. At `0x16A0C`, bits 0…3 and 16 enable drive (`0x1000F`), bit 17 is group-0 common and bit 18 group-1 common; **low** common turns a group on. Group 0 line 0 is `<`; group 1 line 0 is `>` and line 12 is green SEQ PLAY. Group 0 line 12 is KNOB BANK, so lighting both groups indiscriminately causes an unwanted LED. [E2 S/O]

During menu waits, alternate groups every millisecond. Turn both commons off before loading active-low line bits into the scan word, then activate only that group. Group 0 clears lower bit 0; group 1 clears lower bits 0 and 12. Preserve analog-mux bits. Save/restore the original PHY register before application entry; after reset into ROM mode the key LEDs are not expected to stay lit. Pro menu keys/LEDs/display have reported hardware tests; Elite is inferred from shared firmware transactions. [E1, E2 S/O/I]

### 8.4 Display wiring and transactions, shared by A and B

| Signal | FM-1 | SMK-37 |
|---|---|---|
| SPI1 group selector | Mux-control-1 bit 4 set (group B) | bit 4 clear (group A) |
| Clock | PC9 | PB6 |
| MOSI | PC10 | PB7 |
| CS, active low | PC7 | PC8 |
| Command/data (0 command, 1 data) | PC8 | PB5 |
| Backlight | PA2, active low | No controllable pin used |

Initialize CS high, command/data and clock/MOSI output levels low, corresponding pins as outputs. SPI1 control `0x4021`, baud 4. At A LSB 60 MHz this is nominally 12 MHz; at B LSB 48 MHz nominally 9.6 MHz. Commands are one byte with command/data low; data bytes have it high. Hold CS through each command/data burst; toggle high at completion. Use completion bit 15 / clear bit 14. DMA sends pixel data from internal RAM. Do not begin zero-length DMA. Panel absence/failure MUST not block B flash operations. [E2, E4 S; frequencies I]

Panel address window is commands `2A` (x start/end), `2B` (y start/end), each with two big-endian 16-bit inclusive endpoints; `2C` starts memory write. Pixels are RGB565, high byte first, normal origin (0,0), no row offset. Full usable size is 240×240. The recovery menu and loader text interface assume 20 columns ×15 rows of 12×16-pixel cells. A MAY use the 5 × 7 font of Appendix A (the author's own design, provided under MIT) or independently draw a different legible font in those cells. For B, lowercase MAY be rendered as uppercase, unsupported characters as blank; host restricts its text to `A–Z`, digits, space, `- . / : < > + ?`. [E2–E4 S/H]

Required panel initialization transactions, in order. Each command byte and its following data are hardware setup facts, not an encoded vendor table to reproduce as source structure. [E2, E4 S; Pro hardware E1 O; FM-1 initialization derived from existing firmware]

| FM-1 step | Data / wait |
|---|---|
| Software reset `01` | Wait 150 ms. |
| Sleep exit `11` | Wait 120 ms. |
| Pixel format `3A` | `55`. |
| Orientation `36` | `00`. |
| Inversion on `21` | No data. |
| Normal display `13` | No data. |
| Clear visible RAM, then display on `29` | Black RGB565 fill; backlight on. |

| SMK step | Data / wait |
|---|---|
| Initial wait | 100 ms. |
| Sleep exit `11` | Wait 120 ms. |
| Orientation `36` | `00`. |
| Pixel format `3A` | `05` (as used by this panel). |
| Porch `B2` | `0C 0C 00 33 33`. |
| Gate `B7` | `35`. |
| VCOM `BB` | `32`. |
| Control `C2` | `01`. |
| Voltage `C3` | `15`. |
| Voltage `C4` | `20`. |
| Frame rate `C6` | `0F`. |
| Power `D0` | `A4 A1`. |
| Positive gamma `E0` | `D0 08 0E 09 09 05 31 33 48 17 14 15 31 34`. |
| Negative gamma `E1` | Same fourteen bytes as positive gamma. |
| Inversion on `21` | No data. |
| Clear visible RAM, then display on `29` | Black RGB565 fill. |

## 9. A: recovery menu, wiping and calibration guard

### 9.1 Menu status and controls

The menu MUST remain until the user chooses an action; it MUST show a reason when normal boot is impossible. Required content: device/bootloader title, build version, app state and `app.bin` byte length when available, retained update presence and flash-record count, JEDEC ID/capacity, available actions and board-specific movement/select hints. The precise wording, font shape and colors are free design choices except where compatible state names help comparison below. Do not claim app CRC validity proves runtime behavior. [E1, E2 S/O; visual freedom policy]

Suggested geometry matching the established 20×15 interface: row 0 title, 1 version, 3 app status, 4 length, 5 updates, 6 flash ID/size, 7 separator, 8…12 actions, 13 transient status/progress, 14 controls. Show checking progress before slow scans. Directory/file reads before XIP can be raw SPI plus software ENC and CRC. [E1, E2 S; geometry MAY]

Suggested wording and colors, as used by the previous open bootloader and seen on an SMK-37 Pro (any clear wording is acceptable; on a black background):

```
row 0   FM-1 BOOTLOADER | SMK-37 BOOTLOADER      yellow
row 1   VER <identity version string>            grey
row 3   APP     OK | BAD CRC | INCOMPLETE | NOT FOUND    green if OK, else red
row 4           <app.bin length> B               white (blank without app.bin)
row 5   UPDATE  NONE | RAM | <n> FLASH | RAM <n> FLASH   yellow if any, else white
row 6   FLASH   <JEDEC, 6 hex digits> <capacity>K        white
row 7   --------------------                     grey
rows 8-12  "> " + selected action (yellow), "  " + others (white)
row 13  CHECKING / ERASING / DONE / progress bar
row 14  OCT-/+ MOVE  PLAY OK  |  < > MOVE SEQ PLAY OK    grey
```

Confirmation prompts: row 14 `HOLD PLAY 3 S` (`HOLD SEQ PLAY 3 S` on SMK-37, yellow); row 13 `OTHER KEYS: CANCEL` (grey) while waiting; a progress bar on row 13 (green growing over grey, 12 px high, 2 px below the row's top) while held; then `RELEASE PLAY` (green) until released. BOOT ANYWAY's screen on rows 8–11: `BOOT ANYWAY` (red), `THE APP MAY CRASH OR`, `MISBEHAVE. CHECKED`, `AGAIN AT NEXT START.`. WIPE USER DATA's screen: `WIPE USER DATA` (red), one line `ERASE <start>-<end−1>` (five hex digits each) per region, `KEEP  ALL ELSE` (grey); during the erase `ERASING` and a bar, then `DONE` (green) for a second before the reset. The download-mode screen clears rows 3–14 and shows (row 3 green, rest white): `DOWNLOAD MODE`, blank, `CONNECT USB TO A PC`, `TO WRITE THE FLASH.`, blank, `SWITCH OFF AND ON`, `TO LEAVE.`. Rewriting a row pads it to 20 characters so nothing of the previous text remains.

Required app classification:

| State | Meaning |
|---|---|
| OK | Required directory/entry valid, nonempty `app.bin`, all declared file CRCs match. |
| NOT FOUND | No usable area/header/table or no nonempty `app.bin`. |
| BAD CRC | A file fails CRC and no incompleteness condition has been found. |
| INCOMPLETE | File runs beyond physical flash, or a CRC-failing file contains a sampled raw erased span. This is a diagnosis heuristic, not proof of an interrupted updater. |

The old menu's erased-span heuristic examines the last 32 raw bytes before each 4 KiB boundary within a failed file, and the last 32 bytes of the file, ignoring a partial span shorter than 32. Thirty-two FF bytes at any sampled tail imply incompleteness. Encrypted data makes it unlikely for intact code to have such a span, but it is **not mathematically impossible**, contrary to an overstatement in the documentation. Host inspection instead checks entire erased sectors and can classify a tail-only erase differently. A SHOULD keep its existing useful heuristic; exact A/host labels on such ambiguous corruption need not be forced identical. [E2, E3 H/S; probabilistic interpretation I]

Update status counts flash records with magic/CRC at the same scan positions as §6.2; retained request gets its own indication. Counting stale records does not mean a normal intact-app boot would run them. The supplied stock SMK record is inside VM and is removed by wiping that VM. The old menu once had separate “clear update” and “wipe all” actions; they are **not required** and SHOULD NOT be reinstated merely because old comments mention them. [E1, E2 S/O]

Required actions, in this order among actions present:

| Action | When present | Effect / confirmation |
|---|---|---|
| BOOT APP | App OK | Continue normal update/app decisions; no confirmation. |
| DOWNLOAD MODE | Always | Show USB/PC instructions and power-cycle exit instruction, then ROM-mode reset; no confirmation. |
| BOOT ANYWAY | BAD CRC or INCOMPLETE with otherwise safe executable metadata | Warn that firmware may crash or misbehave; fresh 3-second select hold, act only after release. Bypass file data CRC once. |
| WIPE USER DATA | At least one validated declared VM or BTIF range | Show exact ranges and that all other data is kept; fresh 3-second select hold, act only after release. Erase then restart. |

Menu navigation wraps at each end. Entry-held PLAY must be released before it can select an action. Confirmation requires a fresh select press held alone, shows progress, resets progress on early release and lets another key cancel. After successful hold, prompt for release before acting so a restart does not reinterpret held PLAY as another menu request. Refresh status after cancellation. [E2 S/O]

BOOT ANYWAY MUST be volatile, one boot only, never a flash write or permanent setting. From a menu reached after failed XIP, retain eight bytes `BOOTANY` plus NUL at R and reset; next startup recognizes and clears all eight before skipping the check. A MAY use another private retained encoding only if it preserves one-shot consumption, does not conflict with ROM/update strings, and survives exactly the needed soft reset. A failed/returned app must not retain a bypass forever. From a power-on menu before XIP, continuing without reset is allowed after the release check. [E2 S; private encoding MAY]

### 9.2 Wipe safety requirements

Only validated `VM` and `BTIF` **region** entries with attributes `12` or `92`, positive lengths, 4 KiB-aligned start/length and overflow-safe bounds within the chip are eligible. Do not erase a similarly named ordinary file. Duplicate/conflicting descriptors SHOULD be rejected or safely unioned with an intelligible display. No valid declarations means no wipe action; do not use guessed stock addresses as fallback. [E2, E3 S/H; stricter attribute/nonzero/duplicate checks policy]

As a replacement safety policy, reject a proposed VM/BTIF range that overlaps any validated application file's physical allocation. A valid header CRC alone does not prove that a semantically contradictory region declaration is safe to erase. This additional check preserves all supplied layouts, including Baud Girl. [Policy; fixture separation E8 H]

Per erase transaction, independently protect:

* `[0,0x4000)` for both boards: boot filesystem, A and configuration.
* `[capacity−0x1000,capacity)` for both: MAC, flash efuse and guard backup.
* `[0xC1000,0xC2000)` on SMK, even if no USRTRIM descriptor exists.

Subtract protected sectors from eligible ranges. Use aligned 64 KiB erases only when a whole block lies within the eligible range **and** contains no protected sector; otherwise 4 KiB erases. Recheck protection in the actual erase loop, not just the UI. No program operation is necessary for a wipe. Wait completion, show progress/done, and reset. A SHOULD read back erased sectors or report hardware failure; current code's blind wait does not prove erase success. A MUST never erase the last sector just to create room for its guard. [E1, E2 S/O; readback SHOULD]

Keep everything outside those declared regions, especially stock sounds/resources and SMK presets/wheel data. A wipe can erase third-party projects/samples residing inside VM; the user confirmation must not describe it as harmless metadata only. Baud Girl's VM overlaps the stock presets allocation, so while Baud Girl is installed that declared VM is eligible except fixed protected sectors. It is impossible to promise preservation of both arbitrary firmware-declared settings and all other firmware's historical layout simultaneously; a unit backup is the way to restore prior presets. [E1, E3 S/O]

### 9.3 SMK calibration guard

The guard is absent for FM-1. On SMK, use fixed source record at `0xC1000`, length `0x22C`, valid iff record bytes +`0x224…0x22B` equal `trimdat` plus NUL. This marker check alone is not a calibration-content checksum. Backup slot is physical `capacity−0x1000` = FF000 for 1 MiB, with this little-endian header **before** the record:

| Slot offset | Size | Meaning |
|---:|---:|---|
| 0 | 8 | `TRIMCOPY`. |
| 8 | 4 | Original record address, `0xC1000`. |
| 12 | 2 | Record length, `0x022C`. |
| 14 | 2 | CRC16 of copied record. |
| 16 | `0x22C` | Copied calibration bytes. |

The occupied range ends at `0xFF23C`, well before MAC at FFF40 and efuse at FFF00. Host recognizes `TRIMCOPY` at slot start as SMK evidence. One documentation sentence ambiguously places the header after the record; actual source/host establishes the layout above. [E1–E3 S/H; test Pro O]

Required behavior:

1. Check capacity and all fixed bounds. Read the candidate backup header/record raw. A valid copy requires magic, original address, exact length and data CRC all matching; replacement SHOULD also require the copied calibration marker.
2. If there is no valid copy and **all 16+0x22C slot bytes are FF**, and the source record is marker-valid, write the copied record first and the header last, splitting page programs correctly. Do not erase the slot. This is the first-start backup.
3. If slot contains anything else, leave it untouched. Invalid/partial slot data must not be “repaired” by erasing unique unit data. In particular, power loss halfway through first copy may leave the slot permanently unusable without a separate maintenance tool.
4. If source is marker-valid, leave it alone even if it differs from backup: this permits deliberate recalibration. Never automatically overwrite the existing backup with the newer source.
5. If source marker is invalid and a valid copy exists, erase the C1000 4 KiB sector and restore only the copied 556 bytes. This is intentionally destructive of other damaged bytes in that sector; it is the known factory record allocation. Verify restored bytes before continuing and report failure. Repeated power-loss recovery must remain possible while the backup survives.

[E2 S; copy/restoration behavior E1 O; extra marker/readback/bounds requirements policy]

The guard runs before stock firmware. If that firmware erases calibration during a later factory-reset path, restoration happens at the **next boot**, not instantaneously. This does not recover missing presets-header wheel values, sounds, calibration before the first backup, a damaged nonblank backup slot, or another unit's calibration. A MUST not present it as a full factory-data backup. Official M-VAVE updates can replace A with stock, leaving the copy present but inactive until A is reinstalled. [E1 O, E2 S]

## 10. B: ROM plug-in and exact host command protocol

### 10.1 ROM registration and lifecycle

The ROM accepts normal mass-storage SCSI commands plus its private memory upload/jump commands. No USB descriptors, endpoints or new enumeration are provided by B. Through ROM command `FB06`, host uploads each block to RAM; `FB08` jumps to B with target argument 1 (SPI NOR). These remain ROM operations after the plug-in is installed. [E3, E4, E7 H/S]

At the **first byte of the linked B image**, the ROM makes a normal C-ABI call with `r0` pointing to three words:

| Offset | Size | Meaning |
|---:|---:|---|
| 0 | 4 | Callback: send RAM buffer pointer and byte length to host, returns int. |
| 4 | 4 | Callback: receive host bytes into RAM buffer, same two arguments/return type. |
| 8 | 4 | Pointer to the ROM's command-handler pointer slot. |

B MUST save callback values, initialize all B-owned runtime state and workspace, initialize SPI0, install its handler pointer in the supplied slot, and **return** to the ROM (current implementation returns zero). It MUST preserve the calling convention and ROM stack/control environment. It MUST NOT execute A's exception-exit sequence or globally clear RAM. No CRT or `.bss` clearing is provided by the image upload contract; the current loader happens to leave some globals dependent on fresh RAM, which a replacement MUST fix. Reinstallation into a previously used slot must reinitialize state, including display enable, last error, CRC state and clock reporting. [E4 S; initialization policy]

The ROM calls the handler as `(31-byte USB command block pointer, writable 16-byte answer pointer)`. Return **1** if handled, **0** to defer to the ROM. The command CDB starts at command-block +15. Command-block +8…11 is the little-endian data transfer byte count; +12 contains USB direction. ROM supplies bulk transport/status behavior. B's callback calls move the command's data; handler return value is not a general SCSI error code. In particular, after consuming OUT data B MUST return handled even when refusing to write; returning 0 after receive can leave ROM unable to complete the transaction. [E4 S; descriptor framing E3 H]

For completeness, the standard command block has LE signature `0x43425355` at +0, LE transaction tag at +4, LE transfer count at +8, flags at +12 (bit 7 = IN), logical-unit byte at +13, CDB length at +14 (16 for private commands), CDB bytes at +15. Its 13-byte status block has LE signature `0x53425355` at +0, matching tag at +4, residue at +8, status at +12. B MUST leave construction of this status block to ROM. For zero-length host data phases, do not emit/expect an extra zero-length USB packet: the direct USB host explicitly avoids it because ROM could interpret it as the next command. [E3 H; zero-length behavior documented in host]

Actual send/receive callback return values and partial-transfer/error semantics are not reverse-engineered here. Existing B ignores the returns and chunks callback calls at at most 512 bytes. A replacement MUST retain the proven chunking and framing for valid host requests; robust callback error handling needs hardware investigation rather than an invented success convention. Do not let display failures or extended diagnostics change flash framing. [E4 S; callbacks U]

### 10.2 CDB and answer conventions

Every private host CDB is sixteen bytes. Bytes 0…1 are opcode big endian; arguments begin at +2; unused bytes are FF. Unless explicitly marked LE, all multibyte command arguments and reply numbers are **big endian**. Reply-based commands return sixteen data-IN bytes, with the opcode echoed at +0/+1 and unused bytes zero. B SHOULD honor shorter requested replies by sending at most `min(requested IN count,16)`; current host always asks for sixteen. Raw read has no opcode prefix; data-OUT commands have no sixteen-byte answer data phase. ROM handles completion/status separately. [E3, E4 H/S]

All flash addresses in this section are absolute physical byte offsets, all reads/checksums use raw stored bytes. `n16` is a BE u16 at CDB+6; `a32` is BE u32 at CDB+2. B MUST support these exact valid request/response contracts:

| Opcode | CDB fields | Data direction / exact useful reply bytes |
|---:|---|---|
| `FB00` | +2 `a32` | IN 16: echo, remaining zero. Erase aligned 64 KiB block and wait complete. |
| `FB01` | +2 `a32` | IN 16: echo, remaining zero. Erase 4 KiB sector and wait complete. |
| `FB04` | +2 address u32, +6 length u16, +8 zero, +9 checksum u16 **LE** | OUT exactly length bytes. Verify CRC16 of received raw data before any program; no reply data. Length at most 4096. |
| `FD05` | +2 address u32, +6 length u16 | IN exactly length raw bytes, no echo header. Must handle host's 16,384-byte reads. |
| `FC09` | +2 magic argument u32 `00AC6900` | IN 16; encoded key in reply +6/+7, see below. |
| `FC0A` | None | IN 16: +2 device type 3 (SPI0 NOR), +3 zero, +4…7 JEDEC ID as little-endian u32 (top byte zero). |
| `FC0B` | None | IN 16: +2 manufacturer, +3 type, +4 capacity code. |
| `FC0E` | +2 address u32, +6 length u16 | IN 16: +2…3 CRC16/XMODEM, BE. |
| `FC14` | None | IN 16: +2…5 receive/program buffer size BE, **4096**. This is not the maximum streaming read length. |
| `FC40` | None | IN 16: identity layout below. |
| `FC41` | +2 address u32, +6 length **u32** | IN 16: +2…5 IEEE CRC32 BE. Length includes full 1 MiB requests. |
| `FC42` | None | IN 16: +2…9 eight raw unique-ID bytes in read order. |
| `FC43` | +2 board byte: 1 FM-1, 2 SMK-37 | IN 16: +2 is 1 on accepted/initialized display, 0 for unsupported board. |
| `FC44` | +2 row byte, +3 FG RGB565 u16, +5 BG u16, +7 text length u16 | OUT exactly declared text bytes. Draw from column zero and clear the rest of the 20-cell row to BG. Host length ≤20. No reply data. |
| `FC45` | +2 percentage byte, +3 fill RGB565 u16 | IN 16: echo, other bytes zero. Update progress graphic, clamp percent to 100. |
| `FC46` | None | IN 16: +2 last refusal code, then clear it. 0 none, 1 oversized write, 2 write-transfer CRC mismatch. |

[E3/E4 H/S; extended refusal behavior below is policy]

`FC09`: take efuse key as two little-endian bytes, apply the USB CRC stream transform with FFFFFFFF to those two bytes in that order. Put transformed first/low byte in reply **+7**, second/high byte in **+6**; other bytes zero apart from echo. For key 980F the complete reply is `FC 09 00 00 00 00 3C 20 00 00 00 00 00 00 00 00`. Host reverses bytes 6/7 and applies the same transform to recover the LE key. The command argument is sent for compatibility; current B does not use it to select another key. [E3, E4, E8 H]

`FC40` identity:

| Reply offset | Size | Meaning |
|---:|---:|---|
| 0 | 2 | FC40 echo. |
| 2 | 4 | ASCII `MFLR`. |
| 6 | 3 | ASCII version, three bytes; current host reads exactly three. |
| 9 | 1 | Zero padding. |
| 10 | 4 | This image's executable entry address, BE. |
| 14 | 1 | Features: bit 0 CRC32, bit 1 unique ID, bit 2 display, bit 3 status; all four implemented → `0F`. |
| 15 | 1 | Actual CPU MHz, normally 96 after successful safe acceleration, 48 in known ROM fallback. |

Host currently uses the identity prefix/version/address rather than feature bits to decide whether to call extensions. A replacement claiming `MFLR` MUST implement the complete extension set used by this host; simply clearing a feature bit will not keep the old host from calling it. Include a clear `MFLR` plus three-character version literal in the plain executable asset because current host discovers its bundled version by scanning the binary for that marker. Ensure an earlier unrelated marker cannot give the wrong version. [E3, E4 H]

The inspected old B reports version `0.4`. The clean replacement SHOULD use a distinct three-character version: this host can reuse an already running loader solely because its version matches the bundled asset and cannot distinguish changed code with the same version. [E3, E4 H]

For `FC0A` and ID EB6014, full reply is `FC 0A 03 00 14 60 EB 00 00 00 00 00 00 00 00 00`. `FC14` reply is `FC 14 00 00 10 00 00 00 00 00 00 00 00 00 00 00`. `FC40` for an example version `1.0` at 01C20000 and 96 MHz is `FC 40 4D 46 4C 52 31 2E 30 00 01 C2 00 00 0F 60`; the example version is not a required release number. [E3/E4 contract H; example serialization derived]

An example write of ASCII `123456789` to 0x4000 has CDB `FB 04 00 00 40 00 00 09 00 C3 31 FF FF FF FF FF` and nine data-OUT bytes. A 16 KiB read there has CDB `FD 05 00 00 40 00 40 00 FF FF FF FF FF FF FF FF`. A whole-flash CRC32 has `FC 41 00 00 00 00 00 10 00 00 FF FF FF FF FF FF`. These examples expose the mixed checksum/argument endianness. [E3 H; derived vectors]

### 10.3 Programming, refusals and unsupported commands

B MUST receive the complete valid write command before modifying flash, check its CRC16 seed zero against the LE CDB checksum, clear block protection as needed, split at 256-byte page boundaries, issue WREN before each page, and wait WIP clear. It MUST NOT silently treat a failed CRC as data to program. On `n>4096`, drain the entire OUT payload into bounded scratch chunks without programming; record refusal 1. On CRC mismatch, record refusal 2 and write nothing. Return handled in both cases. Successful writes do not clear an older refusal; FC46 reads and clears it. [E4 S]

Host does not currently poll FC46 after each write, so valid framing and later raw CRC32/readback remain essential. Reported refusal is not a substitute for host verification. A refusal MUST NOT corrupt ROM state, overflow the receive buffer or leave unread bytes for the next command. [E3, E4 H/S; bounds policy]

Replacement policy requires overflow-safe read/write/checksum bounds within detected supported capacity and erase alignment/extent validation. Do not wrap a 24-bit flash address or erase a block partly outside the chip. Reject mismatched CDB length versus USB transfer length/direction without programming; drain OUT data safely if it has been accepted. Implementations MAY extend last-status codes for bad ranges, timeout, forbidden command, invalid display parameters, etc., but MUST keep meanings 0/1/2 and document added codes. The existing loader lacks these bounds; they are not claims of its behavior. A new refusal mechanism must respect ROM data-phase constraints; do not invent “return −1” as a working transport error. [Policy; ROM refusal limitation E4 S]

Do not implement `FB02` chip erase, `FC12` efuse-key burn, arbitrary flash-command replacement or flash-selection changes. For forbidden destructive commands, ensure no operation is executed; returning unhandled is acceptable only if verified that ROM itself will not implement the destructive action. A defensive handled refusal is preferable where request framing is known. General unknown opcodes MUST remain available to ROM when they are ROM commands, specifically memory write `FB06`, memory read `FD07` and jump `FB08`. [E4 unsupported set S; safe deferral policy; forbidden-command ROM behavior U]

Unlike A's menu, B MUST allow host-authorized raw writes to boot, calibration and the final sector: same-unit backup restoration and bootloader installation need them. Board/factory-data policy is enforced by M-Flasher's operation planner (§11), not by a blanket B sector ban. B's local safety refusals concern malformed/oversized/corrupt operations and unsupported irreversible features. Do not add a confirmation UI in B or make it depend on board identification to read flash. [E3, E4 H/S]

FC44 before display initialization is a no-op after receiving its data. Invalid board selection MUST not touch candidate-board pins. Host uses only rows 0…14 and ≤20 bytes; replacement policy is to validate row, consume any excessive payload completely with bounded buffering, and avoid out-of-panel writes. Current B receives at most 32 bytes for an excessive text length and fails to drain more; that bug is not a requirement. [E3, E4 S/H; bounds/drain policy]

### 10.4 Clock, DMA and performance

B MUST preserve the ROM USB service's clocks, interrupts and stack. The established acceleration applies only when `(clock-control-2 & FF)==80` and `(system-divider & 0x30700)==0`: ROM PLL 192 MHz /4 gives 48 MHz. First set divider bit 8 so LSB will divide HSB by two; synchronize the core; change clock-control-2 low byte to 40, preserving other bits; synchronize again. This changes CPU/HSB to 96 MHz while LSB remains 48 MHz, preserving SPI0/SPI1 timing. Do not apply A's 360 MHz PLL initialization in B. [E4 S; performance observation E4 O]

If a prior B already accelerated the chip, the same check will not match. A replacement MUST recognize/report an established already-96 MHz state accurately or use a conservative documented unknown/fallback report; the current code initializes its report to 48 on every installation even when replacing an accelerated loader. It MUST not divide LSB again or destabilize USB during A→B→A slot swaps. `0` MHz is acceptable as “not reported” in host display when exact state cannot be established; preserve the measured valid clock state. [E3/E4 H/S; accurate reentry policy]

Use SPI0 raw `03` DMA reads with baud 1, two private buffers of 4096 bytes or an equivalent bounded streaming design, and USB callback sends in pieces ≤512 bytes. An efficient read starts the next DMA before transmitting the completed buffer so USB hides flash time. CRC32 and CRC16 scans can use the same streaming mechanism, using software CRC while SPI DMA is active. The 4096 advertised program buffer MUST NOT limit streaming read requests to 4096. Send exactly the requested raw byte count, including a final partial buffer/callback chunk. Empty reads/checksums must complete without starting a zero-count DMA. [E3, E4 S/H]

Documented performance targets are approximately 1.2 s for a 1 MiB read (~860 KiB/s), 0.4 s for an on-device 1 MiB CRC32, and about 10 s for a firmware installation including host work. These are O from E4 documentation, not guaranteed minima. Full-speed USB host-controller scheduling can reduce throughput to ~230 KiB/s /4.5 s per MiB even with the same loader. Acceptance SHOULD demonstrate DMA/USB overlap and absence of an avoidable per-byte read bottleneck on a known fast port; do not fail otherwise correct hardware solely because of a slow PC USB controller. [E4 O; performance acceptance policy]

### 10.5 Display behavior during host operations

Only FC43 chooses wiring; B does not autodetect the board. Its display services are best effort and never a prerequisite for writing or verifying flash. Use panel sequences in §8.4, clear to black on init and display on. FC44 draws one row from column zero using supplied foreground/background, truncating visible text at twenty characters and padding the rest with background. Row position is in 16-pixel cells. Host screen content, percentages and colors are entirely host-supplied. [E3, E4 H/S]

For compatibility with current host layout, the progress indicator occupies the band around rows 9/10: white two-pixel frame outer x=10…229, y=148…173; interior fill starts x=12, y=152, width `floor(216×clampedPercent/100)`, height 18; remaining interior black. Avoid zero-area DMA when percentage is 0 or 100. Exact font pixels and internal buffering are free choices; keep graphic/text legible and the same coordinate contract. [E4 S; shape is visible service behavior]

## 11. Builds, installation and unchanged M-Flasher compatibility

### 11.1 Independent build products

Use JieLi's clang 4.0.1 fork with target `pi32v2`, CPU `r3`, integrated assembly and the corresponding target linker/objcopy. The inspected environment's 20250324.1 toolchain is a known source build target; this document makes no claim about whatever distribution is latest. A commonly uses size optimization and LTO; B currently uses size optimization without LTO. Those optimization/linker heuristics are **not** binary compatibility requirements. A clean implementation MUST NOT import vendor reconstruction code or layout tricks merely to match byte sequences. Implement primitives and the LZ4 decoder independently or select license-compatible independent dependencies with their notices. Do not link the toolchain's own `libc.a` or `libcompiler-rt.a` (§0.1); provide any compiler helper the generated code needs. [E1, E4, E6 S; clean-room/build policy]

Required A artifacts per board are an ELF/map useful for inspection and a plain single-bank `.boot` container. Link loadable code, initialized data and constants at `0x01C02000`, ensure the first payload byte is the true entry, and ensure all initialized state is included in the payload. Uninitialized RAM, stacks and staging buffers need not be stored in flash but MUST be allocated by the link map below `0x01C0A800`. Startup must initialize them explicitly. Keep identity at +40 hex. Container including header MUST be ≤14,384 (`0x3830`) bytes; payload ≤14,368 (`0x3820`). At flash placement A0, it must end no later than 38D0, preserving the stock configuration location. Fail the build on any overflow. Stack sizing, text layout and private buffers may differ completely from the old image. [E1, E2 S; image geometry E3/E8 H]

Required B artifacts are two plain flat RAM images linked at `0x01C20000` and `0x01C28000`, plus maps/ELFs for verification. No BANKCB header. Its callable entry must be the first byte, not merely the ELF entry symbol. Each image plus private initialized/uninitialized storage must fit before the next `0x8000` boundary. Slot A must end ≤01C28000, slot B ≤01C30000. Avoid dependency on A remaining present: ROM download may have been entered with invalid/blank flash. No compressed image or arbitrary runtime relocation is required because host already bundles two linked addresses. [E3, E4 H/S]

### 11.2 Installing A in a flash image

The host expects a plain BANKCB container, validates header/payload CRC and board identity, preserves the unit's complete existing first 16 KiB, then replaces only the boot file and its record. Replacement record retains fields except data CRC, address A0 and new length; recompute header CRC after changing them. File data CRC is over the **entire plain container**, header included. Encode the record with fresh FFFF ENC. Encode the 16-byte container header and payload separately with fresh FFFF ENC; fill remaining bytes up to 38D0 with raw FF. Preserve the original flash header, configuration record/blob, app directory record and unit reservation descriptor. A MUST fit the stock slot. [E3 H; successful layout E8 H]

M-Flasher plans bootloader installation against 4 KiB sectors only within `[0,4000)`, compares changed sectors, makes a backup before its first write, erases/programs only differences, and verifies. A smaller image therefore need not change all four sectors. Do not supply a replacement full boot area with generic configuration as the primary release artifact: the host intentionally keeps each unit's configuration. [E3 H]

An official M-VAVE update can write its package flash image including `[0,4000)` and replace A with stock. A MUST remain compatible with that updater handoff; preventing the update from replacing A is outside scope. Reported SMK stock update wrote boot/app, left the guard copy and unit areas intact, and did not reinstall missing separate USR sounds. A must then be reinstalled separately if desired. This is not an installer bug that A can fix by hiding the boot area. [E1 O; E3 install policy H]

### 11.3 Uploading and replacing B

Current host starts by checking the ROM disk identity, then tries FC40 to discover an already running own loader and FC0A to detect a running JieLi loader. It reuses its own bundled version when equal. If an own older version is at slot B, upload new image at A; otherwise when replacing an own loader use B. With no running loader use A. Host avoids jumping through JieLi's already-running loader and simply uses it as-is. If a first own-loader start fails to identify, host can upload its SDK fallback at `0x01C02000`; if replacing an own loader fails, host errors rather than assume a safe fallback. [E3 H]

For own-image upload, host divides the **plain image** into pieces of at most 512 bytes, independently transforms each using the USB CRC stream, then sends ROM `FB06` with address BE u32 +2, length BE u16 +6, zero +8, CRC16 LE +9 over the **transmitted transformed block**, followed by that block OUT. ROM decodes it into executable RAM. It sends `FB08`, entry address BE u32 +2 and target argument BE u16=1 +6, asking for a 16-byte reply, then waits 100 ms and identifies. This encoding is ROM-side framing, not B's raw flash-write transform. B MUST leave ROM upload/read/jump paths usable after registering itself. [E3, E7 H; ROM descramble action S]

This lifecycle requires testing both slot directions and repeated replacement without power cycling, including a slot whose RAM holds stale `.bss` from a prior instance. Make each version distinct when testing so the host really takes the upload path. Do not write a new image over currently executing handler code or its buffers. [E3 H; test policy]

### 11.4 Host assumptions that the replacement must satisfy

These are important compatibility facts, not instructions to rewrite the PC application. [E3 H]

| Host behavior | Consequence for A / B |
|---|---|
| Current host reads exactly three version bytes after MFLR. | B versions and embedded version marker must fit this scheme. |
| Uses own loader extensions when MFLR is recognized without checking each feature bit. | B claiming own identity implements CRC32/UID/display/status contract. |
| Reads own-loader flash in 16 KiB commands; writes in reported buffer-size chunks, clamped 256…4096. | B streams reads and correctly reports 4096. |
| Skips all-FF program chunks, expecting erased flash already. | B page programming alone need not erase; erased skipped bytes must stay FF. |
| Chooses 64 KiB erases where aligned and long enough, otherwise 4 KiB. | Both erase commands must wait for completion and be repeatable. |
| Retries transient failures up to four attempts with 250/500/750 ms pauses. | Repeating identical read/erase/program must be safe. Program is repeatable only for the same data already being ANDed into NOR. |
| At connection, verifies the full 1 MiB read using on-device CRC32; on mismatch/fallback it reads again and compares. | FC41 must CRC raw bytes and accept full-chip length; don't return decoded app CRC. |
| Before first session write saves full unit backup and unit metadata. | B's raw read must preserve every byte, including last sector and factory areas. |
| Verifies each written contiguous run by on-device CRC32, or readback with fallback; re-erases/reprograms once on mismatch. | B does not need a new verify command or automatic erase inside FB04. |
| Recognizes open A from BANKCB+AC79BLID. | Keep A identity format and valid container/file CRCs. |
| Detects unit identity from eight-byte flash UID, ignoring all-zero/all-FF values. | FC42 returns bytes in factory read order, no custom formatting. Lack of a meaningful UID need not prevent raw access. |
| Enables unit screen only when board is certain or explicitly selected. | FC43 is the only board-selection/pin-driving trigger in B. |

Stock expected chip key is 980F; host logs unexpected keys and refuses a package whose app does not decrypt/check under the reported unit key. It does not re-encrypt packages automatically. SMK Pro/Elite package names accepted by the SMK board target are both `SMK-37 Pro` and `SMK-37 Elite`. The application-reported product name is not a safe electrical detection mechanism by itself. [E3 H]

Factory-data policy is host-owned: FM-1 factory range is the last sector; SMK adds C1000…C1FFF. For a backup known by UID to be another unit's, host preserves current factory ranges; matching UID can restore all flash. If UID metadata is missing, it uses a weaker heuristic based on factory-data equality or currently blank data. This is not proof of provenance. SMK presets/wheel storage is not in the host's fixed factory-range list even though it holds unit values; the specification must not imply otherwise. A's guard and menu do not replace the host's full backup. [E3 H; limitations I]

## 12. Acceptance tests and reference fixtures

### 12.1 What was actually verified for this specification

Read-only offline checks, independently performed here, passed the following: four image header/directory decoding checks; both declared application file CRCs in each; stock boot header/payload/container CRC; all four boot areas byte equal; config plain-record parsing and prefix CRC; package header/list/flash-member checks; six staged LZ4 block output lengths, total output length and CRC. No replacement executable exists in this task and no hardware boot/USB tests were performed. The tests below are implementation acceptance requirements, **not results already achieved by a new implementation**. [E8 H]

Each binary fixture is exactly 1,048,576 bytes. The hashes below are of the original images (dirty side); the implementer gets the blanked copies of Appendix C, whose hashes are listed there:

| File under `/root/flasher-testdata` | SHA-256 | Raw IEEE CRC32 |
|---|---|---:|
| `fm1-stock.bin` | `afb1efadbc3fa098e1272195fd53036c37ef505dcb48ba5f2eecdee0c9e370f1` | `05714D84` |
| `smk37-stock.bin` | `5f768da118d141450fcddb51ee21c4352249175826ff0a14cd2a504d55c80d0e` | `72A267B2` |
| `smk37-snoop37.bin` | `be32292ca0d2d59e45676198934383d0101feb09619f9ca55ac5c2d75f6a2336` | `9880EC18` |
| `smk37-baudgirl.bin` | `9616a2b2815b37a03a5fb94412c760df705dc93cddaf9a84373985d7d9f7c4a4` | `E25E202E` |

The SMK stock image equals the documented private backup. Use only its structural/checksum results in distributable tests; do not ship factory data to other units. The hashes identify fixtures, not suggested factory contents. [E8, E9 H/O]

Decrypted app reference fields:

| Image | Area CPU entry | `app.bin` relative address | `app.bin` bytes / CRC16 | `cfg_tool.bin` relative address | Config bytes / CRC16 |
|---|---:|---:|---|---:|---|
| FM-1 stock | `02000120` | `120` | 581564 / `5D48` | `8E0DC` | 383 / `2E75` |
| SMK stock | `02000120` | `120` | 619704 / `F0DA` | `975D8` | 383 / `2E75` |
| Snoop37 | `02000120` | `120` | 619520 / `6283` | `97520` | 383 / `2E75` |
| Baud Girl | `02000120` | `120` | 748520 / `FBDB` | `B6D08` | 383 / `2E75` |

[E8 H; “Config” here is application file `cfg_tool.bin`, not the boot configuration blob]

Package reference checks: `fm1-015.fwsc` identity FM-1_015, stored file-list CRC 13B0, `flash.bin` length 602112 and raw member CRC 9B24. `smk37-016.fwsc` identity SMK-37 Pro_016, stored file-list CRC 60E8, `flash.bin` length 643072 and raw member CRC 0FA7. Both have eight list entries and `ota.bin` length 19969, CRC A1C9. Their package flash images do not include all unit-specific storage. [E8 H]

### 12.2 Offline A format and behavior tests

The clean implementation's independently written parsers, image wrapper and state logic MUST be exercised with real fixtures and synthetic malformed inputs. It can use a simulated flash adapter or a host build; no original dirty-side source should be a runtime dependency. [Policy]

| Test | Required result |
|---|---|
| CRC16/CRC32 and cipher vectors in §5.1 | Exact matches, chunked streaming equal to whole-buffer results. |
| Decode all four fixtures | App OK and lengths/CRCs above; raw boot/config records and region boundaries as §§5/12. |
| Build A container | Correct load/entry/CRC, identity exact offset/tag, max flash length, linked RAM/stacks below updater. |
| Install each A into a fixture with host-compatible rules | Only boot-area bytes differ; configuration/header/other records preserved; extract round-trip gives supplied replacement container. No change to application or factory data. |
| App file bit flip | CRC refusal and BAD CRC; a bit flip in cfg_tool must be caught too. |
| Entire app sector or tail erased | CRC refusal; INCOMPLETE using documented tail sampling; recognize heuristic differences from host. |
| Missing app, bad area/header, zero-length app | NOT FOUND or a precise malformed-app reason; no arbitrary jump and recovery available. |
| Overflow addresses/lengths; full non-NUL name; malformed records | Bounded failure; no flash/MMIO access outside intended ranges and no infinite traversal. |
| Malformed/oversized/truncated config; missing clock records | Validated recovery fallback, no unsafe PLL/division, no memory overwrite; unknown keys safely ignored. |
| RAM update request | CRC/type rules, private pointer points to original retained bytes, no flash scan when mounted and RAM request invalid. |
| Stale flash update and intact app | Normal app boot ignores flash request; menu may display it. |
| Failed mount with saved request | Exact scan placements and ordering; supplied BEF00 fixture found and staged updater decoded to expected CRC/length. |
| Broken compressed block/header/backreference/output length/address | Refuse without overwriting A or retained memory and without calling invalid code. |
| Dual bank synthetic fixtures, if supported | Primary/secondary invalid-info cases, older/newer/equal versions, selected first-header invalid, out-of-bounds metadata; no invented version wrapping. Hardware remains separate test. |
| App returns / CRC failure after XIP | Menu; subsequent boot-anyway uses reset and one-shot marker consumed before retry. |

### 12.3 Wipe and calibration guard tests

With a NOR model enforcing 1→0 programming and 4 KiB/64 KiB erase units, record every write/erase address. [Policy]

* FM-1 stock wipe: erase exactly eligible VM `[93000,E9000)` and BTIF `[E9000,EA000)`, preserving boot, USR, gaps and FF000.
* SMK stock/Snoop37 wipe: erase VM `[9D000,C0000)` and BTIF `[C0000,C1000)`; calibration, presets and resources unchanged.
* Baud Girl wipe: erase declared VM `[D9000,E9000)` and BTIF C0000; preserve EA000 resources and fixed calibration. Do not erase stock VM at 9D000 just from the board type.
* Malicious descriptors spanning protected areas: only safe allowed sectors erased (or request rejected); never a broad block erase spanning a protected sector. Descriptors not sector-aligned or outside capacity are ineligible. Ordinary files named VM/BTIF are ineligible.
* Simulate power loss during wipe: app and protected sectors unchanged; repeating wipe completes.
* Blank guard slot + valid source: record bytes written before header, no erase of FF000, page boundaries honored; exact slot layout/CRC.
* Nonblank invalid slot: no writes anywhere. Simulate partial first-copy interruption and confirm no destructive “repair.”
* Valid copy + valid differing source: no changes to source or copy. Valid copy + invalid source marker: restore exact copied bytes via C1000 sector erase only. Bad copy CRC/address/length: no restore.
* Power loss at each restore page boundary: original backup preserved and later boot can restore again. Test program/erase/readback failure and visible failure handling.

### 12.4 Offline and ROM-stub B tests

An independently written ROM callback harness SHOULD capture send/receive buffers and flash effects; it cannot establish real ROM timing or callback error conventions. [Policy]

* Exact CDB/reply vectors in §10, every opcode, unaligned range reads, zero-length reads/CRCs, final short DMA/callback chunks, and read lengths 4096/16384/65535.
* FC41 full-chip CRC equals table above; FC0E CRC matches host over raw encrypted bytes. No accidental application ENC.
* FB04 lengths 0, 1, 255, 256, 257, 4096 at page-aligned and unaligned addresses. Program page splits and WREN/WIP behavior correct; no automatic erase.
* Corrupt CRC and length 4097/65535: receive/drain complete, no program, handled result, codes 2/1 respectively; read-and-clear status, retained status after intervening successful command.
* Bad range, overflow, bad erase alignment, direction/length mismatch and text overflow: bounded rejection without ROM transport desynchronization or flash writes.
* Unknown ROM read/write/jump dispatch remains unhandled by B; forbidden destructive commands cause no destructive effect.
* Reinitialize B with deliberately dirty private RAM: no inherited display state/refusal, pointers valid and CRC table correct.
* Display commands before init/with wrong board/row>14 do not corrupt memory or block flash. Percent 0/100/255 and empty text complete without zero-count DMA hang.
* Build both slots, inspect ELF/map and raw first-byte entry; callbacks obey target ABI and no linked object reaches the other slot.

### 12.5 Hardware acceptance, with USB_KEY ready

Before testing a replacement on each physical unit: prove USB_KEY recovery works with its current firmware, read and verify a full unit backup, preserve it privately with UID and SHA-256, and demonstrate at least a read-only ROM session. These actions are acceptance procedure; this document does not authorize writing a user's actual hardware during specification preparation. [E9 O; test policy]

1. Test B first from USB_KEY ROM mode with existing flash untouched: identify/key/JEDEC/UID, twice-read or CRC32-verified backup, ROM memory command deferral, display on the correctly identified board, and measured raw throughput.
2. Upload versions alternating A-slot/B-slot/A-slot repeatedly without power cycling. Confirm registered handler is the new version/address, CPU report is accurate and USB stays stable. Reuse a dirty slot. Also test reuse of same-version loader and compatibility with an already-running SDK loader; do not deliberately jump through the latter.
3. For destructive B checks use an explicitly backed-up safe test sector, restore and verify it before continuing. Test erase/program/readback, wrong-CRC refusal and oversized-write drain followed by a normal command. Never use efuse burn/chip erase as a test.
4. Install board-correct A only after readback comparison of the planned boot-area image. Verify A CRC/container identity and full first-16-KiB readback. Confirm every byte outside allowed sectors remains unchanged.
5. Cold boot and soft reboot stock firmware; audio, MIDI, USB, Bluetooth, knobs, display, keyboard/pads and pitch/mod wheels where present. Confirm startup key scanning leaves app pin states usable.
6. Recovery key held ≥1 s, early release, bounce and both menu+recovery held; recovery priority and normal ROM enumeration. Menu key opens menu; all menu LEDs and navigation work on **both** actual boards. FM-1 LED multiplexing is specifically unverified in old documentation and needs this test.
7. Boot Felucca/SLOOP on FM-1, Snoop37 and Baud Girl where board-compatible, with their own calibration/USB-rescue shortcuts preserved. The provided fixtures lack a distinct Felucca and FM-1 SLOOP image, so obtain those from the user's trusted local artifacts for this acceptance step; do not silently count Snoop37 as covering all of them.
8. Controlled corrupted application with backup available: automatic recovery, displayed reason, three-second confirmation/release, one-shot boot-anyway, next power-on checks again; app-return recovery uses reset for retry. A hung app can still be recovered by key.
9. Menu wipe with full before/after raw images: changes restricted to declared VM/BTIF and protected exclusions, flash ID/capacity and addresses displayed correctly. Check that unlisted gaps and resources remain unchanged. Verify firmware-specific consequences rather than assume every erased VM is merely settings.
10. SMK guard: first backup exact, no last-sector erase, then controlled invalidation/restoration. Any experiment blanking USRFLASH can make stock erase calibration and lose wheel/preset data; restore the **same unit's** full relevant regions afterward. Confirm wheel behavior, not just the trim marker. This guard must also be tested when the app cannot mount.
11. Official updater compatibility on a backed-up SMK: valid app-created request, staged RAM updater runs, official install succeeds, stock bootloader replacement is understood, and reinstalling A only affects boot area. Test FM-1 separately; old Pro success does not prove FM-1 or Elite updates.
12. Repeat recovery after controlled power interruption of app update, wipe and guard restore, while keeping USB_KEY recovery available. Record clock/register state and board revision for any failure, rather than assuming undocumented ROM state.

Release evidence SHOULD report device variant/revision, firmware versions, unit-key/JEDEC without publishing private calibration, toolchain, artifact hashes, tests actually performed, measured timing/performance and remaining failures. A claim of Pro success MUST NOT automatically be relabeled Elite or FM-1 hardware success. [Policy]

## 13. Open questions, confidence limits and resolved contradictions

### 13.1 Work still requiring verification

| Question | What is known / practical consequence |
|---|---|
| Complete WL82 ROM boot validation/search state machine | Successful fixed-base/container flow and host tooling are known; no full ROM analysis here. Outer CRC enforcement order, scan fallback and error responses remain U. Use the supported fixed layout. |
| Exact ROM state at A entry | Exception context and descriptor flow are S. Complete clocks/cache/interrupt/reset-register state and all ABI details are U. Test minimal independent startup on hardware. |
| ROM descriptor read callback return and modes beyond 0/1 calls | Proven invocation pattern only. Avoid extrapolating to other boot devices or filesystem bases. |
| ROM USB callbacks | Function pointer layout/handler framing S; short transfers, failure return interpretation, reentrancy/interrupt requirements and exact ROM-owned RAM bounds U. Keep to proven sizes/slots. |
| Legal recovery clock fallback with damaged config | Old zero defaults do not prove safe operation. Validate ROM-clock key scan or a conservative 24 MHz-board profile before claiming recovery for every damaged-config case. |
| Minimum PLL/CS/P33 settling times, lock bits, voltage margins | Existing counts/register sequences S, physical minima U. Avoid inventing precise microseconds from uncalibrated loops. |
| All SPI/SFC modes and automatic probing | Supplied quad-output mode is the principal target. BB single-bit address anomaly and continuous-mode transitions need hardware checks before advertising generalized support. |
| Protection-register universality | Generic SR1/SR2 model is used by existing code on observed parts. Different JEDEC families may need explicit support/refusal. No SFDP compatibility established. |
| Efuse trim disjoint-merge intention | Actual OR accumulation is S and specified; analog validity of “fixing” it is U. Measure row contents/firmware behavior before changing handoff trim. |
| Correcting OSC tag from 0 to intended OSC0=1 | Proper bounded parsing is required; impact of changed updater metadata needs an official-update hardware test. Clock frequencies must remain correct. |
| Dual-bank update lifecycle | Selection algorithm is S; no real dual-bank fixture or device test here. Versions, interrupted bank installation and info-record conventions beyond the decoded contract remain U. |
| Unsupported request type 5A0D | Real stock record uses it; supplied SDK enumeration ends earlier. A CRC/nonzero validity rule must not reject it just because the SDK header lacks a name. |
| Arbitrary updater destinations/extended arguments | Supported stock destination and ABI are checked. Extension payload semantics and all legal address ranges are not established. Preserve opaque fields and validate bounds. |
| FM-1 menu indicators and full replacement hardware behavior | Existing FM-1 pin/firmware evidence S, old menu LED implementation explicitly untested. No new A/B hardware test here. |
| Elite physical behavior | Shared pin/setup/boot/firmware evidence supports one SMK build, but an actual Elite hardware test is outstanding. |
| Recalibration after guard backup | Current guard leaves valid newer source intact but retains the old backup. If new source is later erased, old calibration returns. Updating backup safely requires a separately designed maintenance protocol; not silently solved here. |
| Guard interruption and backup corruption | Header-last helps identify validity, but no redundant slot or atomic recovery is guaranteed. Nonblank partial copy is intentionally left alone. |
| Whole-unit factory restoration | USRTRIM is not all wheel calibration; USRFLASH and resources matter. Exact broader factory formats are not specified and cannot be regenerated from a marker. |
| All `.fwsc` non-flash member conventions | Package/list/flash checks verified; some other members' stored CRCs differ from raw-byte CRC. A/B need not decode them. |
| Verification of all named firmware variants | Supplied stock, Baud Girl and Snoop37 bytes checked. Felucca/FM-1 SLOOP/Elite images not independently decoded here; their compatibility still requires fixtures and hardware acceptance. |

### 13.2 Resolutions the implementer should not undo

* Configuration blob is plain; top-level metadata and boot container have their own ENC boundaries. A blanket “encrypt the first 16 KiB” implementation is wrong. [E8 H]
* App boot information is physical base at +4, logical base at +8; a conflicting annotation reverses them. [E5 H, E6 stores S]
* Efuse trim accumulation does not reset between rows; the attractive disjoint-mask prose is not the actual stock arithmetic. [E6 instructions S]
* Calibration slot has header at FF000 and record at FF010; header is **written last**, not located last. [E2/E3 S/H]
* A's menu and host use different incompleteness heuristics; encrypted data does not make a 32-byte FF sequence impossible. [E2/E3 H/S]
* M-Flasher connection comments about “always reading twice” lag its CRC32 path; current behavior is read plus on-device CRC, with reread on fallback/mismatch. [E3 H]
* Current B's already-accelerated reinstall can misreport 48 MHz. Preserve clock safety while fixing reporting; do not rerun A's PLL sequence. [E3/E4 S]
* Safe bounds, explicit initialization, malformed-request draining and readback are replacement requirements where labeled, not assertions that the old code already implements them. [Policy]

Decisions taken when the two drafts were merged:

* Clock-control-0 bits 5:4 stay untouched and the updater's oscillator tag is 0, as on every working unit (§3.2, §5.3); one draft had specified clearing the bits for OSC0.
* The efuse trim value is specified by its result, `R2 OR (R3 AND 0xFFF238FC)`, low 16 bits (§3.5); the step-by-step accumulation in one draft gives the same value.
* The menu's wording is free; the previous wording is given as a suggestion (§9.1), and the previous font is offered as an option (Appendix A).
* All test vectors of §5.1 and §10.2 were recomputed independently and match.

## 14. Completion criteria for the clean implementation

A deliverable is ready for review when it includes both independent programs and reproducible board/slot builds, a map/size check, exact container and host identity compatibility, bounded parsers and DMA operations, a working recovery path, declared supported modes and explicit failure behavior, and the acceptance record above. Hardware tests not performed MUST remain listed as outstanding. None of the unknowns permits borrowing original source, calling undisclosed vendor routines by guessed address, or distributing another unit's calibration. Internal algorithms, data structures, module boundaries, names, font drawing and optimization strategy remain the clean implementer's choices within these interfaces.

## Appendix A: menu font (optional)

The 5 × 7 font drawn for the previous open bootloader by its author, provided here under MIT for reuse (§8.4). Characters, in this order: `` -./:<>+?0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ`` (the first is a space). Each glyph is five column bytes, left to right; bit 0 is the top row. Drawn at twice the size, a glyph occupies pixels 0…9 × 0…13 of its 12 × 16 cell.

```
' ' 00 00 00 00 00    '-' 08 08 08 08 08    '.' 00 60 60 00 00
'/' 60 10 08 04 03    ':' 00 36 36 00 00    '<' 00 08 14 22 41
'>' 41 22 14 08 00    '+' 08 08 3E 08 08    '?' 02 01 51 09 06
'0' 3E 51 49 45 3E    '1' 00 42 7F 40 00    '2' 42 61 51 49 46
'3' 21 41 45 4B 31    '4' 18 14 12 7F 10    '5' 27 45 45 45 39
'6' 3C 4A 49 49 30    '7' 01 71 09 05 03    '8' 36 49 49 49 36
'9' 06 49 49 29 1E    'A' 7E 09 09 09 7E    'B' 7F 49 49 49 36
'C' 3E 41 41 41 22    'D' 7F 41 41 41 3E    'E' 7F 49 49 49 41
'F' 7F 09 09 09 01    'G' 3E 41 49 49 7A    'H' 7F 08 08 08 7F
'I' 00 41 7F 41 00    'J' 20 40 41 3F 01    'K' 7F 08 14 22 41
'L' 7F 40 40 40 40    'M' 7F 02 0C 02 7F    'N' 7F 04 08 10 7F
'O' 3E 41 41 41 3E    'P' 7F 09 09 09 06    'Q' 3E 41 51 21 5E
'R' 7F 09 19 29 46    'S' 46 49 49 49 31    'T' 01 01 7F 01 01
'U' 3F 40 40 40 3F    'V' 1F 20 40 20 1F    'W' 3F 40 38 40 3F
'X' 63 14 08 14 63    'Y' 03 04 78 04 03    'Z' 61 51 49 45 43
```

## Appendix B: public sources

The implementer may consult these for facts (§0.1). Public sources can be wrong or describe other chips of the family: treat their facts as I until tested.

* kagaimiq's JieLi notes, github.com/kagaimiq/jielie: `cpu/pi32v2.md` (registers, instructions), `datafmt/bankcb.md`, `datafmt/jlfs.md`, `datafmt/sdkcfg.md`, `misc/cipher.md`, `isp/usb/usb-key.md`.
* jl-uboot-tool, github.com/kagaimiq/jl-uboot-tool (MIT): the download-mode protocol from the PC side.
* jl-misctools, github.com/kagaimiq/jl-misctools: firmware and key-file tools.
* M-Flasher's host code, github.com/jvitkauskas/m-flasher, `src/` (MIT): `formats.rs`, `jl.rs`, `boards.rs`, `sectormap.rs`, `ops.rs`, `tests.rs`.
* The LZ4 block format description, github.com/lz4/lz4, `doc/lz4_Block_format.md`.
* Felucca (FM-1 firmware: pins, panel), SLOOP and its SMK-37 port Snoop37, and genkigenki/fm1-to-smk37 (SMK-37 hardware notes): for pin and panel facts; check each project's licence before reusing any code.
* Datasheets: AC791N family (SDK `doc/datasheet`), ST7789V, the flash parts, a Winbond W25Q-series part.
* JieLi's AC79 SDK headers (register names/addresses, `boot.h`, `update.h` layouts) and documentation, and doc.zh-jieli.com.

## Appendix C: test images for the implementer

Copies of the four fixtures of §12.1 with the stock bootloader bytes (flash `0xA0`…`0x38CF`) overwritten with `0xFF`; everything else (flash header, tables, configuration, app area, regions, last sector) is unchanged. Not to be published (§0.3). SHA-256 of the blanked copies:

| File | SHA-256 |
|---|---|
| `fm1-stock.bin` | `1cfe35585ab2da776638d8eb13957d8f2d7817db4a7a194693c8992be58b22df` |
| `smk37-stock.bin` | `ed1e8533375457a08b0de23922993c314eae9f171bd4bd925ee0bba43bb7e0c4` |
| `smk37-snoop37.bin` | `444088709abebfb6790011d405a87437ec4fb09b79589e1c47008083db7827c1` |
| `smk37-baudgirl.bin` | `fedde66e70a85d5feaa74d2de4bdd8c6d137cabd1931b8e1cbc04c55631bb312` |

With the bootloader blanked, the boot area's `uboot.boot` record still describes the stock container (its data CRC will no longer match the blanked bytes); everything the tests of §12.2 need is intact. The staged update loader at `0xB9000` in `smk37-stock.bin` (§6.3) stays in the image as the fixture for the update path: it is a JieLi SDK update loader packaged by M-VAVE, a different program from the bootloader. Use it only as compressed data to decode and checksum (its block lengths, size and CRC are in §6.3); do not disassemble or study its code. M-VAVE's `.fwsc` packages are not provided: they contain the stock bootloader.

## Appendix D: amendments

Answers to the implementer's questions (§0.2) and corrections found in testing, numbered in order. Each states the question or finding, the answer as facts, its evidence class, and the sections it changes.

*None yet.*
