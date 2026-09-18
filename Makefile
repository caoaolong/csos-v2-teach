CC      = x86_64-elf-gcc
LD      = x86_64-elf-ld
OBJCOPY = x86_64-elf-objcopy
AR      = x86_64-elf-ar

SRC_DIR    = src
KERNEL_DIR = $(SRC_DIR)/kernel
INC_DIR    = $(SRC_DIR)/inc
BUILD_DIR  = build

# 用户态 C 库：独立源码、独立归档
LIB_DIR    = $(SRC_DIR)/lib
LIBC_A     = $(BUILD_DIR)/lib/libc.a
CRT0       = $(BUILD_DIR)/lib/crt0.o

KERNEL     = $(BUILD_DIR)/kernel.elf

# 用户程序：独立源码、独立链接（基址见 src/user/user.ld）
USER_DIR   = $(SRC_DIR)/user
USER_ELF   = $(BUILD_DIR)/user.elf
USER_BIN   = $(BUILD_DIR)/user.bin

CFLAGS = -ffreestanding -mno-red-zone -g -O0 -mcmodel=large -fno-asynchronous-unwind-tables -I$(INC_DIR) -MMD -MP
UFLAGS = -ffreestanding -mno-red-zone -fno-builtin -fno-asynchronous-unwind-tables -g -O0 -I$(USER_DIR) -I$(LIB_DIR) -MMD -MP

KERNEL_SRCS_C = $(wildcard $(KERNEL_DIR)/*.c) \
                $(wildcard $(KERNEL_DIR)/*/*.c)
KERNEL_SRCS_S = $(wildcard $(KERNEL_DIR)/*.S) \
                $(wildcard $(KERNEL_DIR)/*/*.S)
KERNEL_OBJS   = $(patsubst $(SRC_DIR)/%.S,$(BUILD_DIR)/%.o,$(KERNEL_SRCS_S)) \
                $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(KERNEL_SRCS_C))
KERNEL_DEPS   = $(KERNEL_OBJS:.o=.d)

USER_SRCS_S = $(wildcard $(USER_DIR)/*.S)
USER_SRCS_C = $(wildcard $(USER_DIR)/*.c)
USER_OBJS   = $(patsubst $(SRC_DIR)/%.S,$(BUILD_DIR)/%.o,$(USER_SRCS_S)) \
              $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(USER_SRCS_C))
USER_DEPS   = $(USER_OBJS:.o=.d)

# libc：crt0 单独链接，其余归档进 libc.a
LIB_SRCS_C = $(wildcard $(LIB_DIR)/*.c)
LIBC_OBJS  = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(LIB_SRCS_C))
LIB_DEPS   = $(LIBC_OBJS:.o=.d) $(CRT0:.o=.d)

all: $(KERNEL)

-include $(KERNEL_DEPS)
-include $(USER_DEPS)
-include $(LIB_DEPS)

# 内核：CFLAGS（通配规则）
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.S
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# 用户态 + libc：UFLAGS
$(BUILD_DIR)/user/%.o: $(SRC_DIR)/user/%.S
	mkdir -p $(dir $@)
	$(CC) $(UFLAGS) -c -o $@ $<

$(BUILD_DIR)/user/%.o: $(SRC_DIR)/user/%.c
	mkdir -p $(dir $@)
	$(CC) $(UFLAGS) -c -o $@ $<

$(BUILD_DIR)/lib/%.o: $(SRC_DIR)/lib/%.S
	mkdir -p $(dir $@)
	$(CC) $(UFLAGS) -c -o $@ $<

$(BUILD_DIR)/lib/%.o: $(SRC_DIR)/lib/%.c
	mkdir -p $(dir $@)
	$(CC) $(UFLAGS) -c -o $@ $<

# 归档 libc.a（crt0 除外，链接时排第一）
$(LIBC_A): $(LIBC_OBJS)
	mkdir -p $(dir $@)
	$(AR) rcs $@ $^

# 用户程序：ELF 留符号供 gdb，bin 剥离后嵌入内核
# （crt0/libc 由 user.ld 隐入；-T 置 .o 后：ld 单遍，归档需后搜）
$(USER_ELF): $(CRT0) $(USER_OBJS) $(LIBC_A) $(USER_DIR)/user.ld
	mkdir -p $(dir $@)
	$(LD) -L$(BUILD_DIR)/lib -o $@ $(USER_OBJS) -T $(USER_DIR)/user.ld

$(USER_BIN): $(USER_ELF)
	$(OBJCOPY) -O binary $< $@

# user.bin → 目标文件（build 下执行，符号 _binary_user_bin_*）
USER_BLOB  = $(BUILD_DIR)/user_blob.o
$(USER_BLOB): $(USER_BIN)
	mkdir -p $(dir $@)
	cd $(BUILD_DIR) && $(LD) -r -b binary -o user_blob.o user.bin

KERNEL_OBJS += $(USER_BLOB)

$(KERNEL): $(KERNEL_OBJS) $(USER_BIN) $(KERNEL_DIR)/kernel.ld
	mkdir -p $(dir $@)
	$(LD) -T $(KERNEL_DIR)/kernel.ld -e _start -o $@ $(KERNEL_OBJS)

clean:
	rm -rf $(BUILD_DIR)

master: $(KERNEL)
	mkdir -p qemu/hda-contents/EFI/BOOT
	cp -f edk2\Build\OvmfX64\NOOPT_VS2026\FV\OVMF.fd OVMF.fd
	cp -f edk2\Build\CsosBootPkg\DEBUG_VS2026\X64\EfiBoot.efi qemu/hda-contents/EFI/BOOT/BOOTX64.EFI
	cp -f $(KERNEL) qemu/hda-contents/kernel.elf
	cp -f $(USER_ELF) qemu/hda-contents/user.elf

# 启动qemu模拟环境
qemu: master
	qemu-system-x86_64 \
		-s -S -m 512M \
		-smp 4 \
		-bios OVMF.fd \
		-drive format=raw,file=fat:rw:qemu/hda-contents \
		-net none \
		-serial stdio \
		-debugcon file:debug.log \
		-global isa-debugcon.iobase=0x402