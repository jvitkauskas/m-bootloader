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
# The pi32v2 Rust compiler (see README), unpacked in pi32v2-rust/ or at PI32V2_RUST=<directory>. It has to be built on
# the toolchain's LLVM: the linker generates the code from its bitcode.
PI32V2_RUST ?= pi32v2-rust
RUSTC := $(PI32V2_RUST)/bin/rustc
# Any recent rustc, for the tests and for the programs of this computer that the build runs.
HOST_RUSTC ?= rustc
CARGO ?= cargo
RUST_SRC := $(wildcard rust/src/*.rs rust/src/*/*.rs) VERSION
RUST_TESTS := $(wildcard rust/tests/*.rs)
# What the RAM stage tells a host its version is.
export STAGE_VERSION := $(shell cat VERSION)
# Rust's optimisation level: z, the smallest (rust/PORT.md), with -inline-minsize-hint-threshold, which the linker and
# rustc have from pi32v2-llvm 23.1.6-2 (without it z is far bigger, and does not fit). RUST_OPT=s builds without the
# option, about 1% bigger.
RUST_OPT ?= z
ifeq ($(RUST_OPT),z)
LINK += -mllvm -inline-minsize-hint-threshold=50
RUST_HINT := -C llvm-args=-inline-minsize-hint-threshold=50
endif
RUST_FLAGS := --target pi32v2-unknown-none --edition 2024 --crate-name mboot -C opt-level=$(RUST_OPT) -C codegen-units=1 -C debuginfo=0 -C symbol-mangling-version=v0 -Zunstable-options -C panic=immediate-abort $(RUST_HINT) --remap-path-prefix=$(CURDIR)=.
RUST_LIB := lib/rustlib/pi32v2-unknown-none/lib
# Rust's core library, as the linker takes it.
RUST_CORE_lto := build/rust/core.o
HOST_TOOL := build/host/tool/libmboot.rlib
# The tests' programs, each built with the --cfg names in its directory's name (rust/tests/mod.rs).
RUST_TEST_SETS := host platform_sim-platform_test platform_sim-import_early-panel_test \
	platform_sim-import_early-sdboot_test platform_sim-import_early-sdboot_test-smk \
	mu_test_app_ram mu_test_app_ram-smk boot_sim-platform_sim-lifecycle_test \
	boot_sim-platform_sim boot_sim-platform_sim-smk boot_sim-platform_sim-test_requests \
	boot_sim-platform_sim-midi_mode-mu_test_app_ram boot_sim-platform_sim-midi_mode-mu_test_app_ram-smk
RUST_TEST_BINS := $(RUST_TEST_SETS:%=build/host/%/tests)
# How they run: one at a time, as the simulated chip is one, and saying what they did.
RUN_TESTS := --test-threads=1 --nocapture
# A test build: make clean, then make TEST_REQUESTS=1 (docs/SPEC.md §11.9). (Without the clean the Rust is built again
# too, but the last build's packages stay in build/.)
ifdef TEST_REQUESTS
RUST_TEST := test_requests
export BOOT_VERSION_SUFFIX := -test
endif
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
.PHONY: all stage test test-requests clean reproducible FORCE
all: $(A_PRODUCTS) $(B_PRODUCTS) $(INSTALLERS) $(C_PRODUCTS) $(STAGE_RAM) $(STAGE_PRODUCTS) build/manifest.json

# Refresh on every invocation; unchanged versions keep the header's mtime.
# Only the bootloader's entry includes this header. The flash loader has a version of its own (rust/src/loader.rs).
build/boot_version.h: FORCE VERSION tools/version.py
	python3 tools/version.py $@
FORCE:

# What changes the Rust without changing a file: a build with another RUST_OPT, or one that is a test build where the
# last was not, starts the Rust again.
build/rust/options: FORCE
	@mkdir -p $(@D)
	@echo 'RUST_OPT=$(RUST_OPT) TEST_REQUESTS=$(TEST_REQUESTS)' | cmp -s - $@ || echo 'RUST_OPT=$(RUST_OPT) TEST_REQUESTS=$(TEST_REQUESTS)' > $@
# Rust's core library for pi32v2, built once from the compiler's sources, as bitcode. -Clto=fat makes that the kind of
# bitcode C's is: the linker keeps rustc's usual kind (for ThinLTO) apart from C's, and couldn't optimise across them.
build/rust/sysroot.stamp: rust/sysroot/Cargo.toml rust/sysroot/src/lib.rs Makefile build/rust/options
	@mkdir -p build/rust/sysroot/$(RUST_LIB)
	$(CARGO) clean --quiet --manifest-path rust/sysroot/Cargo.toml --target-dir build/rust/target
	CARGO_TARGET_DIR=build/rust/target CARGO_PROFILE_RELEASE_OPT_LEVEL=$(RUST_OPT) RUSTC_BOOTSTRAP=1 RUSTC=$(RUSTC) RUSTFLAGS="-Csymbol-mangling-version=v0 -Zunstable-options -Cpanic=immediate-abort $(RUST_HINT) --remap-path-prefix=$(CURDIR)=. -Clto=fat" \
		$(CARGO) build --quiet --release --manifest-path rust/sysroot/Cargo.toml --target pi32v2-unknown-none -Zbuild-std=core
	@# (wherever this cargo keeps them: under deps/ up to 1.99, under build/<crate>/ from 1.100)
	for f in core compiler_builtins; do \
		set -- $$(find build/rust/target/pi32v2-unknown-none/release -name "lib$$f-*.rlib"); \
		[ $$# = 1 ] && cp "$$1" build/rust/sysroot/$(RUST_LIB)/lib$$f.rlib || exit 1; done
	touch $@
# The library's one object, for the linker.
build/rust/core.o: build/rust/sysroot.stamp
	$(TOOLCHAIN)/bin/llvm-ar p build/rust/sysroot/$(RUST_LIB)/libcore.rlib $$($(TOOLCHAIN)/bin/llvm-ar t build/rust/sysroot/$(RUST_LIB)/libcore.rlib | grep '\.o$$') > $@
# The crate for one image, with the --cfg names that image has ($(2)): an object of bitcode, for the linker to optimise
# the image as a whole. (rustc can do that itself, but it keeps every C name, and stage 1 came out 1.4 KB bigger.)
define RUST_template
$(1): $$(RUST_SRC) build/rust/sysroot.stamp Makefile
	@mkdir -p $$(@D)
	$$(RUSTC) $$(RUST_FLAGS) -Clinker-plugin-lto -Clto=fat $$(addprefix --cfg ,$(2)) --sysroot build/rust/sysroot --crate-type lib --emit=obj -o $$@ rust/src/lib.rs
endef
# The tests (rust/tests/), built into the crate for this computer: build/host/<its --cfg names, with - between them>/tests
# With overflow checks, and debug assertions, which also check what unsafe code promises the standard library.
# (The flash loader's is linked at fixed addresses, below 4 GB: the loader is given the ROM's functions as 32-bit
# addresses, there as on the chip. The boots' aren't, as they map the chip's RAM at its own addresses, which are low.)
build/host/%/tests: $(RUST_SRC) $(RUST_TESTS) Makefile
	@mkdir -p $(@D)
	$(HOST_RUSTC) --edition 2024 --crate-name mboot --test -C opt-level=2 -C debuginfo=2 -C overflow-checks=on -C debug-assertions=on $(if $(findstring lifecycle_test,$*),-C relocation-model=static) $(addprefix --cfg ,$(subst -, ,$*)) rust/src/lib.rs -o $@
# The crate as a library for the programs of this computer that the build runs.
build/host/tool/libmboot.rlib: $(RUST_SRC) Makefile
	@mkdir -p $(@D)
	$(HOST_RUSTC) --edition 2024 --crate-name mboot --crate-type rlib -C opt-level=2 -C debuginfo=2 -C overflow-checks=on --cfg tool rust/src/lib.rs -o $@
build/sfx-layout: tools/sfx_layout.rs $(HOST_TOOL)
	$(HOST_RUSTC) --edition 2024 -C opt-level=2 -C debuginfo=2 -C overflow-checks=on --extern mboot=$(HOST_TOOL) $< -o $@
build/upkr-test: tests/upkr_test.rs $(HOST_TOOL)
	$(HOST_RUSTC) --edition 2024 -C opt-level=2 -C debuginfo=2 -C overflow-checks=on --extern mboot=$(HOST_TOOL) $< -o $@
build/installer-host: tests/installer_host.rs $(HOST_TOOL)
	$(HOST_RUSTC) --edition 2024 -C opt-level=2 -C debuginfo=2 -C overflow-checks=on --extern mboot=$(HOST_TOOL) $< -o $@

# A uses only our own freestanding objects with LTO, and an independent entry.
define BOARD_template
$$(eval $$(call RUST_template,build/$(1)/mboot.o,$(3)))
$$(eval $$(call RUST_template,build/$(1)/s0/mboot.o,$(3) stage0))
$$(eval $$(call RUST_template,build/$(1)/s1/mboot.o,$(3) stage1 $$(RUST_TEST)))
$$(eval $$(call RUST_template,build/$(1)/st/ota/mboot.o,$(3) ram_stage stage_ota))
$$(eval $$(call RUST_template,build/$(1)/st/ram/mboot.o,$(3) ram_stage))
build/$(1)/startup.o: src/startup.S build/boot_version.h Makefile
	@mkdir -p $$(@D)
	$(CLANG) -target pi32v2 -mcpu=r3 -Ibuild $(2) -c $$< -o $$@
build/$(1)/stage0.ld: tools/stage0.ld.S
	@mkdir -p $$(@D)
	$(CLANG) -E -P -x c $$< -o $$@
# What stage 0 has to keep for stage 1: every C name that stage 1 doesn't define itself (docs/SPEC.md §4.4), and
# the four that the compiler calls without the object saying so.
build/$(1)/stage0.keep: build/$(1)/s1/mboot.o
	{ echo -u memcpy -u memset -u __udivsi3 -u __umodsi3; $(NM) -u $$< | sed -n '/ U _R/!s/^ *U /-u /p'; } > $$@
build/$(1)/stage0.elf: build/$(1)/startup.o build/$(1)/s0/mboot.o $$(RUST_CORE_lto) build/$(1)/stage0.keep build/$(1)/stage0.ld
	$(LINK) --plugin-opt=O3 --relax --gc-sections @build/$(1)/stage0.keep -T build/$(1)/stage0.ld -Map build/$(1)/stage0.map -o $$@ $$(filter %.o,$$^)
build/$(1)/stage0.bin: build/$(1)/stage0.elf
	$(OBJCOPY) -O binary -j .text $$< $$@
# What stage 1 may call: stage 0's C names. Rust's own names stay inside stage 0, as stage 1 has its own of each.
build/$(1)/stage0.syms: build/$(1)/stage0.elf
	$(OBJCOPY) --wildcard --localize-symbol='_R*' $$< $$@
build/$(1)/stage1.ld: tools/stage1.ld.S build/$(1)/stage0.bin
	$(CLANG) -E -P -x c -DSTAGE1_ORIGIN=$$$$((0x1c02000+$$$$(stat -c%s build/$(1)/stage0.bin))) $$< -o $$@
build/$(1)/stage1.elf: build/$(1)/s1/mboot.o $$(RUST_CORE_lto) build/$(1)/stage0.syms build/$(1)/stage1.ld
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
# The installer: a staged updater carrying this uboot.boot (rust/src/installer.rs).
build/$(1)/installer_start.o: src/installer_start.S build/$(1)/uboot.boot Makefile
	$(CLANG) -target pi32v2 -mcpu=r3 -Ibuild/$(1) $(2) -c $$< -o $$@
build/$(1)/installer.ld: tools/target.ld.S
	$(CLANG) -E -P -x c -DENTRY_SYMBOL=installer_entry -DRAM_ORIGIN=0x1c0a800 -DRAM_LENGTH=0xc000 -DINSTALLER_BUILD $$< -o $$@
build/$(1)/installer.elf: build/$(1)/mboot.o $$(RUST_CORE_lto) build/$(1)/installer_start.o build/$(1)/installer.ld
	$(LINK) --plugin-opt=O3 --relax --gc-sections -T build/$(1)/installer.ld -Map build/$(1)/installer.map -o $$@ $$(filter %.o,$$^)
build/$(1)/installer.bin: build/$(1)/installer.elf tools/check_build.py
	$(OBJCOPY) -O binary -j .text $$< $$@
	python3 tools/check_build.py $$< $$@ 0x1c0a800 0x1c16800 installer_entry > build/$(1)/installer-size.json
# As packages carry it: packed like stage 1, behind an unpacker that unpacks it in place (rust/src/sfx.rs), so the
# stream fits where firmware stores an updater with room to spare. sfx-layout places the pack and checks the image.
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
build/$(1)/stage_start.o: src/stage_start.S Makefile
	$(CLANG) -target pi32v2 -mcpu=r3 $(2) -c $$< -o $$@
build/$(1)/stage-ota.ld: tools/target.ld.S
	$(CLANG) -E -P -x c -DENTRY_SYMBOL=stage_entry -DRAM_ORIGIN=0x1c0a800 -DRAM_LENGTH=0xc000 -DINSTALLER_BUILD $$< -o $$@
build/$(1)/stage-ram.ld: tools/target.ld.S
	$(CLANG) -E -P -x c -DENTRY_SYMBOL=stage_entry -DRAM_ORIGIN=0x1c28000 -DRAM_LENGTH=0x8000 -DINSTALLER_BUILD $$< -o $$@
build/$(1)/stage-ota.elf: build/$(1)/st/ota/mboot.o $$(RUST_CORE_lto) build/$(1)/stage_start.o build/$(1)/stage-ota.ld
	$(LINK) --plugin-opt=O3 --relax --gc-sections -T build/$(1)/stage-ota.ld -Map build/$(1)/stage-ota.map -o $$@ $$(filter %.o,$$^)
build/$(1)/stage-ram.elf: build/$(1)/st/ram/mboot.o $$(RUST_CORE_lto) build/$(1)/stage_start.o build/$(1)/stage-ram.ld
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
$$(eval $$(call RUST_template,build/loader-$(1)/mboot.o,loader_$(1)))
build/loader-$(1)/target.ld: tools/target.ld.S
	@mkdir -p $$(@D)
	$(CLANG) -E -P -x c -DENTRY_SYMBOL=loader_entry -DRAM_ORIGIN=$(2) -DRAM_LENGTH=0x8000 -DLOADER_BUILD $$< -o $$@
build/loader-$(1)/loader.elf: build/loader-$(1)/mboot.o $$(RUST_CORE_lto) build/loader-$(1)/target.ld
	$(LINK) --plugin-opt=O3 --relax --gc-sections -T build/loader-$(1)/target.ld -Map build/loader-$(1)/loader.map -o $$@ $$(filter %.o,$$^)
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
# What asks a test build's bootloader for MIDI mode or the SD card item, through run-ram.
build/test-request.ld: tools/target.ld.S
	@mkdir -p build
	$(CLANG) -E -P -x c -DENTRY_SYMBOL=test_request_entry -DRAM_ORIGIN=0x1c28000 -DRAM_LENGTH=0x8000 -DLOADER_BUILD $< -o $@
$(eval $(call RUST_template,build/test-request-midi/mboot.o,test_request_midi))
$(eval $(call RUST_template,build/test-request-sd/mboot.o,test_request_sd))
build/test-request-%.bin: build/test-request-%/mboot.o $(RUST_CORE_lto) build/test-request.ld
	$(LINK) --plugin-opt=O3 --gc-sections -T build/test-request.ld -o build/test-request-$*.elf $(filter %.o,$^)
	$(OBJCOPY) -O binary -j .text build/test-request-$*.elf $@
test-requests: build/test-request-midi.bin build/test-request-sd.bin
test: all $(RUST_TEST_BINS) build/upkr-test build/installer-host
	set -e; for t in $(RUST_TEST_BINS); do echo $$t; $$t $(RUN_TESTS); done
	python3 tests/startup_test.py
	python3 tests/updater_context_test.py $(TOOLCHAIN)/bin/llvm-nm build/fm1/stage1.elf build/smk37/stage1.elf
	python3 tests/version_test.py
	python3 tests/container_test.py
	$(if $(TEST_REQUESTS),NO_INSTALLER_PACKAGES=1 )python3 tests/fwsc_test.py
	python3 tests/stage2_test.py
reproducible: all
	python3 tools/reproducible.py $(A_PRODUCTS) $(B_PRODUCTS) $(INSTALLERS) $(C_PRODUCTS) $(STAGE_RAM) $(STAGE_PRODUCTS)
clean:
	rm -rf build
