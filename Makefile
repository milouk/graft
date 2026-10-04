# nvmm-darwin
#
#   make unit        userspace tests for the nested page table builder
#   make bare        compile the bare-metal test kernel (objects only)
#   make test-bare   link it and run it under QEMU with emulated AMD-V (Docker)
#   make kext        compile and link the macOS kernel extension (x86_64)
#   make release     the kext without debug symbols, for installing
#   make libnvmm     the userland library and its headers, for macOS x86_64
#   make selftest    the self-test kext (stand-in engine) and its test programs
#   make check       unit, test-bare and kext
#   make clean

BUILD		:= build
CC		:= clang

CORE_SRCS	:= src/nvmm.c src/x86/nvmm_x86.c src/x86/nvmm_x86_svm.c
CORE_ASM	:= src/x86/nvmm_x86_svmfunc.S
PORT_SRCS	:= port/npt.c port/nvmm_port_vm.c port/nvmm_port_x86.c
COMMON_INC	:= -Iport -Iport/compat -Isrc
WARN		:= -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
		   -Wno-missing-field-initializers

.PHONY: all check unit bare test-bare kext release selftest guest-test libnvmm clean
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
	$(CC) $(BARE_CFLAGS) -MMD -MP -c $< -o $@

$(BARE_DIR)/%.o: %.S
	@mkdir -p $(BARE_DIR)
	$(CC) $(BARE_CFLAGS) -MMD -MP -c $< -o $@

-include $(BARE_OBJS:.o=.d)

test-bare: bare
	./test/baremetal/run.sh

# ------------------------------------------------------- macOS kernel extension
SDK		?= $(shell ls -d /Library/Developer/CommandLineTools/SDKs/MacOSX15*.sdk 2>/dev/null | tail -1)
KHDR		:= $(SDK)/System/Library/Frameworks/Kernel.framework/Headers
KEXT_DIR	:= $(BUILD)/kext
KEXT_BUNDLE	:= $(BUILD)/NVMM.kext
KEXT_COMMON	:= -arch x86_64 -mkernel -nostdinc -fno-builtin \
		   -fno-stack-protector -mmacosx-version-min=10.13 \
		   -isysroot $(SDK) -isystem $(KHDR) \
		   -DKERNEL -DKERNEL_PRIVATE -D_KERNEL -DAPPLE -DNeXT \
		   -DNVMM_PORT $(COMMON_INC) -Idarwin -O2 -g $(WARN) -Werror \
		   -Wno-shorten-64-to-32 -Wno-sign-conversion \
		   -Wno-implicit-int-conversion
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
	$(CC) $(KEXT_CFLAGS) -MMD -MP -c $< -o $@

$(KEXT_DIR)/%.o: %.S
	@mkdir -p $(KEXT_DIR)
	$(CC) $(KEXT_CFLAGS) -MMD -MP -c $< -o $@

$(KEXT_DIR)/%.o: %.cpp
	@mkdir -p $(KEXT_DIR)
	clang++ $(KEXT_CXXFLAGS) -MMD -MP -c $< -o $@

-include $(KEXT_OBJS:.o=.d)

$(KEXT_BUNDLE)/Contents/MacOS/NVMM: $(KEXT_OBJS) darwin/Info.plist
	@mkdir -p $(KEXT_BUNDLE)/Contents/MacOS
	clang++ -arch x86_64 -nostdlib -Xlinker -kext -Xlinker -export_dynamic \
	    -mmacosx-version-min=10.13 -isysroot $(SDK) \
	    -o $@ $(KEXT_OBJS) -lkmod -lkmodc++ -lcc_kext
	cp darwin/Info.plist $(KEXT_BUNDLE)/Contents/Info.plist

# A copy without debug symbols, for installing. Keep the unstripped one while
# debugging: its symbols are what make a panic log readable.
RELEASE_BUNDLE	:= $(BUILD)/release/NVMM.kext

release: $(KEXT_BUNDLE)/Contents/MacOS/NVMM
	@rm -rf $(RELEASE_BUNDLE) && mkdir -p $(RELEASE_BUNDLE)/Contents/MacOS
	strip -S -x -o $(RELEASE_BUNDLE)/Contents/MacOS/NVMM $(KEXT_BUNDLE)/Contents/MacOS/NVMM
	cp darwin/Info.plist $(RELEASE_BUNDLE)/Contents/Info.plist
	@ls -l $(RELEASE_BUNDLE)/Contents/MacOS/NVMM | awk '{printf "built $(RELEASE_BUNDLE) (%.0f KB)\n", $$5/1024}'

# ----------------------------------------------- glue self-test (any Intel Mac)
# The same kext without the engine's CPU check, plus a device that exercises
# the macOS glue. See darwin/nvmm_darwin_selftest.c.
ST_DIR		:= $(BUILD)/kext-selftest
ST_BUNDLE	:= $(BUILD)/NVMMSelfTest.kext
ST_OBJS		:= $(patsubst $(KEXT_DIR)/%,$(ST_DIR)/%,$(KEXT_OBJS)) \
		   $(ST_DIR)/nvmm_darwin_selftest.o \
		   $(ST_DIR)/nvmm_fake_engine.o
ST_TOOL		:= $(BUILD)/nvmm-selftest
ST_FLAGS	:= -DNVMM_DARWIN_SELFTEST -DNVMM_FAKE_ENGINE
FAKE_TOOL	:= $(BUILD)/nvmm-fake-test

selftest: $(ST_BUNDLE)/Contents/MacOS/NVMMSelfTest $(ST_TOOL) $(FAKE_TOOL)
	@echo "built $(ST_BUNDLE), $(ST_TOOL) and $(FAKE_TOOL)"

$(ST_DIR)/%.o: %.c
	@mkdir -p $(ST_DIR)
	$(CC) $(KEXT_CFLAGS) $(ST_FLAGS) -MMD -MP -c $< -o $@

$(ST_DIR)/%.o: %.S
	@mkdir -p $(ST_DIR)
	$(CC) $(KEXT_CFLAGS) $(ST_FLAGS) -MMD -MP -c $< -o $@

$(ST_DIR)/%.o: %.cpp
	@mkdir -p $(ST_DIR)
	clang++ $(KEXT_CXXFLAGS) $(ST_FLAGS) -MMD -MP -c $< -o $@

-include $(ST_OBJS:.o=.d)

$(ST_BUNDLE)/Contents/MacOS/NVMMSelfTest: $(ST_OBJS) darwin/Info.plist
	@mkdir -p $(ST_BUNDLE)/Contents/MacOS
	clang++ -arch x86_64 -nostdlib -Xlinker -kext -Xlinker -export_dynamic \
	    -mmacosx-version-min=10.13 -isysroot $(SDK) \
	    -o $@ $(ST_OBJS) -lkmod -lkmodc++ -lcc_kext
	sed -e 's/org\.nvmm\.driver\.NVMM/org.nvmm.driver.NVMMSelfTest/' \
	    -e 's|<string>NVMM</string>|<string>NVMMSelfTest</string>|' \
	    darwin/Info.plist > $(ST_BUNDLE)/Contents/Info.plist

$(ST_TOOL): test/darwin/selftest.c darwin/nvmm_selftest_ioctl.h
	@mkdir -p $(BUILD)
	$(CC) -arch x86_64 -mmacosx-version-min=10.13 -O2 -Wall -Wextra -Werror \
	    -Idarwin -o $@ test/darwin/selftest.c

# ------------------------------------------------------------ libnvmm (macOS)
# The userland library, and the kernel ABI headers it is built against, laid
# out the way an emulator expects to find them: <nvmm.h> on the include path.
INC_DIR		:= $(BUILD)/include
LIBNVMM		:= $(BUILD)/libnvmm.a
USER_CFLAGS	:= -arch x86_64 -mmacosx-version-min=10.13 -O2 -Wall -Wextra \
		   -Wno-unused-parameter -Wno-sign-compare -Werror \
		   -I$(INC_DIR)
LIB_HEADERS	:= $(INC_DIR)/nvmm.h $(INC_DIR)/dev/nvmm/nvmm.h \
		   $(INC_DIR)/dev/nvmm/nvmm_ioctl.h \
		   $(INC_DIR)/dev/nvmm/x86/nvmm_x86.h $(INC_DIR)/sys/bitops.h

libnvmm: $(LIBNVMM)
	@echo "built $(LIBNVMM), headers in $(INC_DIR)"

$(INC_DIR)/nvmm.h: lib/nvmm.h
	@mkdir -p $(dir $@) && cp $< $@
$(INC_DIR)/dev/nvmm/%.h: src/%.h
	@mkdir -p $(dir $@) && cp $< $@
$(INC_DIR)/dev/nvmm/x86/%.h: src/x86/%.h
	@mkdir -p $(dir $@) && cp $< $@
$(INC_DIR)/sys/bitops.h: port/compat/sys/bitops.h
	@mkdir -p $(dir $@) && cp $< $@

$(LIBNVMM): lib/libnvmm.c lib/libnvmm_x86.c lib/libnvmm_darwin.h $(LIB_HEADERS)
	$(CC) $(USER_CFLAGS) -Ilib -c lib/libnvmm.c -o $(BUILD)/libnvmm.o
	rm -f $@ && ar rcs $@ $(BUILD)/libnvmm.o

# The first real guests. Needs NVMM.kext loaded on an AMD machine.
GUEST_TOOL	:= $(BUILD)/nvmm-guest-test
guest-test: $(GUEST_TOOL)
$(GUEST_TOOL): test/darwin/nvmm-guest-test.c $(LIBNVMM)
	@mkdir -p $(BUILD)
	$(CC) $(USER_CFLAGS) -o $@ test/darwin/nvmm-guest-test.c $(LIBNVMM)

$(FAKE_TOOL): test/darwin/nvmm-fake-test.c darwin/nvmm_fake_engine.h $(LIBNVMM)
	@mkdir -p $(BUILD)
	$(CC) $(USER_CFLAGS) -Idarwin -o $@ test/darwin/nvmm-fake-test.c $(LIBNVMM)

clean:
	rm -rf $(BUILD)
