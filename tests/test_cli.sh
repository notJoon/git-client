#!/bin/sh
set -eu

bin=$1
tmp=$(mktemp -d "${TMPDIR:-/tmp}/git-client-cli-test.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
cd "$tmp"

test "$(printf 'hello\n' | "$bin" hash-object --stdin)" = \
    "$(printf 'hello\n' | git hash-object --stdin)"

"$bin" init >/dev/null
blob=$(printf 'hello\n' | "$bin" hash-object -w --stdin)
test "$blob" = "$(printf 'hello\n' | git hash-object --stdin)"
test "$("$bin" cat-file -p "$blob")" = "$(git cat-file -p "$blob")"

mkdir -p dir
printf 'hello\n' >alpha
printf '#!/bin/sh\nexit 0\n' >dir/run
chmod +x dir/run
"$bin" add alpha dir/run
test "$("$bin" write-tree)" = "$(git write-tree)"
"$bin" commit -m initial >/dev/null
git fsck --strict --no-dangling >/dev/null
test "$(git cat-file -t HEAD)" = commit
git diff --quiet
git diff --cached --quiet HEAD

"$bin" branch feature
test "$("$bin" branch)" = "$(git branch --no-color)"

printf 'main\n' >alpha
"$bin" add alpha
"$bin" commit -m main >/dev/null
"$bin" checkout feature >/dev/null
test "$("$bin" branch)" = "$(git branch --no-color)"
test "$(git symbolic-ref --short HEAD)" = feature
test "$(sed -n '1p' alpha)" = hello
test -x dir/run
git diff --quiet
git diff --cached --quiet HEAD

echo "CLI tests passed"
