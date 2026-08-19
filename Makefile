# Bootstrap build for molto.
# molto builds itself (see README.md), but something has to compile the first
# one, so this Makefile drives gcc directly and hardcodes what molto asks
# pickup: a compiler that implements the C23 subset (-std=c2x). Bump STD to c23
# and CC to gcc-14/clang-19 once a modern toolchain is available.

# Force gcc-12 over Make's built-in default (cc), but honor an explicit
# override from the environment or command line (make CC=clang-19 ...).
ifeq ($(origin CC),default)
    CC := gcc-12
endif

STD    ?= c2x
# The version comes from the manifest, the way it does under molto: one place
# to change it, and no binary that disagrees with the file it was built from.
VERSION := $(shell sed -n 's/^version = "\(.*\)"/\1/p' Project.toml | head -1)

CFLAGS ?= -std=$(STD) -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -pthread -Iinclude
CFLAGS += -DMOLTO_PKG_VERSION='"$(VERSION)"'

# For a caller that wants to add to the build rather than replace it: -Werror,
# sanitizers, an optimisation level. Setting CFLAGS on the command line wins
# over both lines above, which quietly takes the version define with it — and a
# binary built that way answers `-V` with 0.0.0-unknown and fails the test that
# compares the two. Adding through here keeps everything that is not being
# changed.
EXTRA_CFLAGS ?=
CFLAGS += $(EXTRA_CFLAGS)

LDFLAGS ?=

BUILD_DIR := build
BIN       := $(BUILD_DIR)/molto
TEST_BIN  := $(BUILD_DIR)/molto_tests

LIB_SRC  := $(shell find src -name '*.c' ! -name 'main.c')
MAIN_SRC := src/main.c
TEST_SRC := $(shell find tests -name '*.c')

# moltest: the test framework, a standalone module linked only into the tests.
MOLTEST_DIR := modules/moltest
MOLTEST_SRC := $(shell find $(MOLTEST_DIR)/src -name '*.c')

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
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Header dependencies, so editing a header rebuilds what includes it. Without
# this, changing a struct recompiles only the file it lives in and leaves the
# rest reading the old layout — which is exactly the incremental correctness
# molto itself provides, and its bootstrap lacked.
-include $(LIB_OBJ:.o=.d) $(MAIN_OBJ:.o=.d)

run: build
	./$(BIN) $(ARGS)

test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): $(LIB_OBJ) $(MOLTEST_SRC) $(TEST_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(MOLTEST_DIR)/include $(LIB_OBJ) $(MOLTEST_SRC) $(TEST_SRC) \
	    -o $@ $(LDFLAGS)

clean:
	rm -rf $(BUILD_DIR)
