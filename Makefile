# SPDX-License-Identifier: MIT
SHELL := /bin/bash
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
# The pi32v2 LLVM toolchain (see README), unpacked in toolchain/ or at TOOLCHAIN=<directory>.
export TOOLCHAIN ?= toolchain
CLANG := $(TOOLCHAIN)/bin/clang
LINK := $(TOOLCHAIN)/bin/ld.lld
OBJCOPY := $(TOOLCHAIN)/bin/llvm-objcopy
NM := $(TOOLCHAIN)/bin/llvm-nm
# Upkr 0.2.3 (see README), at UPKR=<path> or on the PATH.
export UPKR ?= upkr
HOSTCC ?= cc
# The pi32v2 rustc (see README), on the same LLVM as the toolchain: the linker optimises Rust and C together.
RUSTC ?= rustc-pi32v2
# Any recent rustc, for the tests on the host.
HOST_RUSTC ?= rustc
CARGO ?= cargo
RUST_SRC := $(wildcard rust/src/*.rs rust/src/*/*.rs) VERSION
# What the RAM stage tells a host its version is.
export STAGE_VERSION := $(shell cat VERSION)
RUST_FLAGS := --target pi32v2-unknown-none --edition 2024 --crate-name mboot -C opt-level=s -C codegen-units=1 -C debuginfo=0 -Zunstable-options -C panic=immediate-abort --remap-path-prefix=$(CURDIR)=.
RUST_LIB := lib/rustlib/pi32v2-unknown-none/lib
# What the linker needs of Rust's core library, where it optimises Rust and C together.
RUST_CORE_lto := build/rust/core.o
HOST_RUST := build/host/host/libmboot.a
# For the simulated chip's tests, which have their own MIDI mode.
HOST_RUST_SIM := build/host/boot_sim-platform_sim/libmboot.a
HOST_RUST_SIM_SMK := build/host/boot_sim-platform_sim-smk/libmboot.a
HOST_RUST_SIM_REQUESTS := build/host/boot_sim-platform_sim-test_requests/libmboot.a
HOST_RUST_SIM_MIDI := build/host/boot_sim-platform_sim-midi_mode-mu_test_app_ram/libmboot.a
HOST_RUST_SIM_MIDI_SMK := build/host/boot_sim-platform_sim-midi_mode-mu_test_app_ram-smk/libmboot.a
# The simulated chip's library for a test: $(1) is smk37 or fm1, $(2) is _MIDI or nothing.
sim_rust = $(HOST_RUST_SIM$(2)$(if $(filter smk37,$(1)),_SMK,))
HOST_RUST_PSIM := build/host/platform_sim/libmboot.a
# For the card's test and the panel's, which stand in for the pins and the timer.
HOST_RUST_SD := build/host/platform_sim-import_early/libmboot.a
HOST_RUST_SD_SMK := build/host/smk-platform_sim-import_early/libmboot.a
HOST_RUST_MU := build/host/mu_test_app_ram/libmboot.a
HOST_RUST_MU_SMK := build/host/smk-mu_test_app_ram/libmboot.a
CFLAGS := -target pi32v2 -mcpu=r3 -std=c99 -Oz -ffreestanding -fno-builtin -fno-common -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Iinclude
# A test build: make clean, then make TEST_REQUESTS=1 (docs/SPEC.md §11.9).
ifdef TEST_REQUESTS
CFLAGS += -DTEST_REQUESTS
export BOOT_VERSION_SUFFIX := -test
endif
COMMON :=
# The bootloader's two stages (docs/SPEC.md §4.4). Stage 0 is linked alone, so it cannot depend on stage 1.
STAGE0 :=
STAGE1 :=
LOADER := $(COMMON)
INSTALLER := $(COMMON)
# MIDI mode in RAM, for units without m-bootloader (docs/SPEC.md §11.7).
STAGE := $(COMMON)
HEADERS := $(wildcard include/*.h)
SIM_SOURCES := tests/boot_sim.c tests/boot_nor.c
SIM_FLAGS := -std=c11 -g -Wall -Wextra -Werror -DPLATFORM_SIM -DBOOT_SIM -no-pie -Iinclude
LIFECYCLE_SOURCES := tests/boot_sim.c tests/boot_nor.c tests/loader_lifecycle_test.c
A_PRODUCTS := build/fm1/uboot.boot build/smk37/uboot.boot
B_PRODUCTS := build/loader/mflr-loader-a.bin build/loader/mflr-loader-b.bin
# M-UPGRADE packages: only our installer, which writes the bootloader, carried self-extracting. A test build has
# none, as it goes on with M-Flasher, but its installer images are still built and checked.
INSTALLERS := build/fm1/installer.bin build/smk37/installer.bin build/fm1/installer-sfx.bin build/smk37/installer-sfx.bin
ifdef TEST_REQUESTS
C_PRODUCTS :=
else
C_PRODUCTS := build/fm1/uboot.fwsc build/smk37/uboot-pro.fwsc build/smk37/uboot-elite.fwsc
endif
# The RAM stage: for run-ram, and M-UPGRADE-style packages holding only it.
STAGE_RAM := build/fm1/stage-ram.bin build/smk37/stage-ram.bin
STAGE_PRODUCTS := build/fm1/stage.fwsc build/smk37/stage-pro.fwsc build/smk37/stage-elite.fwsc \
	build/fm1/stage-listed.fwsc build/smk37/stage-pro-listed.fwsc build/smk37/stage-elite-listed.fwsc
.PHONY: all stage test test-requests sanitize clean reproducible FORCE
all: $(A_PRODUCTS) $(B_PRODUCTS) $(INSTALLERS) $(C_PRODUCTS) $(STAGE_RAM) $(STAGE_PRODUCTS) build/manifest.json

# Refresh on every invocation; unchanged versions keep the header's mtime.
# Only A's entry includes this header. B remains the hardware-tested MFLR1.3.
build/boot_version.h: FORCE VERSION tools/version.py
	python3 tools/version.py $@
FORCE:

# Rust's core library for pi32v2, built once from the compiler's sources, as bitcode. -Clto=fat makes that the kind of
# bitcode C's is: the linker keeps rustc's usual kind (for ThinLTO) apart from C's, and couldn't optimise across them.
build/rust/sysroot.stamp: rust/sysroot/Cargo.toml rust/sysroot/src/lib.rs Makefile
	@mkdir -p build/rust/sysroot/$(RUST_LIB)
	$(CARGO) clean --quiet --manifest-path rust/sysroot/Cargo.toml --target-dir build/rust/target
	CARGO_TARGET_DIR=build/rust/target RUSTC_BOOTSTRAP=1 RUSTC=$(RUSTC) RUSTFLAGS="-Zunstable-options -Cpanic=immediate-abort --remap-path-prefix=$(CURDIR)=. -Clto=fat" \
		$(CARGO) build --quiet --release --manifest-path rust/sysroot/Cargo.toml --target pi32v2-unknown-none -Zbuild-std=core
	for f in core compiler_builtins; do cp build/rust/target/pi32v2-unknown-none/release/deps/lib$$f-*.rlib build/rust/sysroot/$(RUST_LIB)/lib$$f.rlib || exit 1; done
	touch $@
# The library's one object, for the linker.
build/rust/core.o: build/rust/sysroot.stamp
	$(TOOLCHAIN)/bin/llvm-ar p build/rust/sysroot/$(RUST_LIB)/libcore.rlib $$($(TOOLCHAIN)/bin/llvm-ar t build/rust/sysroot/$(RUST_LIB)/libcore.rlib | grep '\.o$$') > $@
# The crate for one image, with the --cfg names that image has ($(2)). Where the linker optimises the image as a
# whole, $(1) is an object of bitcode for it.
define RUST_template
$(1): $$(RUST_SRC) build/rust/sysroot.stamp Makefile
	@mkdir -p $$(@D)
	$$(RUSTC) $$(RUST_FLAGS) -Clinker-plugin-lto -Clto=fat $$(addprefix --cfg ,$(2)) --sysroot build/rust/sysroot --crate-type lib --emit=obj -o $$@ rust/src/lib.rs
endef
# Where rustc does, $(1) is an archive of native code, cut down to what its C names reach.
define RUST_whole_template
$(1): $$(RUST_SRC) build/rust/sysroot.stamp Makefile
	@mkdir -p $$(@D)
	$$(RUSTC) $$(RUST_FLAGS) -C lto=fat $$(addprefix --cfg ,$(2)) --sysroot build/rust/sysroot --crate-type staticlib -o $$@ rust/src/lib.rs
endef
# For the tests: the same crate for this computer, with the C interface the tests call.
# build/host/<its --cfg names, with - between them>/libmboot.a
build/host/%/libmboot.a: $(RUST_SRC) Makefile
	@mkdir -p $(@D)
	$(HOST_RUSTC) --edition 2024 --crate-name mboot --crate-type staticlib -C opt-level=2 -C debuginfo=2 -C panic=abort -C overflow-checks=on $(addprefix --cfg ,$(subst -, ,$*)) rust/src/lib.rs -o $@

# A uses only our own freestanding objects with LTO, and an independent entry.
define BOARD_template
$$(eval $$(call RUST_template,build/$(1)/mboot.o,$(3)))
$$(eval $$(call RUST_whole_template,build/$(1)/s0/libmboot.a,$(3) stage0))
$$(eval $$(call RUST_template,build/$(1)/s1/mboot.o,$(3) stage1))
$$(eval $$(call RUST_template,build/$(1)/st/ota/mboot.o,$(3) ram_stage stage_ota))
$$(eval $$(call RUST_template,build/$(1)/st/ram/mboot.o,$(3) ram_stage))
build/$(1)/%.o: src/%.c $(HEADERS) Makefile
	@mkdir -p $$(@D)
	$(CLANG) $(CFLAGS) -flto $(2) -c $$< -o $$@
build/$(1)/startup.o: src/startup.S build/boot_version.h Makefile
	@mkdir -p $$(@D)
	$(CLANG) -target pi32v2 -mcpu=r3 -Ibuild $(2) -c $$< -o $$@
build/$(1)/s0/%.o: src/%.c $(HEADERS) Makefile
	@mkdir -p $$(@D)
	$(CLANG) $(CFLAGS) -DSTAGE0 $(2) -c $$< -o $$@
build/$(1)/s1/%.o: src/%.c $(HEADERS) Makefile
	@mkdir -p $$(@D)
	$(CLANG) $(CFLAGS) -flto -DSTAGE1 $(2) -c $$< -o $$@
build/$(1)/stage0.ld: tools/stage0.ld.S
	@mkdir -p $$(@D)
	$(CLANG) -E -P -x c $$< -o $$@
build/$(1)/stage0.elf: $$(addprefix build/$(1)/s0/,$$(addsuffix .o,$$(STAGE0))) build/$(1)/startup.o build/$(1)/s0/libmboot.a build/$(1)/stage0.ld
	$(LINK) --relax -T build/$(1)/stage0.ld -Map build/$(1)/stage0.map -o $$@ $$(filter %.o %.a,$$^)
build/$(1)/stage0.bin: build/$(1)/stage0.elf
	$(OBJCOPY) -O binary -j .text $$< $$@
# What stage 1 may call: stage 0's C names. Rust's own names stay inside stage 0, as stage 1 has its own of each.
build/$(1)/stage0.syms: build/$(1)/stage0.elf
	$(OBJCOPY) --wildcard --localize-symbol='_R*' $$< $$@
build/$(1)/stage1.ld: tools/stage1.ld.S build/$(1)/stage0.bin
	$(CLANG) -E -P -x c -DSTAGE1_ORIGIN=$$$$((0x1c02000+$$$$(stat -c%s build/$(1)/stage0.bin))) $$< -o $$@
build/$(1)/stage1.elf: $$(addprefix build/$(1)/s1/,$$(addsuffix .o,$$(STAGE1))) build/$(1)/s1/mboot.o $$(RUST_CORE_lto) build/$(1)/stage0.syms build/$(1)/stage1.ld
	$(LINK) --plugin-opt=O3 --relax --gc-sections --just-symbols=build/$(1)/stage0.syms -T build/$(1)/stage1.ld -Map build/$(1)/stage1.map -o $$@ $$(filter %.o,$$^)
build/$(1)/stage1.bin: build/$(1)/stage1.elf
	$(OBJCOPY) -O binary -j .text $$< $$@
build/$(1)/stage1.pack: build/$(1)/stage1.bin build/upkr-test tools/pack_stage1.py
	python3 tools/pack_stage1.py $$< $$@
	./build/upkr-test $$@ $$<
build/$(1)/payload.bin: build/$(1)/stage0.elf build/$(1)/stage0.bin build/$(1)/stage1.elf build/$(1)/stage1.bin build/$(1)/stage1.pack tools/check_build.py tools/join_stages.py
	python3 tools/join_stages.py build/$(1)/stage0.elf build/$(1)/stage0.bin build/$(1)/stage1.elf build/$(1)/stage1.bin build/$(1)/stage1.pack $$@ > build/$(1)/size.json
build/$(1)/uboot.boot: build/$(1)/payload.bin tools/bankcb.py
	python3 tools/bankcb.py wrap $(1) $$< $$@
# The installer: a staged updater carrying this uboot.boot (src/installer.c).
build/$(1)/installer_start.o: src/installer_start.S build/$(1)/uboot.boot Makefile
	$(CLANG) -target pi32v2 -mcpu=r3 -Ibuild/$(1) $(2) -c $$< -o $$@
build/$(1)/installer.ld: tools/target.ld.S
	$(CLANG) -E -P -x c -DENTRY_SYMBOL=installer_entry -DRAM_ORIGIN=0x1c0a800 -DRAM_LENGTH=0xc000 -DINSTALLER_BUILD $$< -o $$@
build/$(1)/installer.elf: $$(addprefix build/$(1)/,$$(addsuffix .o,$$(INSTALLER))) build/$(1)/mboot.o $$(RUST_CORE_lto) build/$(1)/installer_start.o build/$(1)/installer.ld
	$(LINK) --plugin-opt=O3 --relax --gc-sections -T build/$(1)/installer.ld -Map build/$(1)/installer.map -o $$@ $$(filter %.o,$$^)
build/$(1)/installer.bin: build/$(1)/installer.elf tools/check_build.py
	$(OBJCOPY) -O binary -j .text $$< $$@
	python3 tools/check_build.py $$< $$@ 0x1c0a800 0x1c16800 installer_entry > build/$(1)/installer-size.json
# As packages carry it: packed like stage 1, behind an unpacker that unpacks it in place (src/sfx.c), so the stream
# fits where firmware stores an updater with room to spare. sfx-layout places the pack and checks the image.
build/$(1)/installer.pack: build/$(1)/installer.bin build/upkr-test tools/pack_stage1.py
	python3 tools/pack_stage1.py $$< $$@
	./build/upkr-test $$@ $$<
build/$(1)/sfx.ld: tools/sfx.ld.S build/$(1)/installer.bin build/$(1)/installer.pack build/sfx-layout
	$(CLANG) -E -P -x c -DSFX_PACK=$$$$(./build/sfx-layout place build/$(1)/installer.bin build/$(1)/installer.pack) $$< -o $$@
build/$(1)/sfx_start.o: src/sfx_start.S build/$(1)/installer.pack Makefile
	$(CLANG) -target pi32v2 -mcpu=r3 -Ibuild/$(1) $(2) -c $$< -o $$@
build/$(1)/sfx.elf: build/$(1)/sfx_start.o build/$(1)/mboot.o $$(RUST_CORE_lto) build/$(1)/sfx.ld
	$(LINK) --plugin-opt=O3 --relax --gc-sections -T build/$(1)/sfx.ld -Map build/$(1)/sfx.map -o $$@ $$(filter %.o,$$^)
build/$(1)/installer-sfx.bin: build/$(1)/sfx.elf build/sfx-layout tools/sfx_symbols.py
	$(OBJCOPY) -O binary -j .entry -j .pack -j .text $$< $$@
	./build/sfx-layout check build/$(1)/installer.bin $$@ $$$$(python3 tools/sfx_symbols.py $(NM) $$<)
# The RAM stage: linked for the updater's address, and for run-ram's slot.
build/$(1)/st/%.o: src/%.c $(HEADERS) Makefile
	@mkdir -p $$(@D)
	$(CLANG) $(CFLAGS) -flto -DRAM_STAGE $(2) -c $$< -o $$@
build/$(1)/stage_start.o: src/stage_start.S Makefile
	$(CLANG) -target pi32v2 -mcpu=r3 $(2) -c $$< -o $$@
build/$(1)/stage-ota.ld: tools/target.ld.S
	$(CLANG) -E -P -x c -DENTRY_SYMBOL=stage_entry -DRAM_ORIGIN=0x1c0a800 -DRAM_LENGTH=0xc000 -DINSTALLER_BUILD $$< -o $$@
build/$(1)/stage-ram.ld: tools/target.ld.S
	$(CLANG) -E -P -x c -DENTRY_SYMBOL=stage_entry -DRAM_ORIGIN=0x1c28000 -DRAM_LENGTH=0x8000 -DINSTALLER_BUILD $$< -o $$@
build/$(1)/stage-ota.elf: $$(addprefix build/$(1)/st/,$$(addsuffix .o,$$(STAGE))) build/$(1)/st/ota/mboot.o $$(RUST_CORE_lto) build/$(1)/stage_start.o build/$(1)/stage-ota.ld
	$(LINK) --plugin-opt=O3 --relax --gc-sections -T build/$(1)/stage-ota.ld -Map build/$(1)/stage-ota.map -o $$@ $$(filter %.o,$$^)
build/$(1)/stage-ram.elf: $$(addprefix build/$(1)/st/,$$(addsuffix .o,$$(STAGE))) build/$(1)/st/ram/mboot.o $$(RUST_CORE_lto) build/$(1)/stage_start.o build/$(1)/stage-ram.ld
	$(LINK) --plugin-opt=O3 --relax --gc-sections -T build/$(1)/stage-ram.ld -Map build/$(1)/stage-ram.map -o $$@ $$(filter %.o,$$^)
build/$(1)/stage-ota.bin: build/$(1)/stage-ota.elf tools/check_build.py
	$(OBJCOPY) -O binary -j .text $$< $$@
	python3 tools/check_build.py $$< $$@ 0x1c0a800 0x1c16800 stage_entry > build/$(1)/stage-ota-size.json
build/$(1)/stage-ram.bin: build/$(1)/stage-ram.elf tools/check_build.py
	$(OBJCOPY) -O binary -j .text $$< $$@
	python3 tools/check_build.py $$< $$@ 0x1c28000 0x1c30000 stage_entry > build/$(1)/stage-ram-size.json
endef
$(eval $(call BOARD_template,fm1,,))
$(eval $(call BOARD_template,smk37,-DBOARD_SMK,smk))
build/fm1/uboot.fwsc: build/fm1/uboot.boot build/fm1/installer.bin build/fm1/installer-sfx.bin tools/fwsc.py tools/bankcb.py
	python3 tools/fwsc.py build --staged build/fm1/installer-sfx.bin fm1 $< build/fm1/installer.bin $@
build/smk37/uboot-%.fwsc: build/smk37/uboot.boot build/smk37/installer.bin build/smk37/installer-sfx.bin tools/fwsc.py tools/bankcb.py
	python3 tools/fwsc.py build --staged build/smk37/installer-sfx.bin smk37-$* $< build/smk37/installer.bin $@

define LOADER_template
$$(eval $$(call RUST_whole_template,build/loader-$(1)/libmboot.a,loader_$(1)))
build/loader-$(1)/%.o: src/%.c $(HEADERS) Makefile
	@mkdir -p $$(@D)
	$(CLANG) $(CFLAGS) -DLOADER_ADDRESS=$(2) -c $$< -o $$@
build/loader-$(1)/target.ld: tools/target.ld.S
	@mkdir -p $$(@D)
	$(CLANG) -E -P -x c -DENTRY_SYMBOL=loader_entry -DRAM_ORIGIN=$(2) -DRAM_LENGTH=0x8000 -DLOADER_BUILD $$< -o $$@
build/loader-$(1)/loader.elf: $$(addprefix build/loader-$(1)/,$$(addsuffix .o,$$(LOADER))) build/loader-$(1)/libmboot.a build/loader-$(1)/target.ld
	$(LINK) --plugin-opt=O3 --relax --gc-sections -T build/loader-$(1)/target.ld -Map build/loader-$(1)/loader.map -o $$@ $$(filter %.o %.a,$$^)
build/loader/mflr-loader-$(1).bin: build/loader-$(1)/loader.elf tools/check_build.py
	@mkdir -p $$(@D)
	$(OBJCOPY) -O binary -j .text $$< $$@
	python3 tools/check_build.py $$< $$@ $(2) $(3) loader_entry > build/loader-$(1)/size.json
endef
$(eval $(call LOADER_template,a,0x1c20000,0x1c28000))
$(eval $(call LOADER_template,b,0x1c28000,0x1c30000))

stage: $(STAGE_RAM) $(STAGE_PRODUCTS)
build/fm1/stage.fwsc: build/fm1/stage-ota.bin tools/fwsc.py tools/bankcb.py
	python3 tools/fwsc.py stage fm1 $< $@
build/smk37/stage-pro.fwsc: build/smk37/stage-ota.bin tools/fwsc.py tools/bankcb.py
	python3 tools/fwsc.py stage smk37-pro $< $@
build/smk37/stage-elite.fwsc: build/smk37/stage-ota.bin tools/fwsc.py tools/bankcb.py
	python3 tools/fwsc.py stage smk37-elite $< $@
# For Felucca-family firmware, which refuses a package without a flash.bin, as M-VAVE's refuses one with it.
build/fm1/stage-listed.fwsc: build/fm1/stage-ota.bin tools/fwsc.py tools/bankcb.py
	python3 tools/fwsc.py stage --listed-flash fm1 $< $@
build/smk37/stage-pro-listed.fwsc: build/smk37/stage-ota.bin tools/fwsc.py tools/bankcb.py
	python3 tools/fwsc.py stage --listed-flash smk37-pro $< $@
build/smk37/stage-elite-listed.fwsc: build/smk37/stage-ota.bin tools/fwsc.py tools/bankcb.py
	python3 tools/fwsc.py stage --listed-flash smk37-elite $< $@
build/manifest.json: $(A_PRODUCTS) $(B_PRODUCTS) $(C_PRODUCTS) $(STAGE_RAM) $(STAGE_PRODUCTS) tools/manifest.py
	python3 tools/manifest.py $(A_PRODUCTS) $(B_PRODUCTS) $(C_PRODUCTS) $(STAGE_RAM) $(STAGE_PRODUCTS) > $@
build/midi-update-test: $(HOST_RUST_MU) tests/midi_update_test.c $(HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -g -Wall -Wextra -Werror -DMU_TEST_APP_RAM -Iinclude tests/midi_update_test.c $(HOST_RUST_MU) -Wl,--gc-sections -o $@
build/midi-update-test-smk37: $(HOST_RUST_MU_SMK) tests/midi_update_test.c $(HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -g -Wall -Wextra -Werror -DBOARD_SMK -DMU_TEST_APP_RAM -Iinclude tests/midi_update_test.c $(HOST_RUST_MU_SMK) -Wl,--gc-sections -o $@
build/sfx-layout: $(HOST_RUST) tools/sfx_layout.c $(HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -g -Wall -Wextra -Werror -Iinclude tools/sfx_layout.c $(HOST_RUST) -Wl,--gc-sections -o $@
build/upkr-test: $(HOST_RUST) tests/upkr_test.c $(HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -g -Wall -Wextra -Werror -Iinclude tests/upkr_test.c $(HOST_RUST) -Wl,--gc-sections -o $@
build/core-test: $(HOST_RUST) tests/core_test.c $(HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) -std=c99 -O2 -g -Wall -Wextra -Werror -Iinclude tests/core_test.c $(HOST_RUST) -Wl,--gc-sections -o $@
build/installer-host: $(HOST_RUST) tests/installer_host.c $(HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -g -Wall -Wextra -Werror -Iinclude tests/installer_host.c $(HOST_RUST) -Wl,--gc-sections -o $@
build/loader-test: $(HOST_RUST) tests/loader_test.c $(HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) -std=c99 -O2 -g -Wall -Wextra -Werror -Iinclude tests/loader_test.c $(HOST_RUST) -Wl,--gc-sections -o $@
build/contract-test: $(HOST_RUST) tests/contract_test.c $(HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -g -Wall -Wextra -Werror -Iinclude tests/contract_test.c $(HOST_RUST) -Wl,--gc-sections -o $@
build/panel-board-test: $(HOST_RUST_SD) tests/panel_board_test.c $(HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -g -Wall -Wextra -Werror -DPLATFORM_SIM -Iinclude tests/panel_board_test.c $(HOST_RUST_SD) -Wl,--gc-sections -o $@
build/platform-test: $(HOST_RUST_PSIM) tests/platform_test.c $(HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -g -Wall -Wextra -Werror -DPLATFORM_SIM -Iinclude tests/platform_test.c $(HOST_RUST_PSIM) -Wl,--gc-sections -o $@
build/sdboot-test: $(HOST_RUST_SD) tests/sdboot_test.c $(HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -g -Wall -Wextra -Werror -DPLATFORM_SIM -Iinclude tests/sdboot_test.c $(HOST_RUST_SD) -Wl,--gc-sections -o $@
build/sdboot-test-smk37: $(HOST_RUST_SD_SMK) tests/sdboot_test.c $(HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -g -Wall -Wextra -Werror -DPLATFORM_SIM -DBOARD_SMK -Iinclude tests/sdboot_test.c $(HOST_RUST_SD_SMK) -Wl,--gc-sections -o $@
.SECONDEXPANSION:
build/boot-test-fm1 build/boot-test-smk37: build/boot-test-%: $(SIM_SOURCES) tests/boot_test.c tests/boot_sim.h $(HEADERS) $$(call sim_rust,$$*,)
	@mkdir -p $(@D)
	$(HOSTCC) $(SIM_FLAGS) -O2 $(if $(filter smk37,$*),-DBOARD_SMK,) $(SIM_SOURCES) tests/boot_test.c $(call sim_rust,$*,) -Wl,--gc-sections -o $@
build/boot-test-requests: $(SIM_SOURCES) tests/boot_test.c tests/boot_sim.h $(HEADERS) $(HOST_RUST_SIM_REQUESTS)
	@mkdir -p build
	$(HOSTCC) $(SIM_FLAGS) -O2 -DTEST_REQUESTS $(SIM_SOURCES) tests/boot_test.c $(HOST_RUST_SIM_REQUESTS) -Wl,--gc-sections -o $@
# What asks a test build's bootloader for MIDI mode or the SD card item, through run-ram.
build/test-request.ld: tools/target.ld.S
	@mkdir -p build
	$(CLANG) -E -P -x c -DENTRY_SYMBOL=test_request_entry -DRAM_ORIGIN=0x1c28000 -DRAM_LENGTH=0x8000 -DLOADER_BUILD $< -o $@
$(eval $(call RUST_whole_template,build/test-request-midi/libmboot.a,test_request_midi))
$(eval $(call RUST_whole_template,build/test-request-sd/libmboot.a,test_request_sd))
build/test-request-%.bin: build/test-request-%/libmboot.a build/test-request.ld
	$(LINK) --gc-sections -T build/test-request.ld -u test_request_entry -o build/test-request-$*.elf $<
	$(OBJCOPY) -O binary -j .text build/test-request-$*.elf $@
test-requests: build/test-request-midi.bin build/test-request-sd.bin
build/boot-midi-test-fm1 build/boot-midi-test-smk37: build/boot-midi-test-%: $(SIM_SOURCES) tests/boot_midi_test.c tests/boot_test.c tests/boot_sim.h $(HEADERS) $$(call sim_rust,$$*,_MIDI)
	@mkdir -p $(@D)
	$(HOSTCC) $(SIM_FLAGS) -O2 $(if $(filter smk37,$*),-DBOARD_SMK,) $(SIM_SOURCES) tests/boot_midi_test.c $(call sim_rust,$*,_MIDI) -Wl,--gc-sections -o $@
build/loader-lifecycle-test: $(LIFECYCLE_SOURCES) tests/boot_sim.h $(HEADERS) $(HOST_RUST_SIM)
	$(HOSTCC) $(SIM_FLAGS) -O2 $(LIFECYCLE_SOURCES) $(HOST_RUST_SIM) -Wl,--gc-sections -o $@
test: all build/boot-test-requests build/midi-update-test build/midi-update-test-smk37 build/upkr-test build/sdboot-test build/sdboot-test-smk37 build/installer-host build/core-test build/loader-test build/contract-test build/panel-board-test build/platform-test build/boot-test-fm1 build/boot-test-smk37 build/boot-midi-test-fm1 build/boot-midi-test-smk37 build/loader-lifecycle-test
	./build/core-test
	./build/loader-test
	./build/contract-test
	./build/panel-board-test
	./build/platform-test
	./build/sdboot-test
	./build/sdboot-test-smk37
	./build/midi-update-test
	./build/midi-update-test-smk37
	./build/boot-test-fm1
	./build/boot-test-smk37
	./build/boot-test-requests
	./build/boot-midi-test-fm1
	./build/boot-midi-test-smk37
	./build/loader-lifecycle-test
	python3 tests/startup_test.py
	python3 tests/updater_context_test.py $(TOOLCHAIN)/bin/llvm-nm build/fm1/stage1.elf build/smk37/stage1.elf
	python3 tests/version_test.py
	python3 tests/container_test.py
	$(if $(TEST_REQUESTS),NO_INSTALLER_PACKAGES=1 )python3 tests/fwsc_test.py
	python3 tests/stage2_test.py
sanitize: $(HOST_RUST) $(HOST_RUST_PSIM) $(HOST_RUST_SD) $(HOST_RUST_SD_SMK) $(HOST_RUST_SIM) $(HOST_RUST_SIM_SMK) $(HOST_RUST_SIM_MIDI) $(HOST_RUST_SIM_MIDI_SMK) $(HOST_RUST_MU) $(HOST_RUST_MU_SMK)
	@mkdir -p build
	$(HOSTCC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Iinclude tests/core_test.c $(HOST_RUST) -Wl,--gc-sections -o build/core-test-sanitize
	./build/core-test-sanitize
	$(HOSTCC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Iinclude tests/loader_test.c $(HOST_RUST) -Wl,--gc-sections -o build/loader-test-sanitize
	./build/loader-test-sanitize
	$(HOSTCC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Iinclude tests/contract_test.c $(HOST_RUST) -Wl,--gc-sections -o build/contract-test-sanitize
	./build/contract-test-sanitize
	$(HOSTCC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -DPLATFORM_SIM -Iinclude tests/panel_board_test.c $(HOST_RUST_SD) -Wl,--gc-sections -o build/panel-board-test-sanitize
	./build/panel-board-test-sanitize
	$(HOSTCC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -DPLATFORM_SIM -Iinclude tests/platform_test.c $(HOST_RUST_PSIM) -Wl,--gc-sections -o build/platform-test-sanitize
	./build/platform-test-sanitize
	$(HOSTCC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -DPLATFORM_SIM -Iinclude tests/sdboot_test.c $(HOST_RUST_SD) -Wl,--gc-sections -o build/sdboot-test-sanitize
	./build/sdboot-test-sanitize
	$(HOSTCC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -DMU_TEST_APP_RAM -Iinclude tests/midi_update_test.c $(HOST_RUST_MU) -Wl,--gc-sections -o build/midi-update-test-sanitize
	./build/midi-update-test-sanitize
	$(HOSTCC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -DBOARD_SMK -DMU_TEST_APP_RAM -Iinclude tests/midi_update_test.c $(HOST_RUST_MU_SMK) -Wl,--gc-sections -o build/midi-update-test-smk37-sanitize
	./build/midi-update-test-smk37-sanitize
	$(HOSTCC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Iinclude tests/upkr_test.c $(HOST_RUST) -Wl,--gc-sections -o build/upkr-test-sanitize
	./build/upkr-test-sanitize build/fm1/stage1.pack build/fm1/stage1.bin
	./build/upkr-test-sanitize build/smk37/stage1.pack build/smk37/stage1.bin
	$(HOSTCC) $(SIM_FLAGS) -O1 -fsanitize=address,undefined $(SIM_SOURCES) tests/boot_test.c $(HOST_RUST_SIM) -Wl,--gc-sections -o build/boot-test-fm1-sanitize
	./build/boot-test-fm1-sanitize
	$(HOSTCC) $(SIM_FLAGS) -O1 -fsanitize=address,undefined -DBOARD_SMK $(SIM_SOURCES) tests/boot_test.c $(HOST_RUST_SIM_SMK) -Wl,--gc-sections -o build/boot-test-smk37-sanitize
	./build/boot-test-smk37-sanitize
	$(HOSTCC) $(SIM_FLAGS) -O1 -fsanitize=address,undefined $(SIM_SOURCES) tests/boot_midi_test.c $(HOST_RUST_SIM_MIDI) -Wl,--gc-sections -o build/boot-midi-test-fm1-sanitize
	./build/boot-midi-test-fm1-sanitize
	$(HOSTCC) $(SIM_FLAGS) -O1 -fsanitize=address,undefined -DBOARD_SMK $(SIM_SOURCES) tests/boot_midi_test.c $(HOST_RUST_SIM_MIDI_SMK) -Wl,--gc-sections -o build/boot-midi-test-smk37-sanitize
	./build/boot-midi-test-smk37-sanitize
	$(HOSTCC) $(SIM_FLAGS) -O1 -fsanitize=address,undefined $(LIFECYCLE_SOURCES) $(HOST_RUST_SIM) -Wl,--gc-sections -o build/loader-lifecycle-test-sanitize
	./build/loader-lifecycle-test-sanitize
reproducible: all
	python3 tools/reproducible.py $(A_PRODUCTS) $(B_PRODUCTS) $(INSTALLERS) $(C_PRODUCTS) $(STAGE_RAM) $(STAGE_PRODUCTS)
clean:
	rm -rf build
