# Hand-over records

These are the documents the two implementations of m-bootloader 1.0 worked
from, kept verbatim so they match the hashes below and in
[PROVENANCE.md](../PROVENANCE.md). They are a record, not the current
specification: [SPEC.md](../SPEC.md) is. Paths in them are of the machine they
were written on; links in them may not resolve here.

All of them date from 2026-10-16. Implementation A's folder and implementation
B's folder each had its own git history.

| File | What | From | SHA-256 |
|---|---|---|---|
| `SPEC-1.0.md` | the specification as first handed over | `d020b11` | `7ae492f34c82bf273bdf9d406f1d64e34883ab6440f9ab6a4c1c3520b5e40b85` |
| `SPEC-1.4.md` | the last version handed over, with amendments D1–D14 | A, `6015647` | `59614e2087c1c83f8ebfe7e9655aa4c15876f3ef139d81e5cdbafce32c934925` |
| `CLEANROOM.md` | the procedure, given to both | `bb29776` | `5758391aae801a5fb1b1df31b39c88042d3cb7ba029326a44b3609be25264c8f` |
| `PROMPT-A.md` | implementation A's instructions | A, `ea1c659` | `c73ea2e47fd1d216214f2db2e6671a7a7ba37f740e204d6850805461eba0da39` |
| `PROMPT-B.md` | implementation B's instructions; they differ from A's only in the folder they forbid | B, `f71451d` | `a425c20588c06b8461dbfa300f3413a13c1f879ed6c1b8d5d25e8aa0bd3bf26e` |
| `QUESTIONS-A.md` | A's questions (Q4–Q10) and the answers | A, `7592d5b` | `d19c66fe337c690d91d54bec3eb475bab79f0f956862ab16d044b2461c67988a` |
| `QUESTIONS-B.md` | B's questions (Q1–Q3) and the answers | B, `50e7d24` | `00cd8ecf7805eb2316b5b1e66fb8a7991b4ce3d15dcdb0779823f50cc0018824` |
| `MERGE.md` | the instructions for merging A into B | B, `03f0013` | `f5cdf9249babcc1a8a9b6c31f5a3af5ce436897a2db5582efe7db0ed05c5511a` |

Kept privately and pinned by the hashes in [PROVENANCE.md](../PROVENANCE.md):
- the brief and the two drafts of the specification;
- the reviews;
- the session logs.

These quote or describe JieLi's bootloader. The test images are kept
privately too, because they hold units' own data and M-VAVE's firmware.
