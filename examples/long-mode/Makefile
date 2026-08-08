# 进入 X64 Long Mode 的最小示例
# 用法：make / make qemu

CC      = x86_64-elf-gcc
LD      = x86_64-elf-ld
OBJCOPY = x86_64-elf-objcopy

BUILD_DIR = build

BOOT_BIN       = $(BUILD_DIR)/boot.bin
BOOTLOADER_BIN = $(BUILD_DIR)/bootloader.bin
BOOTLOADER_PAD = $(BUILD_DIR)/bootloader.pad
KERNEL_BIN     = $(BUILD_DIR)/kernel.bin
OS_IMG         = $(BUILD_DIR)/os.img

# bootloader 固定占 16 扇区，使内核落在物理地址 0xA000
BOOTLOADER_SIZE = 8192
# 镜像：1 个 MBR 扇区 + boot 读取的 32 扇区
IMG_SECTORS     = 64

.PHONY: all clean qemu qemu-gdb

CFLAGS = -g

all: $(OS_IMG)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/boot.o: boot.S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/bootloader.o: bootloader.S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/kernel.o: kernel.S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BOOT_BIN): $(BUILD_DIR)/boot.o boot.ld
	$(LD) -T boot.ld -o $(BUILD_DIR)/boot.elf $<
	$(OBJCOPY) -O binary $(BUILD_DIR)/boot.elf $@

$(BOOTLOADER_BIN): $(BUILD_DIR)/bootloader.o bootloader.ld
	$(LD) -T bootloader.ld -o $(BUILD_DIR)/bootloader.elf $<
	$(OBJCOPY) -O binary $(BUILD_DIR)/bootloader.elf $@

$(BOOTLOADER_PAD): $(BOOTLOADER_BIN)
	dd if=$< of=$@ bs=$(BOOTLOADER_SIZE) conv=sync status=none

$(KERNEL_BIN): $(BUILD_DIR)/kernel.o kernel.ld
	$(LD) -T kernel.ld -o $(BUILD_DIR)/kernel.elf $<
	$(OBJCOPY) -O binary $(BUILD_DIR)/kernel.elf $@

# 磁盘镜像布局：
#   LBA 0        : boot.bin (512)
#   LBA 1 ..     : bootloader.pad (8K) + kernel.bin
$(OS_IMG): $(BOOT_BIN) $(BOOTLOADER_PAD) $(KERNEL_BIN)
	dd if=/dev/zero of=$@ bs=512 count=$(IMG_SECTORS) status=none
	dd if=$(BOOT_BIN) of=$@ bs=512 seek=0 conv=notrunc status=none
	dd if=$(BOOTLOADER_PAD) of=$@ bs=512 seek=1 conv=notrunc status=none
	dd if=$(KERNEL_BIN) of=$@ bs=512 seek=$$((1 + $(BOOTLOADER_SIZE)/512)) conv=notrunc status=none

qemu: $(OS_IMG)
	qemu-system-x86_64 -s -S -m 64M -drive format=raw,file=$(OS_IMG)

clean:
	rm -rf $(BUILD_DIR)
