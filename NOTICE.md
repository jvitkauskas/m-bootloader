# Notice

m-bootloader merges two independent implementations of the same
specification, both MIT and Copyright (c) 2026 Julius Vitkauskas (see
[docs/PROVENANCE.md](docs/PROVENANCE.md)). The
simulated chip and flash in `tests/boot_sim.c`, `tests/boot_sim.h` and
`tests/boot_nor.c`, many scenarios in `tests/boot_test.c`, and the approach of
the wide-read check and the clean-up after an updater returns come from the
second implementation.

The menu font in `src/display.c` is the MIT font supplied by the
specification's author in [docs/SPEC.md](docs/SPEC.md) Appendix A.

`src/usb_midi.c` was written from hardware facts (register addresses, bit
meanings and the order the controller needs them in) found in JieLi's AC79 SDK
(Apache-2.0) and in felucca-bsp's `chip/fm1_usb.h` (MPL-2.0). No code was taken
from either. Its descriptors follow the USB-MIDI 1.0 specification.
