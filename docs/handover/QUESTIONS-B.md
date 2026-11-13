# Questions for the dirty side

The implementer writes questions here (SPEC.md §0.2); answers come back as
amendments in SPEC.md, Appendix D.

Template:

```
## Q1: <short title>
Section: <SPEC.md section>
What is unclear or wrong:
Why it matters:
What was observed (for hardware: steps, register values, symptoms):
Status: open | answered in amendment D<n>
```

## Q1: Exact retained UART-request representations

Section: SPEC.md §6.2 (last paragraph), §7 step 10.

What is unclear or wrong:
The required handling names `UARTUPDATE` and `UART_UPDATE_CUSTOM` but does not
define their retained byte representations, offsets, or comparison lengths.
Are these literal ASCII strings at R, symbolic names for numeric values, or
another encoding? Please give the exact bytes/values, any terminator or padding
requirements, and the bytes that must be cleared before reset for each case.

Why it matters:
A MUST recognize and clear/reset these requests without looping. Guessing a
string or numeric encoding could miss a real request or mistakenly consume an
unrelated retained status/update request. The exact recognition rules are not
specified as a free design choice.

What was observed (for hardware: steps, register values, symptoms):
Complete specification review found an explicit byte string for USB recovery
in §4.2 and BOOTANY in §9.1, but no corresponding UART-request representation.
No hardware was accessed and no original source was consulted.

Status: answered in amendment D1

## Q2: Staged updater storage encoding and restart boundaries

Section: SPEC.md §5.1, §6.3.

What is unclear or wrong:
Please explicitly specify the stored representation of the staged updater's
outer record, skipped inner/file record, compression header, block length
words, and compressed bytes. Are all read as raw/plain bytes without ENC? If
any component is encoded, specify its key, address-dependent seed (if any),
and restart boundaries. Also confirm that the request's absolute updater
address is read using this representation regardless of the selected app key.

Why it matters:
The outer record and compression header MUST be validated and the complete
updater MUST pass length/CRC checks before execution. §5.1 lists top-level and
application encryption rules, but does not explicitly cover this separate
staged stream. Its encoding must not be guessed from a record's shape.

What was observed (for hardware: steps, register values, symptoms):
Using only the Appendix C hash-verified `testdata/smk37-stock.bin`, the 32 raw
bytes at 0xB9000 and 0xB9020 pass CRC16/XMODEM over bytes 2–31 against the
stored halfword at +0. Applying ENC FFFF independently to either 32-byte
object makes that check fail. This suggests raw/plain storage in this fixture;
please confirm the normative rule for the supported updater interface. Only
record/header checksums were examined; updater instructions were not studied
or disassembled. No hardware was accessed.

Status: answered in amendment D2

## Q3: Full-chip CRC32 acceptance values for blanked fixtures

Section: SPEC.md §12.1, §12.4 second bullet, Appendix C.

What is unclear or wrong:
§12.4 requires FC41's full-chip CRC to equal the table above it, but §12.1
explicitly says that table's CRC32 values belong to the original images.
Appendix C's supplied fixtures have their bootloader bytes replaced by FF and
therefore have different raw CRC32 values. Please amend the acceptance wording
or supply the expected CRC32 values for the blanked fixtures.

Why it matters:
B MUST checksum the actual raw flash bytes. Correct checksum behavior on the
allowed fixtures cannot satisfy the original-image CRC values. Recovering or
reading the original boot bytes is prohibited and unnecessary.

What was observed (for hardware: steps, register values, symptoms):
All four supplied images match their Appendix C SHA-256 hashes and have FF
throughout [0xA0,0x38D0). Python's IEEE CRC32 over each complete 1,048,576-byte
blanked fixture gives:

| Fixture | Observed raw CRC32 |
|---|---|
| `fm1-stock.bin` | `F98496A0` |
| `smk37-stock.bin` | `8E57BC96` |
| `smk37-snoop37.bin` | `6475373C` |
| `smk37-baudgirl.bin` | `1EABFB0A` |

The implementation-side CRC16 and cipher calculations match all §5.1 vectors;
the CRC32 check for `123456789` is `CBF43926`. These are offline observations,
not results from an implemented FC41 handler or hardware.

Status: answered in amendment D3
