# SPDX-License-Identifier: MIT
SHELL := /bin/bash
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
CLANG := toolchain/common/bin/clang
LINK := toolchain/pi32v2/bin/lto-wrapper
OBJCOPY := toolchain/common/bin/objcopy
HOSTCC ?= cc
CFLAGS := -target pi32v2 -mcpu=r3 -std=c99 -Oz -ffreestanding -fno-builtin -fno-common -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Iinclude
COMMON := core platform display runtime
BOOT := $(COMMON) contract board boot
LOADER := $(COMMON) loader loader_target
INSTALLER := $(COMMON) installer
HEADERS := $(wildcard include/*.h)
SIM_SOURCES := src/core.c src/contract.c src/platform.c src/board.c src/display.c tests/boot_sim.c tests/boot_nor.c
SIM_FLAGS := -std=c11 -g -Wall -Wextra -Werror -DPLATFORM_SIM -DBOOT_SIM -no-pie -Iinclude
LIFECYCLE_SOURCES := src/core.c src/loader.c src/platform.c src/display.c tests/boot_sim.c tests/boot_nor.c tests/loader_lifecycle_test.c
A_PRODUCTS := build/fm1/uboot.boot build/smk37/uboot.boot
B_PRODUCTS := build/loader/mflr-loader-a.bin build/loader/mflr-loader-b.bin
# M-UPGRADE packages: only our installer, which writes the bootloader.
C_PRODUCTS := build/fm1/uboot.fwsc build/smk37/uboot-pro.fwsc build/smk37/uboot-elite.fwsc
.PHONY: all test sanitize clean reproducible FORCE
all: $(A_PRODUCTS) $(B_PRODUCTS) $(C_PRODUCTS) build/manifest.json

# Refresh on every invocation; unchanged versions keep the header's mtime.
# Only A's entry includes this header. B remains the hardware-tested MFLR1.2.
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
build/$(1)/target.ld: tools/target.ld.S
	@mkdir -p $$(@D)
	$(CLANG) -E -P -x c -DENTRY_SYMBOL=boot_entry -DRAM_ORIGIN=0x1c02000 -DRAM_LENGTH=0x8800 $$< -o $$@
build/$(1)/uboot.elf: $$(addprefix build/$(1)/,$$(addsuffix .o,$$(BOOT))) build/$(1)/startup.o build/$(1)/target.ld
	@mkdir -p build/tmp
	TMPDIR="$(CURDIR)/build/tmp" $(LINK) --plugin-opt=O3 --relax --gc-sections -T build/$(1)/target.ld -Map build/$(1)/uboot.map -o $$@ $$(filter %.o,$$^)
build/$(1)/payload.bin: build/$(1)/uboot.elf tools/check_build.py
	$(OBJCOPY) -O binary -j .text $$< $$@
	python3 tools/check_build.py $$< $$@ 0x1c02000 0x1c0a800 boot_entry --boot > build/$(1)/size.json
build/$(1)/uboot.boot: build/$(1)/payload.bin tools/bankcb.py
	python3 tools/bankcb.py wrap $(1) $$< $$@
# The installer: a staged updater carrying this uboot.boot (src/installer.c).
build/$(1)/installer_start.o: src/installer_start.S build/$(1)/uboot.boot Makefile
	$(CLANG) -target pi32v2 -mcpu=r3 -Ibuild/$(1) $(2) -c $$< -o $$@
build/$(1)/installer.ld: tools/target.ld.S
	$(CLANG) -E -P -x c -DENTRY_SYMBOL=installer_entry -DRAM_ORIGIN=0x1c0a800 -DRAM_LENGTH=0xc000 -DINSTALLER_BUILD $$< -o $$@
build/$(1)/installer.elf: $$(addprefix build/$(1)/,$$(addsuffix .o,$$(INSTALLER))) build/$(1)/installer_start.o build/$(1)/installer.ld
	@mkdir -p build/tmp
	TMPDIR="$(CURDIR)/build/tmp" $(LINK) --plugin-opt=O3 --relax --gc-sections -T build/$(1)/installer.ld -Map build/$(1)/installer.map -o $$@ $$(filter %.o,$$^)
build/$(1)/installer.bin: build/$(1)/installer.elf tools/check_build.py
	$(OBJCOPY) -O binary -j .text $$< $$@
	python3 tools/check_build.py $$< $$@ 0x1c0a800 0x1c16800 installer_entry > build/$(1)/installer-size.json
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
	@mkdir -p build/tmp
	TMPDIR="$(CURDIR)/build/tmp" $(LINK) --plugin-opt=O3 --relax --gc-sections -T build/loader-$(1)/target.ld -Map build/loader-$(1)/loader.map -o $$@ $$(filter %.o,$$^)
build/loader/mflr-loader-$(1).bin: build/loader-$(1)/loader.elf tools/check_build.py
	@mkdir -p $$(@D)
	$(OBJCOPY) -O binary -j .text $$< $$@
	python3 tools/check_build.py $$< $$@ $(2) $(3) loader_entry > build/loader-$(1)/size.json
endef
$(eval $(call LOADER_template,a,0x1c20000,0x1c28000))
$(eval $(call LOADER_template,b,0x1c28000,0x1c30000))

build/manifest.json: $(A_PRODUCTS) $(B_PRODUCTS) $(C_PRODUCTS) tools/manifest.py
	python3 tools/manifest.py > $@
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
test: all build/installer-host build/core-test build/loader-test build/contract-test build/panel-board-test build/platform-test build/boot-test-fm1 build/boot-test-smk37 build/loader-lifecycle-test
	./build/core-test
	./build/loader-test
	./build/contract-test
	./build/panel-board-test
	./build/platform-test
	./build/boot-test-fm1
	./build/boot-test-smk37
	./build/loader-lifecycle-test
	python3 tests/startup_test.py
	python3 tests/version_test.py
	python3 tests/container_test.py
	python3 tests/fwsc_test.py
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
