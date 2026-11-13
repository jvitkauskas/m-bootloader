# Provenance

This records where m-bootloader comes from: what was studied, what the code was
written from, how the two were kept apart, where the separation was not strict,
and the checks made against JieLi's code. In short, it is an independent
reimplementation that contains no code from JieLi's SDK or bootloader. It was
written from a specification made by studying the original, by sessions that
were not given the original.

## What came before

- **A C reconstruction of JieLi's bootloader** (2026-10-10). This is a matching
  decompilation of `cpu/wl82/tools/uboot.boot` from JieLi's AC79 SDK 1.1.9.
  Built with JieLi's compiler, it reproduces the original byte for byte. It is
  Apache-2.0, like the SDK.
- **An earlier FM-1 bootloader** (2026-10-10 to 2026-10-14), restructured from
  that reconstruction and modified. It is Apache-2.0.

Both are derived from JieLi's code. m-bootloader is not: see the checks below.
JieLi's bootloader is Apache-2.0, so derived code would have been usable with
its notices. m-bootloader was written separately so that it could be MIT
throughout.

## How 1.0 was written

All of this happened on 2026-10-16.

1. **Specification.** Two sessions with access to the original each wrote a
   draft from the same brief:
   - Claude Opus 5.5;
   - OpenAI Codex (GPT-6.1 Sol), in its own folder and told not to read the
     other draft.

   The sources they could use were the reconstruction and its annotations, the
   earlier bootloader, JieLi's SDK headers and documentation, public notes on
   the chip, M-Flasher, and stock flash images and packages. The brief allowed
   hardware facts, formats, interfaces and behaviour, including the order of
   operations and the delays the hardware needs. It forbade code and anything
   that copies it: "no C or assembly copied or paraphrased line by line, no
   function names, variable names, comments or module structure". The drafts
   were merged, with Codex's as the base, and the merged text was checked for
   code fragments and for names from the sources. The result is
   specification 1.0.
2. **Two implementations, in two folders.** Each folder held:
   - specification 1.0, the instructions and a file for questions;
   - JieLi's toolchain, to run, not to link its libraries;
   - four flash images with the stock bootloader's bytes blanked.

   M-VAVE's packages were held back because they contain the stock
   bootloader. Implementation A was written with Claude Code (Claude Sonnet
   5.5), implementation B with Codex (GPT-6.1 Sol). The instructions told each
   to work only inside its folder. They named the folders holding the sources
   and forbade opening them, the other implementation's folder, any `.boot` or
   `.fwsc` file, and the SDK's C sources that do the same jobs. SDK header
   files were allowed, for register names and structure layouts.
3. **Questions.** Each implementation wrote its questions to its questions
   file. The specification side answered them from the original, as facts only.
   Each answer became a numbered amendment in both folders:
   - D1–D3 answer B's questions;
   - D4–D10 answer A's;
   - D11–D13 came from the review;
   - D14 came from hardware tests of the start-up clock.

   The last version handed over was 1.4.
4. **Review and merge.** The specification side reviewed both implementations,
   against the specification and against the original. The overlap with the
   original was under 1%, none of it code. B was chosen as the base. B's agent
   then ported A's simulator and tests. It worked from a list of the required
   behaviour, with A's code allowed and the reviews not.

### Records

The specification as handed over, the instructions, the questions and the
session logs are kept privately. SHA-256 of the specification-side documents:

| Document | SHA-256 |
|---|---|
| Brief | `efba379bf030e2768e692fd4185fa9d69ddf9ee72cf06bfcfaa1db19c5006e04` |
| Claude draft | `a24dfa5a06ec40a62125d536c1a996d6052cad27123ac5690e6bacd025bae9ed` |
| Codex draft | `90d6d7ef9e67e683b6313a0514e0f123b257d6284a09b5881cfc4d7556b5e702` |
| Specification 1.0, as handed over | `7ae492f34c82bf273bdf9d406f1d64e34883ab6440f9ab6a4c1c3520b5e40b85` |
| Specification 1.4, the last handed over | `59614e2087c1c83f8ebfe7e9655aa4c15876f3ef139d81e5cdbafce32c934925` |

[SPEC.md](SPEC.md) is not the text that was handed over. After the code was
written, it was rewritten as a standalone document that can be read from start
to finish. That rewrite folded in or removed the clean-room rules, the source
tags, the amendments and the mentions of models, and lost no technical value.

## Where the separation was not strict

- **The separation rested on instructions.** Both implementations ran on the
  machine that held the sources. Their logs show no access to them. B's log
  records one search for file names outside its folder, and no file it found
  was opened.
- **Some facts describe the original's internals.** Examples are loop counts,
  a trim mask, a parsing defect and the registers saved at entry. They came
  from the reconstruction's annotations and reached the implementations through
  the specification and amendments D2 and D5. They are facts, but more than an
  interface needs. SPEC.md now states them as requirements and observed
  effects.
- **The specification side edited code once.** When 1.0 was published, it
  replaced amendment numbers with section numbers in 20 comments and test
  messages. No code changed.
- **After 1.0, the record is thinner.** 1.1 followed the process: the fact went
  into the specification in its own commit, and the clean implementation fixed
  the code. Later work was done by sessions whose exposure was not recorded,
  and 18 of the 44 commits up to 2.4 that change the specification also change `src/` or
  `include/`. The package tests added after 1.0 read M-VAVE's `.fwsc`
  packages, which contain the stock bootloader. Those tests are not part of the
  bootloader that is built.
- **`src/usb_midi.c`** was added after 1.0 for MIDI mode. It was written from
  the register facts in JieLi's SDK and in felucca-bsp (MPL-2.0), with no code
  taken from either. The stock bootloader has no USB code, so this part does
  not reimplement it.

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
- **Against the earlier bootloader:** two names, `board_wait` and `KEY_SELECT`,
  appear in both. They were introduced by implementation B with a different
  signature and key set. The specification's description of the menu (select,
  previous/next keys, LEDs during waits) accounts for them.
