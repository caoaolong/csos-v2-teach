# compositor 程序构建片段：由最外层 Makefile 负责 include，不可单独执行。
# 输入归属 + 合成输出都收归 compositor（独占 /dev/kbd0、/dev/mouse0 与显存）。
# 依赖最外层定义的：SRC_DIR、BUILD_DIR、CC、LD、OBJCOPY、UFLAGS、LIBC_A、CRT0。
ifeq ($(origin SRC_DIR),undefined)
$(error src/services/compositor/Makefile is a fragment, include it from the top-level Makefile)
endif

COMP_DIR = $(SRC_DIR)/services/compositor
COMP_ELF = $(BUILD_DIR)/compositor.elf
COMP_BIN = $(BUILD_DIR)/compositor.bin

COMP_SRCS_S = $(wildcard $(COMP_DIR)/*.S)
COMP_SRCS_C = $(wildcard $(COMP_DIR)/*.c)
COMP_OBJS   = $(patsubst $(SRC_DIR)/%.S,$(BUILD_DIR)/%.o,$(COMP_SRCS_S)) \
              $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(COMP_SRCS_C))
COMP_DEPS   = $(COMP_OBJS:.o=.d)

$(BUILD_DIR)/services/compositor/%.o: $(SRC_DIR)/services/compositor/%.S
	mkdir -p $(dir $@)
	$(CC) $(UFLAGS) -I$(COMP_DIR) -c -o $@ $<

$(BUILD_DIR)/services/compositor/%.o: $(SRC_DIR)/services/compositor/%.c
	mkdir -p $(dir $@)
	$(CC) $(UFLAGS) -I$(COMP_DIR) -c -o $@ $<

# compositor 程序：ELF 留符号供 gdb；暂不嵌入内核，等 spawn 就绪后由 init 拉起
# （crt0/libc 由 compositor.ld 隐入；-T 置 .o 后：ld 单遍，归档需后搜）
$(COMP_ELF): $(CRT0) $(COMP_OBJS) $(LIBC_A) $(COMP_DIR)/compositor.ld
	mkdir -p $(dir $@)
	$(LD) -L$(BUILD_DIR)/lib -o $@ $(COMP_OBJS) -T $(COMP_DIR)/compositor.ld

$(COMP_BIN): $(COMP_ELF)
	$(OBJCOPY) -O binary $< $@
