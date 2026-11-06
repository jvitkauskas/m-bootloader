# SPDX-License-Identifier: MIT
SHELL := /bin/bash
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
# The pi32v2 LLVM toolchain (see README), unpacked in toolchain/ or at TOOLCHAIN=<directory>.
export TOOLCHAIN ?= toolchain
CLANG := $(TOOLCHAIN)/bin/clang
LINK := $(TOOLCHAIN)/bin/ld.lld
OBJCOPY := $(TOOLCHAIN)/bin/llvm-objcopy
# Upkr 0.2.3 (see README), at UPKR=<path> or on the PATH.
export UPKR ?= upkr
HOSTCC ?= cc
CFLAGS := -target pi32v2 -mcpu=r3 -std=c99 -Oz -ffreestanding -fno-builtin -fno-common -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Iinclude
COMMON := core platform display runtime
# The bootloader's two stages (docs/SPEC.md §4.4). Stage 0 is linked alone, so it cannot depend on stage 1.
STAGE0 := stage0 upkr board platform runtime
STAGE1 := core platform display runtime contract boot sdboot midi_update usb_midi
LOADER := $(COMMON) loader loader_target
INSTALLER := $(COMMON) installer
# MIDI mode in RAM, for units without m-bootloader (docs/SPEC.md §11.7).
STAGE := $(COMMON) midi_update usb_midi
HEADERS := $(wildcard include/*.h)
SIM_SOURCES := src/core.c src/contract.c src/platform.c src/board.c src/display.c src/stage0.c tests/boot_sim.c tests/boot_nor.c
SIM_FLAGS := -std=c11 -g -Wall -Wextra -Werror -DPLATFORM_SIM -DBOOT_SIM -no-pie -Iinclude
LIFECYCLE_SOURCES := src/core.c src/loader.c src/platform.c src/display.c tests/boot_sim.c tests/boot_nor.c tests/loader_lifecycle_test.c
A_PRODUCTS := build/fm1/uboot.boot build/smk37/uboot.boot
B_PRODUCTS := build/loader/mflr-loader-a.bin build/loader/mflr-loader-b.bin
# M-UPGRADE packages: only our installer, which writes the bootloader.
C_PRODUCTS := build/fm1/uboot.fwsc build/smk37/uboot-pro.fwsc build/smk37/uboot-elite.fwsc
# The RAM stage: for run-ram, and M-UPGRADE-style packages holding only it.
STAGE_RAM := build/fm1/stage-ram.bin build/smk37/stage-ram.bin
STAGE_PRODUCTS := build/fm1/stage.fwsc build/smk37/stage-pro.fwsc build/smk37/stage-elite.fwsc \
	build/fm1/stage-listed.fwsc build/smk37/stage-pro-listed.fwsc build/smk37/stage-elite-listed.fwsc
.PHONY: all stage test sanitize clean reproducible FORCE
all: $(A_PRODUCTS) $(B_PRODUCTS) $(C_PRODUCTS) $(STAGE_RAM) $(STAGE_PRODUCTS) build/manifest.json

# Refresh on every invocation; unchanged versions keep the header's mtime.
# Only A's entry includes this header. B remains the hardware-tested MFLR1.3.
build/boot_version.h: FORCE VERSION tools/version.py
	python3 tools/version.py $@
FORCE:

# A uses only our own freestanding objects with LTO, and an independent entry.
define BOARD_template
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
build/$(1)/stage0.elf: $$(addprefix build/$(1)/s0/,$$(addsuffix .o,$$(STAGE0))) build/$(1)/startup.o build/$(1)/stage0.ld
	$(LINK) --relax -T build/$(1)/stage0.ld -Map build/$(1)/stage0.map -o $$@ $$(filter %.o,$$^)
build/$(1)/stage0.bin: build/$(1)/stage0.elf
	$(OBJCOPY) -O binary -j .text $$< $$@
build/$(1)/stage1.ld: tools/stage1.ld.S build/$(1)/stage0.bin
	$(CLANG) -E -P -x c -DSTAGE1_ORIGIN=$$$$((0x1c02000+$$$$(stat -c%s build/$(1)/stage0.bin))) $$< -o $$@
build/$(1)/stage1.elf: $$(addprefix build/$(1)/s1/,$$(addsuffix .o,$$(STAGE1))) build/$(1)/stage0.elf build/$(1)/stage1.ld
	$(LINK) --plugin-opt=O3 --relax --gc-sections --just-symbols=build/$(1)/stage0.elf -T build/$(1)/stage1.ld -Map build/$(1)/stage1.map -o $$@ $$(filter %.o,$$^)
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
build/$(1)/installer.elf: $$(addprefix build/$(1)/,$$(addsuffix .o,$$(INSTALLER))) build/$(1)/installer_start.o build/$(1)/installer.ld
	$(LINK) --plugin-opt=O3 --relax --gc-sections -T build/$(1)/installer.ld -Map build/$(1)/installer.map -o $$@ $$(filter %.o,$$^)
build/$(1)/installer.bin: build/$(1)/installer.elf tools/check_build.py
	$(OBJCOPY) -O binary -j .text $$< $$@
	python3 tools/check_build.py $$< $$@ 0x1c0a800 0x1c16800 installer_entry > build/$(1)/installer-size.json
# The RAM stage: linked for the updater's address, and for run-ram's slot.
build/$(1)/st/%.o: src/%.c $(HEADERS) Makefile
	@mkdir -p $$(@D)
	$(CLANG) $(CFLAGS) -flto -DRAM_STAGE $(2) -c $$< -o $$@
build/$(1)/stage_start.o: src/stage_start.S Makefile
	$(CLANG) -target pi32v2 -mcpu=r3 $(2) -c $$< -o $$@
build/$(1)/stage_ota.o: src/stage.c $(HEADERS) Makefile build/boot_version.h
	$(CLANG) $(CFLAGS) -flto $(2) -DSTAGE_OTA -DSTAGE_VERSION=\"$$$$(cat VERSION)\" -c $$< -o $$@
build/$(1)/stage_ram.o: src/stage.c $(HEADERS) Makefile build/boot_version.h
	$(CLANG) $(CFLAGS) -flto $(2) -DSTAGE_VERSION=\"$$$$(cat VERSION)\" -c $$< -o $$@
build/$(1)/stage-ota.ld: tools/target.ld.S
	$(CLANG) -E -P -x c -DENTRY_SYMBOL=stage_entry -DRAM_ORIGIN=0x1c0a800 -DRAM_LENGTH=0xc000 -DINSTALLER_BUILD $$< -o $$@
build/$(1)/stage-ram.ld: tools/target.ld.S
	$(CLANG) -E -P -x c -DENTRY_SYMBOL=stage_entry -DRAM_ORIGIN=0x1c28000 -DRAM_LENGTH=0x8000 -DINSTALLER_BUILD $$< -o $$@
build/$(1)/stage-ota.elf: $$(addprefix build/$(1)/st/,$$(addsuffix .o,$$(STAGE))) build/$(1)/stage_ota.o build/$(1)/stage_start.o build/$(1)/stage-ota.ld
	$(LINK) --plugin-opt=O3 --relax --gc-sections -T build/$(1)/stage-ota.ld -Map build/$(1)/stage-ota.map -o $$@ $$(filter %.o,$$^)
build/$(1)/stage-ram.elf: $$(addprefix build/$(1)/st/,$$(addsuffix .o,$$(STAGE))) build/$(1)/stage_ram.o build/$(1)/stage_start.o build/$(1)/stage-ram.ld
	$(LINK) --plugin-opt=O3 --relax --gc-sections -T build/$(1)/stage-ram.ld -Map build/$(1)/stage-ram.map -o $$@ $$(filter %.o,$$^)
build/$(1)/stage-ota.bin: build/$(1)/stage-ota.elf tools/check_build.py
	$(OBJCOPY) -O binary -j .text $$< $$@
	python3 tools/check_build.py $$< $$@ 0x1c0a800 0x1c16800 stage_entry > build/$(1)/stage-ota-size.json
build/$(1)/stage-ram.bin: build/$(1)/stage-ram.elf tools/check_build.py
	$(OBJCOPY) -O binary -j .text $$< $$@
	python3 tools/check_build.py $$< $$@ 0x1c28000 0x1c30000 stage_entry > build/$(1)/stage-ram-size.json
endef
$(eval $(call BOARD_template,fm1,))
$(eval $(call BOARD_template,smk37,-DBOARD_SMK))
build/fm1/uboot.fwsc: build/fm1/uboot.boot build/fm1/installer.bin tools/fwsc.py tools/bankcb.py
	python3 tools/fwsc.py build fm1 $< build/fm1/installer.bin $@
build/smk37/uboot-%.fwsc: build/smk37/uboot.boot build/smk37/installer.bin tools/fwsc.py tools/bankcb.py
	python3 tools/fwsc.py build smk37-$* $< build/smk37/installer.bin $@

define LOADER_template
build/loader-$(1)/%.o: src/%.c $(HEADERS) Makefile
	@mkdir -p $$(@D)
	$(CLANG) $(CFLAGS) -DLOADER_ADDRESS=$(2) -c $$< -o $$@
build/loader-$(1)/target.ld: tools/target.ld.S
	@mkdir -p $$(@D)
	$(CLANG) -E -P -x c -DENTRY_SYMBOL=loader_entry -DRAM_ORIGIN=$(2) -DRAM_LENGTH=0x8000 -DLOADER_BUILD $$< -o $$@
build/loader-$(1)/loader.elf: $$(addprefix build/loader-$(1)/,$$(addsuffix .o,$$(LOADER))) build/loader-$(1)/target.ld
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
	python3 tools/manifest.py > $@
build/midi-update-test: src/core.c src/midi_update.c tests/midi_update_test.c $(HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -g -Wall -Wextra -Werror -DMU_TEST_APP_RAM -Iinclude src/core.c src/midi_update.c tests/midi_update_test.c -o $@
build/midi-update-test-smk37: src/core.c src/midi_update.c tests/midi_update_test.c $(HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -g -Wall -Wextra -Werror -DBOARD_SMK -DMU_TEST_APP_RAM -Iinclude src/core.c src/midi_update.c tests/midi_update_test.c -o $@
build/upkr-test: src/upkr.c tests/upkr_test.c $(HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -g -Wall -Wextra -Werror -Iinclude src/upkr.c tests/upkr_test.c -o $@
build/core-test: src/core.c tests/core_test.c $(HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) -std=c99 -O2 -g -Wall -Wextra -Werror -Iinclude src/core.c tests/core_test.c -o $@
build/installer-host: src/core.c tests/installer_host.c $(HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -g -Wall -Wextra -Werror -Iinclude src/core.c tests/installer_host.c -o $@
build/loader-test: src/core.c src/loader.c tests/loader_test.c $(HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) -std=c99 -O2 -g -Wall -Wextra -Werror -Iinclude src/core.c src/loader.c tests/loader_test.c -o $@
build/contract-test: src/core.c src/contract.c tests/contract_test.c $(HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -g -Wall -Wextra -Werror -Iinclude src/core.c src/contract.c tests/contract_test.c -o $@
build/panel-board-test: src/board.c src/display.c tests/panel_board_test.c $(HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -g -Wall -Wextra -Werror -DPLATFORM_SIM -Iinclude src/board.c src/display.c tests/panel_board_test.c -o $@
build/platform-test: src/platform.c tests/platform_test.c $(HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -g -Wall -Wextra -Werror -DPLATFORM_SIM -Iinclude src/platform.c tests/platform_test.c -o $@
build/sdboot-test: src/core.c src/sdboot.c tests/sdboot_test.c $(HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -O2 -g -Wall -Wextra -Werror -DPLATFORM_SIM -Iinclude src/core.c tests/sdboot_test.c -o $@
build/boot-test-fm1 build/boot-test-smk37: build/boot-test-%: $(SIM_SOURCES) tests/boot_test.c tests/boot_sim.h src/boot.c $(HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) $(SIM_FLAGS) -O2 $(if $(filter smk37,$*),-DBOARD_SMK,) $(SIM_SOURCES) tests/boot_test.c -o $@
build/loader-slot-a-test.o: tests/loader_slot_test.c src/loader_target.c $(HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) $(SIM_FLAGS) -O2 -DTEST_SLOT_A -DLOADER_ADDRESS=0x01c20000 -c $< -o $@
build/loader-slot-b-test.o: tests/loader_slot_test.c src/loader_target.c $(HEADERS)
	@mkdir -p $(@D)
	$(HOSTCC) $(SIM_FLAGS) -O2 -DLOADER_ADDRESS=0x01c28000 -c $< -o $@
build/loader-lifecycle-test: $(LIFECYCLE_SOURCES) tests/boot_sim.h build/loader-slot-a-test.o build/loader-slot-b-test.o $(HEADERS)
	$(HOSTCC) $(SIM_FLAGS) -O2 $(LIFECYCLE_SOURCES) build/loader-slot-a-test.o build/loader-slot-b-test.o -o $@
test: all build/midi-update-test build/midi-update-test-smk37 build/upkr-test build/sdboot-test build/installer-host build/core-test build/loader-test build/contract-test build/panel-board-test build/platform-test build/boot-test-fm1 build/boot-test-smk37 build/loader-lifecycle-test
	./build/core-test
	./build/loader-test
	./build/contract-test
	./build/panel-board-test
	./build/platform-test
	./build/sdboot-test
	./build/midi-update-test
	./build/midi-update-test-smk37
	./build/boot-test-fm1
	./build/boot-test-smk37
	./build/loader-lifecycle-test
	python3 tests/startup_test.py
	python3 tests/updater_context_test.py $(TOOLCHAIN)/bin/llvm-nm build/fm1/stage1.elf build/smk37/stage1.elf
	python3 tests/version_test.py
	python3 tests/container_test.py
	python3 tests/fwsc_test.py
	python3 tests/stage2_test.py
sanitize:
	@mkdir -p build
	$(HOSTCC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Iinclude src/core.c tests/core_test.c -o build/core-test-sanitize
	./build/core-test-sanitize
	$(HOSTCC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Iinclude src/core.c src/loader.c tests/loader_test.c -o build/loader-test-sanitize
	./build/loader-test-sanitize
	$(HOSTCC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Iinclude src/core.c src/contract.c tests/contract_test.c -o build/contract-test-sanitize
	./build/contract-test-sanitize
	$(HOSTCC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -DPLATFORM_SIM -Iinclude src/board.c src/display.c tests/panel_board_test.c -o build/panel-board-test-sanitize
	./build/panel-board-test-sanitize
	$(HOSTCC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -DPLATFORM_SIM -Iinclude src/platform.c tests/platform_test.c -o build/platform-test-sanitize
	./build/platform-test-sanitize
	$(HOSTCC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -DPLATFORM_SIM -Iinclude src/core.c tests/sdboot_test.c -o build/sdboot-test-sanitize
	./build/sdboot-test-sanitize
	$(HOSTCC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -DMU_TEST_APP_RAM -Iinclude src/core.c src/midi_update.c tests/midi_update_test.c -o build/midi-update-test-sanitize
	./build/midi-update-test-sanitize
	$(HOSTCC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -DBOARD_SMK -DMU_TEST_APP_RAM -Iinclude src/core.c src/midi_update.c tests/midi_update_test.c -o build/midi-update-test-smk37-sanitize
	./build/midi-update-test-smk37-sanitize
	$(HOSTCC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Iinclude src/upkr.c tests/upkr_test.c -o build/upkr-test-sanitize
	./build/upkr-test-sanitize build/fm1/stage1.pack build/fm1/stage1.bin
	./build/upkr-test-sanitize build/smk37/stage1.pack build/smk37/stage1.bin
	$(HOSTCC) $(SIM_FLAGS) -O1 -fsanitize=address,undefined $(SIM_SOURCES) tests/boot_test.c -o build/boot-test-fm1-sanitize
	./build/boot-test-fm1-sanitize
	$(HOSTCC) $(SIM_FLAGS) -O1 -fsanitize=address,undefined -DBOARD_SMK $(SIM_SOURCES) tests/boot_test.c -o build/boot-test-smk37-sanitize
	./build/boot-test-smk37-sanitize
	$(HOSTCC) $(SIM_FLAGS) -O1 -fsanitize=address,undefined -DTEST_SLOT_A -DLOADER_ADDRESS=0x01c20000 -c tests/loader_slot_test.c -o build/loader-slot-a-sanitize.o
	$(HOSTCC) $(SIM_FLAGS) -O1 -fsanitize=address,undefined -DLOADER_ADDRESS=0x01c28000 -c tests/loader_slot_test.c -o build/loader-slot-b-sanitize.o
	$(HOSTCC) $(SIM_FLAGS) -O1 -fsanitize=address,undefined $(LIFECYCLE_SOURCES) build/loader-slot-a-sanitize.o build/loader-slot-b-sanitize.o -o build/loader-lifecycle-test-sanitize
	./build/loader-lifecycle-test-sanitize
reproducible: all
	python3 tools/reproducible.py
clean:
	rm -rf build
