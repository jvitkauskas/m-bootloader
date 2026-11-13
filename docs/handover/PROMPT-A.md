# Instructions for the implementation session

Start a new agent in this folder and tell it: **"Read PROMPT.md and follow it."**

---

You are the **clean-side implementer** of a clean-room project. Your job is to
write two programs from scratch, in C and pi32v2 assembly, under the MIT
licence, from the specification in this folder:

- **A: m-bootloader**, the bootloader for the M-VAVE FM-1 and SMK-37 Pro /
  Elite synthesizers (JieLi AC791N), one build per board;
- **B: the M-Flasher flash loader**, the small program the PC runs on the chip
  through its USB download mode.

## 1. Boundaries: read this first

Nobody on your side may have seen the original code; that is the whole point
of the project.

- **Work only inside this folder.** Do not read, list or search anything
  outside it on this machine. This machine also holds the original sources,
  and you must never open them, even by accident through a search. In
  particular never touch: `/root/fm1-bootloader`, `/root/fm1-toolchain`,
  `/root/fm1`, `/root/m-bootloader-work`, `/root/fm-flasher`,
  `/root/flasher-testdata`, `/root/smk37-backup`, or any `*.boot` or
  `*.fwsc` file anywhere.
- **Do not read `/root/m-bootloader-b`** either: another implementation of the same
  specification is being written there, independently, and the two will be
  compared.
- **On the internet**, read only the public sources in SPEC.md Appendix B.
  Never open the `fm1-bootloader` repository, and in the `m-flasher`
  repository read only `src/` (never `loader/` or `data/`: they contain the
  old loader and bootloader builds).
- Everything else about what you may use is in **SPEC.md §0**. It is binding.

## 2. What is here

| Path | What |
| --- | --- |
| `SPEC.md` | The specification: read all of it before writing code. §0 is the procedure, §14 the completion criteria. |
| `CLEANROOM.md` | How the specification was made, and the rules. |
| `QUESTIONS.md` | Where you write questions for the specification's authors. |
| `toolchain/` | JieLi's Linux toolchain 20250324.1: `toolchain/common/bin/clang` (clang 4.0.1, `-target pi32v2 -mcpu=r3`), `toolchain/pi32v2/bin/lto-wrapper` (linker driver, needs Python 3), `toolchain/common/bin/objcopy`, `objdump`. Do **not** link anything from `toolchain/pi32v2/lib/` (SPEC.md §0.1). |
| `testdata/` | Four flash images (1 MiB each) with the stock bootloader blanked (SPEC.md Appendix C). Never commit them, never publish them. |
| `LICENSE` | MIT. |

`toolchain/`, `testdata/` and `build/` are ignored by git.

## 3. How to work

1. Read SPEC.md completely, then CLEANROOM.md. Start `LOG.md` and keep it up
   to date: decisions, sources consulted for each fact, questions asked,
   answers received.
2. Suggested order (the code's structure is your choice):
   1. Portable C for the logic that can be tested on this PC: CRC16, CRC32,
      the ENC and CrcDecode ciphers, the LZ4 block decoder, the flash
      header and file-table parsing, the configuration parser, the
      update-request logic, the app-state classification, the wipe-range
      and calibration-guard decisions. Build these with the host's own C
      compiler as well and test them against SPEC.md §5.1, §10.2 and §12 and
      against `testdata/` (§12.2, §12.3, §12.4).
   2. Program B (the loader): two builds, at 0x01C20000 and 0x01C28000.
   3. Program A, for `fm1` and `smk37`.
   4. A host-side tool that wraps A's payload into the BANKCB container and
      checks it, and a Makefile that builds everything into `build/` and
      fails on any size or RAM limit (§11.1).
3. You cannot reach the hardware. When something needs a hardware test, write
   the exact steps and what to look for in `HARDWARE-TESTS.md` (SPEC.md §12.5
   is the plan), then stop and ask the user to run them and report back. Record
   the results there.
4. **When SPEC.md is unclear, incomplete or contradicted by a test**: do not
   guess on anything marked MUST, and never look for the original. Write the
   question in `QUESTIONS.md` (template there), stop, and tell the user that
   questions are waiting. The answers come back as amendments in SPEC.md,
   Appendix D. Questions about free design choices (MAY) you decide yourself
   and note in LOG.md.
5. Commit to this repository's git in small steps, with the configured
   identity. Do not push. Start each source file with
   `SPDX-License-Identifier: MIT`.

## 4. Done

When SPEC.md §14 is met: both programs build reproducibly
(`build/fm1/uboot.boot`, `build/smk37/uboot.boot`,
`build/loader/mflr-loader-a.bin`, `build/loader/mflr-loader-b.bin`), all
offline tests pass, the size and RAM checks pass, and `HARDWARE-TESTS.md`
lists what was tested on hardware and what is still outstanding. Finish with
a summary for the user: what was built, what was tested, open questions,
known limitations.
