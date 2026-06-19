#!/bin/sh
# Guard M7 FFI ctype duplicate-name publication.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
OUT=${TMPDIR:-/tmp}/lj_t-ffi-ctype-name-claim

for needle in \
  'ctype_hash_findname(CTState *cts, CTypeID id, GCstr *name,' \
  'lj_ctype_addname_unique(CTState *cts, CType *ct, CTypeID id,' \
  'ctype_hash_findname(cts, head, name, tmask)' \
  'ctype_abandon(cts, id)' \
  'return winner;  /* 11.2 named ctype duplicate winner. */' \
  'return id;  /* 11.2 CAS-prepend named ctype publication. */' \
  'lj_ctype_addname_unique(cp->cts, ct, sid,' \
  'lj_ctype_addname_unique(cp->cts, ct, constid, CPNS_DEFAULT)' \
  'lj_ctype_addname_unique(cp->cts, ct, id, CPNS_DEFAULT)' \
  'force_table_move_after_reserve(lua_State *L, CTState *cts)' \
  'assert(ct3 != ctype_get(cts, id3))' \
  'ffi.typeinfo exposed abandoned ctype' \
  'parser struct tag namespace was shadowed' \
  'parser typedef namespace was shadowed' \
  'parser duplicate enum constant was accepted' \
  'parser duplicate enum loser replaced winner'
do
  if ! rg -F -q "$needle" "$ROOT/src" "$ROOT/tests/t-ffi-ctype-name-claim.c"; then
    echo "guardrail: missing FFI ctype name-claim marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'lj_ctype_addname\(cp->cts' "$ROOT/src/lj_cparse.c"; then
  echo "guardrail: parser global name publication must use duplicate-aware claim helper" >&2
  exit 1
fi

if ! awk '
  /static void ffi_typeinfo_storeint\(lua_State \*L,/ { inint = 1; next }
  inint && /lj_tab_storeint\(L, dst,/ { rawint = 1 }
  inint && /for \(;;\)/ { intloop = 1 }
  inint && /setintV\(&tv, val\)/ { intmake = 1 }
  inint && /lj_tab_setstr\(L, tab, key\)/ { intresolve = 1 }
  inint && /lj_tab_trystoretv_cas\(L, dst, &tv\) == LJ_TAB_STORE_CAS_OK/ { intcas = 1 }
  inint && /FFI typeinfo int store saw FORWARD after lookup\./ { intretry = 1 }
  inint && /^}/ { intok = !rawint && intloop && intmake && intresolve && intcas && intretry; inint = 0 }
  /static void ffi_typeinfo_storestr\(lua_State \*L,/ { instr = 1; next }
  instr && /lj_tab_storestr\(L, dst,/ { rawstr = 1 }
  instr && /for \(;;\)/ { strloop = 1 }
  instr && /setstrV\(L, &tv, val\)/ { strmake = 1 }
  instr && /lj_tab_setstr\(L, tab, key\)/ { strresolve = 1 }
  instr && /lj_tab_trystoretv_cas\(L, dst, &tv\) == LJ_TAB_STORE_CAS_OK/ { strcas = 1 }
  instr && /FFI typeinfo string store saw FORWARD after lookup\./ { strretry = 1 }
  instr && /^}/ { strok = !rawstr && strloop && strmake && strresolve && strcas && strretry; instr = 0 }
  /LJLIB_CF\(ffi_typeinfo\)/ { infn = 1 }
  infn && /ctype_isabandoned\(info\)/ { abandoned = NR }
  infn && /lua_createtable\(L, 0, 4\)/ { create = NR }
  infn && /lj_tab_store(int|str)\(L, lj_tab_setstr/ { raw = 1 }
  infn && /ffi_typeinfo_storeint\(L, t, lj_str_newlit\(L, "info"\), \(int32_t\)info\)/ { info = NR }
  infn && /ffi_typeinfo_storeint\(L, t, lj_str_newlit\(L, "size"\), \(int32_t\)size\)/ { size = NR }
  infn && /ffi_typeinfo_storeint\(L, t, lj_str_newlit\(L, "sib"\), \(int32_t\)sib\)/ { sib = NR }
  infn && /ffi_typeinfo_storestr\(L, t, lj_str_newlit\(L, "name"\), name\)/ { name = NR }
  infn && /lj_gc_pubtab\(L, t\)/ { pub = NR }
  infn && /^}/ { infn = 0 }
  END {
    exit intok && strok && !raw && abandoned && create && info && size && sib &&
	 name && pub && abandoned < create && create < info && info < pub ? 0 : 1
  }
' "$ROOT/src/lib_ffi.c"; then
  echo "guardrail: ffi.typeinfo must hide abandoned ctypes and CAS-publish result fields" >&2
  exit 1
fi

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-ffi-ctype-name-claim.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
timeout 20s "$OUT"
"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-cdef-dup-stack.lua" \
  "${LJ_M7_FFI_CDEF_DUP_ROUNDS:-30}" \
  "${LJ_M7_FFI_CDEF_DUP_ITERS:-200}"

echo "M7 FFI ctype name-claim guard passed"
