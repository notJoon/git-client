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
main.c              entry point
object.c / .h       object store (blob, tree, commit)
sha1.c / .h         SHA-1 hashing
zlib_wrap.c / .h    zlib compression wrappers
tests/              tests
```
