#!/bin/sh
# Exercise the built Linux executable with an independent stock bytecode producer.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
bin=$root/src/luajit
LUAJIT_STOCK=$(command -v "${LUAJIT_STOCK:-luajit}")
export LUAJIT_STOCK
export LUA_PATH="$root/src/?.lua;$root/tests/lib/?.lua;;"

"$bin" -e 'assert(jit.os == "Linux" and jit.arch == "x64" and jit.status() and require("ffi").abi("gc64"))'
"$LUAJIT_STOCK" -e 'assert(string.dump(function() end):sub(1, 4) == "\27LJ\2", "expected an original LuaJIT v2 bytecode producer")'
(
  cd "$root/tests/stock/test"
  "$bin" -jon test.lua --quiet
  "$bin" -joff test.lua --quiet
  timeout 180 "$bin" "$root/tools/ci/stock_bytecode_corpus.lua"
)

cd "$root"
for script in t-gc-workers t-jit-secondary t-jit-flush-thread-stress \
              t-jit-util-flush-race t-jit-park-vmevent-reentrant \
              t-x64-jit-rooted-upvalue t-x64-jit-side-upvalue \
              t-x64-jit-boxed-arithmetic \
              t-x64-jit-next-scalar t-ffi-callxs-production t-ffi-callback-runtime \
              t-ffi-jit-cnew-alloc t-ffi-cdata-shared-hammer \
              t-weak-modes t-strtab-gc-stress; do
  echo "$script"
  timeout 180 "$bin" "tests/$script.lua"
done
timeout 180 "$bin" bench/jit-parity.lua
