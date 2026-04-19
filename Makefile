CC      ?= clang
CFLAGS  := -std=c11 -Wall -Wextra -Werror -g -O0 -Iinclude
LDFLAGS :=

SRC_DIR   := src
BUILD_DIR := build
TARGET    := $(BUILD_DIR)/temu
GENEXPR   := $(BUILD_DIR)/tools/gen-expr/gen-expr
ISATEST   := $(BUILD_DIR)/tests/isa/isa
PROGTEST  := $(BUILD_DIR)/tests/programs/prog

SRCS := $(shell find $(SRC_DIR) -name '*.c')
OBJS := $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

.PHONY: all clean run test test-expr test-expr-fuzz test-pmem test-isa test-prog test-diff tools

all: $(TARGET)

tools: $(GENEXPR)

$(GENEXPR): tools/gen-expr/gen-expr.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@

$(ISATEST): tests/isa/isa.c tests/isa/isa-encoder.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) tests/isa/isa.c -o $@

$(PROGTEST): tests/programs/prog.c tests/isa/isa-encoder.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) tests/programs/prog.c -o $@

$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $^ -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

run: $(TARGET)
	./$(TARGET)

test: test-expr test-pmem test-isa test-prog

test-expr: $(TARGET)
	@./$(TARGET) -t tests/expr/basic.txt

test-expr-fuzz: $(TARGET) $(GENEXPR)
	@./tests/expr/fuzz.sh $(or $(N),500)

test-pmem: $(TARGET)
	@./tests/pmem/load.sh

test-isa: $(TARGET) $(ISATEST)
	@$(ISATEST) $(TARGET)

test-prog: $(TARGET) $(PROGTEST)
	@$(PROGTEST) $(TARGET)

# Re-run isa + program tests with differential testing enabled. The
# reference CPU in src/difftest/difftest.c lock-steps with the main
# ISA implementation; any divergence aborts the offending case.
test-diff: $(TARGET) $(ISATEST) $(PROGTEST)
	@TEMU_DIFFTEST=1 $(ISATEST)  $(TARGET)
	@TEMU_DIFFTEST=1 $(PROGTEST) $(TARGET)

clean:
	rm -rf $(BUILD_DIR)
