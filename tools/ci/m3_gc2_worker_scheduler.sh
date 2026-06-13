#!/bin/sh
# Run the focused staged GC2 parked-worker scheduler test.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
TMP=${TMPDIR:-/tmp}

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" >/dev/null

for needle in \
  'void *worker_thread' \
  'uint32_t n_workers' \
  'uint32_t worker_stop' \
  'uint32_t worker_wake' \
  'uint32_t worker_started' \
  'uint32_t worker_exited' \
  'uint64_t worker_wakes' \
  'uint64_t worker_parks' \
  'uint64_t worker_async_progress' \
  'lj_gc2_worker_start(global_State *g)' \
  'lj_gc2_worker_stop(global_State *g)' \
  'lj_gc2_worker_wake(global_State *g)' \
  'static void *gc2_worker_main(void *arg)' \
  'la_futex_wait(&g->gc2.worker_wake, wake, -1)' \
  'la_futex_wake(&g->gc2.worker_wake, 1)' \
  '05 section 5.6.3 parked worker scheduler' \
  'assert(lj_gc2_worker_start(g) == 1)' \
  'test_async_sweep_and_stop' \
  'wait_until_marked' \
  'lj_gc2_worker_wake(g);'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_gc2.c" "$ROOT/src/lj_gc2.h" \
      "$ROOT/src/lj_obj.h" "$ROOT/tests/t-gc2-worker-scheduler.c"; then
    echo "guardrail: missing GC2 worker scheduler marker: $needle" >&2
    exit 1
  fi
done

out="$TMP/lj_t-gc2-worker-scheduler"
"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-gc2-worker-scheduler.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$out"
"$out"

echo "M3 GC2 worker scheduler test passed"
