CC      = x86_64-elf-gcc
LD      = x86_64-elf-ld
OBJCOPY = x86_64-elf-objcopy

SRC_DIR    = src
KERNEL_DIR = $(SRC_DIR)/kernel
USER_DIR   = $(SRC_DIR)/user
INC_DIR    = $(SRC_DIR)/inc
BUILD_DIR  = build

KERNEL     = $(BUILD_DIR)/kernel.elf

USER_ELF   = $(BUILD_DIR)/user.elf
USER_BLOB  = $(BUILD_DIR)/libuser.o
USER_SRCS_S = $(wildcard $(USER_DIR)/*.S)
USER_OBJS   = $(patsubst $(SRC_DIR)/%.S,$(BUILD_DIR)/%.o,$(USER_SRCS_S))

CFLAGS = -ffreestanding -mno-red-zone -g -O0 -mcmodel=large -fno-asynchronous-unwind-tables -I$(INC_DIR) -MMD -MP

KERNEL_SRCS_C = $(wildcard $(KERNEL_DIR)/*.c) \
                $(wildcard $(KERNEL_DIR)/*/*.c)
KERNEL_SRCS_S = $(wildcard $(KERNEL_DIR)/*.S) \
                $(wildcard $(KERNEL_DIR)/*/*.S)
KERNEL_OBJS   = $(patsubst $(SRC_DIR)/%.S,$(BUILD_DIR)/%.o,$(KERNEL_SRCS_S)) \
                $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(KERNEL_SRCS_C))

all: $(KERNEL) $(USER_ELF)

# 编译C语言和汇编代码
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.S
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/user/%.o: $(USER_DIR)/%.S
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(USER_ELF): $(USER_OBJS)
	mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 -e user_demo_start -Ttext=0x40000000 -o $@ $(USER_OBJS)

$(USER_BLOB): $(USER_OBJS)
	mkdir -p $(dir $@)
	$(OBJCOPY) --only-section=.text \
		--rename-section .text=.rodata.user_blob,alloc,load,readonly,data,contents \
		$(USER_OBJS) $@

KERNEL_OBJS += $(USER_BLOB)

$(KERNEL): $(KERNEL_OBJS) $(SRC_DIR)/linker.ld
	mkdir -p $(dir $@)
	$(LD) -T $(SRC_DIR)/linker.ld -e _start -o $@ $(KERNEL_OBJS)

clean:
	rm -rf $(BUILD_DIR)

master: $(KERNEL)
	mkdir -p qemu/hda-contents/EFI/BOOT
	cp -f edk2\Build\OvmfX64\NOOPT_VS2026\FV\OVMF.fd OVMF.fd
	cp -f edk2\Build\CsosBootPkg\DEBUG_VS2026\X64\EfiBoot.efi qemu/hda-contents/EFI/BOOT/BOOTX64.EFI
	cp -f $(KERNEL) qemu/hda-contents/kernel.elf

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