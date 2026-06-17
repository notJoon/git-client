# git-client — build, test, and tooling

CC      ?= cc
CSTD    ?= c11
CFLAGS  ?= -std=$(CSTD) -Wall -Wextra -Wpedantic -Wshadow -Wconversion -O2 -g
CPPFLAGS = -I.
LDFLAGS ?=
LDLIBS  ?= -lz

BIN        := git-client
BUILD      := build
TEST_BUILD := $(BUILD)/tests
SRC        := $(wildcard *.c)
OBJ        := $(patsubst %.c,$(BUILD)/%.o,$(SRC))
DEP        := $(OBJ:.o=.d)
SHA1_TEST  := $(TEST_BUILD)/test_sha1
ZLIB_TEST  := $(TEST_BUILD)/test_zlib
OBJECT_TEST := $(TEST_BUILD)/test_object
REFS_TEST  := $(TEST_BUILD)/test_refs

.PHONY: all build run test clean fmt fmt-check tidy help

all: build

build: $(BUILD)/$(BIN) ## Compile the binary

$(BUILD)/$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -MMD -MP -c -o $@ $<

run: build ## Build and run the binary (pass ARGS="...")
	./$(BUILD)/$(BIN) $(ARGS)

test: ## Run tests
	@test -f sha1.h || { echo "missing sha1.h; implement the SHA-1 API from spec.md first"; exit 1; }
	@test -f sha1.c || { echo "missing sha1.c; implement the SHA-1 API from spec.md first"; exit 1; }
	@test -f zlib_wrap.h || { echo "missing zlib_wrap.h; implement the zlib API from spec.md first"; exit 1; }
	@test -f zlib_wrap.c || { echo "missing zlib_wrap.c; implement the zlib API from spec.md first"; exit 1; }
	@test -f object.h || { echo "missing object.h; implement the object API from spec.md first"; exit 1; }
	@test -f object.c || { echo "missing object.c; implement the Blob object store from spec.md first"; exit 1; }
	$(MAKE) $(SHA1_TEST)
	$(MAKE) $(ZLIB_TEST)
	$(MAKE) $(OBJECT_TEST)
	$(MAKE) $(REFS_TEST)
	./$(SHA1_TEST)
	./$(ZLIB_TEST)
	./$(OBJECT_TEST)
	./$(REFS_TEST)

$(SHA1_TEST): tests/test_sha1.c sha1.c sha1.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tests/test_sha1.c sha1.c $(LDFLAGS) $(LDLIBS)

$(ZLIB_TEST): tests/test_zlib.c zlib_wrap.c zlib_wrap.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tests/test_zlib.c zlib_wrap.c $(LDFLAGS) $(LDLIBS)

$(OBJECT_TEST): tests/test_object.c object.c sha1.c zlib_wrap.c object.h sha1.h zlib_wrap.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tests/test_object.c object.c sha1.c zlib_wrap.c $(LDFLAGS) $(LDLIBS)

$(REFS_TEST): tests/test_refs.c refs.c refs.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tests/test_refs.c refs.c $(LDFLAGS) $(LDLIBS)

fmt: ## Format sources with clang-format
	@clang-format -i *.c *.h tests/*.c

fmt-check: ## Verify formatting without writing
	@clang-format --dry-run --Werror *.c *.h tests/*.c

tidy: ## Run clang-tidy over sources
	@clang-tidy $(wildcard *.c) -- $(CPPFLAGS) $(CFLAGS)

clean: ## Remove build artifacts
	rm -rf $(BUILD)

help: ## Show this help
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | \
		awk 'BEGIN {FS = ":.*?## "}; {printf "  %-12s %s\n", $$1, $$2}'

-include $(DEP)
