CC      = x86_64-elf-gcc
LD      = x86_64-elf-ld
OBJCOPY = x86_64-elf-objcopy
AR      = x86_64-elf-ar

SRC_DIR    = src
KERNEL_DIR = $(SRC_DIR)/kernel
INC_DIR    = $(SRC_DIR)/inc
BUILD_DIR  = build

KERNEL     = $(BUILD_DIR)/kernel.elf

# 全用户程序统一链接脚本（默认脚本）：改布局只改 src/user.ld
USER_LD    = $(SRC_DIR)/user.ld

# 用户态 C 库与各程序构建片段（变量与规则下沉到各自目录，新增服务在此追加 include）
include $(SRC_DIR)/lib/Makefile
include $(SRC_DIR)/init/Makefile
include $(SRC_DIR)/services/compositor/Makefile
include $(SRC_DIR)/clients/term/Makefile
include $(SRC_DIR)/cmd/Makefile

CSFS       = csfs
INIT_IMG   = qemu/DMBR.img
INIT_PART  = 1
INIT_DST   = /sbin/init
COMP_DST   = /sbin/compositor
TERM_DST   = /bin/term
LS_DST     = /bin/ls
CAT_DST    = /bin/cat

CFLAGS = -ffreestanding -mno-red-zone -g -O0 -mcmodel=large -fno-asynchronous-unwind-tables -I$(INC_DIR) -MMD -MP
UFLAGS = -ffreestanding -mno-red-zone -fno-builtin -fno-asynchronous-unwind-tables -g -O0 -I$(LIB_DIR) -I$(INC_DIR) -MMD -MP

KERNEL_SRCS_C = $(wildcard $(KERNEL_DIR)/*.c) \
                $(wildcard $(KERNEL_DIR)/*/*.c)
KERNEL_SRCS_S = $(wildcard $(KERNEL_DIR)/*.S) \
                $(wildcard $(KERNEL_DIR)/*/*.S)
KERNEL_OBJS   = $(patsubst $(SRC_DIR)/%.S,$(BUILD_DIR)/%.o,$(KERNEL_SRCS_S)) \
                $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(KERNEL_SRCS_C))
KERNEL_DEPS   = $(KERNEL_OBJS:.o=.d)

all: $(KERNEL) $(INIT_ELF) $(COMP_ELF) $(TERM_ELF) $(LS_ELF) $(CAT_ELF)

-include $(KERNEL_DEPS)
-include $(INIT_DEPS)
-include $(COMP_DEPS)
-include $(TERM_DEPS)
-include $(LIB_DEPS)
-include $(CMD_DEPS)

# 内核：CFLAGS（通配规则）
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.S
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# init 二进制嵌入内核，随内核镜像一起加载
KERNEL_OBJS += $(INIT_BLOB)

$(KERNEL): $(KERNEL_OBJS) $(INIT_BIN) $(KERNEL_DIR)/kernel.ld
	mkdir -p $(dir $@)
	$(LD) -T $(KERNEL_DIR)/kernel.ld -e _start -o $@ $(KERNEL_OBJS)

clean:
	rm -rf $(BUILD_DIR)

master: $(KERNEL) $(INIT_ELF) $(COMP_ELF) $(TERM_ELF) $(LS_ELF) $(CAT_ELF) install-init install-compositor install-term install-ls install-cat
	mkdir -p qemu/hda-contents/EFI/BOOT
	cp -f edk2\Build\OvmfX64\NOOPT_VS2026\FV\OVMF.fd OVMF.fd
	cp -f edk2\Build\CsosBootPkg\DEBUG_VS2026\X64\EfiBoot.efi qemu/hda-contents/EFI/BOOT/BOOTX64.EFI
	cp -f $(KERNEL) qemu/hda-contents/kernel.elf
	cp -f $(INIT_ELF) qemu/hda-contents/init.elf
	cp -f $(COMP_ELF) qemu/hda-contents/compositor.elf
	cp -f $(TERM_ELF) qemu/hda-contents/term.elf
	cp -f $(LS_ELF) qemu/hda-contents/ls.elf
	cp -f $(CAT_ELF) qemu/hda-contents/cat.elf

# 将 init.elf 写入根盘 FAT32 分区，供 init=/sbin/init 加载
install-init: $(INIT_ELF)
	$(CSFS) $(INIT_IMG) cp $(INIT_ELF) $(INIT_DST) --partition $(INIT_PART) --mkdirs --overwrite

# 将 compositor.elf 写入根盘，供 init 拉起 /sbin/compositor
install-compositor: $(COMP_ELF)
	$(CSFS) $(INIT_IMG) cp $(COMP_ELF) $(COMP_DST) --partition $(INIT_PART) --mkdirs --overwrite

# 将 term.elf 写入根盘，供 init 拉起 /bin/term
install-term: $(TERM_ELF)
	$(CSFS) $(INIT_IMG) cp $(TERM_ELF) $(TERM_DST) --partition $(INIT_PART) --mkdirs --overwrite

# 将 ls.elf 写入根盘，供 shell 外跑 /bin/ls
install-ls: $(LS_ELF)
	$(CSFS) $(INIT_IMG) cp $(LS_ELF) $(LS_DST) --partition $(INIT_PART) --mkdirs --overwrite

# 将 cat.elf 写入根盘，供 shell 外跑 /bin/cat
install-cat: $(CAT_ELF)
	$(CSFS) $(INIT_IMG) cp $(CAT_ELF) $(CAT_DST) --partition $(INIT_PART) --mkdirs --overwrite

# VirtualBox：把 qemu/ 启动目录 + 数据盘打包成可运行虚拟机
# （需 Node.js 18+、npm link 的 csos-image，以及 VBoxManage；输出在 qemu/vbox/）
VBOX_OUT = qemu/vbox
VM_NAME ?= CSOS

vbox: master
	csos-image all --qemu qemu --out $(VBOX_OUT) --vm "$(VM_NAME)" --force

# 启动qemu模拟环境
qemu: master
	qemu-system-x86_64 \
		-s -S -m 512M \
		-smp 1 \
		-bios OVMF.fd \
		-drive format=raw,file=fat:rw:qemu/hda-contents \
		-drive format=raw,file=qemu/DMBR.img,if=ide,index=2,media=disk \
		-drive format=raw,file=qemu/DGPT.img,if=ide,index=3,media=disk \
		-net none \
		-serial stdio \
		-debugcon file:debug.log \
		-global isa-debugcon.iobase=0x402