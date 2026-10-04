# nvmm-darwin
#
#   make unit        userspace tests for the nested page table builder
#   make bare        compile the bare-metal test kernel (objects only)
#   make test-bare   link it and run it under QEMU with emulated AMD-V (Docker)
#   make kext        compile and link the macOS kernel extension (x86_64)
#   make check       everything above
#   make clean

BUILD		:= build
CC		:= clang

CORE_SRCS	:= src/nvmm.c src/x86/nvmm_x86.c src/x86/nvmm_x86_svm.c
CORE_ASM	:= src/x86/nvmm_x86_svmfunc.S
PORT_SRCS	:= port/npt.c port/nvmm_port_vm.c port/nvmm_port_x86.c
COMMON_INC	:= -Iport -Iport/compat -Isrc
WARN		:= -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
		   -Wno-missing-field-initializers

.PHONY: all check unit bare test-bare kext clean
all: check
check: unit test-bare kext

# ------------------------------------------------------------------ unit tests
UNIT_CFLAGS	:= -std=c11 -g -Wall -Wextra -Werror \
		   -fsanitize=address,undefined -Iport

unit: $(BUILD)/npt_test
	$(BUILD)/npt_test

$(BUILD)/npt_test: test/unit/npt_test.c port/npt.c port/npt.h
	@mkdir -p $(BUILD)
	$(CC) $(UNIT_CFLAGS) -o $@ test/unit/npt_test.c port/npt.c

# --------------------------------------------------- bare-metal test kernel
BARE_DIR	:= $(BUILD)/bare
BARE_CFLAGS	:= -target x86_64-unknown-none-elf -ffreestanding -fno-pic \
		   -mno-red-zone -mgeneral-regs-only -fno-stack-protector \
		   -fno-builtin -O2 -g $(WARN) -Werror \
		   -D_KERNEL -DNVMM_PORT -DNVMM_TEST_NO_NRIPS $(COMMON_INC) \
		   -Itest/baremetal -Itest/baremetal/include
BARE_SRCS	:= $(CORE_SRCS) $(PORT_SRCS) \
		   test/baremetal/bare.c test/baremetal/test_main.c
BARE_ASM	:= $(CORE_ASM) test/baremetal/boot.S
BARE_OBJS	:= $(patsubst %.c,$(BARE_DIR)/%.o,$(notdir $(BARE_SRCS))) \
		   $(patsubst %.S,$(BARE_DIR)/%.o,$(notdir $(BARE_ASM)))
vpath %.c src src/x86 port test/baremetal
vpath %.S src/x86 test/baremetal

bare: $(BARE_OBJS)

$(BARE_DIR)/%.o: %.c
	@mkdir -p $(BARE_DIR)
	$(CC) $(BARE_CFLAGS) -c $< -o $@

$(BARE_DIR)/%.o: %.S
	@mkdir -p $(BARE_DIR)
	$(CC) $(BARE_CFLAGS) -c $< -o $@

test-bare: bare
	./test/baremetal/run.sh

# ------------------------------------------------------- macOS kernel extension
SDK		?= $(shell ls -d /Library/Developer/CommandLineTools/SDKs/MacOSX15*.sdk 2>/dev/null | tail -1)
KHDR		:= $(SDK)/System/Library/Frameworks/Kernel.framework/Headers
KEXT_DIR	:= $(BUILD)/kext
KEXT_BUNDLE	:= $(BUILD)/NVMM.kext
KEXT_COMMON	:= -arch x86_64 -mkernel -nostdinc -fno-builtin \
		   -fno-stack-protector -mmacosx-version-min=10.15 \
		   -isysroot $(SDK) -I$(KHDR) -I$(SDK)/usr/include \
		   -DKERNEL -DKERNEL_PRIVATE -D_KERNEL -DAPPLE -DNeXT \
		   -DNVMM_PORT $(COMMON_INC) -Idarwin -O2 -g $(WARN) -Werror
KEXT_CFLAGS	:= $(KEXT_COMMON) -std=gnu11
KEXT_CXXFLAGS	:= $(KEXT_COMMON) -std=gnu++17 -fno-exceptions -fno-rtti \
		   -fapple-kext -fno-threadsafe-statics
KEXT_SRCS	:= $(CORE_SRCS) $(PORT_SRCS) darwin/nvmm_darwin.c
KEXT_OBJS	:= $(patsubst %.c,$(KEXT_DIR)/%.o,$(notdir $(KEXT_SRCS))) \
		   $(KEXT_DIR)/nvmm_x86_svmfunc.o \
		   $(KEXT_DIR)/nvmm_darwin_mem.o
vpath %.c darwin
vpath %.cpp darwin

kext: $(KEXT_BUNDLE)/Contents/MacOS/NVMM
	@echo "built $(KEXT_BUNDLE)"
	@file $(KEXT_BUNDLE)/Contents/MacOS/NVMM

$(KEXT_DIR)/%.o: %.c
	@mkdir -p $(KEXT_DIR)
	$(CC) $(KEXT_CFLAGS) -c $< -o $@

$(KEXT_DIR)/%.o: %.S
	@mkdir -p $(KEXT_DIR)
	$(CC) $(KEXT_CFLAGS) -c $< -o $@

$(KEXT_DIR)/%.o: %.cpp
	@mkdir -p $(KEXT_DIR)
	clang++ $(KEXT_CXXFLAGS) -c $< -o $@

$(KEXT_BUNDLE)/Contents/MacOS/NVMM: $(KEXT_OBJS) darwin/Info.plist
	@mkdir -p $(KEXT_BUNDLE)/Contents/MacOS
	clang++ -arch x86_64 -nostdlib -Xlinker -kext -Xlinker -export_dynamic \
	    -mmacosx-version-min=10.15 -isysroot $(SDK) \
	    -o $@ $(KEXT_OBJS) -lkmod -lkmodc++ -lcc_kext
	cp darwin/Info.plist $(KEXT_BUNDLE)/Contents/Info.plist

clean:
	rm -rf $(BUILD)
