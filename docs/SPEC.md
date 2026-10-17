# m-bootloader specification

Version 1.0, 2026-10-16

This document specifies how the JieLi AC791N (WL82 family, pi32v2 CPU) in the M-VAVE FM-1 and SMK-37 Pro / Elite boots from its SPI flash; the formats and encryption of the flash contents; the retained-RAM hand-overs between the mask ROM, the bootloader, staged updaters and the application; the required behaviour of the bootloader; the two boards (keys, LEDs, display); the bootloader's recovery menu; and the USB command protocol of the RAM flash loader that the host tool M-Flasher uploads in the chip's USB download mode. It describes interfaces, hardware transactions, data formats and observable behaviour, so that a compatible implementation can be written from it. m-bootloader implements both programs in C and pi32v2 assembly under the MIT licence.

## 1. Scope and requirements

Two independently deployable programs are specified:

* **The bootloader.** A RAM-executed second-stage boot image stored in the first 16 KiB of flash. The mask ROM loads it. It starts the installed application, honors staged update requests, provides a recovery key and a display menu, and protects specified unit data during its own writes.
* **The flash loader.** A RAM program uploaded by the PC through the mask ROM's USB download mode. It registers a command handler in the ROM's existing USB mass-storage implementation. It gives the unchanged M-Flasher host access to raw SPI flash and a few display and identification commands.

"The stock bootloader" is the JieLi bootloader (`uboot.boot`) that M-VAVE ships in the devices' flash and in its firmware packages. "JieLi's loader" is the flash loader from JieLi's SDK that can also be uploaded in download mode. Where this document describes what they do, it is because the bootloader or the flash loader must match it or may deliberately differ from it.

The devices have 1 MiB SPI NOR flash, no external SDRAM/PSRAM, a 24 MHz oscillator, and a 240 × 240 ST7789-family color display. The bootloader MUST boot compatible stock M-VAVE images, FM-1 Felucca and SLOOP images, and SMK-37 Snoop37 images without altering them. Baud Girl is a valuable SMK-37 compatibility case because its declared storage layout differs. Firmware's own startup shortcuts MUST remain available. The flash loader MUST work with the host protocol in §10 without requiring host changes.

### 1.1 Requirement words

**MUST** denotes an external compatibility or safety requirement. **SHOULD** denotes a recommended engineering property. **MAY** denotes a free design choice. Requirements described as **policy** are deliberate improvements over, or constraints beyond, what the stock bootloader and earlier loaders do; they are not claims about existing implementations. Facts described as not established or not verified on hardware should not be silently assumed to hold.

No binary identity, instruction sequence identity, source layout, internal address layout, private helper interface, or particular font is required.

### 1.2 Notation

Numbers with `0x`, opcode/byte strings, compact address ranges and address columns containing values such as `C1000` are hexadecimal. Ordinary byte counts, structure offsets written as 0/2/4/8/12/16, dimensions and timing counts are decimal unless marked otherwise. Memory and flash ranges are half-open `[start,end)` unless inclusive endpoints are explicitly stated. Source-level hardware register labels are used only as hardware identifiers.

### 1.3 Deliberately excluded support

External RAM initialization/training, SPI3 PSRAM, SDRAM application images, flash larger than 16 MiB / four-byte flash addressing, SD/NAND/OTP boot targets, UART updating, RF provisioning, efuse programming, chip erase, a new USB stack in the bootloader, firmware installation from the bootloader's menu, and general settings UI are outside this specification. These devices do not require them; the flash loader MUST NOT expose irreversible efuse programming or chip erase. The bootloader MAY omit unused oscillator and flash read modes as described in §3, but MUST support the stock configuration and firmware interfaces. Dual-bank directory selection is specified because the existing boot contract supports it, although no known image exercises it and it has not been verified on hardware.

## 2. Machine, address spaces and peripheral register facts

### 2.1 CPU and memory

The CPU target is pi32v2, toolchain CPU `r3`, little endian, 32-bit pointers. Use the target compiler's C calling convention: the first two argument words are in `r0`, `r1`, the result is in `r0`; normal calls must preserve the ABI's saved registers. The CPU has a main stack and a system/exception stack, and exception return state separate from normal return state. The bootloader's entry is exceptional; the flash loader's entry is a normal ROM call.

| Address/range | Meaning and constraint |
|---|---|
| `0x00010000` onward | Low-speed peripherals; these are CPU addresses, not flash offsets. |
| `0x00040000` onward | High-speed peripherals, including SFC. |
| `0x00050000` onward | GPIO and pin mux. |
| `0x01C00000` through below `0x01C80000` | Internal RAM address region used by SDK link layouts; this is not permission to overwrite all of it while ROM runs. Its full physical extent is inferred. |
| `0x01C02000` | Bootloader payload load and entry address. |
| `[0x01C02000,0x01C0A800)` | The bootloader's entire live code/data/stacks/workspace MUST fit here while a stock updater can execute. |
| `0x01C0A800` | Decompressed load/entry address of the stock staged updater. |
| `0x01C20000`, `0x01C28000` | The flash loader's two link/entry locations (first and second slot). Each image and all private RAM MUST fit within its own `0x8000`-byte slot. |
| `0x01C7FD80` onward | Retained boot/update/application communication area; exact layouts in §6. MUST survive the bootloader's own initialization. |
| `0x01EEE008` | Cache control. |
| `0x01F00000` etc. | Cache internal arrays, cleared only as specified in §3.6. |
| `0x02000000` | Decrypted, cached SFC logical flash window. Application logical address zero is mapped here. |
| `0x02000120` | Entry for all known XIP application images. |
| `0x04000000` | External RAM window, absent on these boards. Reject applications requiring it. |

The complete mask-ROM address map, its private RAM allocations, full CPU control-state reset values, interrupt vector arrangement and all ABI subtleties are not established. The flash loader MUST restrict itself to its slots and the supplied ROM callbacks rather than guessing private ROM addresses.

### 2.2 Register directory

MMIO register slots below are four bytes apart. Where the SDK declares an 8- or 16-bit register, unused bytes in the slot are reserved. Known-working implementations use word accesses to many such slots; byte-lane equivalence has not been measured. Read/modify/write MUST preserve unrelated bits where only a field change is specified. `bit n` means `1 << n`.

| Block | CPU base/address | Registers/offsets needed |
|---|---|---|
| Power | `0x10000` | Control; bit 4 requests core software reset. |
| Clock dividers | `0x10008` | HSB divisor minus one: bits 17:16; LSB divisor minus one: bits 10:8. |
| Clock controls | `0x1000C`, `0x10014` | Control 0; control 2 respectively. Details §3.2. |
| Chip stepping ID | `0x10200` | Low byte selects efuse read width. This is different from the 16-bit encryption key. |
| Timer 0 | `0x10400` | `+0` control, `+4` count, `+8` period, `+0xC` PWM. All four are 32-bit registers (§3.3). |
| Main PLL / secondary PLL | `0x119A0`, `0x119A4`; `0x119A8`, `0x119AC` | First/second control words of each PLL. |
| SPI0 (flash) | `0x11C00` | `+0` control, `+4` baud divisor, `+8` byte buffer, `+0xC` DMA RAM address, `+0x10` DMA count. |
| SPI1 (panel) | `0x11D00` | Same five register offsets. |
| Peripheral ENC | `0x13000` | `+0` control (SDK u8), `+4` key (u16), `+8` cipher address. |
| CRC16 | `0x13500` | `+0` input FIFO, `+4` result/initial value. |
| P33 serial bridge | `0x13E00` | `+0` PMU control, `+4` secondary serial/RTC control, `+8` main serial control, `+0xC` serial byte data. |
| SMK LED commons | `0x16A0C` | High-speed USB PHY control; bits in §8.3. |
| SFC | `0x40200` | `+0` control, `+4` baud, `+8` code, `+0xC` physical flash base. |
| XIP ENC | `0x40300` | `+0` control (u8), `+4` key (u16); further exception-range registers exist but are not needed by the boot flow. |
| GPIO A–H | `0x50000 + 0x40 × port`, A=0…H=7 | `+0` output, `+4` input, `+8` direction (1 input), `+0xC` digital-input enable, `+0x10` pull-up, `+0x14` pull-down, `+0x18` drive option 0 (SDK `HD0`), `+0x1C` drive option (SDK `HD`), `+0x20` second digital-input enable. |
| Mux controls | `0x5101C`, `0x51020` | First and second mux control words. |
| Cache control | `0x01EEE008` | Bits 8 and 9 enable read-only flash and read/write cache; bit 14 indicates idle. |

For SPI control, bit 0 enables, bit 1 selects slave (0 master), bit 3 selects separate transmit/receive lines/full duplex, bits 5:4 determine update/sample edges, bits 11:10 select DMA width (00 single, 01 dual receive, 10 quad), bit 12 is direction (0 TX, 1 RX), bit 13 is interrupt enable, bit 14 clears completion and bit 15 reports completion. Some SDK symbolic names for bits 10/11 are confusing; the actual TX/RX usage matters. Writing DMA count starts the transfer; count is 16-bit. A zero count MUST NOT be used to wait for a display transfer. The SPI clock is the source clock divided by `(baud + 1)`; this relationship is inferred and consistent with working timing.

## 3. Hardware initialization and transactions

### 3.1 P33 power domain and reset

The main P33 bridge performs a three-byte transaction. Assert bridge serial-control bit 0 for the entire transaction. For each byte: OR `0x0C` into serial control (bits 3:2 = `11`); write the byte to the serial data register; OR bit 4 into serial control to start; immediately poll until bit 1 reads 0; then read the returned data from the data register if needed. There is no barrier between setting bit 4 and the first poll, and software never clears bit 4: it is set again for each byte. So bit 1 already reads busy at the first poll, or the transfer is short enough that the immediate poll sees the result. Whether bit 4 clears itself is not known. A core synchronization (`csync`) before polling does no harm. With this sequence the efuse key, voltages and watchdog are read and written correctly on hardware.

The three bytes are (1) operation OR address bits 9:8, (2) address bits 7:0, (3) data or read dummy `0x5E`. Operations are write `0x00`, OR `0x20`, AND `0x40`, read `0x80`. Clear bridge bit 0 after the third byte. These are P33 register addresses, not CPU addresses.

| P33 address | Required purpose |
|---|---|
| `0x05` bits 2:0 | Main I/O voltage selection: code 0…7 corresponds to 2.8…3.5 V, 0.1 V increments. |
| `0x06` bits 2:0 | 1.4 V domain selection: 1.25 V + code × 0.05 V. Default code 3 = 1.40 V. |
| `0x09` bits 3:0 | Digital core voltage: 0.93 V + code × 0.03 V. Default code 11 = 1.26 V. |
| `0x80` bit 4 | Watchdog enable; clear for bootloader startup. |
| `0xAD`, `0xAE`, `0xAF` | P33/other/USB pin latch controls; write zero to release at startup. |
| `0xB0`, `0xB1`, `0xB2` | Efuse bank selection, row/byte/read strobe, returned byte. |
| `0xD0…0xD7` | A…H pin latches, pins 0…7. |
| `0xD8…0xDF` | A…H pin latches, pins 8…15. |

The voltage mappings are SDK declarations. The bootloader MUST set core/domain voltage before increasing clock frequency. It always uses core code 11 and domain code 3 and leaves the I/O voltage unchanged. The stock bootloader can instead take voltages from the configuration keys `DVDD`, `DCDC14` and `VDDIO`, treating a zero value as unset (zero does not request the minimum voltage); the devices' configuration contains none of these keys, and the bootloader does not support them (§5.3).

At startup, clear the sixteen A–H latch bytes and the three additional latch registers, with a settling interval before and after, before switching clocks. The stock bootloader first applies configured `PORT_INPUT`/`PORT_OUTPUT` GPIO states (keys the bootloader does not support, §5.3). Its settling intervals are two uncalibrated busy-loop iterations at each side; no electrical minimum is established. These waits run before the clock switch, on the very slow ROM start-up clock, and MUST stay within the limits of §3.2.

Before a software reset, the stock bootloader latches every pin explicitly named by the configured `PORT_OUTPUT` policy: it sets the pin's bit in the appropriate low/high P33 latch byte without changing its output level, then sets power-control bit 4. This keeps named outputs across reset; it does not encode their levels in the latch byte. Normal startup releases those latches after reapplying outputs. The bootloader has no configured outputs, so before a software reset it latches nothing and only sets power-control bit 4.

### 3.2 Clock contract for the bootloader

For the stock configuration, the bootloader MUST establish nominal system 360 MHz, HSB 180 MHz and LSB 60 MHz. SPI0, SPI1 and Timer 0 use LSB timing. It is unsafe to interpret the configured divider values as divisors directly: add one. The UART clock, if queried by a compatible updater, is treated as 48 MHz. No UART service is required.

The following are the hardware field settings of the working stock sequence. Unnamed fields in secondary-PLL controls are preserved. Delay counts here are **uncalibrated loop counts**, not microseconds. The bootloader MUST allow PLL settling; actual minima and whether a lock status can replace delays are not established. How long these waits may be is specified below the steps.

1. Clear clock-control-0 bit 1, settle for the equivalent of two short loop iterations; clear bit 8 and settle again. **Leave bits 5:4 as found.** The stock bootloader intends to clear them for OSC0, but because of the oscillator-tag defect described in §5.3 it never does on these devices: every working unit has run with bits 5:4 untouched. Clearing them has not been verified on hardware and MUST NOT be done without a hardware test. No additional distinct 24 MHz hardware programming for OSC1 or OSC_RTC is established.
2. Select the required system source/division in clock-control-2's low byte. The 360 MHz case uses source nibble 6 and both divider-index fields 5:4 and 7:6 zero: low byte `0x06`.
3. For the 24 MHz oscillator and main 480 MHz PLL, program first PLL control initially `0x45400200`, second control `0x3F503026`, enable first-control bit 0, settle ten loop iterations, then enable bit 1. The resulting first control is `0x45400203`. These values follow a 12 MHz reference divider of 2 and multiplier field 38.
4. The 360 MHz system source needs the secondary PLL. Disable its control-0 bits 0 and 1. Set control-0 fields: bits 6:2 = 10, bit 7 = 0, bits 9:8 = 2, bit 10 = 0, bits 23:22 = 1, bits 25:24 = 1, bits 27:26 = 2. Set control-1 fields: bits 11:0 = 268, bits 14:12 = 3, bits 18:15 = 0, bits 21:20 = 1, bits 23:22 = 1, bits 26:24 = 7. This is a 2 MHz reference and nominal 540 MHz PLL target used by system source 6. Enable control-0 bit 0, settle ten iterations, then bit 1. With other bits zero, final controls would be `0x0940022B` and `0x0750310C`; do not use those whole-word examples to destroy unknown retained bits.
5. Allow the equivalent of ninety more loop iterations. Set clock-control-0 bits 6 and 7. Set system-divider bits 17:16 = 1, bits 10:8 = 2. Set clock-control-0 bit 8 to select the prepared clock path.

**Waits before the clock switch.** From the bootloader's entry until the clock switch above completes, the CPU runs on the ROM's start-up clock, which is very slow: on an SMK-37 Pro, a busy loop of one `nop` per iteration with its counter in a register ran about **48,000 iterations per second** (672,000 iterations took about 14 s, and the boot reached the key check about 15 s after power-on). After the switch everything runs at full speed. The stock bootloader waits, before the switch, **2** iterations around each clock-control-0 change and the latch release (§3.1), **10** after enabling each PLL, and **90** before switching, in a loop whose counter lives in memory (a few CPU cycles more per iteration than a register loop). Those values work on every unit. Waits before the switch MUST stay of that order: these counts, or at most a few times more (a register-counter loop may use up to about 10× the counts to match the stock real time). They MUST NOT be scaled up for safety: 1,000 iterations already cost about 20 ms. Timer 0 cannot be used for these waits, because its clock before the switch is not established; after the switch, use the timer (§3.3).

The bootloader MAY support the following other fixed system settings of the stock bootloader. In this table the three columns give clock-control-2 fields 3:0, 5:4 and 7:6. Electrical validation beyond the 360 MHz case is not established.

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

For optional arbitrary clock settings the stock bootloader uses secondary-PLL system selection 5 below 160 MHz (multiplier factor 4), 6 through 384 MHz (factor 3), otherwise 7 (factor 2); the PLL target is `system MHz × factor / 2`. Reference prescaler is oscillator Hz / 2 MHz minus 2; multiplier field is PLL target Hz / 2 MHz minus 2. Supporting arbitrary unvalidated frequencies is not necessary for these devices.

For optional non-24 MHz main-PLL support, choose a 12 MHz reference when oscillator frequency is divisible by 12 MHz, otherwise a 2 MHz reference when legal. First-control base `0x01400200` plus prescaler `(oscillator/reference − 2)` in bits 6:2, reference-type bits 27:26 (2 for 2 MHz, 1 for 12 MHz), and bits 31:30 = 1; second-control base `0x3F503000` plus `(480 MHz/reference − 2)` in its multiplier field. Validate divisibility and field bounds first. HTC is treated by the stock bootloader as a preexisting 12 MHz PLL-derived reference and skips main-PLL reprogramming. Those paths are not verified and MAY be rejected with an explicit recovery reason rather than guessed.

The unused 12 MHz OSC_RTC path uses a second P33 bridge channel: hold secondary-control bit 0; send `0xA0 + register` then dummy zero for read, or `0x20 + register` then value for write, using the shared serial data register. After each byte, synchronize the core and wait for secondary-control bit 1 set; release bit 0 at the end. It sets target register 0 bit 0, and target register 6 bit 4 according to high-current oscillator selection while clearing bit 5. The physical target of this channel is unresolved; it is **not** established as the SDK's normal RTC-address-space access. This option is unnecessary for the 24 MHz boards and MAY be excluded.

### 3.3 Delays, timeouts and CRC peripheral

Timer 0 provides calibrated delays after clock setup: stop/clear completion with control bit 14, set count zero, set period to `(LSB Hz / 1,000,000) × requested microseconds`, start with control bit 0, wait for bit 15, then clear/stop with bit 14. Period (SDK `PRD`) and count (SDK `CNT`) are 32-bit registers. Control is written only with the values `0x4000` and `1`, so every other control field, including any prescaler field, is 0: one count per LSB clock. Single waits of up to 120 ms (7,200,000 counts at the 60 MHz LSB clock; the SMK-37 panel set-up) work without splitting on an SMK-37 Pro; the FM-1 panel set-up's 150 ms wait (9,000,000 counts) has not been run unsplit on hardware. Splitting long waits into steps of at most 1 ms is also correct. Recovery keys are sampled every 20 ms; display LED refresh uses 1 ms. The timer is not usable before the clock switch (§3.2).

The CRC peripheral computes CRC16/XMODEM when result is seeded zero and each byte is written to its FIFO. Synchronize before reading the result. SPI0 DMA also feeds this CRC unit; software CRC MUST be used for calculations that overlap such DMA, or the peripheral must be isolated. See the algorithm in §5.1.

Policy: malformed flash contents, missing SPI completion, stuck flash busy, or failed screen initialization MUST NOT cause memory corruption or an infinite silent startup hang before recovery is offered. Use bounded waits and a recoverable failure state. Exact timeout values are design choices and require hardware validation. The flash loader's valid commands SHOULD complete inside the host's 10-second command timeout, especially erase commands.

### 3.4 SPI0 flash access

The proven WL82 transactions use software CS on PD0. Mux control 1 bit 1 and mux control 0 bit 5 are cleared; raw SPI0 works with mux-control-0 bit 16 cleared (§3.6). Configure PD4/PD5 as outputs, set the PD4 drive-strength bit (bit 4 of port D's second drive register at `+0x1C`, SDK `HD`; the register at `+0x18`, `HD0`, is not touched), PD1 as input. For quad mode, PD2 and PD3 are outputs initially high. The inferred remaining signal roles are PD4 clock, PD5 output/IO0, PD1 input/IO1, PD2/PD3 additional quad lines. The roles beyond CS are inferred from register accesses; SDK configuration comments describe another pin mapping and MUST NOT override these WL82-specific accesses without measurement.

Disable SFC before claiming SPI0. The initial SPI control is `0x28` before enable/clear, in both the bootloader and the flash loader. Set desired baud, clear pending with bit 14, enable with bit 0, deassert CS and allow a short settling interval. For a byte TX, clear direction bit 12, write the byte to the byte buffer, wait completion, clear pending. For byte RX, set direction and clock a dummy `0xFF` byte. Known-working drivers synchronize before polling and delay after CS changes (ten busy-loop iterations at boot, forty in a flash loader); the electrical minimum CS high/setup time is not known.

All supported NOR commands use MSB-first 24-bit byte addresses. Flash command facts (not a claim of universal support on all NOR brands):

| Opcode | Operation / data |
|---|---|
| `9F` | Read JEDEC manufacturer, memory-type and capacity-code bytes. |
| `03` | Single-bit ordinary read, address then data, no dummy byte. Used by the flash loader. |
| `0B` | Single-bit fast read, address then one dummy byte. |
| `3B` | Dual-output read. Address is single-bit, then 8 dummy clock cycles, then dual data. |
| `6B` | Quad-output read. Address is single-bit, then 8 dummy clock cycles, then quad data. The stock configuration selects this. |
| `BB` | Dual I/O, with address/mode packet; optional, see caveat below. |
| `EB` | Quad I/O, with quad address/mode packet, then dummy clocks. |
| `05`, `35` | Read status registers 1 and 2. |
| `06` | Write enable; separate CS transaction before each write/erase/status write. |
| `01` | Write status 1 and, for compatible parts, status 2 in the same transaction. |
| `31` | Write status 2 alone; used as a quad-enable fallback. |
| `02` | Page program, address then data, maximum 256-byte page, MUST split at page boundaries. |
| `20` | Erase 4 KiB sector. |
| `D8` | Erase aligned 64 KiB block. |
| `4B` | Factory unique ID: four dummy bytes after opcode, then eight ID bytes. Flash loader only. |

For raw DMA read, hold CS active for opcode/address/data, set peripheral ENC control to 2 (pass through), direction RX and width fields, set DMA RAM address then nonzero count. On completion clear pending, turn peripheral ENC off, and release CS. Never reuse the DMA destination until complete. For decoded top-level headers, key `0xFFFF`, cipher address zero, and ENC control `0x83` restart the stream for each independent read. Off afterward is zero. Software decryption MAY replace this path when its exact restart boundaries are preserved.

Summary of the read command chosen from the configured width and read mode (mode digit 0 → output, 1 → I/O, 2 → continuous I/O):

| Width | Output (mode 4) | I/O (mode 8) | Continuous I/O (mode 12) |
|---|---|---|---|
| 1 | `0B`, one dummy byte, single-bit data | `0B` | `0B` |
| 2 | `3B`: command/address single-bit, two dummy bytes received dual, data dual | `BB`: address/mode packet (address, `00`), data dual | `BB` with mode byte `A0`, opcode only on the first transaction |
| 4 | `6B`: command/address single-bit, four dummy bytes received quad, data quad (**the stock configuration**) | `EB`: address/mode packet quad, two dummy bytes quad, data quad | `EB` with mode byte `A0`, opcode only on the first transaction |

The bootloader MUST support width 4 output reads and a single-bit read for its safe initial phase; the other cells MAY be omitted with a documented fallback (§5.3, §13).

Every multi-bit phase is clocked with an SPI0 DMA transfer, never with single-byte register (`+8`) transfers: for `6B` the four dummy bytes are one 4-byte DMA receive at quad width, for `3B` the two dummy bytes one 2-byte DMA receive at dual width, and for `EB`/`BB` the address/mode packet one 4-byte DMA transmit. Each such DMA is set up like the data DMA that follows it: direction, width bits, bit 3 cleared, ENC control 2 (pass-through), RAM address, count; wait for completion and clear it; then the data DMA. Only the command byte and the 24-bit address are sent as single-byte register transfers, at single width. Whether single-byte register transfers honour the width bits is **not established**; implementations MUST use DMA for the dummy phases unless a hardware test shows that byte transfers work. This is how the stock driver reads on hardware.

Quad receive uses width bits `10`, bit 3 clear; dual receive uses `01`, bit 3 clear. Restore width zero and separate-line mode after the multi-bit data phase for subsequent commands. At 6B, four dummy **bytes received in quad width** give the eight dummy cycles; at 3B, two dual bytes do the same. At EB, send a four-byte address/mode packet in quad width, then receive two quad dummy bytes. Mode byte zero is ordinary I/O; `0xA0` requests continuous read, where the initial opcode is omitted after the first transaction. The known BB path sends address/mode single-bit and has no explicit extra dummy phase. Whether real dual-I/O NOR accepts that is unresolved; that path MUST NOT be claimed validated.

If a wide read does not match a single-bit read of the same bytes, an implementation MAY fall back to a narrower width, but MUST NOT do so silently: the boot continues only if the app check passes, and the fallback MUST be visible (a reason in the recovery menu, and in the flash loader's status where applicable). Running the app through single-bit XIP makes the synthesizer markedly slower.

To use quad access, read both status bytes; keep existing status bits except clear busy/write-enable bits in the status-1 value to be written, and set status-2 QE bit 1. Try the combined `01` status write, poll status-1 WIP bit 0 until zero (the stock bootloader waits 100 μs between polls), read back QE. The stock bootloader permits an initial attempt plus up to three retries using separate status-1 `01` and status-2 `31` writes. Failure to establish QE MUST fall back to a validated narrower path or recovery; do not proceed blindly.

Before any erase/program that needs it, clear status-1 mask `0x7C` (BP/TB/SEC protection) and status-2 mask `0x40` (CMP), preserving QE and unrelated bits. This Winbond-style status model is used on the observed `0x856014` and `0xEB6014` parts; its generalization to other brands is not established, and no general SFDP support is required. Exit a previously enabled continuous-read mode first: send two `FF` bytes under CS and reset the continuous-read tracking. Then WREN, command/address/data, release CS, and wait WIP clear.

Capacity is derived from the JEDEC capacity exponent: the observed last byte `0x14` means `2^20 = 0x100000` bytes. Reject implausible exponents rather than overflowing a shift. The flash header's `0xFF000` size is filesystem space and excludes the final reservation; it MUST NOT be mistaken for physical chip capacity. The flash loader operates on raw bytes with no application encryption/decryption.

### 3.5 Efuse and flash key fallback

Efuse is read only. Byte width per row is four when chip-stepping ID low byte is nonzero, otherwise three. For bank selector `s`, write P33 `0xB0 = s << 6`. For row `r` and byte `j`, write `0xB1 = (r << 6) | (j << 2) | 2`, wait at least 50 μs, read `0xB2`, clear `0xB1`, and assemble bytes little endian. An uncalibrated loop in place of the 50 μs delay has been used in a flash loader; a calibrated delay SHOULD be used.

The encryption key is bank 1, row 3, bits 23:8, truncated to 16 bits. The bootloader uses it unless it is `0xFFFF`; then read the last 256 physical flash bytes. In that page, a valid fallback has CRC16 of bytes 0…7 equal to the little-endian halfword at +8, and those first eight bytes are not all zero with zero CRC. Use bits 23:8 of the little-endian word at +4, i.e. page bytes +5/+6, as key. All known devices and packages use `0x980F`, but the bootloader and the flash loader MUST read the unit's key; hardcoding it is inappropriate. The flash loader's FC09 reports the physical efuse key, not the bootloader's fallback.

For the bootloader's 16-bit trim export, bank 0 rows 2 and 3 are read. The stock bootloader keeps the accumulated row-2 bits when row 3 is added: if the row values are R2/R3, the stock result is **`R2 OR (R3 AND 0xFFF238FC)`**, of which only the low 16 bits are handed to the app. A disjoint merge that selects row-2 bits with `0x000DC703` and row-3 bits with `0xFFF238FC` is not established as equivalent. The bootloader MUST preserve this stock-visible result unless a hardware-backed correction is explicitly adopted. The low trim byte feeds the bandgap trim: magnitude bits 6:2, sign bit 7; other fields are not needed here. Their analog meaning is partly inferred.

The eight Bluetooth address bytes are copied without transformation from physical `capacity − 192`, i.e. `0xFFF40` on 1 MiB. They represent six address bytes plus a little-endian CRC16 according to SDK interfaces. They are passed on without validation: all-FF data is allowed to reach firmware, which decides how to handle it. Last-page reads are absolute for the supported base-zero layouts; do not add a nonzero filesystem base, which would reach beyond chip capacity.

### 3.6 Cache and XIP setup

Before executing an application, the bootloader MUST disable SPI0 ownership, select the SFC pin route, reset SFC control by writing `0x00F00000` then zero, program baud from the configured SPI divider, write the mode word below, and set the physical app-area base in `0x4020C`. Program the XIP key with the selected unit key.

The SFC pin route has mux-control-0 bit 16 cleared. The bootloader never sets bit 16: it clears it before the XIP set-up and leaves it cleared. Its raw SPI0 set-up does not touch bit 16. After a failed XIP attempt, re-initialising SPI0 and reading the flash for the recovery menu works with bit 16 still cleared (observed on an SMK-37 Pro). The value of bit 16 at the bootloader's entry is not established. Saving it before XIP and restoring it afterwards is not required, and restoring a set value has not been tested; leaving it cleared is the known-good state.

| Data width | Output read mode | I/O read mode | Continuous I/O mode |
|---|---:|---:|---:|
| Quad | `0x00280381` | `0x00260581` | `0x00260781` |
| Dual | `0x00280281` | `0x00240481` | `0x00240681` |
| Single | `0x00200089` | `0x00280189` | not established |

The internal mode numbers 4/8/12 correspond to the three columns, not flash opcodes. After writing the chosen mode word, replace bits 23:20 with `floor(floor(HSB Hz / 1,000,000) × 45 / 1000) + 1`, for roughly 45 ns chip-select-high timing. At HSB 180 MHz this is 9. Generic community SFC notes speculate a different meaning for this field; the WL82 boot flow and SDK establish this timing use more strongly. Exact fields behind the rest of each mode word are not all named; the mode words are the required hardware facts. The optional (non-quad-output) modes are not verified on hardware.

Invalidate caches by clearing enable bits 8/9 and clearing these half-open internal ranges with word writes: `[0x01F00000,0x01F04000)`, `[0x01F08000,0x01F0A000)`, `[0x01F0A000,0x01F0A200)`, `[0x01F0B000,0x01F0B200)`. Restore enable bits. Disable interrupts (`cli`), wait cache-control bit 14 idle, enable XIP ENC with control 1 and ensure cache bits 8/9 set before reading/checking or calling decrypted XIP code. This clearing operation is what the stock bootloader does; it is not a complete description of the cache organization.

If the CRC check or the application call fails/returns, release XIP flash ownership before menu SPI reads. After a failed XIP attempt, a subsequent boot choice MUST restart through reset rather than directly attempting XIP setup a second time; the direct retry failed on an SMK-37 Pro.

## 4. Mask ROM boot and recovery interfaces

### 4.1 Finding and loading the bootloader

For these devices the ROM's boot filesystem base is physical flash zero. Its first 32 bytes are a flash header; 32-byte directory records start at +`0x20`. The first record denotes the boot container, stored at physical `0xA0`. The container is decoded with fixed key `0xFFFF`; its header and payload restart encryption separately. The stock first-record attribute is zero, meaning an uncompressed boot image. Multi-bank BANKCB and ROM-dictionary compressed boot images are not required.

The bootloader MUST use this 16-byte single-bank BANKCB container, all numeric fields little endian:

| Container offset | Size | Meaning / required value |
|---:|---:|---|
| `0` | 2 | Bank count, 1. |
| `2` | 2 | Payload length in bytes, excludes this header. |
| `4` | 4 | Load address `0x01C02000`. |
| `8` | 4 | Payload file offset, 16. |
| `12` | 2 | CRC16/XMODEM of plain payload. |
| `14` | 2 | CRC16/XMODEM of plain bytes 0…13. |

The ROM loads the payload, not the container header, and transfers control to its first byte. A bare release `.boot` file MUST be a plain container; flash installation applies encryption and updates the outer filesystem entry. Header/payload CRC behavior is established by tools, stock images and working boots. The precise order of the ROM's checks, which outer filesystem CRCs it enforces, its response to each malformed header, and its complete search algorithm have not been verified against a WL82 ROM dump; they are not a proved ROM state machine.

At bootloader entry, `r0` points to the ROM boot-device descriptor:

| Descriptor offset | Size | Meaning |
|---:|---:|---|
| `0` | 4 | Pointer to device parameter storage. Its byte +1 is the read-mode byte. |
| `4` | 4 | Flash/filesystem base offset, zero for the supported images. |
| `8` | 4 | Device-online value; exact semantics unused/unverified. |
| `12` | 4 | Read callback address. |

The callback receives device parameter pointer in `r0`, destination pointer in `r1`, absolute storage byte offset in the next argument, and byte length in the fourth. Before invoking it, set the parameter's byte +1 to the requested mode: metadata reads use 1, plain configuration data uses 0. A mode-1 read returns the 32-byte top-level record **already decoded** (ENC `FFFF` removed): the stock bootloader uses its address and length fields directly, without decoding them and without checking its CRC. Accepting either representation is harmless. The stock flow reads the configuration descriptor at relative +64 in mode 1, then its data in mode 0. A read of +96 exists but its result is unused and is not a requirement. Exact callback return conventions and mode behavior beyond these calls are not established.

The bootloader starts in the ROM's exception context. It MUST preserve the incoming descriptor while establishing its own initialized state, use a valid exception return to enter normal execution, and establish its own system and main stacks. The sequence that runs on SMK-37 Pro hardware is:

* The exception-return state register **`rete`** is set to **0**. Then **`reti`** is set to the address of the continuation and **`rti`** is executed. Neither `retx`, `psr`, `icfg` nor any other control register is written.
* **No stack pointer is changed before `rti`.** At the continuation (after `rti`), **`ssp`** is set to the top of the system stack and then **`sp`** to the top of the main stack, and the C code is called. `usp` is not written. Whether `rti` itself switches stack pointers is not known; the sequence does not depend on it, because it neither uses a stack nor changes a stack pointer before `rti`.
* `.bss` is cleared before `rti` using registers other than `r0` (no stack, no memory store of `r0`); `r0` (the descriptor pointer) stays in its register until the C code is called with it. The stock bootloader instead saves `r0`…`r2` on the ROM's stack before `rti`, so the ROM's stack is usable in that context, but nothing requires using it. If the descriptor pointer is stored to memory, that location must not be inside a region cleared afterwards.
* Interrupts: neither the bootloader nor the stock bootloader enables interrupts; both execute `cli` only just before the XIP set-up (§3.6). Nothing in the boot path uses interrupts (SPI, timer and P33 are polled). The bootloader never returns to the ROM, and nothing it calls relies on the ROM's vector or interrupt state. The interrupt-enable state at entry and after `rti` is not established.

Initialized data already resides at its linked RAM addresses. Clear only bootloader-owned uninitialized storage, never all RAM or the retained area. Stack sizes/layout are design choices; a 1 KiB exception stack and 4 KiB main stack are known to work. The required effect is correct ABI execution, not copying the stock entry instructions.

### 4.2 Entering ROM USB download mode

The bootloader MUST write the 16 bytes `usb_update_mode` followed by NUL starting at `0x01C7FD80`, then request software reset (power-control bit 4; no pins are latched, §3.1). The string intentionally occupies more than the SDK's nominal eight-byte status reservation and overlaps the start of the update request. The ROM recognizes it and exposes its download-mode disk. This mechanism works for key and menu recovery on SMK-37 Pro; FM-1 use is described by existing firmware but not verified on FM-1 hardware. Do not replace the reset with a guessed direct ROM jump.

The ROM owns USB enumeration and bulk-only mass storage. Host-visible identity is USB VID/PID `4C4A:8057`, SCSI vendor `WL82`, product `UBOOT1.00`; reported USB descriptive string may be `WL80UBOOT1.00`. Normal power cycling exits. A key-triggered entry does not initialize the screen and gives no deliberate LED confirmation. Menu-triggered entry leaves a visible download-mode screen that the panel retains through reset.

### 4.3 Recovery independent of a valid boot image

The USB_KEY dongle can force the mask ROM into download mode even when the bootloader itself is invalid. It sends `0x16EF`, 16 bits MSB first, with USB D+ as clock and D− sampled on the rising edge. A Raspberry Pi Pico implementation uses open-drain lines, nominal 10 μs half periods (50 kHz), 150 μs gaps, and detects acknowledgement as both lines held low for roughly 1–2 ms. It requires seeing the released lines high after power-on before accepting low as acknowledgement; an unpowered target also clamps them low. After acknowledgement, release both signals and move the cable to the PC.

Known working wiring: Pico GP2 → D+, GP3 → D−, common ground, 2.2 kΩ pull-ups from 3.3 V to each signal, no breakout VBUS connection; the SMK runs on its battery. This recovery procedure works on SMK-37 Pro. The electrical timing tolerance is not established. A broken bootloader cannot provide its own key; the dongle remains the way back during testing.

## 5. Flash formats, encryption and layout

### 5.1 Checksum and cipher algorithms

All format CRC16s below are **CRC16/XMODEM**: width 16, polynomial `0x1021`, initial zero, no input/output reflection, no final XOR, check value for ASCII `123456789` is `0x31C3`. Streaming continuation carries the previous 16-bit remainder. CRC32 is IEEE reflected, reversed polynomial `0xEDB88320`, initial `0xFFFFFFFF`, final XOR `0xFFFFFFFF`, check value `0xCBF43926`. These are checksums, not authentication.

The ENC transform is its own inverse. Keep a 16-bit state initialized to the key. For each byte, XOR it with the state's low byte, then advance the state one bit left, retaining 16 bits, XORing `0x1021` if its previous top bit was set. There is **one state step per data byte**, not eight. Distinguish this from the eight steps per byte in CRC16.

Encryption boundaries MUST be respected. The boot area is not uniformly encrypted: encrypting the whole first 16 KiB is wrong.

| Object | Starting state / reset boundary |
|---|---|
| Flash header | ENC `0xFFFF`, restart for its 32 bytes. |
| Each top-level directory record | ENC `0xFFFF`, restart independently per 32-byte record. |
| Boot container header | ENC `0xFFFF`, restart for 16 bytes. |
| Boot container payload | ENC `0xFFFF`, restart for the whole payload, with no 32-byte resets. |
| Configuration blob | **Plain bytes** on flash, including its encoded chip-key prefix. Do not apply ENC to the blob. |
| Application area | For each 32-byte aligned relative offset `q`, ENC state = `unit key XOR ((q >> 2) modulo 65536)`, restart every 32 bytes. Offset is relative to the selected app area, not absolute flash. |
| Staged update stream and flash update records | **Plain bytes**: outer and inner records, compression header, block length words and compressed data (§6.3), and update request records found by the flash scan (§6.2). No key, no address-dependent seed, no 32-byte restarts. |
| VM, BTIF, calibration, presets and resource regions | Treat as raw bytes unless their own firmware format is explicitly being interpreted. The bootloader's wipe/guard and the flash loader need no generic decryption here. |

For an unaligned application read, expand to surrounding 32-byte blocks, decrypt each with its own aligned seed, then select the requested bytes. The offset-dependent seed repeats every `0x40000` bytes; the data stream still restarts each line. The SFC hardware implements the matching XIP behavior.

USB loader upload and chip-key replies use a **different** CRC-based stream transform. Start a CRC16 accumulator at the low half of a 32-bit key; feed the high half as two little-endian bytes using CRC16 continuation. For each byte index i, feed one byte from the cyclic constant sequence below into that CRC, then XOR the data byte with the remainder's low byte. For this protocol the key is always `0xFFFFFFFF`. Reset for each uploaded block, not once for the entire loader. The 16 entropy bytes are: `C3 CF C0 E8 CE D2 B0 AE C4 E3 A3 AC D3 F1 C1 D6`. This algorithm and constant are interoperability data.

Test vectors, input 32 zero bytes:

| Transform/key | Output hex |
|---|---|
| ENC `FFFF` | `ffdf9f1f1f3e7cf8f0c1a367cebd5b970f1e3c78d183274e9c1913264c983060` |
| ENC `980F` | `0f3f7efcd9b264c8b162c48810012346ad5a950b162c79d3a66dfbf6ecf9d3a6` |
| USB CRC stream `FFFFFFFF` | `2fa481cc4ec15deaeea2b6a6298ad4f70384ebe1cf8a8a14bde6dd1cd0f8463b` |

### 5.2 Flash header and records

The decrypted flash header is 32 bytes, little endian:

| Offset | Size | Meaning |
|---:|---:|---|
| 0 | 2 | Header CRC16 over bytes 2…31. |
| 2 | 2 | Burner-size field; preserve, not interpreted for application boot. |
| 4 | 4 | Version/vendor ID bytes. |
| 8 | 4 | Filesystem flash-size field. Stock images: `0xFF000`. |
| 12 | 1 | Filesystem version; stock decoded value `0x10`. |
| 13 | 1 | Block alignment in 256-byte units; stock value 16. |
| 14 | 1 | Reserved. |
| 15 | 1 | Special options; preserve. |
| 16 | 16 | Product ID bytes. |

For validation, decrypt **all 32 bytes** and compute CRC. Raw stored version/product bytes can look like plain text (`0.01`, `AC791N_STORY` plus FF padding) even though after ENC they are not text. This packager convention is real; do not reject the valid decoded header for unreadable ID strings, and do not substitute raw ID fields into a CRC-checked decoded copy. In the stock FM-1 and SMK-37 images and in the Snoop37 and Baud Girl images, the decoded header CRC is `0xDCA1` and the burner field `0x0220`.

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

Top-level addresses are filesystem-base-relative; here that base is zero. Top-level records are decoded with fixed ENC key. The bootloader MUST check record CRC before using its address/length and bound traversal before data or chip end (bounded traversal is policy); unknown general attribute semantics MUST NOT be inferred just from their bit patterns.

The common top-level layout is:

| Physical location | Record/object | Address field | Length | Attribute |
|---:|---|---:|---:|---:|
| `0x00000` | Flash header | — | 32 | — |
| `0x00020` | `uboot.boot` | `0x000A0` | stock `0x3830` | `00` |
| `0x00040` | `isd_config.ini` | `0x038D0` | `0x2BB` = 699 | `02` |
| `0x00060` | `app_dir_head` | `0x04000` | `FFFFFFFF` | `81` |
| `0x00080` | `key_mac` | `0xFF000` | `0x1000` | `12` |
| `0x000A0` | Boot container | — | up to `0x3830` | — |
| `0x038D0` | Plain configuration blob | — | 699 in stock images | — |
| `0x04000` | Encrypted application area | — | firmware dependent | — |

The boot areas of the stock FM-1 and SMK-37 images and of the Snoop37 and Baud Girl images are byte-identical. The stock boot container's 16 plain header bytes are `01 00 20 38 00 20 C0 01 10 00 00 00 A2 81 F5 BC`; its payload is `0x3820` bytes, payload CRC `0x81A2`, container CRC `0x71C8`. These are format reference values, not payload bytes to incorporate in the bootloader.

### 5.3 Configuration blob

At blob offsets 0…31 is an encoded chip-key record; +32 is its little-endian CRC16. The bootloader ignores this prefix for runtime key selection. PC tools decode it as follows: sum the first sixteen bytes modulo 256; clamp sums ≥`0xE0` to `0xAA`, sums ≤`0x10` to `0x55`. For each bit i=0…15, set that key bit iff `(byte[16+i] XOR byte[15−i])` is less than the adjusted sum. The stock prefix decodes to `0x980F`, CRC `0x1C1D`. This is format documentation, not permission to use this record in place of physical efuse.

Starting at +34, each record consists of one **value length byte**, a NUL-terminated ASCII key name, then exactly that many raw value bytes. The length excludes key and NUL. A length byte 0 or FF ends records; exact exhaustion of the descriptor length after a complete record is also valid. The stock 699-byte blobs contain 39 records ending exactly at byte 699, with no terminator byte. Numeric values are little-endian four-byte integers. Strings need not contain a trailing NUL within their value. The bootloader MUST parse within the descriptor length, reject missing NUL/truncated values/oversized destinations, and terminate its own string copies (parser bounds are policy). A blob larger than 4096 bytes is unsupported; recover safely. Unknown keys MAY be ignored. The configuration file entry's data CRC, where checked, covers these plain blob bytes.

The keys `PORT_INPUT`, `PORT_OUTPUT`, `ERR_PORT_OUTPUT`, `BOOT_DELAY`, `UTRX`, `UTTX`, `DVDD`, `VDDIO` and `DCDC14` are **not supported** by the bootloader: the devices' configuration never contains them. The bootloader SHOULD NOT contain code for them. If one appears in a configuration, it is ignored (no pins driven, no latching before reset, no delay); it MUST NOT make the configuration invalid. Their stock meaning is listed below for reference.

Runtime keys and stock values:

| Key | Value / interpretation | Required handling |
|---|---|---|
| `SPI` | Seven plain bytes `4_1_0_0`; width_divider_mode_port | MUST support quad, divider 1, output read. Mode digit 0→output, 1→I/O, 2→continuous I/O. Fourth digit is not used on WL82. |
| `OSC` | Four bytes `OSC0` | 24 MHz board oscillator; see oscillator-tag caveat below. |
| `OSC_FREQ` | 24,000,000 | MUST establish correct timing. |
| `OSC_HC_EN` | 0 | Optional oscillator drive setting; unused in 24 MHz path. |
| `OSC_1PIN_EN` | 0 | Stored in handoff metadata; no identified hardware effect. |
| `SYS_CLK` | 360,000,000 | MUST support this system frequency. |
| `HSB_DIV`, `LSB_DIV` | 1, 2 | Encoded divisor minus one. |
| `RESET` | Nine bytes `PB01_08_0` | Pin PB1, configured hold time 8 seconds, active level 0. Parsed and reported in updater metadata (§6.4). No long-press reset controller programming from this key is established. |
| `BOOT_DELAY` | Absent | Not supported; ignored. Stock: optional startup delay, nominally milliseconds, uncalibrated before clock setup. |
| `PORT_INPUT` | Absent | Not supported; ignored. Stock: optional `PXnn_ud`: pull-up/down flags u/d. Direction input. |
| `PORT_OUTPUT` | Absent | Not supported; ignored. Stock: optional `PXnn_v`, or H/L followed by two-hex-digit `(port×16 + pin)` codes, with subsequent H/L groups allowed. Also used for reset latching (§3.1). |
| `ERR_PORT_OUTPUT` | Absent | Not supported; ignored. Stock: optional `PXnn_LTd`: output level L, toggle flag T, hex half-period d in 50 ms units (zero means 50 ms); the stock parser drives the pin low when parsing. Belongs to the excluded fatal-memory-error feature. |
| `UTRX` | Absent | Not supported; ignored. Stock: optional updater metadata RX pin string. |
| `DVDD`, `DCDC14`, `VDDIO` | Absent | Not supported; ignored. Stock: optional voltage selectors of §3.1; zero treated as unset. |
| `ENABLE_SDRAM` | 0 | MUST remain no external RAM. Nonzero unsupported on these boards. |
| `SDRAM_*` | Present but unused | Ignore because external RAM is excluded; do not initialize nonexistent RAM. |
| `UTBD`, `VLVD`, `UPDATE_JUMP` | 1,000,000; 4; 0 | Present but not queried by the stock non-debug boot flow. They impose no UART/brownout/jump behavior. |
| `UTTX` | Absent | Not supported; ignored. Not read by the stock non-debug flow; TX metadata stays empty. |

For an otherwise readable configuration with a missing SPI record, the stock flow chooses dual width, divider 3 and output-read mode. This fallback does not prove that arbitrary missing clock records are safe; the whole chosen recovery profile must be validated.

GPIO parsing (for `RESET`) MUST accept only actual A–H pins 0…15. Some legacy parser aliases for charge/USB/RTC pins use another chip family's numbering and can address unintended WL82 pins; they are not required. Validate pin numbers, lengths, numeric widths and supported clock values before applying them (these restrictions are policy).

**Oscillator-tag caveat:** the stock bootloader reuses an unterminated SPI string buffer when reading OSC; with the stock bytes, the comparison sees residual characters and leaves the oscillator tag at 0 rather than the intended OSC0 tag 1. This is not a requirement to reproduce unsafe parsing. The 24 MHz PLL path still works. The bootloader MUST parse safely, but its effects MUST match what the units have always run: clock-control-0 bits 5:4 untouched (§3.2) and oscillator tag **0** in the updater metadata (§6.4), the value every tested official update has seen. Reporting tag 1 instead has not been verified on hardware and needs an official-update hardware test. Numeric tags in the legacy contract are RTC=0, OSC0=1, OSC1=2, HTC=3, PLL-derived 12 MHz=4.

Recovery MUST remain usable if the app, application table or config is damaged. The stock “ignore oversized config and use defaults” path does not establish safe complete clock defaults: several fields start zero. Policy: use a specifically validated recovery clock/pin configuration, or test recovery keys while still using a known ROM clock, before acting on malformed configuration. No requirement mandates reproducing divide-by-zero or invalid PLL programming on broken config. The ROM clock at bootloader entry has not been fully measured (see §3.2 for how slow it is).

### 5.4 Application directory and bank selection

Find the top-level `app_dir_head` by bounded name lookup. For a supported single-bank image its length is zero or `FFFFFFFF`, and its address selects the encrypted app area. The area's first decrypted record has attribute `83`, name `app_area_head`, **address is the CPU entry address**, and length describes the area extent. Subsequent `82` records are CRC-bearing files (`app.bin`, `cfg_tool.bin`), with addresses **relative to the selected area**. `12` / `92` region descriptors (`VM`, `PRCT`, `BTIF`, `USR`, etc.) use **absolute physical flash offsets** and do not carry file-data CRCs. Do not add the area base to them.

Stock and known third-party tables occupy at most nine 32-byte records (area header plus eight children) before code at relative `0x120`. The bootloader MUST validate all CRC-bearing application files, including `cfg_tool.bin`, over their **plain/decrypted** data, and validate overflow-safe bounds against physical capacity minus selected area base. The area header's own data CRC is not the per-file acceptance check. Require a nonempty `app.bin` and a plausible XIP entry inside its supported mapped area; an absent/invalid directory must not accidentally count as an intact application. Boot-anyway may bypass file-data CRC refusal, not invalid pointers or out-of-bounds reads. (The empty-file, bounds and entry checks are policy.)

File-table traversal terminates at the first bad record CRC, with limits of eight children for the bootloader and sixteen records for host inspection. Do not read instructions at +`0x120` as an unbounded directory. Index/end values in known region tables vary (1 at the last region), whereas the top-level key reservation has `FFFF`; CRC plus layout bounds is the established safe termination basis.

The bootloader SHOULD preserve legacy dual-bank selection; a build advertising this support MUST implement the following contract. A build omitting it must list that limitation rather than silently selecting a random bank. If the primary directory length is neither zero nor `FFFFFFFF`, it is interpreted as the location of an application-info record. If a top-level `app_dir_head2` exists, its length similarly addresses its bank info. These info records are read with default fixed-key metadata decoding. Each is 32 bytes, little endian:

| Offset | Size | Meaning |
|---:|---:|---|
| 0 | 2 | CRC16 over bytes 2…31. |
| 2 | 2 | Data CRC field, not the bank selection criterion. |
| 4 | 2 | Encoded-data CRC field, not the selection criterion. |
| 6 | 2 | Unsigned version. |
| 8 | 4 | Length. |
| 12 | 4 | Reserved. |
| 16 | 16 | Name. |

If primary info is valid, use secondary only if its info is valid and its unsigned version is strictly greater. Ties use primary; this is an ordinary comparison, not a wrap-aware version counter. If primary info is invalid and secondary exists, the stock flow selects secondary even without validating its info first, then validates its first decrypted app record. The bootloader SHOULD require valid selected app metadata regardless. With no secondary, retain primary and validate it. The bank info's length field interpretation and the update lifecycle beyond this selection are not verified on hardware, and no dual-bank image has been tested. There is no automatic switch to an older bank merely because a selected bank's later **data** CRC fails.

Before parsing top-level metadata set both hardware ENC keys to `FFFF`. After bank selection set both to the selected unit key and decode the first app record; the normal SPI path is then returned to raw mode. Alternatively software decrypt all reads with the same seeds. The bootloader's SFC base is filesystem base plus selected directory address.

### 5.5 Device data regions

These are reference layouts, half-open ranges; the bootloader MUST derive wipe targets from the installed table, not blindly use this table. The bootloader MUST protect the fixed ranges even when a corrupt/foreign table disagrees.

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

PRCT describes a protection range starting at physical zero; it is not another file to checksum or erase. Unlisted gaps are not necessarily disposable: third-party firmware may use them. FM-1 Felucca/SLOOP can put projects/samples within declared VM while also keeping settings or sample data elsewhere. The bootloader's “wipe user data” is exactly a VM/BTIF wipe, not a promise to remove every third-party setting or sample.

SMK-37's first `0x22C` calibration bytes start at `0xC1000`, with eight-byte marker `trimdat` plus NUL at +`0x224`. Unit wheel values also reside in the presets header around physical `0xC2060` and `0xC2080` and in presets themselves. Those values are unit-specific; never copy one unit's calibration into another unit or into a distributed image. Stock firmware has been observed to reset presets and erase USRTRIM/BTIF when USRFLASH is blank, and default presets do not recreate wheel calibration. Keeping USRTRIM during the wipe alone is insufficient to guarantee that the app will keep it later.

### 5.6 Package format context

The bootloader and the flash loader do **not** need a `.fwsc` parser. M-Flasher parses packages and sends raw bytes to the flash loader. This context explains what must remain compatible and supplies test extraction rules.

A package is a UFW-like image with identity bytes inserted in an initial prefix of 48-byte blocks: byte 47 of prefix block i carries `(identity character + i + 1) modulo 256`, followed by `0x7D` sentinel bytes after the identity. The host removes the 48th byte from at least the first twenty blocks, continuing while the next block's marker is `7D`; it concatenates their 47 retained bytes and appends the untouched remainder. Identity is decoded from prefix markers until first `7D`. The FM-1 package identity is `FM-1_015` with 20 prefix blocks; the SMK identity is `SMK-37 Pro_016` with 36. Do not assume the insertion applies throughout the complete file.

The resulting image's first 64 bytes are ENC FFFF as a single object. Its LE fields include CRC16 at +0 over decoded bytes 2…63, file-list CRC at +2, total image size at +4, count u16 at +8. File list starts +64 and consists of 80-byte records, each independently ENC FFFF. The list checksum is over **stored/encrypted list bytes**, not decrypted entries. Each entry has type u16 +0, index u16 +2, member CRC u16 +4, offset u32 +8, size u32 +12, name[16] +64. Offsets address the image after removing marker bytes. Other fields are opaque.

Type-0 `flash.bin` is a raw flash image starting at zero, already encrypted as required, with member CRC over those raw bytes. The FM-1 package supplies `0x93000` bytes and the SMK package `0x9D000`. Other members include `ota.bin` type `0x64` and `USR` type `0x32`. CRCs for some non-flash members do not equal the raw stored-byte CRC; the encryption/checksum conventions for those members are not established and MUST NOT be generalized from `flash.bin`. M-Flasher's firmware install does not install the separate USR member. Package flash images do not include all unit-specific storage.

Reference values of the two official packages:

| Package | Identity | Stored file-list CRC | List entries | `flash.bin` bytes / raw member CRC | `ota.bin` bytes / CRC |
|---|---|---|---:|---|---|
| `fm1-015.fwsc` | `FM-1_015` | `13B0` | 8 | 602112 / `9B24` | 19969 / `A1C9` |
| `smk37-016.fwsc` | `SMK-37 Pro_016` | `60E8` | 8 | 643072 / `0FA7` | 19969 / `A1C9` |

## 6. Retained RAM and executable handoffs

All structures in this section are byte layouts with little-endian integers and 32-bit pointers. They do not require any particular C type names. Use explicit layout assertions; do not rely on host-compiler bitfield packing.

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

Do not infer that gaps are free for arbitrary use. The bootloader MUST avoid clearing retained request/status before examining them, and MUST populate stable copies before calling the app. The +80/+88 offsets mentioned by newer SDK applications are relative to the application boot-info pointer at R+136, hence R+216/R+224, not relative to R itself.

### 6.2 Update request format and interpretation

| Request offset | Size | Meaning |
|---:|---:|---|
| 0 | 2 | CRC16 over bytes 2…79. |
| 2 | 2 | Update type; zero is invalid. |
| 4 | 2 | Update result. |
| 6 | 2 | Flash-record magic `0x5441`; not required by the RAM validity predicate. |
| 8 | 32 | File/path or descriptive name. |
| 40 | 32 | Opaque private arguments. |
| 72 | 4 | Absolute flash address of staged updater's outer file record. |
| 76 | 2 | Extension-argument length. |
| 78 | 2 | Extension-argument CRC. |

A RAM request is valid when computed CRC is nonzero, matches +0, and type is nonzero. The bootloader MUST preserve all 80 bytes for interpreting and executing a valid request. No extra whitelist of types is established. SDK type series starts at `0x5A00` (USB), followed by SD0, SD1, PC, UART, Bluetooth, BLE app, SPP, dual-bank, BLE test, NOR, user NOR UFW, and network UFW; the real stock SMK flash record uses `0x5A0D`, which lies beyond the SDK's named enumeration. The CRC/nonzero validity rule MUST NOT reject it just because the SDK header lacks a name for it. Result series also starts `0x5A00` (none), then ready, success, parameter/device/key errors. These symbolic meanings are context; the staged-image boot path primarily tests nonzero type and CRC.

If the RAM request is valid, the bootloader MUST use its private-argument address (the **original retained** request +40, not a pointer into an overwritten staging buffer) in the updater's private/memory-info pointer field. It remains an opaque 32-byte request-specific region; do not assume it always contains the complete 60-byte default memory-info object below.

With no valid RAM request and a successfully mounted application, do not scan flash for stale update records during normal boot. With failed mount, scan raw flash downward: step = header alignment ×256 if alignment is 1…16, otherwise 4096. Candidate boundary starts `capacity − step`, continues while boundary >4096, decreases by step; candidate's 80 bytes begin `boundary − 256`. Accept the first magic `5441` and valid CRC record, then apply the RAM-style nonzero CRC/type validity rule. This exact placement excludes the last sector's own tail for step 4096. Do not “simplify” it to a different end-of-sector scan. The flash scan reads plain bytes (§5.1). A menu status may count all matching records regardless of mount success.

Before invoking a staged updater, clear the halfword at **R+2**, which is boot status storage, not request+2. The stock flow falls through toward app boot without cleaning up if the updater fails or returns. The bootloader instead, after a staged updater returns or is refused:

1. Before anything else is decided, reinitialises the SPI0 flash path from scratch (the updater may have changed it and the flash contents), reads the flash header again, re-mounts and re-checks the application files.
2. Makes the attempted request ineligible: for a RAM request, it writes 0 to the request's CRC halfword (R+8) after the attempt; a flash-scan request is never acted on while the application mounts, so it needs no change. A request that was refused (bad record, header, CRC or bounds) is treated the same way. No request may be attempted twice in one power cycle.
3. Then continues as §7: boot the application if it checks out, otherwise the recovery menu with the reason (updater refused / updater returned).

Invalid request/address/compression MUST never cause an unchecked jump.

`UARTUPDATE` and `UART_UPDATE_CUSTOM` boot-status requests are unsupported. Both are plain ASCII strings starting at R, NUL-terminated, recognised by exact string equality including the terminating NUL: `UARTUPDATE` is the 11 bytes `55 41 52 54 55 50 44 41 54 45 00` at R…R+10; `UART_UPDATE_CUSTOM` is the 19 bytes of the string and its NUL at R…R+18. Any other content at those addresses is not a UART request. Both extend past the nominal 8-byte status field into the update-request area; a UART request is only examined when no valid update request was found, so that overlap does not matter. They are examined after the menu (if requested) and after the update-request search, and only if the search found nothing **and** the application mounted (with no mount and no request the boot goes to recovery first, §7 step 9); a valid RAM update request is acted on instead. On a match, write zero to the four bytes R…R+3 (this includes the halfword at R+2) and soft-reset. The rest of the string stays in RAM; with its first four bytes zeroed it no longer matches, so the next boot proceeds normally. Nothing else is cleared. The bootloader MUST NOT enter a perpetual updater loop for these requests. It MAY explain the unsupported mode in recovery before resetting/clearing it.

### 6.3 Staged updater format and LZ4

Every part of the staged stream (outer record, inner record, compression header, block length words, compressed bytes) is stored **plain**: no ENC, no key, no address-dependent seed and no 32-byte restarts, whatever the unit key, whether or not the application mounted, and whether the request came from RAM or from the flash scan. The request record's absolute address field (+72) is read from the request itself (RAM, or the plain flash copy); nothing else selects the representation. The stock bootloader has one defect here that is not to be reproduced: if the flash header fails to decode but a valid RAM request is present, it reads the stream through a still-enabled decryptor (ENC `FFFF`). The bootloader always reads the stream plain.

At the request's absolute updater address, read an ordinary 32-byte outer file record. Its address field is an **absolute** flash address for reads; its length bounds the staged stream. In the stock SMK-37 Pro example the address points to the beginning of that same record and length includes it. The first 32 bytes of the stream are an inner/file record which the boot path skips. At stream +32 is this compression header:

| Offset within compression header | Size | Meaning |
|---:|---:|---|
| 0 | 2 | CRC16 of bytes 2…31. |
| 2 | 2 | CRC16 of complete decompressed updater. |
| 4 | 4 | Decompressed byte count. |
| 8 | 4 | RAM load and entry address. |
| 12 | 4 | Argument field in file, **replaced** at execution with absolute stream end. |
| 16 | 16 | Descriptive name. |

Blocks begin at stream +64. Each is `[compressed length u32 LE][output length u32 LE][raw LZ4 bytes]`. Each compressed block must fit a 4096-byte input staging limit; output length must be nonzero. The decoder carries prior contiguous output as LZ4 history, so treating every block as an independent LZ4 stream is wrong. There is no standard LZ4 frame header or per-block frame checksum. The final produced byte count and CRC MUST match the compression header before execution.

Decoder contract: each token's upper nibble is literal length, with extended bytes adding until one is <255 when the nibble is 15. Copy those literal bytes. If input remains, read a nonzero little-endian 16-bit backward distance, then match length is low nibble +4, extending similarly when the nibble is 15. Copy the match with overlap semantics from already produced output, including preceding blocks. Retain up to 64 KiB history, or use the still-contiguous complete output. Bound all reads, writes, length additions and backreferences. Last input is a literal tail; reject malformed/truncated blocks. No vendor-specific decoder over-copy behavior is required.

The stock bootloader does **not** check the outer record's CRC, and skips the inner record without checking it; it checks only the stream length (nonzero), the compression header's CRC, each block's lengths, the total length and the output CRC. Policy, beyond the stock checks: validate the outer record's CRC (stock staged records pass it) and the header bounds, stream length at least 64, each `8 + compressed length` within remaining stream, each output within the declared total and a safe RAM range, executable address alignment/range, and no overlap with the bootloader's live workspace, retained structures or ROM runtime reservations. A malformed “negative” u32 length is simply huge and MUST be rejected unsigned. The supported stock updater destination is `0x01C0A800`; other destinations require explicit memory-allocation validation rather than arbitrary writes. Exact ROM RAM reservations for arbitrary updater destinations are not established.

A stock SMK-37 Pro flash image has a valid request at `0xBEF00`, type `5A0D`, result `5A01`, magic `5441`, name `ota-SMK-37 Pro_013`, pointing at `0xB9000`. Its staged stream length is 19,969 bytes. Compression header: header CRC `569C`, output CRC `B295`, output size 23,324 (`0x5B1C`), load address `0x01C0A800`. Six `(compressed,output)` lengths are `(3213,4096)`, `(3544,4096)`, `(3575,4096)`, `(3616,4096)`, `(3467,4096)`, `(2442,2844)`; they consume the stream exactly and decode to CRC `B295`. Both official packages (§5.6) contain the same updater payload with their package-specific outer wrapping.

After successful verification, call the updater with `r0` = pointer to the 24-byte updater context below and `r1` = **absolute file data address + file length**. The updater depends on the bootloader's context objects remaining alive below its load address. It is a normal call, not the application handoff or a reset. Watchdog is off, flash access is in the SPI/raw stage; no prior application XIP transfer is required. An official SMK-37 Pro update has run this way on hardware. Further updater assumptions about every CPU register are not established; preserve the target ABI and supplied context. After the updater returns or is refused, continue as specified in §6.2.

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
| 0 | 4 | Packed flags: bits 7:0 reserved zero; bits 11:8 oscillator tag (0, §5.3); bit 12 high-current flag; bit 13 one-pin flag; bits 17:14 LSB divider encoding; remaining bits zero. |
| 4 | 4 | Oscillator frequency Hz. |
| 8 | 4 | System frequency Hz. |
| 12 | 8 | TX pin name, bounded NUL string; empty. |
| 20 | 8 | RX pin name; empty. |
| 28 | 4 | UART baud metadata; zero in the non-debug flow despite the `UTBD` record. |
| 32 | 8 | Configured output pin string; empty. |
| 40 | 8 | Configured input pin string; empty. |
| 48 | 1 | Encoded reset pin, port×16+pin. Stock PB1 = 17. |
| 49 | 1 | Reset active level. |
| 50 | 1 | Reset hold time number. |
| 51 | 1 | Reserved zero. |

The pin strings are empty because the bootloader does not support the pin keys (§5.3), as with the devices' own configuration, which contains none of them. The `RESET` key is parsed and reported at +48…+50.

The default memory/power object occupies 60 bytes. For no external RAM, keep unused fields zero, but preserve these placements: +8 byte external-RAM enable =0; +12 low nibble HSB-divider encoding; +20/24/28/32/36 external-memory size/test/config/refresh data =0; +40/+44 timing words =0; +48 packed external-memory mode/timing flags =0; +52 bits 15:0 external delays =0, bits 19:16 core voltage selector, bits 23:20 I/O voltage selector, bits 27:24 domain voltage selector, high nibble zero; +56 halfword optional encoded error output; +58 padding zero. +0/+4/+16 and +9…11 are reserved zero. The voltage selectors reflect parsed configuration values, not the codes actually applied; because the voltage keys are not supported, they are 0 (zero means hardware default/unset). The exact bitfield layout is derived from the target layout and stock instructions.

Optional error-output halfword (stock `ERR_PORT_OUTPUT`, not supported by the bootloader): pin in bits 3:0, port in 7:4, level in bit 8, toggle in bit 9, half-period digit in 13:10, valid bit 15. External-RAM-specific fields are defined only as reserved zero here because their hardware is excluded; a memory controller set-up must not be derived from them.

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

This **+4 physical / +8 logical** ordering is established by SDK `boot.h` and the stock bootloader's stores; the reverse order is wrong. The SDK 1.2.x extra locations at +80 and +88 relative to this pointer MUST be initialized to zero as §6.1, although the stock bootloader leaves them unspecified.

Before the call, establish the required clocks/voltages and XIP state (§3), copy the decoded flash header and app object to their fixed locations, finish all flash DMA, validate the app files unless a one-shot bypass is authorized, disable interrupts, and pass the object pointer in `r0`. Call the area-header entry (normally `0x02000120`). The application runs from flash and initializes its own runtime; no loading of the entire app into internal RAM is required. The bootloader's stacks must still be valid at the call. No requirement establishes zero contents of other general registers or bootloader-owned RAM as an app ABI. The stock bootloader restores an optional configured TX pin to plain input with digital input enabled and no pulls before app entry; it is empty on these devices. Restore board key/LED temporary settings before handoff.

If the application returns, the boot has failed and MUST enter the recovery menu; it must not fall through into unrelated RAM or repeat a stale update indefinitely. An app that hangs without returning cannot be diagnosed automatically by the bootloader's CRC check: the recovery key or USB_KEY is still necessary.

## 7. Bootloader: required boot decisions and failure behavior

The following decision order is externally significant; internal organization is free.

1. Establish safe entry execution, retained-state access and watchdog policy. Consume any one-shot boot-anyway marker before it can survive another failure. Initialize bootloader-owned storage. Preserve the ROM descriptor.
2. Obtain and validate configuration, prepare voltages/pins/clocks, and test board recovery **before any application filesystem mounting, CRC scan, update scan, calibration write or display initialization** can stall boot. The bootloader MAY make recovery even earlier using validated ROM timing; until the clock switch the CPU is very slow and waits MUST stay within the limits of §3.2. Malformed configuration must not prevent the recovery key (§5.3 policy).
3. A continuously held recovery key for approximately one second requests ROM download mode. Recovery takes priority over the menu when both keys are held. A key released early boots normally. Restore temporary scan pins if the key check ends without reset.
4. Test the menu key for approximately one second. Record the request; mount/probe flash before opening the menu because status/actions need flash access. Do not steal OCT− / OCT+ firmware shortcuts.
5. Establish safe initial flash reads (the stock flow uses dual-output mode), discover JEDEC/capacity and MAC, read physical key and possible fallback, then mount selected app directory with configured bus mode. When width is the legacy probe value 3, try quad before dual and, within each, continuous I/O, I/O, output read. Optional modes must be validated; do not claim a successful mount on invalid CRC. On failure clear the selected app base.
6. If the LSB flash source exceeds 66 MHz, the stock probe temporarily raises the SPI divider by one for mounting, then restores the configured divider for XIP. The 60 MHz LSB of the stock configuration does not need this. Preserve safe bus timing; the exact need outside the stock configuration is inferred.
7. Run the SMK calibration guard after SPI setup and before app/menu activity, subject to safe capacity/address checks. It may run even when application mounting failed; damaged application metadata must not give it arbitrary write addresses.
8. If menu was requested, show it. An accepted boot choice continues through normal update selection; it does not promise to ignore a valid pending updater. Recovery/download and wipe take their explicit actions.
9. Check retained update request first. If valid, forward its private bytes and attempt the verified staged updater. Otherwise, only if mounting failed, look for saved flash requests. If mounted with no request, proceed to app. With no mount and no request, go to recovery.
10. If no update request was found and the application mounted, handle retained UART requests (§6.2) without looping. If a staged updater fails/returns or is refused, reinitialise flash access, re-mount, make the request ineligible (§6.2), then attempt the supported app if it checks out, otherwise recovery. Record/show an intelligible reason for an invalid updater/address/CRC instead of executing it.
11. Establish XIP, check file data CRCs unless one-shot bypass applies, populate the fixed app ABI, and call the app.
12. No usable app, CRC failure, unsupported external-RAM entry or app return leads to the recovery menu. A display-less diagnostic build MAY enter ROM mode directly. Once an XIP boot has been attempted, later menu boot choices reset before retrying.

Normal successful boot MUST NOT erase or rewrite user firmware/settings. The SMK guard's first backup and recovery of invalid calibration are explicit exceptions. Normal boot MUST not require the recovery display to be initialized. The bootloader does not need to reproduce the stock bootloader's silent reset loops, unsafe parsers, stale scratch bytes, vestigial storage reads or unused timestamps.

## 8. Bootloader: boards, keys, indicators and panel

### 8.1 Board selection and image identity

The bootloader MUST have separate FM-1 and SMK-37 build targets. SMK-37 Pro and Elite share the same target; their hardware equivalence is inferred from shared firmware pin and peripheral behavior and has not been verified on Elite hardware. The bootloader MUST trust the installed board target for the recovery key rather than drive candidate boards' pins in an autodetection procedure. The installer checks model: an FM-1 key row is an analog input on SMK and an SMK output-enable pin is an FM-1 key row. Wrong-board installation can make recovery ineffective.

M-Flasher recognizes the following 48-byte identity at **payload offset `0x40`**, hence container offset `0x50`, loaded RAM `0x01C02040`:

| Offset within identity | Size | Required contents |
|---:|---:|---|
| 0 | 8 | ASCII `AC79BLID`, exactly eight bytes. |
| 8 | 8 | NUL-padded lowercase board tag: `fm1` or `smk37`. |
| 16 | 32 | ASCII version/build description, bounded and NUL-terminated. |

The entry path MUST branch around this data. Initial instructions before offset 40 hex MUST fit without overlapping it. Version text can be chosen freely and SHOULD identify the implementation distinctly. A valid BANKCB plus the tag lets the host display an open bootloader and reject the wrong board. Do not change the magic/tag/offset merely to rename the project.

Board identity is an installer decision, not a guarantee supplied by a flash chip model. M-Flasher's identification rules:

* Strong SMK evidence: `trimdat`+NUL at physical `0xC1224`, `TRIMCOPY` at `0xFF000`, or an installed valid tagged SMK bootloader. Tagged FM-1 is likewise strong FM-1 evidence.
* Weak layout evidence: declared USRTRIM at C1000 suggests SMK; VM at 93000 and USR at EA000 suggests FM-1. Weak JEDEC hints: `856014` FM-1 and `EB6014` SMK. Firmware can be changed and parts can vary.
* One strong model with no opposing weak evidence is considered certain. With no strong evidence, two agreeing weak signals and no competing model are considered certain. Conflicting strong evidence leaves it unresolved. The flash loader's display is enabled only after certain identification or explicit host board selection.

### 8.2 Keys and scan timing

| Function | FM-1 | SMK-37 Pro / Elite |
|---|---|---|
| Recovery at startup | REC, 1 s | SEQ REC, 1 s |
| Menu at startup | PLAY, 1 s | SEQ PLAY, 1 s |
| Move previous / next | OCT− / OCT+ | `<` / `>` above octave buttons |
| Select | PLAY | SEQ PLAY |
| Menu LEDs | OCT−, OCT+, PLAY | `<`, `>`, SEQ PLAY |

Firmware shortcuts that MUST pass through: OCT− + OCT+ calibration on Felucca/SLOOP/Snoop37; OCT− alone for SLOOP/Snoop37 USB rescue. SEQ PLAY/REC here are sequencer controls, not the octave controls.

Reference hold logic: poll every 20 ms for 1000 ms after an initially detected press. One missing sample is tolerated; two consecutive missing samples cancel. Menu presses become stable only after two equal polls and trigger on the newly pressed edge, with no automatic repeat. An implementation MAY improve debounce while preserving approximate hold duration, release behavior and priority. Save and restore every modified GPIO/mux/PHY state needed by the application; deselect scan columns on exit.

**FM-1 scan facts:** two cascaded shift registers, data PA4, clock PA3, latch PA1. Shift sixteen bits MSB first, pulse clock high then low for each, latch high then low afterward. Idle/deselected word FFFF. Low selected-column bits. OCT− reads PA8 with word `F7FE` (bits 0/11 clear); OCT+ PA8 with `EFFD` (bits 1/12 clear); PLAY PA6 with `FBFF` (bit 10 clear); REC PA6 with `FF7F` (bit 7 clear). Enable digital input/pull-ups and remove pull-downs on PA6/PA8; closed key reads low. Allow 10 μs after changing column before sampling. Use GPIO read/modify/write edges with real bus access time; overly short edges have been observed to select an adjacent column. These FM-1 facts match the Felucca firmware; they are not verified on FM-1 hardware with the bootloader.

FM-1 indicator lines are PA9, PA10, PH6, PH9 for matrix rows PA5…PA8; high lights a selected-column LED. The menu needs PH9 for octave keys and PA10 for PLAY. Keep those lines dark while changing/reading columns. During waits cycle OCT− column, OCT+ column and PLAY column, one per millisecond, with only the relevant LED line high, giving approximately one-third duty. Restore saved state before app. This menu multiplexing is **not verified on FM-1 hardware**.

**SMK scan facts:** four shift registers, data PA4, clock PA3, latch PA1, output enable PA0 active low. A 32-bit logical word is sent **least significant byte first, MSB first within each byte** (same stream as the firmware's DMA of a little-endian word). Do not shift the whole 32-bit number MSB first. Initialize output-enable/clock/latch/data low and outputs enabled. Lower sixteen logical bits contain active-low LED lines except bits 5:3 used for analog mux addressing. Upper sixteen select columns low: columns 0…7 correspond to bits 24…31, columns 8…15 to bits 16…23. All-off/deselected word FFFFFFFF.

For the recovery/menu controls, sample PH8 with digital input and pull-up enabled, pull-down disabled, after 10 μs settling:

| SMK control | Selected logical word with LEDs off | Column word index | Firmware key-code context |
|---|---:|---:|---:|
| `<` | `FFFEFFFF` | 8 | `96` hex |
| `>` | `EFFFFFFF` | 4 | `56` hex |
| SEQ PLAY | `FEFFFFFF` | 0 | `16` hex |
| SEQ REC | `F7FFFFFF` | 3 | `46` hex |

Firmware codes name a column one scan tick later, so their code's column number is one greater than the logical selected column word index. Closed key is PH8 low. Other matrix row pins are PA2, PA5, PA6, PA7, PA9, PH6 and PH9, but the bootloader need not scan them. Recovery and menu keys work on SMK-37 Pro hardware.

### 8.3 SMK LED commons

The LED groups share the high-speed USB PHY's D+ and D− pads, separate from the ROM download USB handling assumptions. At `0x16A0C`, bits 0…3 and 16 enable drive (`0x1000F`), bit 17 is group-0 common and bit 18 group-1 common; **low** common turns a group on. Group 0 line 0 is `<`; group 1 line 0 is `>` and line 12 is green SEQ PLAY. Group 0 line 12 is KNOB BANK, so lighting both groups indiscriminately causes an unwanted LED.

During menu waits, alternate groups every millisecond. Turn both commons off before loading active-low line bits into the scan word, then activate only that group. Group 0 clears lower bit 0; group 1 clears lower bits 0 and 12. Preserve analog-mux bits. Save/restore the original PHY register before application entry; after reset into ROM mode the key LEDs are not expected to stay lit. Menu keys, LEDs and display work on SMK-37 Pro hardware; Elite behavior is inferred from shared firmware transactions and not verified on hardware.

### 8.4 Display wiring and transactions, shared by the bootloader and the flash loader

| Signal | FM-1 | SMK-37 |
|---|---|---|
| SPI1 group selector | Mux-control-1 bit 4 set (group B) | bit 4 clear (group A) |
| Clock | PC9 | PB6 |
| MOSI | PC10 | PB7 |
| CS, active low | PC7 | PC8 |
| Command/data (0 command, 1 data) | PC8 | PB5 |
| Backlight | PA2, active low | No controllable pin used |

Initialize CS high, command/data and clock/MOSI output levels low, corresponding pins as outputs. SPI1 control `0x4021`, baud 4. At the bootloader's LSB 60 MHz this is nominally 12 MHz; at the flash loader's LSB 48 MHz nominally 9.6 MHz (both frequencies inferred). Commands are one byte with command/data low; data bytes have it high. Hold CS through each command/data burst; toggle high at completion. Use completion bit 15 / clear bit 14. DMA sends pixel data from internal RAM. Do not begin zero-length DMA. Panel absence/failure MUST not block flash loader flash operations.

Panel address window is commands `2A` (x start/end), `2B` (y start/end), each with two big-endian 16-bit inclusive endpoints; `2C` starts memory write. Pixels are RGB565, high byte first, normal origin (0,0), no row offset. Full usable size is 240×240. The recovery menu and loader text interface assume 20 columns ×15 rows of 12×16-pixel cells. An implementation MAY use the 5 × 7 font of Appendix A (provided under MIT) or draw a different legible font in those cells. In the flash loader, lowercase MAY be rendered as uppercase, unsupported characters as blank; the host restricts its text to `A–Z`, digits, space, `- . / : < > + ?`.

Required panel initialization transactions, in order. Each command byte and its following data are hardware setup facts, not an encoded vendor table to reproduce as source structure. The SMK sequence has run on SMK-37 Pro hardware; the FM-1 sequence follows existing FM-1 firmware.

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

## 9. Bootloader: recovery menu, wiping and calibration guard

### 9.1 Menu status and controls

The menu MUST remain until the user chooses an action; it MUST show a reason when normal boot is impossible. Required content: device/bootloader title, build version, app state and `app.bin` byte length when available, retained update presence and flash-record count, JEDEC ID/capacity, available actions and board-specific movement/select hints. The precise wording, font shape and colors are free design choices except where compatible state names help comparison below. Do not claim app CRC validity proves runtime behavior.

Suggested geometry matching the 20×15 interface: row 0 title, 1 version, 3 app status, 4 length, 5 updates, 6 flash ID/size, 7 separator, 8…12 actions, 13 transient status/progress, 14 controls. Show checking progress before slow scans. Directory/file reads before XIP can be raw SPI plus software ENC and CRC.

Suggested wording and colors (any clear wording is acceptable; on a black background):

```
row 0   FM-1 M-BOOTLOADER | SMK-37 M-BOOTLOADER  yellow
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

The menu's erased-span heuristic examines the last 32 raw bytes before each 4 KiB boundary within a failed file, and the last 32 bytes of the file, ignoring a partial span shorter than 32. Thirty-two FF bytes at any sampled tail imply incompleteness. Encrypted data makes it unlikely for intact code to have such a span, but it is **not mathematically impossible** (this interpretation is probabilistic). Host inspection instead checks entire erased sectors and can classify a tail-only erase differently. The bootloader SHOULD use this heuristic; bootloader and host labels on such ambiguous corruption need not be identical.

Update status counts flash records with magic/CRC at the same scan positions as §6.2; the retained request gets its own indication. Counting stale records does not mean a normal intact-app boot would run them. The stock SMK record is inside VM and is removed by wiping that VM. Separate “clear update” and “wipe all” actions are **not required** and SHOULD NOT be added.

Required actions, in this order among actions present:

| Action | When present | Effect / confirmation |
|---|---|---|
| BOOT APP | App OK | Continue normal update/app decisions; no confirmation. |
| DOWNLOAD MODE | Always | Show USB/PC instructions and power-cycle exit instruction, then ROM-mode reset; no confirmation. |
| BOOT ANYWAY | BAD CRC or INCOMPLETE with otherwise safe executable metadata | Warn that firmware may crash or misbehave; fresh 3-second select hold, act only after release. Bypass file data CRC once. |
| WIPE USER DATA | At least one validated declared VM or BTIF range | Show exact ranges and that all other data is kept; fresh 3-second select hold, act only after release. Erase then restart. |

Menu navigation wraps at each end. Entry-held PLAY must be released before it can select an action. Confirmation requires a fresh select press held alone, shows progress, resets progress on early release and lets another key cancel. After successful hold, prompt for release before acting so a restart does not reinterpret held PLAY as another menu request. Refresh status after cancellation.

BOOT ANYWAY MUST be volatile, one boot only, never a flash write or permanent setting. From a menu reached after failed XIP, retain eight bytes `BOOTANY` plus NUL at R and reset; next startup recognizes and clears all eight before skipping the check. An implementation MAY use another private retained encoding only if it preserves one-shot consumption, does not conflict with ROM/update strings, and survives exactly the needed soft reset. A failed/returned app must not retain a bypass forever. From a power-on menu before XIP, continuing without reset is allowed after the release check.

### 9.2 Wipe safety requirements

Only validated `VM` and `BTIF` **region** entries with attributes `12` or `92`, positive lengths, 4 KiB-aligned start/length and overflow-safe bounds within the chip are eligible. Do not erase a similarly named ordinary file. Duplicate/conflicting descriptors SHOULD be rejected or safely unioned with an intelligible display. No valid declarations means no wipe action; do not use guessed stock addresses as fallback.

Policy: reject a proposed VM/BTIF range that overlaps any validated application file's physical allocation. A valid header CRC alone does not prove that a semantically contradictory region declaration is safe to erase. This check preserves all known layouts, including Baud Girl.

Per erase transaction, independently protect:

* `[0,0x4000)` for both boards: boot filesystem, bootloader and configuration.
* `[capacity−0x1000,capacity)` for both: MAC, flash efuse and guard backup.
* `[0xC1000,0xC2000)` on SMK, even if no USRTRIM descriptor exists.

Subtract protected sectors from eligible ranges. Use aligned 64 KiB erases only when a whole block lies within the eligible range **and** contains no protected sector; otherwise 4 KiB erases. Recheck protection in the actual erase loop, not just the UI. No program operation is necessary for a wipe. Wait completion, show progress/done, and reset. The bootloader SHOULD read back erased sectors or report hardware failure; a blind wait does not prove erase success. The bootloader MUST never erase the last sector just to create room for its guard.

Keep everything outside those declared regions, especially stock sounds/resources and SMK presets/wheel data. A wipe can erase third-party projects/samples residing inside VM; the user confirmation must not describe it as harmless metadata only. Baud Girl's VM overlaps the stock presets allocation, so while Baud Girl is installed that declared VM is eligible except fixed protected sectors. It is impossible to promise preservation of both arbitrary firmware-declared settings and all other firmware's historical layout simultaneously; a unit backup is the way to restore prior presets.

### 9.3 SMK calibration guard

The guard is absent for FM-1. On SMK, use fixed source record at `0xC1000`, length `0x22C`, valid iff record bytes +`0x224…0x22B` equal `trimdat` plus NUL. This marker check alone is not a calibration-content checksum. Backup slot is physical `capacity−0x1000` = FF000 for 1 MiB, with this little-endian header **before** the record:

| Slot offset | Size | Meaning |
|---:|---:|---|
| 0 | 8 | `TRIMCOPY`. |
| 8 | 4 | Original record address, `0xC1000`. |
| 12 | 2 | Record length, `0x022C`. |
| 14 | 2 | CRC16 of copied record. |
| 16 | `0x22C` | Copied calibration bytes. |

So the header is at FF000 and the record at FF010; the header is **written last**, not stored last. The occupied range ends at `0xFF23C`, well before MAC at FFF40 and efuse at FFF00. The host recognizes `TRIMCOPY` at slot start as SMK evidence.

Required behavior:

1. Check capacity and all fixed bounds. Read the candidate backup header/record raw. A valid copy requires magic, original address, exact length and data CRC all matching; an implementation SHOULD also require the copied calibration marker.
2. If there is no valid copy and **all 16+0x22C slot bytes are FF**, and the source record is marker-valid, write the copied record first and the header last, splitting page programs correctly. Do not erase the slot. This is the first-start backup.
3. If slot contains anything else, leave it untouched. Invalid/partial slot data must not be “repaired” by erasing unique unit data. In particular, power loss halfway through first copy may leave the slot permanently unusable without a separate maintenance tool.
4. If source is marker-valid, leave it alone even if it differs from backup: this permits deliberate recalibration. Never automatically overwrite the existing backup with the newer source.
5. If source marker is invalid and a valid copy exists, erase the C1000 4 KiB sector and restore only the copied 556 bytes. This is intentionally destructive of other damaged bytes in that sector; it is the known factory record allocation. Verify restored bytes before continuing and report failure. Repeated power-loss recovery must remain possible while the backup survives.

The copy and restoration behavior works on SMK-37 Pro hardware; the extra marker, readback and bounds requirements are policy.

The guard runs before stock firmware. If that firmware erases calibration during a later factory-reset path, restoration happens at the **next boot**, not instantaneously. This does not recover missing presets-header wheel values, sounds, calibration before the first backup, a damaged nonblank backup slot, or another unit's calibration. The bootloader MUST not present it as a full factory-data backup. Official M-VAVE updates can replace the bootloader with the stock bootloader, leaving the copy present but inactive until the bootloader is reinstalled.

## 10. Flash loader: ROM plug-in and exact host command protocol

### 10.1 ROM registration and lifecycle

The ROM accepts normal mass-storage SCSI commands plus its private memory upload/jump commands. No USB descriptors, endpoints or new enumeration are provided by the flash loader. Through ROM command `FB06`, the host uploads each block to RAM; `FB08` jumps to the flash loader with target argument 1 (SPI NOR). These remain ROM operations after the plug-in is installed.

At the **first byte of the linked flash loader image**, the ROM makes a normal C-ABI call with `r0` pointing to three words:

| Offset | Size | Meaning |
|---:|---:|---|
| 0 | 4 | Callback: send RAM buffer pointer and byte length to host, returns int. |
| 4 | 4 | Callback: receive host bytes into RAM buffer, same two arguments/return type. |
| 8 | 4 | Pointer to the ROM's command-handler pointer slot. |

The flash loader MUST save callback values, initialize all of its own runtime state and workspace, initialize SPI0, install its handler pointer in the supplied slot, and **return** to the ROM (returning zero is known to work). It MUST preserve the calling convention and ROM stack/control environment. It MUST NOT execute the bootloader's exception-exit sequence or globally clear RAM. No CRT or `.bss` clearing is provided by the image upload contract, so the flash loader MUST NOT depend on fresh RAM for any global. Reinstallation into a previously used slot must reinitialize state, including display enable, last error, CRC state and clock reporting.

The ROM calls the handler as `(31-byte USB command block pointer, writable 16-byte answer pointer)`. Return **1** if handled, **0** to defer to the ROM. The command CDB starts at command-block +15. Command-block +8…11 is the little-endian data transfer byte count; +12 contains USB direction. The ROM supplies bulk transport/status behavior. The flash loader's callback calls move the command's data; the handler return value is not a general SCSI error code. In particular, after consuming OUT data the flash loader MUST return handled even when refusing to write; returning 0 after receive can leave the ROM unable to complete the transaction.

The handler sends each 16-byte reply itself with the **send callback**, from the ROM-supplied answer buffer, `min(transfer count from command block +8…11, 16)` bytes, and returns 1. The ROM does not send the answer buffer by itself; a handler that only filled it and returned 1 would leave the host's data phase unanswered. For every command it handles, the flash loader first sets all 16 bytes of the answer buffer to zero, writes the opcode echo to bytes 0…1, fills the reply fields, and (for reply commands) passes that same buffer to the send callback. FD05 sends only the raw flash data (no answer); FB04 and FC44 receive their OUT data and send nothing; their answer buffer is zeroed and echoed but not sent. This scheme has been used by M-Flasher on SMK-37 Pro hardware for every listed command. Whether the ROM's answer buffer is needed at all (a private 16-byte buffer passed to send instead) is not verified; sending from the ROM's buffer is the known-good way.

For completeness, the standard command block has LE signature `0x43425355` at +0, LE transaction tag at +4, LE transfer count at +8, flags at +12 (bit 7 = IN), logical-unit byte at +13, CDB length at +14 (16 for private commands), CDB bytes at +15. Its 13-byte status block has LE signature `0x53425355` at +0, matching tag at +4, residue at +8, status at +12. The flash loader MUST leave construction of this status block to the ROM. For zero-length host data phases, do not emit/expect an extra zero-length USB packet: M-Flasher's direct USB host explicitly avoids it because the ROM could interpret it as the next command.

The send/receive callbacks' actual return values and partial-transfer/error semantics are not known. A known-working loader ignores the returns and makes callback calls of at most 512 bytes. The flash loader MUST retain this chunking and framing for valid host requests; robust callback error handling needs hardware investigation rather than an invented success convention. Do not let display failures or extended diagnostics change flash framing.

### 10.2 CDB and answer conventions

Every private host CDB is sixteen bytes. Bytes 0…1 are opcode big endian; arguments begin at +2; unused bytes are FF. Unless explicitly marked LE, all multibyte command arguments and reply numbers are **big endian**. Reply-based commands return sixteen data-IN bytes, with the opcode echoed at +0/+1 and unused bytes zero. The flash loader SHOULD honor shorter requested replies by sending at most `min(requested IN count,16)`; M-Flasher always asks for sixteen. Raw read has no opcode prefix; data-OUT commands have no sixteen-byte answer data phase. The ROM handles completion/status separately.

All flash addresses in this section are absolute physical byte offsets, all reads/checksums use raw stored bytes. `n16` is a BE u16 at CDB+6; `a32` is BE u32 at CDB+2. The flash loader MUST support these exact valid request/response contracts:

| Opcode | CDB fields | Data direction / exact useful reply bytes |
|---:|---|---|
| `FB00` | +2 `a32` | IN 16: echo, remaining zero. Erase aligned 64 KiB block and wait complete. |
| `FB01` | +2 `a32` | IN 16: echo, remaining zero. Erase 4 KiB sector and wait complete. |
| `FB04` | +2 address u32, +6 length u16, +8 zero, +9 checksum u16 **LE** | OUT exactly length bytes. Verify CRC16 of received raw data before any program; no reply data. Length at most 4096. |
| `FD05` | +2 address u32, +6 length u16 | IN exactly length raw bytes, no echo header. Must handle the host's 16,384-byte reads. |
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

`FC09`: take the efuse key as two little-endian bytes, apply the USB CRC stream transform with FFFFFFFF to those two bytes in that order. Put the transformed first/low byte in reply **+7**, second/high byte in **+6**; other bytes zero apart from echo. For key 980F the complete reply is `FC 09 00 00 00 00 3C 20 00 00 00 00 00 00 00 00`. The host reverses bytes 6/7 and applies the same transform to recover the LE key. The command argument is sent for compatibility; the flash loader does not use it to select another key.

`FC40` identity:

| Reply offset | Size | Meaning |
|---:|---:|---|
| 0 | 2 | FC40 echo. |
| 2 | 4 | ASCII `MFLR`. |
| 6 | 3 | ASCII version, three bytes; M-Flasher reads exactly three. |
| 9 | 1 | Zero padding. |
| 10 | 4 | This image's executable entry address, BE. |
| 14 | 1 | Features: bit 0 CRC32, bit 1 unique ID, bit 2 display, bit 3 status; all four implemented → `0F`. |
| 15 | 1 | Actual CPU MHz, normally 96 after successful safe acceleration, 48 in known ROM fallback. |

M-Flasher uses the identity prefix/version/address rather than feature bits to decide whether to call extensions. A loader claiming `MFLR` MUST implement the complete extension set used by this host; simply clearing a feature bit will not keep the host from calling it. Include a clear `MFLR` plus three-character version literal in the plain executable asset, because M-Flasher discovers its bundled version by scanning the binary for that marker. Ensure an earlier unrelated marker cannot give the wrong version.

Earlier M-Flasher loader builds report version `0.4`. Each flash loader release SHOULD use a distinct three-character version: M-Flasher reuses an already running loader solely because its version matches the bundled asset and cannot distinguish changed code with the same version.

For `FC0A` and ID EB6014, the full reply is `FC 0A 03 00 14 60 EB 00 00 00 00 00 00 00 00 00`. The `FC14` reply is `FC 14 00 00 10 00 00 00 00 00 00 00 00 00 00 00`. `FC40` for an example version `1.0` at 01C20000 and 96 MHz is `FC 40 4D 46 4C 52 31 2E 30 00 01 C2 00 00 0F 60`; the example version is not a required release number.

An example write of ASCII `123456789` to 0x4000 has CDB `FB 04 00 00 40 00 00 09 00 C3 31 FF FF FF FF FF` and nine data-OUT bytes. A 16 KiB read there has CDB `FD 05 00 00 40 00 40 00 FF FF FF FF FF FF FF FF`. A whole-flash CRC32 has `FC 41 00 00 00 00 00 10 00 00 FF FF FF FF FF FF`. These examples show the mixed checksum/argument endianness.

### 10.3 Programming, refusals and unsupported commands

The flash loader MUST receive the complete valid write command before modifying flash, check its CRC16 (seed zero) against the LE CDB checksum, clear block protection as needed, split at 256-byte page boundaries, issue WREN before each page, and wait WIP clear. It MUST NOT silently treat a failed CRC as data to program. On `n>4096`, drain the entire OUT payload into bounded scratch chunks without programming; record refusal 1. On CRC mismatch, record refusal 2 and write nothing. Return handled in both cases. Successful writes do not clear an older refusal; FC46 reads and clears it.

M-Flasher does not poll FC46 after each write, so valid framing and later raw CRC32/readback remain essential. A reported refusal is not a substitute for host verification. A refusal MUST NOT corrupt ROM state, overflow the receive buffer or leave unread bytes for the next command.

Policy: overflow-safe read/write/checksum bounds within detected supported capacity and erase alignment/extent validation are required. Do not wrap a 24-bit flash address or erase a block partly outside the chip. Reject mismatched CDB length versus USB transfer length/direction without programming; drain OUT data safely if it has been accepted. Implementations MAY extend last-status codes for bad ranges, timeout, forbidden command, invalid display parameters, etc., but MUST keep meanings 0/1/2 and document added codes. A refusal mechanism must respect ROM data-phase constraints; do not invent “return −1” as a working transport error.

Do not implement `FB02` chip erase, `FC12` efuse-key burn, arbitrary flash-command replacement or flash-selection changes. For forbidden destructive commands, ensure no operation is executed; returning unhandled is acceptable only if it is verified that the ROM itself will not implement the destructive action, which is not established. A defensive handled refusal is preferable where request framing is known. General unknown opcodes MUST remain available to the ROM when they are ROM commands, specifically memory write `FB06`, memory read `FD07` and jump `FB08`.

Unlike the bootloader's menu, the flash loader MUST allow host-authorized raw writes to boot, calibration and the final sector: same-unit backup restoration and bootloader installation need them. Board/factory-data policy is enforced by M-Flasher's operation planner (§11), not by a blanket sector ban in the flash loader. The flash loader's local safety refusals concern malformed/oversized/corrupt operations and unsupported irreversible features. Do not add a confirmation UI to the flash loader or make it depend on board identification to read flash.

FC44 before display initialization is a no-op after receiving its data. Invalid board selection MUST not touch candidate-board pins. The host uses only rows 0…14 and ≤20 bytes; policy is to validate the row, consume any excessive payload completely with bounded buffering, and avoid out-of-panel writes.

### 10.4 Clock, DMA and performance

The flash loader MUST preserve the ROM USB service's clocks, interrupts and stack. The acceleration applies only when `(clock-control-2 & FF)==80` and `(system-divider & 0x30700)==0`: ROM PLL 192 MHz /4 gives 48 MHz. First set divider bit 8 so LSB will divide HSB by two; synchronize the core; change clock-control-2 low byte to 40, preserving other bits; synchronize again. This changes CPU/HSB to 96 MHz while LSB remains 48 MHz, preserving SPI0/SPI1 timing. Do not apply the bootloader's 360 MHz PLL initialization in the flash loader.

If a prior flash loader already accelerated the chip, the same check will not match. The flash loader MUST recognize/report an established already-96 MHz state accurately or use a conservative documented unknown/fallback report; reporting 48 on every installation is wrong when replacing an accelerated loader. It MUST not divide LSB again or destabilize USB during first-slot → second-slot → first-slot swaps. `0` MHz is acceptable as “not reported” in host display when exact state cannot be established; preserve the measured valid clock state.

Use SPI0 raw `03` DMA reads with baud 1, two private buffers of 4096 bytes or an equivalent bounded streaming design, and USB callback sends in pieces ≤512 bytes. An efficient read starts the next DMA before transmitting the completed buffer so USB hides flash time. CRC32 and CRC16 scans can use the same streaming mechanism, using software CRC while SPI DMA is active. The 4096 advertised program buffer MUST NOT limit streaming read requests to 4096. Send exactly the requested raw byte count, including a final partial buffer/callback chunk. Empty reads/checksums must complete without starting a zero-count DMA.

Observed performance with this design is approximately 1.2 s for a 1 MiB read (~860 KiB/s), 0.4 s for an on-device 1 MiB CRC32, and about 10 s for a firmware installation including host work. These are not guaranteed minima. Full-speed USB host-controller scheduling can reduce throughput to ~230 KiB/s / 4.5 s per MiB even with the same loader. Testing SHOULD demonstrate DMA/USB overlap and absence of an avoidable per-byte read bottleneck on a known fast port; an otherwise correct loader does not fail solely because of a slow PC USB controller.

### 10.5 Display behavior during host operations

Only FC43 chooses wiring; the flash loader does not autodetect the board. Its display services are best effort and never a prerequisite for writing or verifying flash. Use panel sequences in §8.4, clear to black on init and display on. FC44 draws one row from column zero using supplied foreground/background, truncating visible text at twenty characters and padding the rest with background. Row position is in 16-pixel cells. Host screen content, percentages and colors are entirely host-supplied.

For compatibility with M-Flasher's layout, the progress indicator occupies the band around rows 9/10: white two-pixel frame outer x=10…229, y=148…173; interior fill starts x=12, y=152, width `floor(216×clampedPercent/100)`, height 18; remaining interior black. Avoid zero-area DMA when percentage is 0 or 100. Exact font pixels and internal buffering are free choices; keep graphic/text legible and the same coordinate contract.

## 11. Builds, installation and M-Flasher compatibility

### 11.1 Independent build products

Use JieLi's clang 4.0.1 fork with target `pi32v2`, CPU `r3`, integrated assembly and the corresponding target linker/objcopy. Toolchain release 20250324.1 is known to work. Size optimization (with LTO for the bootloader, without LTO for the flash loader) has been used; such optimization/linker choices are **not** binary compatibility requirements. Build freestanding: do not link the toolchain's own `libc.a` or `libcompiler-rt.a`; provide the few helpers needed (memory copy/fill/compare, and any compiler helper the code generator calls).

Required bootloader artifacts per board are an ELF/map useful for inspection and a plain single-bank `.boot` container. Link loadable code, initialized data and constants at `0x01C02000`, ensure the first payload byte is the true entry, and ensure all initialized state is included in the payload. Uninitialized RAM, stacks and staging buffers need not be stored in flash but MUST be allocated by the link map below `0x01C0A800`. Startup must initialize them explicitly. Keep identity at +40 hex. Container including header MUST be ≤14,384 (`0x3830`) bytes; payload ≤14,368 (`0x3820`). At flash placement A0, it must end no later than 38D0, preserving the stock configuration location. Fail the build on any overflow. Stack sizing, text layout and private buffers may differ completely from the stock image.

Required flash loader artifacts are two plain flat RAM images linked at `0x01C20000` and `0x01C28000`, plus maps/ELFs for verification. No BANKCB header. Its callable entry must be the first byte, not merely the ELF entry symbol. Each image plus private initialized/uninitialized storage must fit before the next `0x8000` boundary: the first-slot image must end ≤01C28000, the second-slot image ≤01C30000. Avoid dependency on the bootloader remaining present: ROM download may have been entered with invalid/blank flash. No compressed image or arbitrary runtime relocation is required because the host already bundles two linked addresses.

### 11.2 Installing the bootloader in a flash image

The host expects a plain BANKCB container, validates header/payload CRC and board identity, preserves the unit's complete existing first 16 KiB, then replaces only the boot file and its record. The replacement record retains fields except data CRC, address A0 and new length; recompute header CRC after changing them. File data CRC is over the **entire plain container**, header included. Encode the record with fresh FFFF ENC. Encode the 16-byte container header and payload separately with fresh FFFF ENC; fill remaining bytes up to 38D0 with raw FF. Preserve the original flash header, configuration record/blob, app directory record and unit reservation descriptor. The bootloader MUST fit the stock slot.

M-Flasher plans bootloader installation against 4 KiB sectors only within `[0,4000)`, compares changed sectors, makes a backup before its first write, erases/programs only differences, and verifies. A smaller image therefore need not change all four sectors. Do not supply a replacement full boot area with generic configuration as the primary release artifact: the host intentionally keeps each unit's configuration.

An official M-VAVE update can write its package flash image including `[0,4000)` and replace the bootloader with the stock bootloader. The bootloader MUST remain compatible with that updater handoff; preventing the update from replacing the bootloader is outside scope. A stock SMK update has been observed to write boot/app, leave the guard copy and unit areas intact, and not reinstall missing separate USR sounds. The bootloader must then be reinstalled separately if desired. This is not an installer bug that the bootloader can fix by hiding the boot area.

### 11.3 Uploading and replacing the flash loader

M-Flasher starts by checking the ROM disk identity, then tries FC40 to discover an already running own loader and FC0A to detect a running JieLi loader. It reuses its own bundled version when equal. If an own older version is in the second slot, it uploads the new image to the first slot; otherwise when replacing an own loader it uses the second slot. With no running loader it uses the first slot. The host avoids jumping through JieLi's already-running loader and simply uses it as-is. If a first own-loader start fails to identify, the host can upload JieLi's loader (its SDK fallback) at `0x01C02000`; if replacing an own loader fails, the host errors rather than assume a safe fallback.

For own-image upload, the host divides the **plain image** into pieces of at most 512 bytes, independently transforms each using the USB CRC stream, then sends ROM `FB06` with address BE u32 +2, length BE u16 +6, zero +8, CRC16 LE +9 over the **transmitted transformed block**, followed by that block OUT. The ROM decodes it into executable RAM. It sends `FB08`, entry address BE u32 +2 and target argument BE u16=1 +6, asking for a 16-byte reply, then waits 100 ms and identifies. This encoding is ROM-side framing, not the flash loader's raw flash-write transform. The flash loader MUST leave ROM upload/read/jump paths usable after registering itself.

This lifecycle requires testing both slot directions and repeated replacement without power cycling, including a slot whose RAM holds stale `.bss` from a prior instance. Make each version distinct when testing so the host really takes the upload path. Do not write a new image over currently executing handler code or its buffers.

### 11.4 Host assumptions that the flash loader and bootloader must satisfy

These are important compatibility facts, not instructions to change the PC application.

| Host behavior | Consequence for the bootloader / flash loader |
|---|---|
| Reads exactly three version bytes after MFLR. | Flash loader versions and the embedded version marker must fit this scheme. |
| Uses own loader extensions when MFLR is recognized without checking each feature bit. | A flash loader claiming own identity implements the CRC32/UID/display/status contract. |
| Reads own-loader flash in 16 KiB commands; writes in reported buffer-size chunks, clamped 256…4096. | The flash loader streams reads and correctly reports 4096. |
| Skips all-FF program chunks, expecting erased flash already. | Page programming alone need not erase; erased skipped bytes must stay FF. |
| Chooses 64 KiB erases where aligned and long enough, otherwise 4 KiB. | Both erase commands must wait for completion and be repeatable. |
| Retries transient failures up to four attempts with 250/500/750 ms pauses. | Repeating identical read/erase/program must be safe. Program is repeatable only for the same data already being ANDed into NOR. |
| At connection, verifies the full 1 MiB read using on-device CRC32; on mismatch/fallback it reads again and compares. | FC41 must CRC raw bytes and accept full-chip length; don't return decoded app CRC. |
| Before the first session write saves a full unit backup and unit metadata. | The flash loader's raw read must preserve every byte, including last sector and factory areas. |
| Verifies each written contiguous run by on-device CRC32, or readback with fallback; re-erases/reprograms once on mismatch. | The flash loader does not need a new verify command or automatic erase inside FB04. |
| Recognizes the open bootloader from BANKCB+AC79BLID. | Keep the bootloader identity format and valid container/file CRCs. |
| Detects unit identity from eight-byte flash UID, ignoring all-zero/all-FF values. | FC42 returns bytes in factory read order, no custom formatting. Lack of a meaningful UID need not prevent raw access. |
| Enables unit screen only when board is certain or explicitly selected. | FC43 is the only board-selection/pin-driving trigger in the flash loader. |

The stock expected chip key is 980F; the host logs unexpected keys and refuses a package whose app does not decrypt/check under the reported unit key. It does not re-encrypt packages automatically. SMK Pro/Elite package names accepted by the SMK board target are both `SMK-37 Pro` and `SMK-37 Elite`. The application-reported product name is not a safe electrical detection mechanism by itself.

Factory-data policy is host-owned: the FM-1 factory range is the last sector; SMK adds C1000…C1FFF. For a backup known by UID to be another unit's, the host preserves current factory ranges; a matching UID can restore all flash. If UID metadata is missing, it uses a weaker heuristic based on factory-data equality or currently blank data. This is not proof of provenance. SMK presets/wheel storage is not in the host's fixed factory-range list even though it holds unit values; this specification does not imply otherwise. The bootloader's guard and menu do not replace the host's full backup.

## 12. Testing

Real flash images of units can be used as test fixtures, but they are not published because they contain M-VAVE's firmware and the unit's own calibration. Use only their structural/checksum results in distributable tests; never ship one unit's factory data to other units. Tests of parsers, the image wrapper and state logic SHOULD use such real images and synthetic malformed inputs, through a simulated flash adapter or a host build.

### 12.1 Reference values

The test vectors of §5.1 (CRC16, CRC32, ENC and USB CRC stream), the CDB and reply vectors of §10.2, the stock boot container values of §5.2, the configuration prefix of §5.3, the package values of §5.6 and the staged updater values of §6.3 are reference values for offline tests. In stock and known third-party images (FM-1 stock, SMK-37 stock, Snoop37, Baud Girl) the area CPU entry is `02000120`, `app.bin` starts at relative `120`, and `cfg_tool.bin` is 383 bytes with CRC16 `2E75` (this is the application file `cfg_tool.bin`, not the boot configuration blob).

### 12.2 Offline bootloader format and behavior tests

| Test | Required result |
|---|---|
| CRC16/CRC32 and cipher vectors in §5.1 | Exact matches, chunked streaming equal to whole-buffer results. |
| Decode real stock and third-party images | App OK with matching `app.bin`/`cfg_tool.bin` lengths and CRCs; raw boot/config records and region boundaries as §5. |
| Build bootloader container | Correct load/entry/CRC, identity exact offset/tag, max flash length, linked RAM/stacks below updater. |
| Install each bootloader into an image with host-compatible rules | Only boot-area bytes differ; configuration/header/other records preserved; extract round-trip gives the replacement container. No change to application or factory data. |
| App file bit flip | CRC refusal and BAD CRC; a bit flip in cfg_tool must be caught too. |
| Entire app sector or tail erased | CRC refusal; INCOMPLETE using documented tail sampling; recognize heuristic differences from host. |
| Missing app, bad area/header, zero-length app | NOT FOUND or a precise malformed-app reason; no arbitrary jump and recovery available. |
| Overflow addresses/lengths; full non-NUL name; malformed records | Bounded failure; no flash/MMIO access outside intended ranges and no infinite traversal. |
| Malformed/oversized/truncated config; missing clock records | Validated recovery fallback, no unsafe PLL/division, no memory overwrite; unknown keys safely ignored. |
| RAM update request | CRC/type rules, private pointer points to original retained bytes, no flash scan when mounted and RAM request invalid. |
| Stale flash update and intact app | Normal app boot ignores flash request; menu may display it. |
| Failed mount with saved request | Exact scan placements and ordering; a stock SMK-37 Pro image's request at `0xBEF00` found and its staged updater decoded to the output size and CRC of §6.3. |
| Broken compressed block/header/backreference/output length/address | Refuse without overwriting the bootloader or retained memory and without calling invalid code. |
| Dual bank synthetic fixtures, if supported | Primary/secondary invalid-info cases, older/newer/equal versions, selected first-header invalid, out-of-bounds metadata; no invented version wrapping. Hardware remains a separate test. |
| App returns / CRC failure after XIP | Menu; subsequent boot-anyway uses reset and one-shot marker consumed before retry. |

### 12.3 Wipe and calibration guard tests

With a NOR model enforcing 1→0 programming and 4 KiB/64 KiB erase units, record every write/erase address.

* FM-1 stock wipe: erase exactly eligible VM `[93000,E9000)` and BTIF `[E9000,EA000)`, preserving boot, USR, gaps and FF000.
* SMK stock/Snoop37 wipe: erase VM `[9D000,C0000)` and BTIF `[C0000,C1000)`; calibration, presets and resources unchanged.
* Baud Girl wipe: erase declared VM `[D9000,E9000)` and BTIF C0000; preserve EA000 resources and fixed calibration. Do not erase stock VM at 9D000 just from the board type.
* Malicious descriptors spanning protected areas: only safe allowed sectors erased (or request rejected); never a broad block erase spanning a protected sector. Descriptors not sector-aligned or outside capacity are ineligible. Ordinary files named VM/BTIF are ineligible.
* Simulate power loss during wipe: app and protected sectors unchanged; repeating wipe completes.
* Blank guard slot + valid source: record bytes written before header, no erase of FF000, page boundaries honored; exact slot layout/CRC.
* Nonblank invalid slot: no writes anywhere. Simulate partial first-copy interruption and confirm no destructive “repair.”
* Valid copy + valid differing source: no changes to source or copy. Valid copy + invalid source marker: restore exact copied bytes via C1000 sector erase only. Bad copy CRC/address/length: no restore.
* Power loss at each restore page boundary: original backup preserved and later boot can restore again. Test program/erase/readback failure and visible failure handling.

### 12.4 Offline and ROM-stub flash loader tests

A ROM callback harness SHOULD capture send/receive buffers and flash effects; it cannot establish real ROM timing or callback error conventions.

* Exact CDB/reply vectors in §10, every opcode, unaligned range reads, zero-length reads/CRCs, final short DMA/callback chunks, and read lengths 4096/16384/65535.
* FC41 over `0x000000`…`0x100000` returns the IEEE CRC32 (§5.1) of the raw bytes actually in the (simulated) flash; FC0E CRC matches the host over raw encrypted bytes. No accidental application ENC.
* FB04 lengths 0, 1, 255, 256, 257, 4096 at page-aligned and unaligned addresses. Program page splits and WREN/WIP behavior correct; no automatic erase.
* Corrupt CRC and length 4097/65535: receive/drain complete, no program, handled result, codes 2/1 respectively; read-and-clear status, retained status after intervening successful command.
* Bad range, overflow, bad erase alignment, direction/length mismatch and text overflow: bounded rejection without ROM transport desynchronization or flash writes.
* Unknown ROM read/write/jump dispatch remains unhandled by the flash loader; forbidden destructive commands cause no destructive effect.
* Reinitialize the flash loader with deliberately dirty private RAM: no inherited display state/refusal, pointers valid and CRC table correct.
* Display commands before init/with wrong board/row>14 do not corrupt memory or block flash. Percent 0/100/255 and empty text complete without zero-count DMA hang.
* Build both slots, inspect ELF/map and raw first-byte entry; callbacks obey target ABI and no linked object reaches the other slot.

### 12.5 Hardware tests, with USB_KEY ready

Before testing on each physical unit: prove USB_KEY recovery works with its current firmware, read and verify a full unit backup, keep it privately with UID and SHA-256, and demonstrate at least a read-only ROM session.

1. Test the flash loader first from USB_KEY ROM mode with existing flash untouched: identify/key/JEDEC/UID, twice-read or CRC32-verified backup, ROM memory command deferral, display on the correctly identified board, and measured raw throughput.
2. Upload versions alternating first slot / second slot / first slot repeatedly without power cycling. Confirm the registered handler is the new version/address, the CPU report is accurate and USB stays stable. Reuse a dirty slot. Also test reuse of a same-version loader and compatibility with an already-running JieLi loader; do not deliberately jump through the latter.
3. For destructive flash loader checks use an explicitly backed-up safe test sector, restore and verify it before continuing. Test erase/program/readback, wrong-CRC refusal and oversized-write drain followed by a normal command. Never use efuse burn/chip erase as a test.
4. The first flash-read hardware test MUST include a quad read compared against a single-bit read of the same bytes (§3.4).
5. Install the board-correct bootloader only after readback comparison of the planned boot-area image. Verify the container CRC/identity and full first-16-KiB readback. Confirm every byte outside allowed sectors remains unchanged.
6. Cold boot and soft reboot stock firmware; audio, MIDI, USB, Bluetooth, knobs, display, keyboard/pads and pitch/mod wheels where present. Confirm startup key scanning leaves app pin states usable.
7. Recovery key held ≥1 s, early release, bounce and both menu+recovery held; recovery priority and normal ROM enumeration. Menu key opens menu; all menu LEDs and navigation work on **both** actual boards. FM-1 LED multiplexing is not verified and needs this test.
8. Boot Felucca/SLOOP on FM-1, Snoop37 and Baud Girl where board-compatible, with their own calibration/USB-rescue shortcuts preserved. Snoop37 does not stand in for Felucca or FM-1 SLOOP.
9. Controlled corrupted application with backup available: automatic recovery, displayed reason, three-second confirmation/release, one-shot boot-anyway, next power-on checks again; app-return recovery uses reset for retry. A hung app can still be recovered by key.
10. Menu wipe with full before/after raw images: changes restricted to declared VM/BTIF and protected exclusions, flash ID/capacity and addresses displayed correctly. Check that unlisted gaps and resources remain unchanged. Verify firmware-specific consequences rather than assume every erased VM is merely settings.
11. SMK guard: first backup exact, no last-sector erase, then controlled invalidation/restoration. Any experiment blanking USRFLASH can make stock firmware erase calibration and lose wheel/preset data; restore the **same unit's** full relevant regions afterward. Confirm wheel behavior, not just the trim marker. This guard must also be tested when the app cannot mount.
12. Official updater compatibility on a backed-up SMK: valid app-created request, staged RAM updater runs, official install succeeds, stock bootloader replacement is understood, and reinstalling the bootloader only affects the boot area. Test FM-1 separately; SMK-37 Pro success does not prove FM-1 or Elite updates.
13. Repeat recovery after controlled power interruption of app update, wipe and guard restore, while keeping USB_KEY recovery available. Record clock/register state and board revision for any failure, rather than assuming undocumented ROM state.

Test reports SHOULD state device variant/revision, firmware versions, unit key/JEDEC without publishing private calibration, toolchain, artifact hashes, tests actually performed, measured timing/performance and remaining failures. Success on SMK-37 Pro MUST NOT be reported as Elite or FM-1 hardware success.

## 13. Not verified

| Topic | What is known / practical consequence |
|---|---|
| Complete WL82 ROM boot validation/search state machine | The fixed-base/container flow and host tooling work. Outer CRC enforcement order, scan fallback and error responses are not established. Use the supported fixed layout. |
| Exact ROM state at bootloader entry | The exception context and descriptor flow are known (§4.1). Complete clocks/cache/interrupt/reset-register state and all ABI details are not established. |
| ROM descriptor read callback return and modes beyond the 0/1 calls | Only the invocation pattern of §4.1 is known. Do not extrapolate to other boot devices or filesystem bases. |
| ROM USB callbacks | Function pointer layout and handler framing are known; short transfers, failure return interpretation, reentrancy/interrupt requirements and exact ROM-owned RAM bounds are not. Keep to the proven sizes/slots. |
| Legal recovery clock fallback with damaged config | Stock zero defaults do not prove safe operation. Validate ROM-clock key scan or a conservative 24 MHz-board profile before claiming recovery for every damaged-config case. |
| Minimum PLL/CS/P33 settling times, lock bits, voltage margins | Working counts/register sequences are known, physical minima are not. Do not invent precise microseconds from uncalibrated loops. |
| All SPI/SFC modes and automatic probing | Quad-output mode is the principal target. The BB single-bit address anomaly, multi-bit single-byte register transfers and continuous-mode transitions need hardware checks before generalized support is advertised. |
| Protection-register universality | The generic SR1/SR2 model works on the observed parts. Different JEDEC families may need explicit support/refusal. No SFDP compatibility is established. |
| Efuse trim disjoint-merge intention | The actual OR accumulation is specified (§3.5); the analog validity of “fixing” it is unknown. Measure row contents/firmware behavior before changing the handoff trim. |
| Oscillator tag 1 instead of 0 | Proper bounded parsing is required; the impact of changed updater metadata needs an official-update hardware test. Clock frequencies must remain correct. |
| Dual-bank update lifecycle | The selection algorithm is specified; no real dual-bank image or device has been tested. Versions, interrupted bank installation and info-record conventions beyond the decoded contract are not established. |
| Arbitrary updater destinations/extended arguments | The stock destination and ABI are known. Extension payload semantics and all legal address ranges are not established. Preserve opaque fields and validate bounds. |
| FM-1 menu indicators and full FM-1 hardware behavior | FM-1 pins and firmware behavior are known; the menu LED multiplexing is not verified on hardware. |
| Elite physical behavior | Shared pin/setup/boot/firmware behavior supports one SMK build; Elite hardware is not verified. |
| Recalibration after guard backup | The guard leaves a valid newer source intact but retains the old backup. If the new source is later erased, the old calibration returns. Updating the backup safely requires a separately designed maintenance protocol. |
| Guard interruption and backup corruption | Header-last writing helps identify validity, but no redundant slot or atomic recovery is guaranteed. A nonblank partial copy is intentionally left alone. |
| Whole-unit factory restoration | USRTRIM is not all wheel calibration; USRFLASH and resources matter. Broader factory formats are not specified and cannot be regenerated from a marker. |
| All `.fwsc` non-flash member conventions | Package/list/flash checks are known; some other members' stored CRCs differ from the raw-byte CRC. The bootloader and the flash loader need not decode them. |
| Felucca, FM-1 SLOOP and Elite images | Not decoded; their compatibility requires their images and hardware tests. |

## Appendix A: menu font

An optional 5 × 7 font, provided under MIT for reuse (§8.4). Characters, in this order: `` -./:<>+?0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ`` (the first is a space). Each glyph is five column bytes, left to right; bit 0 is the top row. Drawn at twice the size, a glyph occupies pixels 0…9 × 0…13 of its 12 × 16 cell.

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

## Appendix B: References

Public notes can be wrong or describe other chips of the family; check their facts before relying on them.

* kagaimiq's JieLi notes, github.com/kagaimiq/jielie: `cpu/pi32v2.md` (registers, instructions), `datafmt/bankcb.md`, `datafmt/jlfs.md`, `datafmt/sdkcfg.md`, `misc/cipher.md`, `isp/usb/usb-key.md`.
* jl-uboot-tool, github.com/kagaimiq/jl-uboot-tool (MIT): the download-mode protocol from the PC side.
* jl-misctools, github.com/kagaimiq/jl-misctools: firmware and key-file tools.
* M-Flasher, github.com/jvitkauskas/m-flasher, `src/` (MIT): `formats.rs`, `jl.rs`, `boards.rs`, `sectormap.rs`, `ops.rs`, `tests.rs`, `scsi.rs`, `usb.rs`.
* The LZ4 block format description, github.com/lz4/lz4, `doc/lz4_Block_format.md`.
* Felucca (FM-1 firmware: pins, panel), SLOOP and its SMK-37 port Snoop37, and genkigenki/fm1-to-smk37 (SMK-37 hardware notes): pin and panel facts.
* JieLi's AC79 SDK (`fw-AC79_AIoT_SDK`): headers `include_lib/driver/cpu/wl82/asm/WL82.h`, `csfr.h`, `spi.h`, `p33.h`, `clock.h`; `include_lib/system/boot.h`; `include_lib/update/update.h`; `cpu/wl82/tools/isd_config_rule.c`; `cpu/wl82/sdk_ld_sfc.c`; the AC791N datasheets in its `doc/` folder. JieLi's documentation at doc.zh-jieli.com.
* Datasheets: ST7789V, the flash parts with JEDEC IDs `0x856014` and `0xEB6014`, and a Winbond W25Q-series part for the common command set.
