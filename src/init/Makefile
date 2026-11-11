# init 程序构建片段：由最外层 Makefile 负责 include，不可单独执行。
# 依赖最外层定义的：SRC_DIR、BUILD_DIR、CC、LD、OBJCOPY、UFLAGS、LIBC_A、CRT0。
ifeq ($(origin SRC_DIR),undefined)
$(error src/init/Makefile is a fragment, include it from the top-level Makefile)
endif

INIT_DIR  = $(SRC_DIR)/init
INIT_ELF  = $(BUILD_DIR)/init.elf
INIT_BIN  = $(BUILD_DIR)/init.bin
INIT_BLOB = $(BUILD_DIR)/init_blob.o

INIT_SRCS_S = $(wildcard $(INIT_DIR)/*.S)
INIT_SRCS_C = $(wildcard $(INIT_DIR)/*.c)
INIT_OBJS   = $(patsubst $(SRC_DIR)/%.S,$(BUILD_DIR)/%.o,$(INIT_SRCS_S)) \
              $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(INIT_SRCS_C))
INIT_DEPS   = $(INIT_OBJS:.o=.d)

$(BUILD_DIR)/init/%.o: $(SRC_DIR)/init/%.S
	mkdir -p $(dir $@)
	$(CC) $(UFLAGS) -I$(INIT_DIR) -c -o $@ $<

$(BUILD_DIR)/init/%.o: $(SRC_DIR)/init/%.c
	mkdir -p $(dir $@)
	$(CC) $(UFLAGS) -I$(INIT_DIR) -c -o $@ $<

# init 程序：ELF 留符号供 gdb，bin 剥离后嵌入内核
# （crt0/libc 由 init.ld 隐入；-T 置 .o 后：ld 单遍，归档需后搜）
$(INIT_ELF): $(CRT0) $(INIT_OBJS) $(LIBC_A) $(INIT_DIR)/init.ld
	mkdir -p $(dir $@)
	$(LD) -L$(BUILD_DIR)/lib -o $@ $(INIT_OBJS) -T $(INIT_DIR)/init.ld

$(INIT_BIN): $(INIT_ELF)
	$(OBJCOPY) -O binary $< $@

# init.bin → 目标文件（build 下执行，符号 _binary_init_bin_*）
$(INIT_BLOB): $(INIT_BIN)
	mkdir -p $(dir $@)
	cd $(BUILD_DIR) && $(LD) -r -b binary -o init_blob.o init.bin
