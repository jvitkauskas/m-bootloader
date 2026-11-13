# Instructions for the merge session

Start the agent in this folder and tell it: **"Read MERGE.md and follow it."**

---

You are the clean-side implementer. This implementation was chosen as the
base of m-bootloader. Your job now is `WORKLIST.md`: fix, port and test, in
this folder.

## Boundaries

PROMPT.md §1 still applies, with one change: you **may now read
`/root/m-bootloader`**, the other clean implementation of the same
specification (written by another clean-side agent, MIT). Use its code, tests
and simulator freely; credit it in LOG.md where you take something.

Still never open: `/root/fm1-bootloader`, `/root/fm1-toolchain`, `/root/fm1`,
`/root/m-bootloader-work`, `/root/review`, `/root/fm-flasher`,
`/root/flasher-testdata`, `/root/smk37-backup`, any `*.boot` or `*.fwsc` file,
and on GitHub the `fm1-bootloader` repository and the `m-flasher` `loader/`
and `data/` directories. `/root/review` holds the dirty-side reviews: they
cite the original sources, so they are off limits; WORKLIST.md is their
clean summary.

## How to work

- Read SPEC.md again from amendment D11 on (new in version 1.3), then
  WORKLIST.md.
- Keep the build's size, RAM and entry checks; the SMK-37 container stays
  ≤ 14,384 bytes.
- Questions go to QUESTIONS.md as before (SPEC.md §0.2); stop and tell the user.
- Log decisions and sources in LOG.md; commit in small steps; do not push.
- When done: all tests pass (including the new boot simulation), the four
  artifacts build, and you report what changed, what each item freed or cost
  in flash, and what remains for the hardware tests.
