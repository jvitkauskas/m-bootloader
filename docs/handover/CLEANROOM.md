# Clean-room record

## Why

The previous open bootloader, fm1-bootloader, was derived from a byte-exact
reconstruction of JieLi's AC79 SDK bootloader (`uboot.boot`), published under
Apache 2.0 as the SDK is. M-Flasher's flash loader reused part of it. To
leave no question about where the code comes from, both are rewritten in a
clean room: the people (and models) who studied the original write a
specification of facts and behaviour; someone who has never seen the
original writes new code from the specification alone.

## Roles

- **Dirty side** (may read everything, writes no code): a Claude Opus 5.5
  session and an OpenAI Codex session (GPT-6.1 Sol, high reasoning effort),
  run by Julius Vitkauskas on 2026-10-16.
- **Clean side** (writes the code): a separate session that never had access
  to the original bootloader, its reconstruction, JieLi's binaries or
  M-Flasher's current loader (SPEC.md §0.1).

The previous bootloader's code was first published on 2026-10-10, after the
training cutoff of the Claude models used (June 2026), so a fresh model
session cannot have learned it.

## How the specification was made

1. A brief (scope, sources, clean-room rules, outline) was written for both
   dirty-side authors.
2. Each wrote a specification from it independently, Codex in a separate
   directory with an instruction not to read the other draft.
3. The drafts were compared; facts on which they differed were rechecked
   against the sources and the flash images, and the test vectors were
   recomputed. The Codex draft became the base; the clean-room procedure,
   source lists, suggested wording, font and test-data notes came from the
   Claude draft. The decisions are listed in SPEC.md §13.2.
4. The result, SPEC.md version 1.0, contains no code, names or structure from
   the original; register values, formats and behaviour are facts.

SHA-256 of the dirty-side documents (kept privately with the sources):

| Document | SHA-256 |
|---|---|
| Brief | `efba379bf030e2768e692fd4185fa9d69ddf9ee72cf06bfcfaa1db19c5006e04` |
| Claude draft | `a24dfa5a06ec40a62125d536c1a996d6052cad27123ac5690e6bacd025bae9ed` |
| Codex draft | `90d6d7ef9e67e683b6313a0514e0f123b257d6284a09b5881cfc4d7556b5e702` |
| SPEC.md 1.0 | `7ae492f34c82bf273bdf9d406f1d64e34883ab6440f9ab6a4c1c3520b5e40b85` |

## Rules for writing the code

SPEC.md §0: write everything new under MIT; read only the specification and
the public and MIT sources it lists; never the original, its reconstruction,
JieLi's binaries or SDK code that does the same jobs; do not link the
toolchain's libraries; keep a log (`LOG.md`); ask through `QUESTIONS.md`.
Answers come back as numbered amendments in SPEC.md, Appendix D, containing
facts only.

## Setting up the clean side

- This folder holds everything the implementer needs: the specification,
  JieLi's toolchain (`toolchain/`) and the blanked test images (`testdata/`,
  SPEC.md Appendix C). Neither of the last two is in git: the images contain
  M-VAVE's firmware, the toolchain is JieLi's.
- Start the implementer with PROMPT.md. It confines the work to this folder
  and names the paths on the original machine that must never be opened.
  Copying the folder to a machine without the original sources is safer
  still.
- Hardware tests need the USB_KEY dongle at hand (SPEC.md §4.3, §12.5).
