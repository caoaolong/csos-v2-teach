# cmd 小工具构建片段：ls / cat（/bin/*），由最外层 Makefile 负责 include，不可单独执行。
# 依赖最外层定义的：SRC_DIR、BUILD_DIR、CC、LD、OBJCOPY、UFLAGS、LIBC_A、CRT0。
ifeq ($(origin SRC_DIR),undefined)
$(error src/cmd/Makefile is a fragment, include it from the top-level Makefile)
endif

CMD_DIR = $(SRC_DIR)/cmd
LS_ELF  = $(BUILD_DIR)/ls.elf
CAT_ELF = $(BUILD_DIR)/cat.elf

CMD_SRCS_C = $(wildcard $(CMD_DIR)/*.c)
CMD_OBJS   = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(CMD_SRCS_C))
CMD_DEPS   = $(CMD_OBJS:.o=.d)

$(BUILD_DIR)/cmd/%.o: $(SRC_DIR)/cmd/%.c
	mkdir -p $(dir $@)
	$(CC) $(UFLAGS) -I$(CMD_DIR) -c -o $@ $<

# 小工具：ELF 留符号供 gdb；crt0/libc 由 user.ld 隐入
$(LS_ELF): $(BUILD_DIR)/cmd/ls.o $(CRT0) $(LIBC_A) $(USER_LD)
	mkdir -p $(dir $@)
	$(LD) -L$(BUILD_DIR)/lib -o $@ $(BUILD_DIR)/cmd/ls.o -T $(USER_LD)

$(CAT_ELF): $(BUILD_DIR)/cmd/cat.o $(CRT0) $(LIBC_A) $(USER_LD)
	mkdir -p $(dir $@)
	$(LD) -L$(BUILD_DIR)/lib -o $@ $(BUILD_DIR)/cmd/cat.o -T $(USER_LD)
