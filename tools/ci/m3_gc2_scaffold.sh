#!/bin/sh
# Run the Lua-defined M3 GC2 scaffold aggregate.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

check_raw_finreg_udata_next() {
  label=$1
  file=$2
  start=$3
  if hits=$(sed -n "/$start/,/^}/p" "$file" | \
      grep -nE -- 'node[[:space:]]*->[[:space:]]*next' || true); \
      [ -n "$hits" ]; then
    printf '%s\n' "$label" >&2
    printf '%s\n' "$hits" >&2
    printf '%s\n' 'raw GC2 FINREG userdata next-link access is forbidden; use gc2_finreg_udata_next_* helpers' >&2
    exit 1
  fi
}

check_raw_finreg_udata_next "src/lj_gc.c:gc_separateudata_registered" \
  "$ROOT/src/lj_gc.c" "static size_t gc_separateudata_registered"
check_raw_finreg_udata_next "src/lj_gc2.c:lj_gc2_fini" \
  "$ROOT/src/lj_gc2.c" "void lj_gc2_fini"
check_raw_finreg_udata_next "src/lj_gc2.c:lj_gc2_finreg_udata_register" \
  "$ROOT/src/lj_gc2.c" "void lj_gc2_finreg_udata_register"
check_raw_finreg_udata_next "src/lj_gc2.c:lj_gc2_finreg_udata_forget" \
  "$ROOT/src/lj_gc2.c" "void lj_gc2_finreg_udata_forget"

for helper in gc2_finreg_udata_head_acq \
  gc2_finreg_udata_head_store_rlx \
  gc2_finreg_udata_head_xchg_acqrel \
  gc2_finreg_udata_head_cas \
  gc2_finreg_udata_retired_acq \
  gc2_finreg_udata_retired_store_rlx \
  gc2_finreg_udata_retired_xchg_acqrel \
  gc2_finreg_udata_retired_cas; do
  if ! grep -qE "^[[:space:]]*${helper}[[:space:]]*[(]|static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "${helper} helper is required for GC2 FINREG userdata root state" >&2
    exit 1
  fi
done
if hits=$(grep -nE -- '->[[:space:]]*gc2[.]finreg_udata_(head|retired)([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*gc2[.]finreg_udata_(head|retired)([^[:alnum:]_]|$)' \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc2.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 FINREG userdata root access is forbidden; use gc2_finreg_udata_* helpers' >&2
  exit 1
fi
for helper in gc2_finreg_udata_sets_acq \
  gc2_finreg_udata_sets_store_rlx \
  gc2_finreg_udata_sets_add \
  gc2_finreg_udata_clears_acq \
  gc2_finreg_udata_clears_store_rlx \
  gc2_finreg_udata_clears_add \
  gc2_finreg_udata_queued_acq \
  gc2_finreg_udata_queued_store_rlx \
  gc2_finreg_udata_queued_add \
  gc2_finreg_udata_registered_acq \
  gc2_finreg_udata_registered_store_rlx \
  gc2_finreg_udata_registered_add \
  gc2_finreg_udata_retired_nodes_acq \
  gc2_finreg_udata_retired_nodes_store_rlx \
  gc2_finreg_udata_retired_nodes_add \
  gc2_finreg_udata_discovered_acq \
  gc2_finreg_udata_discovered_store_rlx \
  gc2_finreg_udata_discovered_add \
  gc2_finreg_udata_forgets_acq \
  gc2_finreg_udata_forgets_store_rlx \
  gc2_finreg_udata_forgets_add; do
  if ! grep -qE "^[[:space:]]*${helper}[[:space:]]*[(]|static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "${helper} helper is required for GC2 FINREG userdata counters" >&2
    exit 1
  fi
done
if hits=$(grep -nE -- '->[[:space:]]*gc2[.](finreg_udata_(sets|clears|queued|registered|retired_nodes|discovered|forgets))([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*gc2[.](finreg_udata_(sets|clears|queued|registered|retired_nodes|discovered|forgets))([^[:alnum:]_]|$)' \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc2.c" \
    "$ROOT/src/lib_base.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GC2 FINREG userdata counter access is forbidden; use gc2_finreg_udata_* helpers' >&2
  exit 1
fi

if hits=$(grep -nE -- '->[[:space:]]*hdr[.]next|hdr[.]next[[:space:]]*=' \
    "$ROOT/src/lj_arena.c" \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc2.c" \
    "$ROOT/tests/t-arena-alloc.c" \
    "$ROOT/tests/t-arena-gcsweep.c" \
    "$ROOT/tests/t-arena-map.c" \
    "$ROOT/tests/t-gc2-phase.c" \
    "$ROOT/tests/t-gc2-worker-scheduler.c" \
    "$ROOT/tests/t-safepoint-handshake.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw GCArena hdr.next access is forbidden; use lj_arena_next_* helpers' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m3_gc2_scaffold
