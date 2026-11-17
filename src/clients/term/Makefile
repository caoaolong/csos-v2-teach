# term 客户端构建片段：由最外层 Makefile 负责 include，不可单独执行。
# term 是 compositor 的 pipe 客户端（/bin/term），不碰任何 /dev/* 设备。
# 依赖最外层定义的：SRC_DIR、BUILD_DIR、CC、LD、OBJCOPY、UFLAGS、LIBC_A、CRT0。
ifeq ($(origin SRC_DIR),undefined)
$(error src/clients/term/Makefile is a fragment, include it from the top-level Makefile)
endif

TERM_DIR = $(SRC_DIR)/clients/term
TERM_ELF = $(BUILD_DIR)/term.elf
TERM_BIN = $(BUILD_DIR)/term.bin

TERM_SRCS_S = $(wildcard $(TERM_DIR)/*.S)
TERM_SRCS_C = $(wildcard $(TERM_DIR)/*.c)
TERM_OBJS   = $(patsubst $(SRC_DIR)/%.S,$(BUILD_DIR)/%.o,$(TERM_SRCS_S)) \
              $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(TERM_SRCS_C))
TERM_DEPS   = $(TERM_OBJS:.o=.d)

$(BUILD_DIR)/clients/term/%.o: $(SRC_DIR)/clients/term/%.S
	mkdir -p $(dir $@)
	$(CC) $(UFLAGS) -I$(TERM_DIR) -c -o $@ $<

$(BUILD_DIR)/clients/term/%.o: $(SRC_DIR)/clients/term/%.c
	mkdir -p $(dir $@)
	$(CC) $(UFLAGS) -I$(TERM_DIR) -c -o $@ $<

# term 程序：ELF 留符号供 gdb；由 init 拉起 /bin/term
# （crt0/libc 由 term.ld 隐入；-T 置 .o 后：ld 单遍，归档需后搜）
$(TERM_ELF): $(CRT0) $(TERM_OBJS) $(LIBC_A) $(TERM_DIR)/term.ld
	mkdir -p $(dir $@)
	$(LD) -L$(BUILD_DIR)/lib -o $@ $(TERM_OBJS) -T $(TERM_DIR)/term.ld

$(TERM_BIN): $(TERM_ELF)
	$(OBJCOPY) -O binary $< $@
