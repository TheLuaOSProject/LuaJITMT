#!/bin/sh
# M7 FFI guard with Lua suite coverage.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

if hits=$(grep -nE -- 'la_load16_acq\(&[^)]*->[[:space:]]*sib\)' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lib_ffi.c" \
    "$ROOT/src/lj_cparse.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CType sibling acquire-loads are forbidden; use ctype_sib_acq()' >&2
  exit 1
fi
hits=$({ grep -nE -- '\b(ct|root|fct|cta|sct|ctf|rt)->[[:space:]]*sib\b' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lib_ffi.c" \
    "$ROOT/src/lj_cparse.c" || true; } |
  grep -v 'not copied to the ct->sib' || true)
if [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'direct shared CType sibling access is forbidden; use ctype_sib_* helpers' >&2
  exit 1
fi
if hits=$(grep -nE -- 'gcref_acq\([^)]*->[[:space:]]*name\)' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lib_ffi.c" \
    "$ROOT/src/lj_cparse.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CType name acquire-loads are forbidden; use ctype_nameobj_acq() or ctype_name_acq()' >&2
  exit 1
fi
if hits=$(grep -nE -- 'la_load32_acq\(&[^)]*->[[:space:]]*(info|size)\)' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lib_ffi.c" \
    "$ROOT/src/lj_cparse.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CType info/size acquire-loads are forbidden; use ctype_info_acq() or ctype_size_acq()' >&2
  exit 1
fi
if hits=$(awk '
  /^(CType \*lj_ctype_getfieldq|CTSize lj_ctype_size|CTSize lj_ctype_vlsize|CTInfo lj_ctype_info|CTInfo lj_ctype_info_raw|cTValue \*lj_ctype_meta|cTValue \*lj_ctype_metatv|static void ctype_repr)\(/ { in_fn = 1 }
  in_fn && /->[[:space:]]*(info|size)([^[:alnum:]_]|$)/ { print FNR ":" $0 }
  in_fn && /^}/ { in_fn = 0 }
' "$ROOT/src/lj_ctype.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CType info/size reads are forbidden in ctype query helpers; use ctype_info_acq() or ctype_size_acq()' >&2
  exit 1
fi
if hits=$(awk '
  /^int lj_ctype_snapshot\(/ ||
  /^static int ctype_snapshot_copy\(/ ||
  /^static int ctype_getfieldq_snapshot_rec\(/ ||
  /^int lj_ctype_ptrstruct_snapshot\(/ ||
  /^int lj_ctype_info_snapshot\(/ { in_fn = 1 }
  /^static void ctype_storestr_str\(/ ||
  /^int lj_ctype_getfieldq_snapshot\(/ ||
  /^\/\* -- C type information/ { in_fn = 0 }
  in_fn && /(^|[^[:alnum:]_])(ct|cct)\.(info|size|sib)([^[:alnum:]_]|$)/ { print FNR ":" $0 }
' "$ROOT/src/lj_ctype.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw local CType info/size/sibling reads are forbidden in ctype snapshot helpers; use CType helper loads' >&2
  exit 1
fi
if hits=$(awk '
  /^static int ffi_typecmp_rawid\(/ ||
  /^static int ffi_typecmp_rawrefid\(/ ||
  /^static int ffi_typecmp_childqual\(/ { in_fn = 1 }
  /^static int ffi_typecmp_compatptr\(/ { in_fn = 0 }
  in_fn && /->[[:space:]]*(info|size)([^[:alnum:]_]|$)/ { print FNR ":" $0 }
' "$ROOT/src/lib_ffi.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CType info/size reads are forbidden in ffi.istype typecmp walkers; use ctype_info_acq() or ctype_size_acq()' >&2
  exit 1
fi
if hits=$(awk '
  /^int lj_ctype_enumconst_snapshot\(/ { in_fn = 1 }
  /^static int ctype_enumconst_snapshot_id\(/ { in_fn = 0 }
  in_fn && /return[[:space:]]+0[[:space:]]*;/ { print FNR ":" $0 }
' "$ROOT/src/lj_ctype.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'enum constant snapshots must validate the parser sequence before returning a miss' >&2
  exit 1
fi
if hits=$(awk '
  /^int lj_ctype_getfieldq_snapshot\(/ ||
  /^static int ctype_getfieldq_snapshot_id\(/ { in_fn = 1 }
  /^int lj_ctype_getfieldq_wait\(/ ||
  /^int lj_ctype_ptrstruct_snapshot\(/ { in_fn = 0 }
  in_fn && /return[[:space:]]+0[[:space:]]*;/ { print FNR ":" $0 }
' "$ROOT/src/lj_ctype.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'field snapshots must validate the parser sequence before returning a miss' >&2
  exit 1
fi
if hits=$(grep -nE -- 'enum string readers wait out parser rollback|lj_ctype_getfield[[:space:]]*[(][[:space:]]*cts,[[:space:]]*(ct|d),' \
    "$ROOT/src/lj_carith.c" \
    "$ROOT/src/lj_cconv.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'enum string readers must use enumconst snapshot wait/retry instead of parser-lock fallback lookups' >&2
  exit 1
fi
if hits=$(grep -nE -- 'lj_ctype_enumconst_wait[[:space:]]*[(][^)]*const[[:space:]]+CType[[:space:]]*[*]' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lj_ctype.h" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'enum constant wait helpers must take CTypeID and refetch after native waits' >&2
  exit 1
fi
if hits=$(grep -nE -- 'cdata (pointer arithmetic|numeric-key) readers wait out parser rollback' \
    "$ROOT/src/lj_carith.c" \
    "$ROOT/src/lj_cdata.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'cdata element-size readers must use size wait/retry instead of parser-lock fallback lookups' >&2
  exit 1
fi
if hits=$(grep -nE -- 'lj_ctype_size_snapshot[[:space:]]*[(]' \
    "$ROOT/src/lj_carith.c" \
    "$ROOT/src/lj_cdata.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'interpreted cdata element-size readers must use lj_ctype_size_wait()' >&2
  exit 1
fi
if hits=$(grep -nE -- 'lj_ctype_size_wait[[:space:]]*[(][^)]*(const[[:space:]]+)?CType[[:space:]]*[*]' \
    "$ROOT/src/lj_ctype.c" \
    "$ROOT/src/lj_ctype.h" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'ctype size wait helpers must take CTypeID and refetch after native waits' >&2
  exit 1
fi
if hits=$(grep -nE -- 'cdata string-key readers wait out parser rollback' \
    "$ROOT/src/lj_cdata.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'cdata field readers must use field wait/retry instead of parser-lock fallback lookups' >&2
  exit 1
fi
if hits=$(awk '
  /^CType \*lj_cdata_index_l\(/ { in_fn = 1 }
  in_fn && /lj_ctype_parse_(lock|unlock)[[:space:]]*[(]/ { print FNR ":" $0 }
  in_fn && /^}/ { in_fn = 0 }
' "$ROOT/src/lj_cdata.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'lj_cdata_index_l must not acquire the ctype parser token' >&2
  exit 1
fi
if hits=$(awk '
  /^CType \*lj_cdata_index_l\(/ { in_fn = 1 }
  in_fn && /lj_ctype_getfieldq?[[:space:]]*[(]/ { print FNR ":" $0 }
  in_fn && /^}/ { in_fn = 0 }
' "$ROOT/src/lj_cdata.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'lj_cdata_index_l must use field wait/retry helpers, not direct locked field lookup' >&2
  exit 1
fi
if hits=$(awk '
  /^int lj_ctype_getfieldq_wait\(/ { in_sig = 1; sig = $0; next }
  in_sig {
    sig = sig " " $0
    if ($0 ~ /\)/) {
      if (sig !~ /CTypeID[[:space:]]+rootid/)
	print sig
      in_sig = 0
    }
  }
' "$ROOT/src/lj_ctype.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'field wait helpers must be rooted by CTypeID and refetch after native waits' >&2
  exit 1
fi
if hits=$(awk '
  /^static int ffi_typecmp_compatptr\(/ ||
  /^static int ffi_istype_snapshot\(/ { in_fn = 1 }
  /^static void ffi_istype_snapshot_wait\(/ ||
  /^LJLIB_CF\(ffi_istype\)/ { in_fn = 0 }
  in_fn && /(^|[^[:alnum:]_])(ct1|ct2|d|s)\.(info|size)([^[:alnum:]_]|$)/ { print FNR ":" $0 }
' "$ROOT/src/lib_ffi.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw local CType info/size reads are forbidden in ffi.istype snapshot checks; use ctype_info_acq() or ctype_size_acq()' >&2
  exit 1
fi
if hits=$(awk '
  /^LJLIB_CF\(ffi_sizeof\)/ || /^LJLIB_CF\(ffi_offsetof\)/ { in_fn = 1 }
  in_fn && /->[[:space:]]*(info|size)([^[:alnum:]_]|$)/ { print FNR ":" $0 }
  in_fn && /^}/ { in_fn = 0 }
' "$ROOT/src/lib_ffi.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CType info/size reads are forbidden in FFI layout query helpers; use ctype_info_acq() or ctype_size_acq()' >&2
  exit 1
fi
if hits=$(awk '
  /^static CTypeID ffi_checkctype_layout_lock\(/ { in_fn = 1; saw_string_gate = 0 }
  in_fn && /if[[:space:]]*\(!tvisstr\(o\)\)[[:space:]]*goto[[:space:]]+err_argtype[[:space:]]*;/ {
    saw_string_gate = 1
  }
  in_fn && /lj_ctype_parse_lock[[:space:]]*[(]/ && !saw_string_gate {
    print FNR ":" $0
  }
  in_fn && /^}/ { in_fn = 0 }
' "$ROOT/src/lib_ffi.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'ffi_checkctype_layout_lock must reject non-string ctype inputs before taking the parser token' >&2
  exit 1
fi
if hits=$(awk '
  /^LJLIB_CF\(ffi_new\)/ { in_fn = 1; saw_miss = 0 }
  in_fn && /if[[:space:]]*[(]ok[[:space:]]*==[[:space:]]*0[)]/ { saw_miss = 1 }
  in_fn && /ffi_checkctype_layout_lock[[:space:]]*[(]/ && !saw_miss {
    print FNR ":" $0
  }
  in_fn && /^}/ { in_fn = 0 }
' "$ROOT/src/lib_ffi.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'stable ffi.new() misses must not fall through to the parser-lock layout path' >&2
  exit 1
fi
if hits=$(awk '
  /^LJLIB_CF\(ffi_cast\)/ {
    in_fn = 1
    saw_noparse = 0
    saw_stable = 0
    saw_snapshot = 0
    saw_wait = 0
    saw_goto = 0
    saw_string_parse = 0
  }
  in_fn && /ffi_checkctype_noparse[[:space:]]*[(]/ { saw_noparse = 1 }
  in_fn && /if[[:space:]]*\(!isstr\)/ { saw_stable = 1 }
  in_fn && saw_stable && !saw_string_parse && /lj_ctype_info_snapshot[[:space:]]*[(]/ {
    saw_snapshot = 1
  }
  in_fn && saw_stable && !saw_string_parse && /lj_ctype_info_wait[[:space:]]*[(]/ {
    saw_wait = 1
  }
  in_fn && saw_stable && !saw_string_parse && /goto[[:space:]]+got_type[[:space:]]*;/ {
    saw_goto = 1
  }
  in_fn && /ffi_checkctype[[:space:]]*[(][[:space:]]*L,[[:space:]]*cts,[[:space:]]*NULL[[:space:]]*[)]/ {
    if (!saw_goto)
      print FNR ":" $0
    saw_string_parse = 1
  }
  in_fn && /ffi_checkctype_layout_lock[[:space:]]*[(]/ { print FNR ":" $0 }
  in_fn && /^}/ {
    if (!(saw_noparse && saw_stable && saw_snapshot && saw_wait &&
	  saw_goto && saw_string_parse))
      print FNR ":ffi_cast missing stable snapshot/wait before parser string path"
    in_fn = 0
  }
' "$ROOT/src/lib_ffi.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'stable ffi.cast() ctype-object inputs must wait/retry type snapshots before any parser-lock string path' >&2
  exit 1
fi
if hits=$(awk '
  /^LJLIB_CF\(ffi_sizeof\)/ { in_fn = 1; saw_miss = 0 }
  in_fn && /if[[:space:]]*[(]ok[[:space:]]*==[[:space:]]*0[)]/ { saw_miss = 1 }
  in_fn && /ffi_checkctype_layout_lock[[:space:]]*[(]/ && !saw_miss {
    print FNR ":" $0
  }
  in_fn && /^}/ { in_fn = 0 }
' "$ROOT/src/lib_ffi.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'stable ffi.sizeof() misses must not fall through to the parser-lock layout path' >&2
  exit 1
fi
if hits=$(awk '
  /^LJLIB_CF\(ffi_alignof\)/ { in_fn = 1; saw_miss = 0 }
  in_fn && /if[[:space:]]*[(]ok[[:space:]]*==[[:space:]]*0[)]/ { saw_miss = 1 }
  in_fn && /ffi_checkctype_layout_lock[[:space:]]*[(]/ && !saw_miss {
    print FNR ":" $0
  }
  in_fn && /^}/ { in_fn = 0 }
' "$ROOT/src/lib_ffi.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'stable ffi.alignof() misses must not fall through to the parser-lock layout path' >&2
  exit 1
fi
if hits=$(awk '
  /^LJLIB_CF\(ffi_offsetof\)/ {
    in_fn = 1
    saw_noparse = 0
    saw_snapshot = 0
    saw_wait = 0
    saw_miss = 0
    saw_isstr_gate = 0
    saw_string_parse = 0
  }
  in_fn && /ffi_checkctype_noparse[[:space:]]*[(]/ { saw_noparse = 1 }
  in_fn && /ffi_layout_offsetof_snapshot[[:space:]]*[(]/ { saw_snapshot = 1 }
  in_fn && /ffi_layout_offsetof_wait[[:space:]]*[(]/ { saw_wait = 1 }
  in_fn && /else[[:space:]]+if[[:space:]]*[(]ok[[:space:]]*==[[:space:]]*0[)]/ { saw_miss = 1 }
  in_fn && /if[[:space:]]*[(]isstr[)]/ { saw_isstr_gate = 1 }
  in_fn && /ffi_checkctype[[:space:]]*[(][[:space:]]*L,[[:space:]]*cts,[[:space:]]*NULL[[:space:]]*[)]/ {
    if (!(saw_noparse && saw_isstr_gate))
      print FNR ":" $0
    saw_string_parse = 1
  }
  in_fn && /ffi_checkctype_layout_lock[[:space:]]*[(]/ { print FNR ":" $0 }
  in_fn && /^}/ {
    if (!(saw_noparse && saw_snapshot && saw_wait && saw_miss &&
	  saw_isstr_gate && saw_string_parse))
      print FNR ":ffi_offsetof missing stable snapshot/wait/miss before parser string path"
    in_fn = 0
  }
' "$ROOT/src/lib_ffi.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'stable ffi.offsetof() ctype-object inputs must snapshot/wait and keep parser fallback string-gated' >&2
  exit 1
fi
if hits=$(awk '
  /^static int ffi_layout_rawref\(/ ||
  /^static int ffi_layout_rawid\(/ ||
  /^static int ffi_layout_raw\(/ ||
  /^static int ffi_layout_rawchild\(/ { in_fn = 1 }
  /^static int ffi_layout_info\(/ { in_fn = 0 }
  in_fn && /->[[:space:]]*(info|size)([^[:alnum:]_]|$)/ { print FNR ":" $0 }
' "$ROOT/src/lib_ffi.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CType info/size reads are forbidden in FFI layout raw snapshots; use ctype_info_acq() or ctype_size_acq()' >&2
  exit 1
fi
if hits=$(awk '
  /^static int ffi_layout_info\(/ ||
  /^static int ffi_layout_info_raw\(/ { in_fn = 1 }
  /^static int ffi_layout_vlsize\(/ { in_fn = 0 }
  in_fn && /(^|[^[:alnum:]_])ct\.(info|size)([^[:alnum:]_]|$)/ { print FNR ":" $0 }
' "$ROOT/src/lib_ffi.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw local CType info/size reads are forbidden in FFI layout info snapshots; use ctype_info_acq() or ctype_size_acq()' >&2
  exit 1
fi
if hits=$(awk '
  /^static int ffi_layout_getfield\(/ ||
  /^static int ffi_layout_offsetof_snapshot\(/ { in_fn = 1 }
  /^LJLIB_CF\(ffi_sizeof\)/ { in_fn = 0 }
  in_fn && /(^|[^[:alnum:]_])(ct|cct)\.(info|size|sib)([^[:alnum:]_]|$)/ { print FNR ":" $0 }
' "$ROOT/src/lib_ffi.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw local CType info/size/sibling reads are forbidden in FFI field layout snapshots; use CType helper loads' >&2
  exit 1
fi
if hits=$(awk '
  /^static int ffi_layout_vlsize\(/ { in_fn = 1 }
  /^static int ffi_new_layout_snapshot\(/ { in_fn = 0 }
  /^static int ffi_layout_sizeof_snapshot\(/ { in_fn = 1 }
  /^static int ffi_layout_alignof_snapshot\(/ { in_fn = 0 }
  in_fn && /(^|[^[:alnum:]_])(cur|elem|ct)\.(info|size|sib)([^[:alnum:]_]|$)/ { print FNR ":" $0 }
' "$ROOT/src/lib_ffi.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw local CType info/size/sibling reads are forbidden in FFI layout size snapshots; use CType helper loads' >&2
  exit 1
fi
if hits=$(awk '
  /^void LJ_FASTCALL recff_ffi_fill\(/ ||
  /^void LJ_FASTCALL recff_ffi_xof\(/ ||
  /^static CTypeID crec_bit64_type\(/ ||
  /^void LJ_FASTCALL lj_crecord_tonumber\(/ { in_fn = 1 }
  /^void LJ_FASTCALL recff_ffi_typeof\(/ ||
  /^void LJ_FASTCALL recff_ffi_gc\(/ ||
  /^static TRef crec_bit64_arg\(/ ||
  /^TRef lj_crecord_loadiu64\(/ { in_fn = 0 }
  in_fn && /->[[:space:]]*(info|size)([^[:alnum:]_]|$)/ { print FNR ":" $0 }
' "$ROOT/src/lj_crecord.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw CType info/size reads are forbidden in recorded FFI library metadata helpers; use ctype_info_acq() or ctype_size_acq()' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m7_ffi_typeinfo_snapshot
