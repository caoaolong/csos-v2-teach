# display 占位程序构建片段：由最外层 Makefile 负责 include，不可单独执行。
# 后续改造成 Display Manager Service 时，在此追加源码与链接配置。
# 依赖最外层定义的：SRC_DIR、BUILD_DIR、CC、LD、OBJCOPY、UFLAGS、LIBC_A、CRT0。
ifeq ($(origin SRC_DIR),undefined)
$(error src/services/display/Makefile is a fragment, include it from the top-level Makefile)
endif

DISP_DIR = $(SRC_DIR)/services/display
DISP_ELF = $(BUILD_DIR)/display.elf
DISP_BIN = $(BUILD_DIR)/display.bin

DISP_SRCS_S = $(wildcard $(DISP_DIR)/*.S)
DISP_SRCS_C = $(wildcard $(DISP_DIR)/*.c)
DISP_OBJS   = $(patsubst $(SRC_DIR)/%.S,$(BUILD_DIR)/%.o,$(DISP_SRCS_S)) \
              $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(DISP_SRCS_C))
DISP_DEPS   = $(DISP_OBJS:.o=.d)

$(BUILD_DIR)/services/display/%.o: $(SRC_DIR)/services/display/%.S
	mkdir -p $(dir $@)
	$(CC) $(UFLAGS) -I$(DISP_DIR) -c -o $@ $<

$(BUILD_DIR)/services/display/%.o: $(SRC_DIR)/services/display/%.c
	mkdir -p $(dir $@)
	$(CC) $(UFLAGS) -I$(DISP_DIR) -c -o $@ $<

# display 程序：ELF 留符号供 gdb；暂不嵌入内核，等 spawn 就绪后由 init 拉起
# （crt0/libc 由 display.ld 隐入；-T 置 .o 后：ld 单遍，归档需后搜）
$(DISP_ELF): $(CRT0) $(DISP_OBJS) $(LIBC_A) $(DISP_DIR)/display.ld
	mkdir -p $(dir $@)
	$(LD) -L$(BUILD_DIR)/lib -o $@ $(DISP_OBJS) -T $(DISP_DIR)/display.ld

$(DISP_BIN): $(DISP_ELF)
	$(OBJCOPY) -O binary $< $@
