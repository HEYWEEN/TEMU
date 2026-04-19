CC      ?= clang
CFLAGS  := -std=c11 -Wall -Wextra -Werror -g -O0 -Iinclude
LDFLAGS :=

SRC_DIR   := src
BUILD_DIR := build
TARGET    := $(BUILD_DIR)/temu
GENEXPR   := $(BUILD_DIR)/tools/gen-expr/gen-expr

SRCS := $(shell find $(SRC_DIR) -name '*.c')
OBJS := $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

.PHONY: all clean run test test-expr test-expr-fuzz tools

all: $(TARGET)

tools: $(GENEXPR)

$(GENEXPR): tools/gen-expr/gen-expr.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@

$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $^ -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

run: $(TARGET)
	./$(TARGET)

test: test-expr

test-expr: $(TARGET)
	@./$(TARGET) -t tests/expr/basic.txt

test-expr-fuzz: $(TARGET) $(GENEXPR)
	@./tests/expr/fuzz.sh $(or $(N),500)

clean:
	rm -rf $(BUILD_DIR)
