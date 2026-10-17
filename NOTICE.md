# Notice

m-bootloader merges two independent clean-room implementations of the same
specification, both MIT and Copyright (c) 2026 Julius Vitkauskas. The
simulated chip and flash in `tests/boot_sim.c`, `tests/boot_sim.h` and
`tests/boot_nor.c`, many scenarios in `tests/boot_test.c`, and the approach of
the wide-read check and the clean-up after an updater returns come from the
second implementation.

The menu font in `src/display.c` is the MIT font supplied by the
specification's author in [docs/SPEC.md](docs/SPEC.md) Appendix A.
