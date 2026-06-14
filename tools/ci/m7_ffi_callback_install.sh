#!/bin/sh
# Guard M7 FFI callback setup slot/miscmap mutation bridge.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

for needle in \
  'lj_ccallback_new_l(lua_State *L, CTState *cts' \
  'callback_slot_new_l(lua_State *L, CTState *cts' \
  'callback_mcode_new_l(lua_State *L, CTState *cts)' \
  'lj_ccallback_init_l(lua_State *L, CTState *cts)' \
  'lj_ccallback_maxslot(void)' \
  'lj_ctype_misc_lock(cts)' \
  'lj_ctype_misc_unlock(cts)' \
  'lj_mem_newvec(L, CALLBACK_MAX_SLOT, CTypeID1)' \
  'la_storeptr_rel((void **)&cts->cb.cbid, cbid)' \
  'la_store32_rel(&cts->cb.sizeid, CALLBACK_MAX_SLOT)' \
  'callback_cbid_load(cbid, top)' \
  'callback_cbid_store(cbid, top, id)' \
  'setfuncV(L, &tv, fn)' \
  'copyTVrel(L, lj_tab_setint(L, t, (int32_t)slot), &tv)' \
  'lj_gc_pubtab(L, t)' \
  'lj_ccallback_new_l(L, cts' \
  'lj_ccallback_init_l(L, cts)' \
  'lj_tab_new(L, (uint32_t)lj_ccallback_maxslot(), 1)' \
  'lj_state_checkstack(L, 1)' \
  'setcdataV(L, L->top++, cd)' \
  'LJLIB_CF(ffi_callback_free)' \
  'la_store16_rel(&cbid[slot], 0)'
do
  if ! rg -F -q "$needle" "$ROOT/src"; then
    echo "guardrail: missing FFI callback install marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'lj_ccallback_new\(CTState|callback_slot_new\(CTState|callback_mcode_new\(CTState' "$ROOT/src"; then
  echo "guardrail: callback setup wrappers must stay explicit-L only" >&2
  exit 1
fi

if awk '
  /void \*lj_ccallback_new_l/ { innew = 1 }
  innew && /^}/ { innew = 0 }
  innew && /lj_ctype_misc_lock\(cts\)/ { sawlock = 1 }
  innew && /lj_tab_setint\(L, t, \(int32_t\)slot\)/ { sawset = 1 }
  innew && /lj_ctype_misc_unlock\(cts\)/ { sawunlock = 1 }
  END { exit sawlock && sawset && sawunlock ? 1 : 0 }
' "$ROOT/src/lj_ccallback.c"; then
  echo "guardrail: callback slot/miscmap install must stay covered by misc token" >&2
  exit 1
fi

if awk '
  /static MSize callback_slot_new_l/ { inslot = 1 }
  inslot && /^}/ { inslot = 0 }
  inslot && /callback_mcode_new_l\(L, cts\)/ { sawmcode = 1 }
  inslot && /for \(top = cts->cb.topid/ { sawfor = 1; if (!sawmcode) bad = 1 }
  END { exit sawmcode && sawfor && !bad ? 1 : 0 }
' "$ROOT/src/lj_ccallback.c"; then
  echo "guardrail: callback mcode must be allocated before slot scan" >&2
  exit 1
fi

if awk '
  /static int ffi_callback_set/ { inset = 1 }
  inset && /^}/ { inset = 0 }
  inset && /lj_ctype_misc_lock\(cts\)/ { sawlock = 1 }
  inset && /la_store16_rel\(&cbid\[slot\], 0\)/ { sawclear = 1 }
  inset && /lj_ctype_misc_unlock\(cts\)/ { sawunlock = 1 }
  END { exit sawlock && sawclear && sawunlock ? 1 : 0 }
' "$ROOT/src/lib_ffi.c"; then
  echo "guardrail: callback free/set must stay covered by misc token" >&2
  exit 1
fi

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-callback-install.lua" \
  "${LJ_M7_FFI_CBACK_THREADS:-6}" "${LJ_M7_FFI_CBACK_ITERS:-64}"

echo "M7 FFI callback install guard passed"
