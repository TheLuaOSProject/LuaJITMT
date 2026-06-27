#!/bin/sh
# Compatibility launcher for migrated Lua M7 FFI case.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -nE -- 'cts[[:space:]]*->[[:space:]]*cb[.](mcode|cbid|owner|func|sizeid)|&[[:space:]]*cts[[:space:]]*->[[:space:]]*cb[.](mcode|cbid|owner|func|sizeid)' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc2.c" \
    "$ROOT/src/lib_ffi.c" \
    "$ROOT/src/lj_ccallback.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CTState callback access is forbidden; use ctype_cb_* helpers' >&2
  exit 1
fi
if hits=$(awk '
  /^static int ffi_callback_set\(/ { in_fn = 1 }
  in_fn && /->[[:space:]]*(info|size)([^[:alnum:]_]|$)/ { print FNR ":" $0 }
  in_fn && /^}/ { in_fn = 0 }
' "$ROOT/src/lib_ffi.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CType info/size reads are forbidden in FFI callback install helpers; use ctype_info_acq() or ctype_size_acq()' >&2
  exit 1
fi
if ! grep -qE 'uint16_t flags;[[:space:]]*/[*] Internal cdata flags' \
    "$ROOT/src/lj_obj.h" ||
   ! grep -qF '#define LJ_CDATA_CALLBACK_FREE' "$ROOT/src/lj_obj.h"; then
  printf '%s\n' 'GCcdata must carry internal flags including LJ_CDATA_CALLBACK_FREE' >&2
  exit 1
fi
if ! awk '
  /cd->[[:space:]]*ctypeid[[:space:]]*=/ { need = FNR + 3 }
  need && FNR <= need && /cdata_flags_rel[[:space:]]*[(][[:space:]]*cd[[:space:]]*,[[:space:]]*0[[:space:]]*[)]/ {
    seen++
    need = 0
  }
  END { exit(seen >= 3 ? 0 : 1) }
' "$ROOT/src/lj_cdata.h" "$ROOT/src/lj_cdata.c"; then
  printf '%s\n' 'all cdata allocation paths must zero internal cdata flags' >&2
  exit 1
fi
if ! awk '
  /^static LJ_AINLINE void \*ffi_callback_ptr_acq[[:space:]]*[(]GCcdata \*cd[)]/ {
    in_load = 1
  }
  in_load && /la_loadptr_acq[[:space:]]*[(][[:space:]]*[(]void \*const \*[)][[:space:]]*cdataptr[[:space:]]*[(]cd[)]/ {
    loads_ptr = 1
  }
  in_load && /^}/ { in_load = 0 }
  /^static LJ_AINLINE int ffi_callback_isfree_acq[[:space:]]*[(]GCcdata \*cd[)]/ {
    in_isfree = 1
  }
  in_isfree && /cdata_flags_acq[[:space:]]*[(]cd[)]/ && /LJ_CDATA_CALLBACK_FREE/ {
    checks_free = 1
  }
  in_isfree && /^}/ { in_isfree = 0 }
  /^static LJ_AINLINE void ffi_callback_markfree[[:space:]]*[(]GCcdata \*cd[)]/ {
    in_mark = 1
  }
  in_mark && /cdata_flags_or_atomic[[:space:]]*[(][[:space:]]*cd[[:space:]]*,[[:space:]]*LJ_CDATA_CALLBACK_FREE[[:space:]]*[)]/ {
    marks_free = 1
  }
  in_mark && /^}/ { in_mark = 0 }
  /^static int ffi_callback_set[[:space:]]*[(]/ {
    in_fn = 1
  }
  in_fn && /ffi_callback_isfree_acq[[:space:]]*[(]cd[)]/ {
    saw_isfree = 1
  }
  in_fn && /lj_ccallback_ptr2slot[[:space:]]*[(][^,]+,[[:space:]]*ffi_callback_ptr_acq[[:space:]]*[(]cd[)]/ {
    uses_load = 1
  }
  in_fn && /ffi_callback_markfree[[:space:]]*[(]cd[)]/ {
    saw_mark = 1
  }
  in_fn && /ctype_cb_cbid_slot_rel[[:space:]]*[(]/ {
    if (!saw_mark)
      bad = FNR ":" $0
    saw_cbid_clear = 1
  }
  in_fn && /^}/ {
    in_fn = 0
  }
  END {
    if (bad != "")
      print bad
    exit(loads_ptr && checks_free && marks_free && saw_isfree &&
         uses_load && saw_mark &&
         saw_cbid_clear && bad == "" ? 0 : 1)
  }
' "$ROOT/src/lib_ffi.c"; then
  printf '%s\n' 'ffi.callback:free() must mark stale cdata before cbid/owner slot reuse while preserving the trampoline pointer' >&2
  exit 1
fi
if hits=$(awk '
  /^static CType \*callback_checkfunc\(/ { in_fn = 1 }
  in_fn && /->[[:space:]]*(info|size|sib)([^[:alnum:]_]|$)/ { print FNR ":" $0 }
  in_fn && /^}/ { in_fn = 0 }
' "$ROOT/src/lj_ccallback.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CType info/size/sib reads are forbidden in callback_checkfunc(); use ctype_*_acq() helpers' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m7_ffi_callback_install
