CC      = x86_64-elf-gcc
LD      = x86_64-elf-ld
OBJCOPY = x86_64-elf-objcopy

SRC_DIR    = src
KERNEL_DIR = $(SRC_DIR)/kernel
INC_DIR    = $(SRC_DIR)/inc
BUILD_DIR  = build

# 上游 edk2 仓库（相对本项目）
EDK2_SRC   = ../edk2
# 本项目内同步下来的 edk2 内容
EDK2_DIR   = edk2

KERNEL     = $(BUILD_DIR)/kernel.elf

CFLAGS = -ffreestanding -mno-red-zone -g -O0 -mcmodel=large -fno-asynchronous-unwind-tables -I$(INC_DIR) -MMD -MP

KERNEL_SRCS_C = $(wildcard $(KERNEL_DIR)/*.c) \
                $(wildcard $(KERNEL_DIR)/*/*.c)
KERNEL_SRCS_S = $(wildcard $(KERNEL_DIR)/*.S) \
                $(wildcard $(KERNEL_DIR)/*/*.S)
KERNEL_OBJS   = $(patsubst $(SRC_DIR)/%.S,$(BUILD_DIR)/%.o,$(KERNEL_SRCS_S)) \
                $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(KERNEL_SRCS_C))

all: $(KERNEL)

# 编译C语言和汇编代码
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.S
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

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