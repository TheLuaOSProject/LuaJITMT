#!/bin/sh
# Run the M5 table resize forwarding stress guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
STRESS="$ROOT/tests/t-tab-resize-stress.lua"
SUITE="$ROOT/tests/suites/m5_tables.lua"

for required in \
    "local function exercise_jit_store_resize()" \
    "local function jit_read_worker(" \
    "local function exercise_jit_read_resize()" \
    "local function exercise_finalizer_resize()" \
    "local function traversal_observer(" \
    "local function exercise_concurrent_traversal_resize()" \
    "exercise_weak_clear_resize()" \
    "exercise_gc_mark_resize()" \
    "exercise_finalizer_resize()" \
    "exercise_jit_store_resize()" \
    "exercise_jit_read_resize()" \
    "exercise_concurrent_traversal_resize()"; do
  if ! grep -Fq "$required" "$STRESS"; then
    printf 'table resize stress fixture is missing required coverage: %s\n' \
      "$required" >&2
    exit 1
  fi
done

for required in \
    "LJ_M5_TAB_RESIZE_STRESS_REPS" \
    "LJ_M5_TAB_RESIZE_STRESS_THREADS" \
    "LJ_M5_TAB_RESIZE_STRESS_JIT_REPS" \
    "LJ_M5_TAB_RESIZE_STRESS_JIT_READ_REPS" \
    "LJ_M5_TAB_RESIZE_STRESS_TRAVERSAL_ROUNDS" \
    "LJ_M5_TAB_RESIZE_STRESS_FIN_OBJECTS"; do
  if ! grep -Fq "$required" "$SUITE"; then
    printf 'm5_tab_resize_stress suite case is missing env wiring: %s\n' \
      "$required" >&2
    exit 1
  fi
done

if ! awk '
  /local function jit_read_worker\(/ { in_fn = 1; saw_array = 0; saw_hash = 0 }
  in_fn && /local av = tbl\[array_key\]/ { saw_array = 1 }
  in_fn && /local hv = tbl\[hash_key\]/ { saw_hash = 1 }
  in_fn && /assert\(av == want/ { saw_array_assert = 1 }
  in_fn && /assert\(hv == want/ { saw_hash_assert = 1 }
  in_fn && /^end$/ {
    if (!(saw_array && saw_hash && saw_array_assert && saw_hash_assert))
      exit 1
    in_fn = 0
  }
' "$STRESS"; then
  printf '%s\n' 'jit read stress must assert stable array and hash keys during resize' >&2
  exit 1
fi

if ! awk '
  /local function exercise_finalizer_resize\(/ { in_fn = 1 }
  in_fn && /ffi\.gc\(ctype\(i\), function\(cd\)/ { ffi_gc = 1 }
  in_fn && /weak\[i\] = obj/ { weak = 1 }
  in_fn && /harness\.fullgc\(3\)/ { fullgc = 1 }
  in_fn && /finalized\[i\] ~= true/ { live = 1 }
  in_fn && /^end$/ {
    if (!(ffi_gc && weak && fullgc && live))
      exit 1
    in_fn = 0
  }
' "$STRESS"; then
  printf '%s\n' 'finalizer resize stress must keep table-held cdata live across resize and GC' >&2
  exit 1
fi

if ! awk '
  /local function traversal_observer\(/ { in_fn = 1 }
  in_fn && /for k, v in pairs\(tbl\)/ { pairs = 1 }
  in_fn && /for i, v in ipairs\(tbl\)/ { ipairs = 1 }
  in_fn && /next\(tbl, nil\)/ { nextnil = 1 }
  in_fn && /^end$/ {
    if (!(pairs && ipairs && nextnil))
      exit 1
    in_fn = 0
  }
' "$STRESS"; then
  printf '%s\n' 'traversal stress must cover pairs(), ipairs(), and next(t, nil)' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m5_tab_resize_stress
