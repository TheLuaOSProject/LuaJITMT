#!/bin/sh
# Guard per-TG arena allocation routing under spawned Lua threads.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

for needle in \
  'gc_arena_allocd_for_new' \
  'gc_arena_allocd_for_ptr' \
  'lj_arena_allocd_alloc(gc_arena_allocd_for_new(L)' \
  'p ? gc_arena_allocd_for_ptr(g, p)' \
  'owner_tid'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_gc.c" "$ROOT/src/lj_arena.c" "$ROOT/src/lj_arena.h"; then
    echo "guardrail: missing per-TG allocator routing marker: $needle" >&2
    exit 1
  fi
done

for needle in \
  'threading_arena_internal' \
  'tg->alloc.owner_tid ='
do
  if ! rg -F -q "$needle" "$ROOT/src/lib_threading.c" "$ROOT/src/lj_tg.c"; then
    echo "guardrail: missing threading allocator owner marker: $needle" >&2
    exit 1
  fi
done

timeout 20s "$ROOT/src/luajit" -joff "$ROOT/tests/t-threading-alloc.lua" 4 6000

echo "M5 threading allocator routing tests passed"
