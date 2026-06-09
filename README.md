# git-client

A git client written in C.

## Requirements

- A C11 compiler (`cc`/`gcc`/`clang`)
- `make`
- `clang-format` (optional, for `make fmt`)

## Build & run

```sh
make build              # compile to build/git-client
make run                # build and run
make run ARGS="status"  # pass arguments
```

## Development

```sh
make fmt        # format sources
make fmt-check  # verify formatting
make tidy       # clang-tidy
make clean      # remove build/
make help       # list targets
```

## Layout

```
src/
  main.c            entry point
  cli/commands/     command implementations
  core/             core git logic (objects, refs, index, ...)
include/git-client/ public headers
tests/              tests (framework TBD)
```
