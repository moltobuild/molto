# Bootstrap build for molto.
# molto cannot build itself yet, so this Makefile drives gcc directly.
# Targets the C23 subset supported by gcc-12 (-std=c2x). Bump STD to c23
# and CC to gcc-14/clang-19 once a modern toolchain is available.

# Force gcc-12 over Make's built-in default (cc), but honor an explicit
# override from the environment or command line (make CC=clang-19 ...).
ifeq ($(origin CC),default)
    CC := gcc-12
endif

STD    ?= c2x
CFLAGS ?= -std=$(STD) -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -pthread -Iinclude
LDFLAGS ?=

BUILD_DIR := build
BIN       := $(BUILD_DIR)/molto
TEST_BIN  := $(BUILD_DIR)/molto_tests

LIB_SRC  := $(shell find src -name '*.c' ! -name 'main.c')
MAIN_SRC := src/main.c
TEST_SRC := $(shell find tests -name '*.c')

LIB_OBJ  := $(LIB_SRC:%.c=$(BUILD_DIR)/%.o)
MAIN_OBJ := $(MAIN_SRC:%.c=$(BUILD_DIR)/%.o)

.PHONY: all build run test clean

all: build

build: $(BIN)

$(BIN): $(LIB_OBJ) $(MAIN_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

run: build
	./$(BIN) $(ARGS)

test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): $(LIB_OBJ) $(TEST_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Itests $(LIB_OBJ) $(TEST_SRC) -o $@ $(LDFLAGS)

clean:
	rm -rf $(BUILD_DIR)
