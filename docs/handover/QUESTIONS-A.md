# Questions for the dirty side

The implementer writes questions here (SPEC.md §0.2); answers come back as
amendments in SPEC.md, Appendix D.

Numbering continues from the other implementation's Q1–Q3 (answered as D1–D3):
this implementation's questions start at Q4.

Template:

```
## Q<n>: <short title>
Section: <SPEC.md section>
What is unclear or wrong:
Why it matters:
What was observed (for hardware: steps, register values, symptoms):
Status: open | answered in amendment D<n>
```

All questions below have been answered (SPEC.md Appendix D, D4–D10). What the
answers changed in the code:

* **Q4 / D4**: the send-callback model was right and is now the only one (the
  `LDR_ANSWER_MODEL` option and its test build are gone).
* **Q5 / D5**: the entry sequence was wrong in detail and has been rewritten:
  bss cleared with registers only before `rti`, `rete = 0` (not `retx`), no
  stack pointer touched before `rti`, then `ssp` and `sp` after it; the
  descriptor stays in r0. `make report` checks the sequence in the disassembly.
* **Q6 / D6, Q7 / D7, Q8 / D8, Q10 / D10**: the implementation already matched.
* **Q9 / D9**: bit 16 is no longer restored after XIP (cleared is the known-good
  state).

## Q4: How are the 16-byte replies of data-IN commands delivered?
Section: 10.1, 10.2
What is unclear or wrong: §10.1 says the ROM calls the handler with a command
block and "a writable 16-byte answer pointer", and that B's callback calls
"move the command's data". §10.2 says reply-based commands "return sixteen
data-IN bytes" and that B "SHOULD honor shorter requested replies by sending at
most min(requested IN count, 16)". It does not say whether B (a) sends its reply
itself through the send callback, like the raw-read data, or (b) writes the
reply into the answer buffer and returns 1, the ROM then sending the buffer as
the data phase.
Why it matters: with the wrong model every reply-based command (FC0A, FC40,
FC41, FB00 ...) either hangs the transfer (reply never sent) or desynchronises
it (reply sent twice: B sends 16 bytes and the ROM sends 16 more against a
16-byte host transfer). The loader would not be usable at all.
What was observed: nothing (no hardware). Implemented: model (a), replies via
the send callback; model (b) is a build option (`-DLDR_ANSWER_MODEL`) and both
pass the offline tests against a stub ROM that behaves as the respective model
says. Questions: which model is right? In model (b), is the length of the data
phase taken from the command block's transfer count, and must the answer
buffer be left untouched by handled commands that send their data themselves
(FD05 reads, OUT commands)? In model (a), is the answer buffer needed at all?
Status: answered in amendment D4

## Q5: What does the stock entry sequence write before leaving the exception context?
Section: 4.1
What is unclear or wrong: "use a valid exception return to enter normal
execution (stock sets exception-return state to zero and interrupt-return
target to its continuation), and establish its own system and main stacks".
"Exception-return state" is not an SFR name. The pi32v2 notes list `reti`,
`rete`, `retx`, `rets`, `psr`, `cnum`, `icfg`, `usp`, `ssp`, `sp`.
Why it matters: if the entry sequence is wrong, A never runs and the ROM boot
gets no further (recoverable only through the USB_KEY). It is the first thing
the first hardware test exercises.
What was observed: nothing. Implemented (src/boot/start.S), in this order:
`[boot_desc] = r0`; `ssp = exc_top`; `usp = main_top`; `sp = ssp`;
`reti = cont`; `retx = 0`; `rti`; at `cont`: `sp = usp`, clear own bss, `r0 =
[boot_desc]`, `call boot_main`. Interrupts are left as found until A executes
`cli` just before XIP set-up. Questions: (1) is "exception-return state to
zero" `retx = 0`, or a write to `psr`/`icfg`/another register, and with what
value? (2) is `sp = ssp` before the `rti` and `sp = usp` after it right, or does
the hardware switch the stack pointer on `rti`? (3) is anything else the ROM
relies on after A returns control to it never (A never returns), e.g. must
interrupts stay masked, is the ROM's vector/interrupt state needed by anything
A calls (the SPI/timer code polls, no interrupts)?
Status: answered in amendment D5

## Q6: Which drive-strength register does the SPI0 clock pin use?
Section: 3.4, 2.2
What is unclear or wrong: "enable the PD4 drive-strength bit"; the GPIO block
has two drive registers, +0x18 ("drive option 0") and +0x1C ("drive option").
Why it matters: too weak a clock edge at 30 MHz can make the quad/XIP reads
flaky; setting the wrong register is harmless but may not do what the stock
code does.
What was observed: nothing. Implemented: bit 4 of +0x1C (`GP_HD`) on port D.
Status: answered in amendment D6

## Q7: Timer 0 period register
Section: 3.3
What is unclear or wrong: "Split long waits to avoid period overflow" without
the register width, and whether a prescaler is active after reset (so that one
tick is one LSB clock).
Why it matters: every calibrated delay after the clock change (flash busy
polls at 100 us, the panel's 150 ms / 120 ms waits, the key scan at 20 ms and
1 ms) depends on it.
What was observed: nothing. Implemented: control = 0x4000 (stop, clear), count
= 0, period = 60 x microseconds in steps of at most 1000 us (at most 60000, so
16 bits suffice), control = 1, wait for bit 15, control = 0x4000.
Status: answered in amendment D7

## Q8: P33 bridge handshake
Section: 3.1
What is unclear or wrong: after setting serial-control bit 4 the text says
"wait for bit 1 to clear". Is bit 1 set by the hardware while a byte is in
flight (so waiting for it to clear after the start works), or can it still be
clear right after the write and only rise later? Is bit 4 self-clearing?
Why it matters: reading it too early gives wrong returned bytes (the efuse key,
the trim) or lost writes (voltage, watchdog, pin latches).
What was observed: nothing. Implemented: set bits 3:2, write the data byte, set
bit 4, `csync`, poll bit 1 until it is clear (bounded), read the data register.
Status: answered in amendment D8

## Q9: Value of mux-control-0 bit 16 for raw SPI0 access
Section: 3.4, 3.6
What is unclear or wrong: §3.6 says the SFC pin route is "mux-control-0 bit 16
cleared"; §3.4 does not mention bit 16 for SPI0. Does raw SPI0 access need bit
16 set, or is it a don't-care while the SFC is disabled?
Why it matters: after a failed XIP boot A takes SPI0 back for the menu; if bit
16 matters, the menu cannot read the flash then.
What was observed: nothing. Implemented: bit 16 is saved before XIP set-up,
cleared for XIP and restored when XIP is released.
Status: answered in amendment D9

## Q10: Mode 1 of the ROM read callback
Section: 4.1
What is unclear or wrong: "metadata reads use 1, plain configuration data uses
0". Does a mode-1 read return the 32-byte record already decoded (ENC FFFF) or
raw?
Why it matters: without the configuration A runs on the recovery profile.
What was observed: nothing. Implemented: accept either (if the record's CRC
does not check as read, decode it with FFFF and try again).
Status: answered in amendment D10
