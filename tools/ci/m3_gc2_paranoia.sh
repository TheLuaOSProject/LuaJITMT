#!/bin/sh
# Build with GC2 paranoia enabled and run the GC2 oracle tests.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
TMP=${TMPDIR:-/tmp}

for needle in \
  'gc_arena_verify_sweep_boundary(global_State *g)' \
  'gc2_paranoia_check_roots(global_State *g)' \
  'gc2_legacy_has_base(global_State *g, void *p)' \
  'for (o = gcref_acq(g->gc.root); o != NULL; o = lj_obj_gcw_acq(o))'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_gc.c" "$ROOT/src/lj_gc2.c"; then
    echo "guardrail: missing GC2 acquire root-walk marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'for \(o = gcref\(g->gc.root\); o != NULL; o = gcnext\(o\)\)' \
    "$ROOT/src/lj_gc.c" "$ROOT/src/lj_gc2.c"; then
  echo "guardrail: GC2 diagnostic root walks must acquire-load root links" >&2
  exit 1
fi

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" XCFLAGS="-DLUA_USE_ASSERT -DLJ_GC2_PARANOIA=1" \
  -j"$JOBS" >/dev/null
for name in t-gc2-paranoia t-gc2-phase t-gc2-markbits t-gc2-traverse; do
  out="$TMP/lj_${name}_paranoia"
  "$CC" $CFLAGS -DLUA_USE_ASSERT -DLJ_GC2_PARANOIA=1 -I"$ROOT/src" \
    "$ROOT/tests/$name.c" "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$out"
  "$out"
done
"$ROOT/tools/ci/run_stock_tests.sh" "$ROOT/src/luajit" --quiet

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" \
  BUILDMODE=static \
  XCFLAGS="-DLUA_USE_ASSERT -DLJ_GC2_PARANOIA=1 -DLUAJIT_DISABLE_JIT" \
  -j"$JOBS" >/dev/null
"$ROOT/tools/ci/run_stock_tests.sh" "$ROOT/src/luajit" --quiet -jit
