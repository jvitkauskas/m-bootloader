# Provenance

This records where m-bootloader comes from: what was studied, what the code was
written from, where the separation between the two did not hold, and the checks
made against JieLi's code. In short, it is an independent reimplementation that
contains no code from JieLi's SDK or bootloader. It is written from a
specification that was made by studying the original. The separation between
studying and writing is the author's account and was not recorded at the time;
see [Records](#records).

## What came before

- **A C reconstruction of JieLi's bootloader** (2026-10-10). This is a matching
  decompilation of `cpu/wl82/tools/uboot.boot` from JieLi's AC79 SDK 1.1.9.
  Built with JieLi's compiler, it reproduces the original byte for byte. It is
  Apache-2.0, like the SDK.
- **An earlier FM-1 bootloader** (2026-10-10 to 2026-10-14), restructured from
  that reconstruction and modified. It is Apache-2.0. Its recovery menu on the
  display (2026-10-12) is the author's own addition.

Both are derived from JieLi's code. m-bootloader is not: see the checks below.
JieLi's bootloader is Apache-2.0, so derived code would have been usable with
its notices. m-bootloader is a separate implementation so that it can be MIT
throughout.

## How 1.0 was written

1. The specification ([SPEC.md](SPEC.md), version 1.0, 2026-10-16) was written
   by studying JieLi's bootloader and the reconstruction above, the public
   sources in its Appendix B, JieLi's SDK headers (for register facts) and the
   hardware. It records interfaces, formats, hardware transactions and
   observable behaviour. It contains no code, disassembly or text from JieLi.
2. The code was written from the specification in separate sessions that did not
   have the original or the reconstruction. Two such implementations were
   written and merged ([NOTICE.md](../NOTICE.md)).

### Records

The repository starts at the 1.0 commit, which holds the specification and the
code together. The following was not recorded when 1.0 was written. It is to be
completed from the author's records where they exist:

- who or which sessions studied the original and wrote the specification;
- which sessions wrote each of the two implementations, and what each was given
  (the specification revision, any other files);
- how questions from the implementing side were answered.

## Where the separation did not hold

- **Names from the earlier bootloader.** `board_wait(ms, leds)` and
  `KEY_SELECT`, with the order of the menu keys, appear in m-bootloader 1.0 and
  in the earlier bootloader's recovery menu, but not in the specification. That
  menu is the author's own code, not JieLi's.
- **The specification and the code changed together after 1.0.** 18 of the 44
  commits that change the specification also change `src/` or `include/`. The
  facts added in them came from hardware tests and from the behaviour of
  firmware packages and updates. From now on, the rule below applies.
- **The tests use stock material.** Some tests read M-VAVE's firmware packages
  and flash images from `testdata/`, which is not in the repository. The chip
  simulator in `tests/boot_sim.c` names registers as JieLi's SDK headers do
  (`PWR_CON`, `CLK_CON0`, `SPI_CON`, `CACHE_CON`). None of this is part of the
  bootloader that is built.
- **`src/usb_midi.c`** was written from the register facts in JieLi's SDK and in
  felucca-bsp (MPL-2.0), with no code taken from either. The stock bootloader has
  no USB code, so this part does not reimplement it.

## Changing the specification

- A new fact goes into [SPEC.md](SPEC.md) in its own commit, before the code
  that relies on it. The commit message says where the fact came from: a
  hardware test, a public document, the behaviour of a firmware package or
  update, or a study of JieLi's bootloader.
- A fact from studying JieLi's bootloader describes an interface or an
  observable effect. It does not describe how the original does it internally.
  Whoever studied the original for it does not write the code for it.
- Code changes cite the specification section they implement.

## Checks against JieLi's code

These checks were made on 2026-11-13 against m-bootloader 1.0, 1.1, 1.2, 2.3
and `395e6cc`.

**Compared against:**
- JieLi's `uboot.boot` from eleven SDK releases (1.1.8 to 1.2.14), and the 1.2.14
  debug build;
- the SDK's download-mode loader `wl82loader.bin`;
- the reconstruction and the earlier bootloader above.

**Method:**
- m-bootloader 1.x was built with JieLi's clang 4.0.1, the compiler that
  built `uboot.boot`, and 2.3 and later with pi32v2-llvm.
- Each pair is compared by the share of instructions inside shared runs of 8,
  with the immediate values compared too.
- Control pairs show what shared code looks like under the same test.

| Pair | Shared |
|---|---|
| Control: `uboot.boot` 1.2.1 vs 1.2.13 | 78.8% |
| Control: SDK loader vs `uboot.boot` | 6.6% |
| m-bootloader 1.2 (same compiler) vs `uboot.boot` 1.1.9, 1.2.1, 1.2.13 | 0.0% |
| m-bootloader's flash loader 1.2 vs the SDK loader | 0.4% |

**Other results:**
- **Functions:** no m-bootloader function resembles one in `uboot.boot` more
  than unrelated firmware does. The highest 3-gram similarity is 0.13, against
  a baseline of 0.23.
- **Hand-written assembly:** the start-up code shares only the exception return
  (`rete`, `reti`, `rti`), which the ROM's entry requires.
- **Bytes and strings:** the shared byte runs are only these:
  - interface strings (`app_dir_head`, configuration key names,
    `usb_update_mode`);
  - a `memcmp` loop;
  - the two writes of the unit key to the cipher key registers.

  None of the bootloader's debug messages appear.
- **Against the reconstruction:** 0.03% of identifier-preserving 12-token runs
  at 1.0 and 0.01% at `395e6cc`, and no shared comment text.
- **The specification against the SDK files it cites:** no shared run of six or
  more words.
