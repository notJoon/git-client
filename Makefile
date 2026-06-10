# git-client — build, test, and tooling

CC      ?= cc
CSTD    ?= c11
CFLAGS  ?= -std=$(CSTD) -Wall -Wextra -Wpedantic -Wshadow -Wconversion -O2 -g
CPPFLAGS = -Iinclude
LDFLAGS ?=
LDLIBS  ?= -lz

BIN        := git-client
BUILD      := build
TEST_BUILD := $(BUILD)/tests
SRC        := $(shell find src -name '*.c')
OBJ        := $(patsubst src/%.c,$(BUILD)/%.o,$(SRC))
DEP        := $(OBJ:.o=.d)
SHA1_TEST  := $(TEST_BUILD)/test_sha1
ZLIB_TEST  := $(TEST_BUILD)/test_zlib
OBJECT_TEST := $(TEST_BUILD)/test_object

.PHONY: all build run test clean fmt fmt-check tidy help

all: build

build: $(BUILD)/$(BIN) ## Compile the binary

$(BUILD)/$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -MMD -MP -c -o $@ $<

run: build ## Build and run the binary (pass ARGS="...")
	./$(BUILD)/$(BIN) $(ARGS)

test: ## Run tests
	@test -f include/sha1.h || { echo "missing include/sha1.h; implement the SHA-1 API from spec.md first"; exit 1; }
	@test -f src/sha1.c || { echo "missing src/sha1.c; implement the SHA-1 API from spec.md first"; exit 1; }
	@test -f include/zlib_wrap.h || { echo "missing include/zlib_wrap.h; implement the zlib API from spec.md first"; exit 1; }
	@test -f src/zlib_wrap.c || { echo "missing src/zlib_wrap.c; implement the zlib API from spec.md first"; exit 1; }
	@test -f include/object.h || { echo "missing include/object.h; implement the object API from spec.md first"; exit 1; }
	@test -f src/object.c || { echo "missing src/object.c; implement the Blob object store from spec.md first"; exit 1; }
	$(MAKE) $(SHA1_TEST)
	$(MAKE) $(ZLIB_TEST)
	$(MAKE) $(OBJECT_TEST)
	./$(SHA1_TEST)
	./$(ZLIB_TEST)
	./$(OBJECT_TEST)

$(SHA1_TEST): tests/test_sha1.c src/sha1.c include/sha1.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tests/test_sha1.c src/sha1.c $(LDFLAGS) $(LDLIBS)

$(ZLIB_TEST): tests/test_zlib.c src/zlib_wrap.c include/zlib_wrap.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tests/test_zlib.c src/zlib_wrap.c $(LDFLAGS) $(LDLIBS)

$(OBJECT_TEST): tests/test_object.c src/object.c src/sha1.c src/zlib_wrap.c include/object.h include/sha1.h include/zlib_wrap.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tests/test_object.c src/object.c src/sha1.c src/zlib_wrap.c $(LDFLAGS) $(LDLIBS)

fmt: ## Format sources with clang-format
	@find src include tests -name '*.c' -o -name '*.h' | xargs clang-format -i

fmt-check: ## Verify formatting without writing
	@find src include tests -name '*.c' -o -name '*.h' | xargs clang-format --dry-run --Werror

tidy: ## Run clang-tidy over sources
	@find src -name '*.c' | xargs -I{} clang-tidy {} -- $(CPPFLAGS) $(CFLAGS)

clean: ## Remove build artifacts
	rm -rf $(BUILD)

help: ## Show this help
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | \
		awk 'BEGIN {FS = ":.*?## "}; {printf "  %-12s %s\n", $$1, $$2}'

-include $(DEP)
