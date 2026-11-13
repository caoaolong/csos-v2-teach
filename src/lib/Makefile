# 用户态 C 库构建片段：由最外层 Makefile 负责 include，不可单独执行。
# 依赖最外层定义的：SRC_DIR、BUILD_DIR、CC、AR、UFLAGS。
ifeq ($(origin SRC_DIR),undefined)
$(error src/lib/Makefile is a fragment, include it from the top-level Makefile)
endif

LIB_DIR  = $(SRC_DIR)/lib
LIBC_A   = $(BUILD_DIR)/lib/libc.a
CRT0     = $(BUILD_DIR)/lib/crt0.o

LIB_SRCS_C = $(wildcard $(LIB_DIR)/*.c)
LIBC_OBJS  = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(LIB_SRCS_C))
LIB_DEPS   = $(LIBC_OBJS:.o=.d) $(CRT0:.o=.d)

$(BUILD_DIR)/lib/%.o: $(SRC_DIR)/lib/%.S
	mkdir -p $(dir $@)
	$(CC) $(UFLAGS) -c -o $@ $<

# stdio.o（printf）单列 -Os：init/display 二进制限一页，-O0 下 vprintf 超预算
$(BUILD_DIR)/lib/stdio.o: $(SRC_DIR)/lib/stdio.c
	mkdir -p $(dir $@)
	$(CC) $(UFLAGS) -Os -c -o $@ $<

$(BUILD_DIR)/lib/%.o: $(SRC_DIR)/lib/%.c
	mkdir -p $(dir $@)
	$(CC) $(UFLAGS) -c -o $@ $<

# 归档 libc.a（crt0 除外，链接时排第一）
$(LIBC_A): $(LIBC_OBJS)
	mkdir -p $(dir $@)
	$(AR) rcs $@ $^
