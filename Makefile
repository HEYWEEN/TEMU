CC      ?= clang
CFLAGS  := -std=c11 -Wall -Wextra -Werror -g -O0 -Iinclude
LDFLAGS :=

SRC_DIR   := src
BUILD_DIR := build
TARGET    := $(BUILD_DIR)/temu

SRCS := $(shell find $(SRC_DIR) -name '*.c')
OBJS := $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

.PHONY: all clean run test

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $^ -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

run: $(TARGET)
	./$(TARGET)

test:
	@echo "No tests yet (Stage 0)"

clean:
	rm -rf $(BUILD_DIR)
