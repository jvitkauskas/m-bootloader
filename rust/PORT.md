# m-bootloader in Rust

Every C source of version 2.4 (`main` at `121cfbe`) was replaced with Rust. It builds every product, passes every
test, and stays inside every size limit. **Nothing of it has run on a unit.**

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
- **Still C:** the tests, the simulated chip (`tests/boot_sim.c`) and the host tools. `include/*.h` stays for them: it
  is the C interface that the Rust keeps.
- **Size:** 4,297 lines of C became 6,217 lines of Rust, part of them the C interface and what the tests need.

### Tests that changed

Three tests included a C source to reach its private data. They now go through entries the Rust has for them:

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

Bytes. The package is the M-UPGRADE stream of the self-extracting installer.

| | Limit | FM-1, C | FM-1, Rust | SMK-37, C | SMK-37, Rust |
|---|---|---|---|---|---|
| Stage 0 | | 2,752 | 2,836 | 2,752 | 2,836 |
| Stage 1, unpacked | | 15,408 | 15,852 | 16,052 | 16,632 |
| **Payload** | 16,112 | 13,717 | 14,239 | 14,182 | **14,768** |
| **RAM** (stages and data) | 29,440 | 26,456 | 26,872 | 27,104 | **27,656** |
| **Package** | 20,224 | 17,607 | 18,370 | 18,141 | **18,985** |
| RAM stage | 23,324 | 8,436 | 8,456 | 8,516 | 8,584 |
| Flash loader | 32,768 | 6,016 | 5,324 | 6,016 | 5,324 |

The SMK-37 is the tight board. It keeps 1,344 bytes of payload, 1,784 of RAM and 1,239 of package, where the C had
1,930, 2,336 and 2,083. The C here is `main` built with the 23.1.6-1 toolchain. The Rust is built with the releases
above.

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

Data-handling code cost most (`core.c`, `midi_update.c`, `sdboot.c`: about +17% of stage 1's share). Register code came out level.
`boot.c` made the whole smaller, because the last C in stage 1 went with it and nothing crossed a language border any
more.

## What decided the size

Each line is a measurement on this code.

| Choice | Effect | Used |
|---|---|---|
| `opt-level="s"`, not plain `"z"` | at `z`, FM-1 stage 1 is 17,304 against 15,944 and stage 0 is 3,256 against 2,892; the SMK-37 does not link | `s`, or `z` with the compiler change below |
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

What changed, and what it cost on the SMK-37:

| Change | Payload | Checked by the compiler now |
|---|---|---|
| As ported | 14,878 | |
| A register is `Reg::at::<ADDRESS>()`, where each was an `unsafe` block | 14,864 | the address is among the chip's registers (§2.2), not RAM or flash |
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
| `-inline-minsize-hint-threshold=50`: in size-minimum code, callees that ask to be inlined get a threshold of their own | At `z`: payload 14,608 against 14,768 at `s`, RAM 27,456 against 27,656, package 18,796 against 18,985. Stage 0 is 2,776 against 2,836 | `make RUST_OPT=z`, with that LLVM |
| The machine outliner for pi32v2 (`-enable-machine-outliner`) | Stage 1 16,576 against 16,744, but its payload 15,168 against 14,878 (before the unsafe code was reduced): the packer does better with repeats than with calls to them | no |
| Variadic prologues pass the machine verifier | none: found while verifying outlined code | |

- **`RUST_OPT=z`** passes the option to the linker and to rustc. The default stays `s`. Asking for `z` with a
  linker that lacks the option, one before 23.1.6-2, fails at once.
- **How the two were checked.** pi32v2-isa's `tools/llvm/ctest.py`, which runs 3,328 calls in the simulator and
  compares them with JieLi's compiler, agrees on all of them at `-Oz`, `-Os` and `-O2`, with the outliner, and
  with both options. The machine verifier passes on this crate with the outliner on. The pi32v2 and inliner lit
  tests pass (336), three of them new. m-apps' C apps come out byte-identical with the changed compiler, as
  neither option is on by default.
- **The `z` build here:** `make RUST_OPT=z test`, `sanitize` and `reproducible` pass. Its unpacker returned both
  boards' stage 1 in the simulator. Its clock set-up calls `settle` as the C does, where the `s` build inlines the
  loops. It has not run on a unit either.
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

## What is not known

- **Hardware.** Not run on a unit. The clock set-up's `nop` loops and `wait_mask` compile to the same instructions
  as the C (compared in the disassembly). The USB controller's access spacing, the SPI and DMA paths, and everything
  timing-related have not been compared instruction for instruction.
- **The unpacker on the target.** The Rust unpacker, linked by itself, ran in pi32v2-isa's simulator on both
  boards' packs: stage 1 came back byte for byte, the installer unpacked in place as its package carries it, and
  packs that were cut, damaged or given too little room were refused. Stage 0 as a whole did not run there, and
  nothing else did.
- **A test build** (`TEST_REQUESTS=1`) builds and fits (SMK-37 payload 14,881). Its requests were not tried.
- **USB MIDI** (`usb_midi.rs`) has no test at all, as in C.
- **m-apps** builds its drivers from this repository's `src/*.c`, which are gone here.
- **Elite builds** differ from the Pro only in the package's model name, as before.
