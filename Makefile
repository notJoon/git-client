# git-client — build, test, and tooling

CC      ?= cc
CSTD    ?= c11
CFLAGS  ?= -std=$(CSTD) -Wall -Wextra -Wpedantic -Wshadow -Wconversion -O2 -g
CPPFLAGS = -Iinclude
LDFLAGS ?=
LDLIBS  ?=

BIN     := git-client
BUILD   := build
SRC     := $(shell find src -name '*.c')
OBJ     := $(patsubst src/%.c,$(BUILD)/%.o,$(SRC))
DEP     := $(OBJ:.o=.d)

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

test: ## Run tests (no tests yet)
	@echo "no tests configured yet — add *.c files under tests/ and wire them here"

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
