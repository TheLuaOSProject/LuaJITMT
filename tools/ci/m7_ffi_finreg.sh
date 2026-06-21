#!/bin/sh
# Compatibility launcher for migrated Lua M7 FFI case.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -nE -- '(^|[^[:alnum:]_])(gen|ord)[[:space:]]*->[[:space:]]*next' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lj_gc.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw FINREG next-link access is forbidden; use fin_*_next_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- '(^|[^[:alnum:]_])(gen|head)[[:space:]]*->[[:space:]]*tab|&[[:space:]]*(gen|head)[[:space:]]*->[[:space:]]*tab' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lj_gc.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw FINREG generation table access is forbidden; use fin_gen_tab_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- 'cts[[:space:]]*->[[:space:]]*fin_(head|order_head|order_retired)|&[[:space:]]*cts[[:space:]]*->[[:space:]]*fin_(head|order_head|order_retired)' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lj_gc.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw FINREG CTState root access is forbidden; use fin_*_head_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- '(gcref_acq|setgcrefmt|setgcrefnullrel)[(].*(t|ft|headtab)[[:space:]]*->[[:space:]]*metatable' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lj_gc.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw FINREG generation liveness access is forbidden; use fin_gen_tab_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- 'ord[[:space:]]*->[[:space:]]*(obj|tab|slot)' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lj_gc.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw FINREG ordered-node payload access is forbidden; use fin_order_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- 'ord[[:space:]]*->[[:space:]]*(retired_next|active)' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lj_gc.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw FINREG ordered-node retire state access is forbidden; use fin_order_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- '(makewhite|markfinalized|lj_gc_arena_markobj|lj_gc2_finreg_cdata_queue|lj_gc2_finalizer_enqueue)[(].*obj2gco[(]cd[)]' \
    "$ROOT/src/lj_cdata.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'cdata sweep/free must not rescue finalizers; FINREG discovery owns finalizer queueing' >&2
  exit 1
fi
if hits=$(grep -nE -- '(setgcrefnullrel|setgcrefrel)[(](g->gc2[.]finreg_cdata_preclaim_obj\[[^]]+\]|newobj\[[^]]+\])|gcref_acq[(]oldobj\[[^]]+\][)]' \
    "$ROOT/src/lj_gc2.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw FINREG preclaim object-slot access is forbidden; use gc2_queue_slot_* helpers' >&2
  exit 1
fi
for helper in gc2_finreg_cdata_preclaim_objvec_acq \
  gc2_finreg_cdata_preclaim_objvec_store_rlx \
  gc2_finreg_cdata_preclaim_objvec_rel \
  gc2_finreg_cdata_preclaim_finvec_acq \
  gc2_finreg_cdata_preclaim_finvec_store_rlx \
  gc2_finreg_cdata_preclaim_finvec_rel \
  gc2_finreg_cdata_preclaim_capacity_acq \
  gc2_finreg_cdata_preclaim_capacity_store_rlx \
  gc2_finreg_cdata_preclaim_capacity_rel \
  gc2_finreg_cdata_preclaim_head_acq \
  gc2_finreg_cdata_preclaim_head_store_rlx \
  gc2_finreg_cdata_preclaim_head_rel \
  gc2_finreg_cdata_preclaim_count_acq \
  gc2_finreg_cdata_preclaim_count_store_rlx \
  gc2_finreg_cdata_preclaim_count_rel; do
  if ! grep -qE "static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_obj.h"; then
    printf '%s\n' "${helper} helper is required for FINREG preclaim state" >&2
    exit 1
  fi
done
if hits=$(grep -nE -- '->[[:space:]]*gc2[.]finreg_cdata_preclaim_(obj|fin|capacity|head|count)([^[:alnum:]_]|$)|&[[:space:]]*[^)]*->[[:space:]]*gc2[.]finreg_cdata_preclaim_(obj|fin|capacity|head|count)([^[:alnum:]_]|$)' \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc2.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw FINREG preclaim state access is forbidden; use gc2_finreg_cdata_preclaim_* helpers' >&2
  exit 1
fi
if hits=$(awk '
  /^LJLIB_CF\(ffi_gc\)/ { in_fn = 1 }
  in_fn && /->[[:space:]]*(info|size)([^[:alnum:]_]|$)/ { print FNR ":" $0 }
  in_fn && /^}/ { in_fn = 0 }
' "$ROOT/src/lib_ffi.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CType info/size reads are forbidden in ffi.gc(); use ctype_info_acq() or ctype_size_acq()' >&2
  exit 1
fi
"$ROOT/tools/ci/lua_test.sh" m7_ffi_finreg
cc -std=gnu99 -O2 -Wall -Wextra -Werror -mcx16 -I"$ROOT/src" \
  "$ROOT/tests/t-ffi-finreg-free-invariant.c" "$ROOT/src/libluajit.a" \
  -lm -ldl -pthread -o /tmp/lj_t-ffi-finreg-free-invariant
/tmp/lj_t-ffi-finreg-free-invariant
