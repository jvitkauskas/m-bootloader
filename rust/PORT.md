# m-bootloader in Rust

Every C source of version 2.4 (`main` at `121cfbe`) was replaced with Rust. It builds every product, passes every
test, and stays inside every size limit. **It has had its first runs on an FM-1 and an SMK-37 Pro** ("On
hardware", below).

## What changed

| C (removed) | Rust | Notes |
|---|---|---|
| `src/core.c` | `rust/src/formats.rs` | |
| `src/contract.c` | `rust/src/contract.rs` | |
| `src/midi_update.c` | `rust/src/midi_update.rs`, `midi_update/protocol.rs` | |
| `src/sdboot.c` | `rust/src/sdboot.rs` | |
| `src/platform.c` | `rust/src/platform.rs`, `hw.rs` | `hw.rs` has the register type, `csync` and screen text |
| `src/board.c`, `src/display.c` | `rust/src/board.rs`, `display.rs` | |
| `src/usb_midi.c` | `rust/src/usb_midi.rs` | no test runs it: there is no USB in the simulated chip |
| `src/boot.c` | `rust/src/boot.rs` | |
| `src/stage0.c`, `src/upkr.c`, `src/runtime.c` | `rust/src/stage0.rs`, `upkr.rs`, `runtime.rs` | |
| `src/sfx.c`, `src/installer.c`, `src/stage.c` | `rust/src/sfx.rs`, `installer.rs`, `stage.rs` | |
| `src/loader.c`, `src/loader_target.c` | `rust/src/loader.rs` | |
| `src/test_request.c` | `rust/src/test_request.rs` | |

- **Still assembly:** the four entries (`src/*.S`).
- **Still C in 3.0:** the tests, the simulated chip (`tests/boot_sim.c`) and the host tools, with `include/*.h`, the C
  interface that the Rust kept for them. They are Rust since ("The tests in Rust", below), and no C is left.
- **Size:** 4,297 lines of C became 6,217 lines of Rust, part of them the C interface and what the tests need.

### Tests that changed

(As in 3.0, when the tests were C.) Three tests included a C source to reach its private data. They now go through
entries the Rust has for them:

- `tests/boot_test.c`, `tests/boot_midi_test.c`: `boot_test_reset()` and three readers, in place of `#include
  "../src/boot.c"`. No check changed.
- `tests/sdboot_test.c`: `sd_test_init()` and `sd_test_load()`.
- `tests/loader_slot_test.c` is gone. It compiled the loader's entry twice; the test's library now has the two
  entries (`loader_test_install_a`, `_b`).

Every other test is as it was, and links the Rust through `include/*.h`.

## How it builds

One crate, `rust/src/lib.rs`, compiled once for each image with the `--cfg` names the Makefile gives it (`stage0`,
`stage1`, `smk`, `ram_stage`, ...). It needs:

- **pi32v2 LLVM `23.1.6-2`** (`TOOLCHAIN=`). 2.4 was built with 23.1.5-5.
- **The pi32v2 Rust compiler `1.100.0-1`** (`PI32V2_RUST=`), a release of
  [pi32v2-rust](https://github.com/jvitkauskas/pi32v2-rust) built on that LLVM.
- **cargo**, to build `core` from the compiler's sources once (`rust/sysroot`), and any recent rustc for the tests.

```sh
make TOOLCHAIN=<pi32v2-llvm 23.1.6-2> PI32V2_RUST=<pi32v2-rust 1.100.0-1> test
```

CI (`.github/workflows/build.yml`) downloads both releases and checks them against their checksums.

The linker optimises each image as one program: rustc writes bitcode (`-Clinker-plugin-lto -Clto=fat`), and
`ld.lld` generates the code. Stage 1 takes stage 0's C names only (`stage0.syms`), and stage 0 keeps exactly what
stage 1 calls (`stage0.keep`, read from stage 1's object).

## Sizes

Bytes, as released in 3.0 (what they are since is under "The tests in Rust"). The package is the M-UPGRADE stream of
the self-extracting installer. The Rust is the default build, `RUST_OPT=z`.

| | Limit | FM-1, C | FM-1, Rust | SMK-37, C | SMK-37, Rust |
|---|---|---|---|---|---|
| Stage 0 | | 2,752 | 2,776 | 2,752 | 2,776 |
| Stage 1, unpacked | | 15,408 | 15,736 | 16,052 | 16,492 |
| **Payload** | 16,112 | 13,717 | 14,112 | 14,182 | **14,608** |
| **RAM** (stages and data) | 29,440 | 26,456 | 26,696 | 27,104 | **27,456** |
| **Package** | 20,224 | 17,607 | 18,182 | 18,141 | **18,796** |
| RAM stage | 23,324 | 8,436 | 8,468 | 8,516 | 8,600 |
| Flash loader | 32,768 | 6,016 | 5,376 | 6,016 | 5,376 |

The SMK-37 is the tight board. It keeps 1,504 bytes of payload, 1,984 of RAM and 1,428 of package, where the C had
1,930, 2,336 and 2,083. The C here is `main` built with the 23.1.6-1 toolchain. The Rust is built with the releases
above. At `RUST_OPT=s` the SMK-37's payload is 14,768, its RAM 27,656 and its package 18,985.

### As each module moved (SMK-37)

| Step | Payload | RAM | Package |
|---|---|---|---|
| C | 14,182 | 27,104 | 18,141 |
| `core.c` | 14,429 | 27,392 | |
| + `contract.c`, `midi_update.c` | 14,704 | 27,576 | |
| + `sdboot.c` | 14,825 | 27,744 | |
| + `platform.c` | 14,930 | 27,872 | |
| + `board.c`, `display.c` | 15,091 | 27,888 | |
| + `usb_midi.c` | 15,115 | 27,864 | 19,418 |
| + `boot.c` | 14,958 | 27,808 | 19,261 |
| + `stage0.c`, `upkr.c`, `runtime.c`, `sfx.c` | 14,991 | 27,824 | 19,210 |
| + `installer.c`, `stage.c`, `loader.c` | 14,991 | 27,824 | 19,179 |
| the linker optimises stage 0 too | 14,878 | 27,712 | 19,167 |
| less unsafe code (below) | 14,768 | 27,656 | 18,985 |
| `z` with the inlining option (below), the default | 14,608 | 27,456 | 18,796 |

Data-handling code cost most (`core.c`, `midi_update.c`, `sdboot.c`: about +17% of stage 1's share). Register code came out level.
`boot.c` made the whole smaller, because the last C in stage 1 went with it and nothing crossed a language border any
more.

## What decided the size

Each line is a measurement on this code.

| Choice | Effect | Used |
|---|---|---|
| `opt-level="s"`, not plain `"z"` | at `z`, FM-1 stage 1 is 17,304 against 15,944 and stage 0 is 3,256 against 2,892; the SMK-37 does not link | `z` with the compiler change below; `s` is the other that fits |
| The linker's whole-program optimisation, not rustc's (`-C lto` on a staticlib) | stage 1: 16,744 against 18,104. Stage 0: 2,780 against 2,892. Loader: 5,316 against 6,100 | the linker's |
| Full LTO bitcode (`-Clto=fat`), not rustc's default ThinLTO bitcode | with half of `core.c` ported, stage 1 was 16,320 against 16,912: the linker does not optimise across the two kinds, so nothing inlined between C and Rust | full |
| A global that isn't all zeros | the USB state held an empty slice (a non-null pointer), which put its 216 bytes in the image | all zeros |
| Returning a 32-byte struct in an `Option` | two `memcpy` calls each time; parsing in place took half of `core.c` from +31% to +24% over C | in place |
| The flash through a trait, where C's interface was a struct of function pointers and a context | stage 1: 16,636 against 16,728; payload 14,720 against 14,864 | the trait |
| The unpacker on slices, with every access checked | stage 0: 2,836 against 2,780 | checked |
| `-Zinline-mir=yes` | +104 on stage 1 | no |
| `-global-merge-group-by-use=false` (linker) | SMK-37 stage 1 −56, payload −23; FM-1 payload +14; the C build +88 | no |
| `--icf=all` (linker) | loader −32, nothing else | no |
| `-Zmerge-functions=disabled`, `-enable-merge-functions`, `core` at other opt-levels | no change | |
| `-inline-threshold=N` (linker), 0 to 50 | no value was smaller than the default, for C or for Rust | no |

**Why `z` is worse for Rust.** At `z` the compiler's inlining limit is 5, and small generic helpers of `core` stay
functions: `Range::next`, `<[u8]>::get`, `Index::index` for each array size, `ChunksExact::next`. Each is 20 to 46
bytes, each call site pays for a call, and nothing folds through them. C has no such helpers, so `-Oz` costs it
nothing.

## Unsafe code

A bootloader writes registers, jumps to code it has just loaded, and shares memory with a ROM and with firmware, so
some of it stays `unsafe`. After the port, a second pass took out what did not have to be. Counted by the compiler,
in the code that goes into each image (`-Zunpretty=expanded`):

| Image | `unsafe` blocks, as ported | Now | `unsafe fn`, as ported | Now |
|---|---|---|---|---|
| Stage 0 | 38 | 19 | 7 | 5 |
| Stage 1 | 105 | 43 | 35 | 10 |
| RAM stage | 120 | 43 | 39 | 11 |
| Installer | 128 | 39 | 37 | 11 |
| Flash loader | 133 | 62 | 38 | 16 |

What changed, and what it cost on the SMK-37, at `RUST_OPT=s`:

| Change | Payload | Checked by the compiler now |
|---|---|---|
| As ported | 14,878 | |
| A register is `Reg::at::<ADDRESS>()`, where each was an `unsafe` block | 14,864 | the address is in the ranges the chip's registers have (§2.2), not RAM or flash |
| The formats' code reaches the flash through a trait | 14,720 | no function pointers, contexts or raw buffers between the formats and the driver |
| MIDI mode's hooks are a trait, and take slices | 14,710 | the sizes of what is sent, read and programmed |
| The unpacker works on slices | 14,759 | every read and write of the unpacker is inside the pack and the room given |
| C interfaces with raw pointers only where something uses them (the tests, and the loader for the ROM) | 14,768 | none of them is in the bootloader |
| The retained area is claimed by an entry, and its bytes are `bytes::<OFFSET, N>()` of the claim | 14,768 | the bytes are inside the area (§6.1), and two references into it are never alive at once |
| `boot_main` takes stage 1's work buffer once, and passes it down | 14,768 | nothing else reaches the buffer |

The last two leave every image's code as it was, byte for byte: they cost a claim (an `unsafe` block or two and
an `unsafe fn` or two more in each image) and nothing on the unit. `memcpy` and `memmove` also copy their bytes as ones that may not
be initialised, which is what the language asks of them, with the same code coming out.

What is left in stage 1, by what it is for:

| Blocks | What | Why it stays |
|---|---|---|
| 14 | references to globals (`Global::get`): the work buffer where `boot_main` takes it, the request, the app's record and its header, the updater's context, MIDI mode's buffers, and the display's, the card's and USB's state | see below |
| 8 | the hardware: three register accesses, three instructions (`csync`, `cli`, `ssync`), a DMA read, a buffer the USB controller writes | |
| 6 | fixed memory: the claim of the retained area and its bytes, where an updater, an SD card image and a RAM app load, the flash as it is mapped | the addresses are the specification's |
| 4 | calls into code that was just loaded and checked: the app, an updater, the SD card image, a RAM app | |
| 4 | `memmove`, `memcmp`, `bcmp`, `strlen`, which the compiler calls | |
| 5 | inside `Global` and `Text`, and the version's address | |
| 1 | clearing the image's own data at entry | |
| 1 | starting MIDI mode's protocol, whose buffers are borrowed across its hooks | its caller promises that no hook starts it again |

**The other globals are the part the compiler does not check.** The program has one thread and no interrupts, and
its buffers are globals because the stack is small. Functions such as `app()` in `boot.rs` are safe to call but hand
out a `&'static mut`, so two of them could be alive at once, and nothing but the rule at the top of that file
prevents it. Two ways to close that:

1. **Check it in the tests.** `Global` could count its borrows in the builds for this computer, which the boot
   simulations and the sanitizers run, and cost nothing on the unit. It would find a broken rule wherever a test
   reaches it. Not done.
2. **Pass the state down from the entry**, as `&mut`, so that the compiler checks every use. Done for the work
   buffer and the retained area (above). Not done for the rest: MIDI mode's hook that draws the menu again after a
   RAM app reads the app's record, so the hooks would have to carry state, and the protocol's own buffers are
   borrowed across hooks. That touches `boot.rs`, the protocol and the drivers' entries, and its cost in bytes is
   not known.

Also left as it was: the flash loader keeps C's struct of services, which the ROM's two callbacks and the tests
fill in, with about a dozen blocks of its own.

## Compiler work

Four commits in pi32v2-llvm, released in `pi32v2-23.1.6-2`, and a documentation commit in pi32v2-rust, whose
release `pi32v2-1.100.0-1` is built on that LLVM.

| Change | Effect here (SMK-37) | Used here |
|---|---|---|
| `-inline-minsize-hint-threshold=50`: in size-minimum code, callees that ask to be inlined get a threshold of their own | At `z`: payload 14,608 against 14,768 at `s`, RAM 27,456 against 27,656, package 18,796 against 18,985. Stage 0 is 2,776 against 2,836 | the default |
| The machine outliner for pi32v2 (`-enable-machine-outliner`) | Stage 1 16,576 against 16,744, but its payload 15,168 against 14,878 (before the unsafe code was reduced): the packer does better with repeats than with calls to them | no |
| Variadic prologues pass the machine verifier | none: found while verifying outlined code | |

- **`RUST_OPT=z`**, the default, passes the option to the linker and to rustc. Asking for `z` with a linker that
  lacks the option, one before 23.1.6-2, fails at once. `RUST_OPT=s` builds without it.
- **How the two were checked.** pi32v2-isa's `tools/llvm/ctest.py`, which runs 3,328 calls in the simulator and
  compares them with JieLi's compiler, agrees on all of them at `-Oz`, `-Os` and `-O2`, with the outliner, and
  with both options. The machine verifier passes on this crate with the outliner on. The pi32v2 and inliner lit
  tests pass (336), three of them new. m-apps' C apps come out byte-identical with the changed compiler, as
  neither option is on by default.
- **The `z` build here:** `make RUST_OPT=z test`, `sanitize` and `reproducible` pass. Its unpacker returned both
  boards' stage 1 in the simulator. Its clock set-up calls `settle` as the C does, where the `s` build inlines the
  loops. On hardware it has run on both units ("On hardware").
- **The outliner on unpacked code:** the flash loader 5,272 against 5,316, pi32v2-isa's `ctest.c` 3,318 against
  3,520.

Not done:

1. **Using the `core` that the compiler's package has.** The build compiles `core` itself with nightly flags
   (`-Zbuild-std`, `-Cpanic=immediate-abort`, `-Clto=fat`), which the packaged one is not built with.
2. **Loading addresses.** Stage 1 has 351 six-byte constant loads, 2.1 KB of it: 109 addresses of its data, 104 of
   strings and tables, 63 plain constants, 39 registers, 36 others. Merging globals differently moved it by 50
   bytes either way (above). Getting more needs addresses the compiler can relate, which it can't across separately
   placed globals, or a table of strings in the source.
3. **Making the inlining option the target's default** in rustc or LLVM. It would change C built at `-Oz` wherever
   that has `inline` functions, so I left it a switch.

## On hardware

One FM-1 with the older flash chip and one SMK-37 Pro, on 2026-11-22, with `2.4-7b67881` and then, from its
packages, `2.4-46af0f5`, which is the same code at `z`. The tables are the `s` build of `2.4-7b67881` on the FM-1;
the `z` build, which is the default now, and the SMK-37 follow them.

3.0 is that code with its versions changed: the bootloader's, in stage 0's header and in what the RAM stage tells
a host, and the flash loader's, from 1.3 to 1.4. Stage 1 is byte for byte that of `2.4-46af0f5`, and each flash
loader differs from the one tested in one byte.

After the versions changed, on the FM-1, by programs:

- `3.0-7fd456a` was installed with M-Flasher over the stock bootloader, which M-Flasher planned as it had planned
  2.4. It started the firmware (Baud Girl), and nothing outside the boot area changed.
- The 1.4 flash loader replaced itself without a power cycle, in the second slot, the first, the second and the
  first again, and answered each time from the slot it was put in. M-Flasher's own 1.3 replaced it, and was
  replaced by it. The last of them passed the reads and the refusals.

By programs: M-Flasher 0.12.1, and a script speaking §11.6 over USB MIDI with commands that only read. The screen
was read with a camera.

| What | Result |
|---|---|
| The flash loader, in the second slot from download mode, the flash untouched | Identity, flash ID and unique ID as the released loader gives them. M-Flasher then found it running and used it: 1 MiB read at 855 KB/s, the chip's CRC-32 matching, and a backup byte for byte the released loader's. Its rows on the screen. |
| The flash loader writing, alone in the first slot | M-Flasher restored a backup with other firmware through it (197 sectors erased and programmed, 788 KiB, in 11 s, the chip's CRC-32 matching), then the first state again. Read back in a new session with the released loader, the flash was byte for byte as before the test. |
| The flash loader's refusals, sent as SCSI commands | A write with a wrong CRC (refusal 2, cleared once read), one of 4,352 bytes (1, and the next command answered), one past the end (3), an erase off a sector's start (3), a read whose lengths disagree (7): each refused, and the flash's CRC-32 the same afterwards. A 16,384-byte read and a sector's CRC-16 right. Not sent: a chip erase, an efuse burn, an erase past the end (§12.5). |
| The RAM stage, from download mode | On USB as "FM-1 Update". Identity, info and the CRC-32 of the whole flash right; 2,048 reads of 512 bytes, each as the backup has it; reads past the end refused; the download-mode request. Its text on the screen. |
| The bootloader, installed with M-Flasher | The stock firmware (016) started. Nothing outside the boot area changed. |
| Firmware with one bit flipped, with a sector erased, and with its first sector erased | Not started. MIDI mode opened by itself, showing `APP BAD CRC`, `APP INCOMPLETE` and `APP NOT FOUND`, and answered: identity, info, the CRC-32 and reads of the flash as it then was. |
| A RAM app over MIDI, from that MIDI mode | m-apps' `hello` was loaded in 10 parts and started with the 60 MHz clock; when it returned after its 10 s, the screen was drawn again and MIDI mode answered. |
| The restart request, from that MIDI mode | The unit left USB in 0.2 s and came back. |
| A test build, MIDI mode by request | The menu's screen (version, `APP OK`, `MIDI MODE`). The same reads and refusals as the RAM stage, against a new backup. The download-mode request. |
| A test build, the SD card item by request | `/MBOOT/MENU.BIN` of the card in the unit (m-apps' launcher) was loaded and started. |

By hand, on the same unit with the build that is not a test build (for what was tried by hand later, with `z` and
on both units, see the end of this section):

- the menu;
- REC for download mode;
- Felucca installed through MIDI mode;
- the SD card item: m-apps' apps, back from an app to the launcher, and back to the bootloader's menu.

Starting looked as fast as with 2.4's C.

`BOOT ANYWAY`, by hand, started firmware that had one bit flipped.

**The `z` build** went through the tests by programs on the same unit, and passed each: its flash loader (the reads,
the refusals, a sector written and written back), its RAM stage (the reads, a RAM app), and its bootloader installed
(the firmware started; with the firmware damaged in the three ways, the three verdicts, MIDI mode by itself with
its reads, a RAM app, the restart request). By hand, `BOOT ANYWAY` was tried with it.

**The SMK-37 Pro** (another flash chip, Felucca's firmware) had the `z` build only, by programs only, and each test
passed: the flash loader (the reads, the refusals, a sector written and written back), the RAM stage (its text on
the SMK-37's panel, the reads, a RAM app), and the bootloader installed. With that, the firmware started, nothing
outside the boot area changed, and M-Flasher still found the calibration and its copy. With the firmware damaged
in the three ways: the three verdicts on the panel, MIDI mode by itself with its reads, a RAM app, the restart
request. (Its firmware, which M-Flasher names SLOOP 2.7.1, is on USB as Felucca.)

Its calibration guard (§9.3), with one sector erased through the flash loader each time, as M-Flasher does not
write factory data from a changed image: with the calibration erased, the next start put it back from the copy;
with the copy erased, the next start made it again. Each time the flash came back byte for byte as it was.

**By hand, afterwards, with the `z` build** (reported by the owner, each as passed):

- on the SMK-37, what was tried by hand on the FM-1: the menu, REC for download mode, the SD card item;
- the bootloader installed from its M-UPGRADE package, on the FM-1 and on the SMK-37: the installer and its
  self-extracting wrapper, which nothing else here had run;
- Baud Girl installed from its web installer, with the bootloader present;
- on the FM-1, M-VAVE's own update from V15 to V16 through M-UPGRADE, which puts the stock bootloader back, and
  from V16 to V15.

Not tried: M-VAVE's own update on the SMK-37, as there is no other version of its firmware to update to and
M-UPGRADE refuses the version a unit has. Not reported: the RAM stage's M-UPGRADE packages, and the `s` build on the
SMK-37.

Not tried of the flash loader: M-Flasher uploading one itself, as it has its own 1.3, and a 64 KiB erase.

## The tests in Rust

After 3.0 the C that was left went too: 16 test and tool sources and the 12 headers of the C interface, 6,528 lines,
became 8,367 lines of Rust. **That changed no image**: with the tests ported, the flash loaders and the RAM stage
were byte for byte the 3.0 release's, and the two bootloaders differed from it in the commit of their version text
only. One change after it does change them, and makes them smaller ("The C names that only the tests used", below).

| Was | Is |
|---|---|
| `tests/core_test.c`, `contract_test.c`, `loader_test.c` | `rust/tests/formats.rs`, `contract.rs`, `loader.rs` |
| `tests/midi_update_test.c` | `rust/tests/midi_update.rs` |
| `tests/platform_test.c`, `panel_board_test.c`, `sdboot_test.c` | `rust/tests/platform.rs`, `panel_board.rs`, `sdboot.rs` |
| `tests/boot_sim.c`, `boot_sim.h`, `boot_nor.c` | `rust/tests/sim.rs`, `nor.rs` |
| `tests/boot_test.c`, `boot_midi_test.c`, `loader_lifecycle_test.c` | `rust/tests/boot.rs`, `boot_midi.rs`, `loader_lifecycle.rs` |
| `tests/upkr_test.c`, `installer_host.c`, `tools/sfx_layout.c` | `tests/upkr_test.rs`, `installer_host.rs`, `tools/sfx_layout.rs` |
| `include/*.h`, and the crate's C interface for them | gone: 129 exported C names in the crate became 59 |

- **Where they are.** The tests are modules of the crate (`rust/tests/`, from `rust/src/lib.rs` under `cfg(test)`),
  so they call what they test directly, private items included where one is opened to the crate. The Makefile
  builds the crate with them in, `rustc --test`, once for each set of `--cfg` names a C test program had: 13
  programs, each named by its names (`build/host/<names>/tests`). The three programs the build and the Python tests
  run link the crate as a library (`--cfg tool`).
- **Each is its C, line for line**: the same checks in the same order, and the same lines printed. With the private
  images in `testdata/` and without them, every program's output was compared with its C original's, counts
  included (the FM-1's boots: 71 scenarios and 398 checks with them, 13 and 89 without). The checks added since
  are listed below.
- **What stands in for hardware** is named by the crate as `crate::tests::...` and listed in one place, the end of
  `rust/tests/mod.rs`: the registers, the reset, the card, USB. They were names for the linker, which the compiler
  could not check against their uses. One kind still is: under `import_early` a test supplies stage 0's functions
  the way stage 1 gets them from stage 0, by name.
- **A reset in the middle of a boot, or a power cut in the middle of an install**, was `setjmp` and `longjmp`
  (`rust/tests/escape.rs`). A power cut unwinds the install, as a panic that the test catches. A panic can't pass
  the functions with C's calling convention that lie between a boot's start and its reset, so a boot is on a
  thread of its own, which stops for good where it leaves: some 70 of them by the end of a board's boots.
- **Addresses.** The boots map the chip's RAM and its flash window where the chip has them. The flash loader's test
  is linked at fixed addresses below 4 GB (`-C relocation-model=static`), as its entry is given the ROM's functions
  in 32 bits; it checks that they fit.
- **`make sanitize` is gone.** It ran the tests' C under ASan and UBSan, and there is no such C now. The tests are
  built with overflow checks and debug assertions instead, which also check what `unsafe` code promises the standard
  library (alignment, null, overlap, lengths), in the crate and in the tests. That is less than the sanitizers
  saw of the C, and more than they saw of the Rust.

How the port was checked, besides the output: 37 one-line bugs were put into the crate, one at a time (a
polynomial, an opcode, a limit, a text, a key's code, an address), and each was run against the C tests and against
their ports. Both caught the same 28, in the same programs and with the same counts of failed checks, and both
missed the same 9. The first round found one difference, which is why the tests state the keys' codes and the
specification's constants themselves (the C had its own copies from `include/*.h`) and do not take them from the
crate: a wrong value there has to fail a test, not be followed by it.

### The C names that only the tests used

MIDI mode's hooks, the USB-MIDI device's four functions and the card's two had C's calling convention and a name
for the linker, and the flash loader's `loader_init` and the screen's three services had the name: the C tests
called them. They are plain Rust functions now, and the three services keep the calling convention that the
loader's table has, without the name. The images are smaller for it, and **they are images that no unit has run
yet**:

| | 3.0 | Now |
|---|---|---|
| FM-1: payload, RAM, package | 14,112, 26,696, 18,182 | 14,090, 26,656, 18,166 |
| SMK-37: payload, RAM, package | 14,608, 27,456, 18,796 | 14,597, 27,408, 18,780 |
| Stage 1 unpacked, FM-1 and SMK-37 | 15,736, 16,492 | 15,692, 16,448 |
| Flash loader | 5,376 | 5,352 |

Stage 0 is as it was, byte for byte, and the RAM stage is the same size with a few dozen bytes changed. The flash
loader still says it is 1.4, with other bytes than the released 1.4 has: what it does is unchanged, and its version
is for the next release to decide.

### Not as it was

- A check that fails inside a function the crate calls back through C's calling convention (a loader callback, a
  stand-in for stage 0) ends the program with its message, where it would otherwise be one failed test.
- `Display::pixels` is in no image: RAM apps had it from the C. It is built for the panel's test only, which still
  checks it.
- The loader's test on the simulated chip still does not run the entry's clearing of its own data, which is
  compiled for the chip only; its comment no longer says it does.
- Three checks the C did not have, from the review of this port: the simulated flash reports a page program that
  crosses a page boundary, and a wide read (`6B`, `3B`) whose data, not only its dummy clocks, is in the wrong
  width (§3.4); `upkr-test` requires the pack to unpack to the whole reference file, and refuses one bigger than
  its room. Each was shown to fail a bug that passed before: a wide read's data in single-bit width, page splits
  taken out of the driver and the loader, and a reference file one byte short.
- Five more, from the same review: the boots have a scenario in which stage 2 returns after an earlier start of it
  failed, and the reason must go (it fails a menu that keeps the old one, which every earlier scenario passed);
  the flash loader's test sends one FC44 written out byte for byte from §10.2's table; the card's test checks a
  stage 2 image's CRC against one worked out outside the crate; the MIDI mode test refuses a flash dump longer
  than the flash, as the boots' does; and the simulated panel forgets half a pixel at a reset. So the boots count
  one scenario and five checks more than their C did (the FM-1's: 72 and 403, with the private images).

## What is not known

- **Timing.** The clock set-up's `nop` loops and `wait_mask` compile to the same instructions as the C (compared
  in the disassembly). The USB controller's access spacing, the SPI and DMA paths, and everything else
  timing-related have not been compared instruction for instruction; on the two units they worked.
- **The unpacker on the target.** The Rust unpacker, linked by itself, ran in pi32v2-isa's simulator on both
  boards' packs: stage 1 came back byte for byte, the installer unpacked in place as its package carries it, and
  packs that were cut, damaged or given too little room were refused. Stage 0 as a whole did not run there, and
  nothing else did.
- **A test build** (`TEST_REQUESTS=1`) builds and fits (SMK-37 payload 14,725). Its requests were tried on the FM-1
  only.
- **USB MIDI** (`usb_midi.rs`) has no test here, as in C: what is known of it is from the two units.
- **m-apps** builds its drivers from this repository's `src/*.c`, which are gone here, and `include/*.h` went with
  the tests' C.
- **Elite builds** differ from the Pro only in the package's model name, as before.
