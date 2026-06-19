#!/bin/sh
# Guard M7 FFI callback setup slot claiming and publish/free ordering.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

for needle in \
  'lj_ccallback_new_l(lua_State *L, CTState *cts' \
  'callback_slot_claim_l(lua_State *L, CTState *cts)' \
  'callback_checkfunc(CTState *cts, CType *ct, CTypeID *idp)' \
  '*idp = ctype_rawid(cts, ctype_cid(ct->info))' \
  'callback_mcode_new_l(lua_State *L, CTState *cts)' \
  'callback_mcode_new_l(L, cts);  /* 11.5: mcode read-only after FFI init. */' \
  'lj_ccallback_init_l(lua_State *L, CTState *cts)' \
  'lj_ccallback_maxslot(void)' \
  'callback_owner_claim(lua_State **owner, MSize slot,' \
  'la_casptr((void **)&owner[slot], &expect, L,' \
  'callback_owner_load(owner, top) == NULL' \
  'TValue *func;' \
  'if (cbid == NULL || owner == NULL || func == NULL || sizeid == 0)' \
  'lj_mem_newvec(L, CALLBACK_MAX_SLOT, CTypeID1)' \
  'la_storeptr_rel((void **)&cts->cb.cbid, cbid)' \
  'lj_mem_newvec(L, CALLBACK_MAX_SLOT, TValue)' \
  'setnilV(&func[i])' \
  'la_storeptr_rel((void **)&cts->cb.func, func)' \
  'la_store32_rel(&cts->cb.sizeid, CALLBACK_MAX_SLOT)' \
  'callback_cbid_load(cbid, top)' \
  'callback_cbid_store(cbid, slot, id)' \
  'callback_func_load(CTState *cts, MSize slot,' \
  'callback_func_load(cts, slot, &tv)' \
  'lj_ccallback_func_store_l(lua_State *L, CTState *cts' \
  'setfuncV(L, &tv, fn)' \
  'copyTVrel(L, &func[slot], &tv)' \
  'lj_gc_barrierroot(L, &func[slot])' \
  'lj_ccallback_func_clear(CTState *cts, MSize slot)' \
  'lj_tab_storenilraw(&func[slot])' \
  'lj_gc_arena_markmem(g, func)' \
  'lj_gc2_markmem(g, func)' \
  'if (tvisfunc(&tv))' \
  'ffi_miscmap_store(lua_State *L, CTState *cts, GCstr *key,' \
  'FFI miscmap store saw FORWARD after lookup.' \
  'ffi_miscmap_store(L, cts, &cts->g->strempty, L->top-1)' \
  'lj_gc_pubtabobj(L, cts->miscmap, tabV(L->top-1))' \
  'lj_ccallback_new_l(L, cts' \
  'lj_ccallback_init_l(L, cts)' \
  'lj_tab_new(L, 0, 1)' \
  'lj_state_checkstack(L, 1)' \
  'setcdataV(L, L->top++, cd)' \
  'LJLIB_CF(ffi_callback_free)' \
  'la_store16_rel(&cbid[slot], 0)' \
  'la_loadptr_acq((void *const *)&owner[slot]) == NULL' \
  '11.5 disowned callback free: nil function before cbid release.' \
  '11.5 owned callback free: cbid release before owner release.' \
  'lj_ccallback_func_store_l(L, cts, slot, fn)' \
  'lj_ccallback_func_clear(cts, slot)' \
  'la_storeptr_rel((void **)&owner[slot], NULL)'
do
  if ! rg -F -q "$needle" "$ROOT/src"; then
    echo "guardrail: missing FFI callback install marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'lj_ccallback_new\(CTState|callback_slot_new|callback_mcode_new\(CTState|misc_token|lj_ctype_misc_lock|lj_ctype_misc_unlock' "$ROOT/src"; then
  echo "guardrail: callback setup wrappers must stay explicit-L only" >&2
  exit 1
fi

if rg -n 'lj_tab_getint\(cts->miscmap, \(int32_t\)slot\)|lj_tab_setint\(L, [^,]*, \(int32_t\)slot\)|lj_tab_new\(L, \(uint32_t\)lj_ccallback_maxslot\(\), 1\)' "$ROOT/src"; then
  echo "guardrail: callback function slots must stay in the CTState side array, not miscmap" >&2
  exit 1
fi

if ! awk '
  /static TValue \*ffi_miscmap_store\(lua_State \*L,/ { infn = 1; next }
  infn && /lj_tab_storetab\(L, dst,/ { raw = 1 }
  infn && /for \(;;\)/ { loop = 1 }
  infn && /lj_tab_setstr\(L, cts->miscmap, key\)/ { resolve = 1 }
  infn && /lj_tab_trystoretv_cas\(L, dst, src\) == LJ_TAB_STORE_CAS_OK/ { cas = 1 }
  infn && /FFI miscmap store saw FORWARD after lookup\./ { retry = 1 }
  infn && /^}/ { exit(!raw && loop && resolve && cas && retry ? 0 : 1) }
  END { if (raw || !loop || !resolve || !cas || !retry) exit 1 }
' "$ROOT/src/lib_ffi.c"; then
  echo "guardrail: FFI miscmap helper must CAS-publish and retry forwarded slots" >&2
  exit 1
fi

if ! awk '
  /LUALIB_API int luaopen_ffi\(lua_State \*L\)/ { infn = 1; next }
  infn && /lj_tab_storetab\(L, lj_tab_setstr\(L, cts->miscmap, &cts->g->strempty\),/ { raw = 1 }
  infn && /ffi_miscmap_store\(L, cts, &cts->g->strempty, L->top-1\)/ { helper = NR }
  infn && /lj_gc_pubtabobj\(L, cts->miscmap, tabV\(L->top-1\)\)/ { barrier = NR }
  infn && /^}/ { exit(!raw && helper && barrier && helper < barrier ? 0 : 1) }
  END { if (raw || !helper || !barrier || helper > barrier) exit 1 }
' "$ROOT/src/lib_ffi.c"; then
  echo "guardrail: luaopen_ffi must CAS-publish miscmap callback table before barrier" >&2
  exit 1
fi

if rg -n 'cb\.topid|MSize topid' "$ROOT/src/lj_ccallback.c" "$ROOT/src/lj_ctype.h"; then
  echo "guardrail: callback slot reuse must not depend on the old topid cursor" >&2
  exit 1
fi

if ! awk '
  /void lj_ccallback_init_l/ { init = 1 }
  init && /^}/ { init = 0 }
  init && /callback_slots_init_l\(L, cts\)/ { slots = NR }
  init && /callback_mcode_new_l\(L, cts\)/ { mcode = NR }
  /void \*lj_ccallback_new_l/ { innew = 1 }
  innew && /^}/ { innew = 0 }
  innew && /callback_mcode_new_l|lj_ctype_misc_lock|lj_ctype_misc_unlock/ { badlazy = 1 }
  innew && /callback_slot_claim_l\(L, cts\)/ { claim = NR }
  innew && /lj_ccallback_func_store_l\(L, cts, slot, fn\)/ { sawset = NR }
  innew && /callback_cbid_store\(cbid, slot, id\)/ { publish = NR }
  END { exit slots && mcode && slots < mcode && claim && sawset && publish &&
	      claim < sawset && sawset < publish && !badlazy ? 0 : 1 }
' "$ROOT/src/lj_ccallback.c"; then
  echo "guardrail: callback mcode must be initialized before runtime slot/function/cbid publish" >&2
  exit 1
fi

if ! awk '
  /static MSize callback_slot_claim_l/ { inslot = 1 }
  inslot && /^}/ { inslot = 0 }
  inslot && /callback_slots_init_l/ { badinit = 1 }
  inslot && /for \(top = 0; top < sizeid; top\+\+\)/ { sawfor = 1 }
  inslot && /TValue \*func = callback_func_slots\(cts\)/ { sawfunc = 1 }
  inslot && /cbid == NULL \|\| owner == NULL \|\| func == NULL \|\| sizeid == 0/ { sawcheck = 1 }
  inslot && /callback_cbid_load\(cbid, top\) == 0/ { sawcbid = 1 }
  inslot && /callback_owner_claim\(owner, top, L\)/ { sawclaim = 1 }
  END { exit sawfor && sawfunc && sawcheck && sawcbid && sawclaim && !badinit ? 0 : 1 }
' "$ROOT/src/lj_ccallback.c"; then
  echo "guardrail: callback slot claim must use owner CAS over the preallocated table" >&2
  exit 1
fi

if ! awk '
  /static int ffi_callback_set/ { inset = 1 }
  inset && /^}/ { inset = 0 }
  inset && /lj_ctype_misc_lock\(cts\)/ { badlock = 1 }
  inset && /11\.5 disowned callback free/ { branch = "disowned"; disowned = NR }
  inset && branch == "disowned" && /lj_ccallback_func_clear\(cts, slot\)/ && !disowned_nil {
    disowned_nil = NR
  }
  inset && branch == "disowned" && /la_store16_rel\(&cbid\[slot\], 0\)/ && !disowned_clear {
    disowned_clear = NR
  }
  inset && /11\.5 owned callback free/ { branch = "owned"; owned = NR }
  inset && branch == "owned" && /la_store16_rel\(&cbid\[slot\], 0\)/ && !owned_clear {
    owned_clear = NR
  }
  inset && branch == "owned" && /lj_ccallback_func_clear\(cts, slot\)/ && !owned_nil {
    owned_nil = NR
  }
  inset && branch == "owned" && /la_storeptr_rel\(\(void \*\*\)&owner\[slot\], NULL\)/ && !owned_owner {
    owned_owner = NR
  }
  END {
    exit !badlock &&
      disowned && disowned_nil && disowned_clear && disowned_nil < disowned_clear &&
      owned && owned_clear && owned_nil && owned_owner &&
      owned_clear < owned_nil && owned_nil < owned_owner ? 0 : 1
  }
' "$ROOT/src/lib_ffi.c"; then
  echo "guardrail: callback free publish order must distinguish owned and disowned slots" >&2
  exit 1
fi

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-callback-install.lua" \
  "${LJ_M7_FFI_CBACK_THREADS:-6}" "${LJ_M7_FFI_CBACK_ITERS:-64}"

echo "M7 FFI callback install guard passed"
