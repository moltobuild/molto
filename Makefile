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

# __USE_MINGW_ANSI_STDIO: mingw's default printf comes from msvcrt, which does
# not know `%z`. Molto prints a size with it in forty-one places, and each one
# would come out as the literal letters on Windows. The macro asks mingw for its
# own conforming implementation, and has to be set before <stdio.h> — which a
# -D does and a #define in one file cannot. Nothing on Linux reads it.
CFLAGS ?= -std=$(STD) -D_DEFAULT_SOURCE -D__USE_MINGW_ANSI_STDIO=1 -Wall -Wextra -Wpedantic -pthread -Iinclude
# Every object depends on Project.toml, and so does the test binary, because
# this define is the one input to a build that no source file mentions. The
# header dependencies below solve the same problem for headers; without the
# same treatment here, releasing edits the manifest, nothing looks out of date,
# and the binary keeps answering with the version it was first compiled at.
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

.PHONY: all build run test fuzz fuzz-corpus fuzz-run coverage clean

all: build

build: $(BIN)

$(BIN): $(LIB_OBJ) $(MAIN_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: %.c Project.toml
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Header dependencies, so editing a header rebuilds what includes it. Without
# this, changing a struct recompiles only the file it lives in and leaves the
# rest reading the old layout — which is exactly the incremental correctness
# molto itself provides, and its bootstrap lacked.
-include $(LIB_OBJ:.o=.d) $(MAIN_OBJ:.o=.d)

run: build
	./$(BIN) $(ARGS)

# TEST_ARGS reaches the suite: `make test TEST_ARGS="-k glob"` runs a few cases,
# which is what debugging one wants and what shelling out to the binary by hand
# was standing in for.
TEST_ARGS ?=

test: $(TEST_BIN)
	./$(TEST_BIN) $(TEST_ARGS)

$(TEST_BIN): $(LIB_OBJ) $(MOLTEST_SRC) $(TEST_SRC) Project.toml
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(MOLTEST_DIR)/include $(LIB_OBJ) $(MOLTEST_SRC) $(TEST_SRC) \
	    -o $@ $(LDFLAGS)

# What the suite covers.
#
# 863 tests is a number that says nothing about what is not executed, and the
# lines no test reaches are where the next bug is. Built apart from the normal
# objects because `--coverage` changes what the compiler emits, and a tree half
# instrumented measures itself wrong.
#
# gcov and nothing else: the aggregation is a shell script over its output, so
# this runs anywhere the tree already builds. lcov and genhtml are worth having
# to read a specific file line by line, and are not needed to answer the
# question this target exists for — which files does the suite not enter.
COVERAGE_OUT    := $(BUILD_DIR)/coverage
COVERAGE_OBJ    := $(LIB_SRC:%.c=$(COVERAGE_OUT)/obj/%.o)
COVERAGE_BIN    := $(COVERAGE_OUT)/molto_tests
COVERAGE_CFLAGS := $(CFLAGS) --coverage -O0 -g
COVERAGE_REPORT := .github/coverage.sh
# The gcov that matches CC. An older one refuses the notes a newer gcc wrote,
# and says "version 'B23', prefer 'B14'" — which sounds like a corrupt file and
# is a mismatched pair. Derived rather than assumed, because a machine with
# three gccs on it has a /usr/bin/gcov belonging to only one of them.
GCOV ?= $(patsubst gcc%,gcov%,$(notdir $(CC)))

coverage: $(COVERAGE_BIN)
	./$(COVERAGE_BIN) $(TEST_ARGS)
	@$(COVERAGE_REPORT) $(COVERAGE_OUT)/obj $(GCOV)

$(COVERAGE_OUT)/obj/%.o: %.c Project.toml
	@mkdir -p $(dir $@)
	$(CC) $(COVERAGE_CFLAGS) -MMD -MP -c $< -o $@

-include $(COVERAGE_OBJ:.o=.d)

$(COVERAGE_BIN): $(COVERAGE_OBJ) $(MOLTEST_SRC) $(TEST_SRC) Project.toml
	@mkdir -p $(dir $@)
	$(CC) $(COVERAGE_CFLAGS) -I$(MOLTEST_DIR)/include $(COVERAGE_OBJ) $(MOLTEST_SRC) \
	    $(TEST_SRC) -o $@ $(LDFLAGS) --coverage

# Fuzzing the parsers.
#
# Every target under fuzz/ drives one of the readers that take text Molto did
# not write: a manifest from a dependency, a recipe off the registry, what a
# compiler printed, what a plugin answered. They live outside tests/ on purpose
# — each one defines LLVMFuzzerTestOneInput, and six of those in one binary is a
# link error, which is what putting them in the suite would produce.
#
# clang only: libFuzzer is its runtime. The objects are instrumented with
# `fuzzer-no-link` and the target is linked with `fuzzer`, so the coverage
# tracking reaches the parser while only the target brings a main.
FUZZ_CC     ?= clang
FUZZ_DIR    := fuzz
FUZZ_OUT    := $(BUILD_DIR)/fuzz
FUZZ_SRC    := $(wildcard $(FUZZ_DIR)/*.c)
FUZZ_BIN    := $(FUZZ_SRC:$(FUZZ_DIR)/%.c=$(FUZZ_OUT)/bin/%)
FUZZ_OBJ    := $(LIB_SRC:%.c=$(FUZZ_OUT)/obj/%.o)
FUZZ_SANITIZE := address,undefined
# Derived rather than spelled again: a fuzz target that compiles with a
# different standard or a different define than the build is not testing the
# build. -O1 because libFuzzer wants the code optimised enough to be fast and
# unoptimised enough to have frames worth reading.
FUZZ_CFLAGS := $(CFLAGS) -g -O1 -fno-omit-frame-pointer -fsanitize=$(FUZZ_SANITIZE)

# How long each target runs under `fuzz-run`. Seconds, per target.
FUZZ_TIME ?= 60

# Which targets `fuzz-run` searches with. All of them by default; naming one
# spends the whole budget on the parser that was just touched, and lets a
# scheduled run give each its own job — so a finding says which parser without
# anyone reading a log.
FUZZ_TARGETS ?= $(patsubst $(FUZZ_DIR)/fuzz_%.c,%,$(FUZZ_SRC))

fuzz: $(FUZZ_BIN)

$(FUZZ_OUT)/obj/%.o: %.c Project.toml
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(FUZZ_CFLAGS) -fsanitize=fuzzer-no-link -MMD -MP -c $< -o $@

# The same reason the build has them: editing a header has to rebuild what
# includes it, or a fuzz target reads a struct that no longer has that shape.
-include $(FUZZ_OBJ:.o=.d)

$(FUZZ_OUT)/bin/%: $(FUZZ_DIR)/%.c $(FUZZ_OBJ)
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(FUZZ_CFLAGS) -fsanitize=fuzzer $< $(FUZZ_OBJ) -o $@ $(LDFLAGS)

# The corpus, and nothing else: every input under fuzz/corpus is replayed and
# the binary exits. Deterministic and quick, which is what makes it a test
# rather than a search — a crash found once is filed here and never comes back
# unnoticed.
fuzz-corpus: fuzz
	@for bin in $(FUZZ_BIN); do \
	    name=$${bin##*/fuzz_}; \
	    echo "== fuzz_$$name"; \
	    ./$$bin -runs=0 $(FUZZ_DIR)/corpus/$$name || exit 1; \
	done

# The search. Bounded by FUZZ_TIME so it can run unattended.
#
# Two directories, and the order is the point: libFuzzer writes what it
# generates into the first one and only reads the rest. So the growth goes under
# $(BUILD_DIR), which `clean` removes, and the corpus in the repository stays
# what a person put there — seeds and filed crashes, not ninety thousand bytes
# of mutation nobody chose to keep.
fuzz-run: fuzz
	@for name in $(FUZZ_TARGETS); do \
	    work=$(FUZZ_OUT)/work/$$name; \
	    mkdir -p $$work; \
	    echo "== fuzz_$$name"; \
	    $(FUZZ_OUT)/bin/fuzz_$$name $$work $(FUZZ_DIR)/corpus/$$name \
	        -max_total_time=$(FUZZ_TIME) -print_final_stats=1 || exit 1; \
	done

clean:
	rm -rf $(BUILD_DIR)
