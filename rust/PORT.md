# m-bootloader in Rust

This branch (`rust-port`, from `main` at `121cfbe`, version 2.4) replaces every C source with Rust. It builds every
product, passes every test, and stays inside every size limit. **Nothing of it has run on a unit.**

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
- **Size:** 4,297 lines of C became 6,093 lines of Rust, part of them the C interface and what the tests need.

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

- **pi32v2 LLVM `23.1.6-1`** (`TOOLCHAIN=`). `main` uses 23.1.5-5.
- **A rustc for pi32v2** (`RUSTC=`), built from [pi32v2-rust](https://github.com/jvitkauskas/pi32v2-rust) on the same
  LLVM. **There is no release of one**, so CI cannot build this branch yet, and `.github/workflows/build.yml` is not
  changed.
- **cargo**, to build `core` from the compiler's sources once (`rust/sysroot`), and any recent rustc for the tests.

```sh
make TOOLCHAIN=<pi32v2-llvm 23.1.6-1> RUSTC=<pi32v2 rustc> test
```

The linker optimises each image as one program: rustc writes bitcode (`-Clinker-plugin-lto -Clto=fat`), and
`ld.lld` generates the code. Stage 1 takes stage 0's C names only (`stage0.syms`), and stage 0 keeps exactly what
stage 1 calls (`stage0.keep`, read from stage 1's object).

## Sizes

Bytes. The package is the M-UPGRADE stream of the self-extracting installer.

| | Limit | FM-1, C | FM-1, Rust | SMK-37, C | SMK-37, Rust |
|---|---|---|---|---|---|
| Stage 0 | | 2,752 | 2,780 | 2,752 | 2,780 |
| Stage 1, unpacked | | 15,408 | 15,944 | 16,052 | 16,744 |
| **Payload** | 16,112 | 13,717 | 14,274 | 14,182 | **14,878** |
| **RAM** (stages and data) | 29,440 | 26,456 | 26,912 | 27,104 | **27,712** |
| **Package** | 20,224 | 17,607 | 18,420 | 18,141 | **19,167** |
| RAM stage | 23,324 | 8,436 | 8,528 | 8,516 | 8,656 |
| Flash loader | 32,768 | 6,016 | 5,316 | 6,016 | 5,316 |

The SMK-37 is the tight board. It keeps 1,234 bytes of payload, 1,728 of RAM and 1,057 of package, where the C had
1,930, 2,336 and 2,083. The C here is `main` built with the same 23.1.6-1 toolchain.

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

Data-handling code cost most (`core.c`, `midi_update.c`, `sdboot.c`: about +17% of stage 1's share). Register code came out level.
`boot.c` made the whole smaller, because the last C in stage 1 went with it and nothing crossed a language border any
more.

## What decided the size

Each line is a measurement on this code.

| Choice | Effect | Used |
|---|---|---|
| `opt-level="s"`, not `"z"` | at `z`, FM-1 stage 1 is 17,304 against 15,944 and stage 0 is 3,256 against 2,892; the SMK-37 does not link | `s` |
| The linker's whole-program optimisation, not rustc's (`-C lto` on a staticlib) | stage 1: 16,744 against 18,104. Stage 0: 2,780 against 2,892. Loader: 5,316 against 6,100 | the linker's |
| Full LTO bitcode (`-Clto=fat`), not rustc's default ThinLTO bitcode | with half of `core.c` ported, stage 1 was 16,320 against 16,912: the linker does not optimise across the two kinds, so nothing inlined between C and Rust | full |
| A global that isn't all zeros | the USB state held an empty slice (a non-null pointer), which put its 216 bytes in the image | all zeros |
| Returning a 32-byte struct in an `Option` | two `memcpy` calls each time; parsing in place took half of `core.c` from +31% to +24% over C | in place |
| `-Zinline-mir=yes` | +104 on stage 1 | no |
| `-global-merge-group-by-use=false` (linker) | SMK-37 stage 1 −56, payload −23; FM-1 payload +14; the C build +88 | no |
| `--icf=all` (linker) | loader −32, nothing else | no |
| `-Zmerge-functions=disabled`, `-enable-merge-functions`, `core` at other opt-levels | no change | |
| `-inline-threshold=N` (linker), 0 to 50 | no value was smaller than the default, for C or for Rust | no |

**Why `z` is worse for Rust.** At `z` the compiler's inlining limit is 5, and small generic helpers of `core` stay
functions: `Range::next`, `<[u8]>::get`, `Index::index` for each array size, `ChunksExact::next`. Each is 20 to 46
bytes, each call site pays for a call, and nothing folds through them. C has no such helpers, so `-Oz` costs it
nothing.

## Compiler work this points to

None of these is done. The first two need no compiler change, and are what a toolchain release should make the
default.

1. **A released rustc for pi32v2, with `core` built for it.** Today the build compiles `core` itself with nightly
   flags (`-Zbuild-std`, `-Cpanic=immediate-abort`).
2. **`-Clto=fat` with `-Clinker-plugin-lto` is the only way to optimise with C.** rustc already has it; nothing says
   so. Without it the build works and is quietly 3 to 4% bigger.
3. **The machine outliner.** `-enable-machine-outliner` changes nothing: the pi32v2 backend has no support for it.
   It would serve C as well. Its passes after it (`PI32V2IfBlocks`, `PI32V2Packetize`, `PI32V2DecBranch`) make this
   real work, and I did not try it.
4. **Loading addresses.** Stage 1 has 351 six-byte constant loads, 2.1 KB of its 16.7 KB, and 234 of them are
   addresses in the bootloader's own RAM. The backend already has `PI32V2AnchorBase` for this; I did not measure
   how much more there is to get.
5. **Inlining at `z`.** LLVM ignores `inlinehint` when the caller is `minsize`. Honouring it for callees a few
   instructions long would let Rust use `z`. Not tried: `s` gives the result today.

## What is not known

- **Hardware.** Not run on a unit. The clock set-up's `nop` loops and `wait_mask` compile to the same instructions
  as the C (compared in the disassembly). The USB controller's access spacing, the SPI and DMA paths, and everything
  timing-related have not been compared instruction for instruction.
- **The unpacker on the target.** The Rust `stage1_unpack`, linked by itself, ran in pi32v2-isa's simulator on both
  boards' packs: stage 1 came back byte for byte, and a damaged pack was refused. Stage 0 as a whole did not run
  there, and nothing else did.
- **A test build** (`TEST_REQUESTS=1`) builds and fits (SMK-37 payload 14,990). Its requests were not tried.
- **USB MIDI** (`usb_midi.rs`) has no test at all, as in C.
- **m-apps** builds its drivers from this repository's `src/*.c`, which are gone here.
- **Elite builds** differ from the Pro only in the package's model name, as before.
