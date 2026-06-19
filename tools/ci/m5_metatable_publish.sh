#!/bin/sh
# Guard M5 release publication for runtime metatable stores.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

hits=$(
  cd "$ROOT" && \
  awk '
    function startfn() {
      active = 1
      depth = 0
      saw_body = 0
    }
    function brace_delta(line, nopen, nclose) {
      nopen = gsub(/\{/, "{", line)
      nclose = gsub(/\}/, "}", line)
      return nopen - nclose
    }
    /^LUA_API int lua_setmetatable\(/ { startfn() }
    /^LJLIB_ASM\(setmetatable\)/ { startfn() }
    active {
      if ($0 ~ /setgcref\([^;]*metatable/)
	print FILENAME ":" FNR ":" $0
      delta = brace_delta($0)
      if (delta != 0)
	saw_body = 1
      depth += delta
      if (saw_body && depth == 0)
	active = 0
    }
  ' src/lj_api.c src/lib_base.c
)

if [ -n "$hits" ]; then
  printf '%s\n' "$hits"
  echo "guardrail: runtime metatable publications must use setgcrefmt()" >&2
  exit 1
fi

raw_runtime_hits=$(rg -n 'setgcref\(t->metatable' \
  "$ROOT/src/lj_serialize.c" "$ROOT/src/lj_snap.c" \
  "$ROOT/src/lib_base.c" "$ROOT/src/lj_ctype.c" || true)
if [ -n "$raw_runtime_hits" ]; then
  echo "guardrail: runtime metatable publications must use release stores:" >&2
  echo "$raw_runtime_hits" >&2
  exit 1
fi

raw_udata_hits=$(rg -n 'setgcrefr?\(ud->metatable|setgcref\(ud->env' \
  "$ROOT/src" || true)
if [ -n "$raw_udata_hits" ]; then
  echo "guardrail: userdata constructor env/metatable publications must use release stores:" >&2
  echo "$raw_udata_hits" >&2
  exit 1
fi

raw_thread_env_hits=$(rg -n 'setgcrefr?\(L1->env' "$ROOT/src" || true)
if [ -n "$raw_thread_env_hits" ]; then
  echo "guardrail: new-thread env publication must use release stores:" >&2
  echo "$raw_thread_env_hits" >&2
  exit 1
fi

raw_sbuf_dict_hits=$(rg -n 'setgcref\(sbx->dict_(str|mt)' "$ROOT/src" || true)
if [ -n "$raw_sbuf_dict_hits" ]; then
  echo "guardrail: SBuf dictionary publications must use release stores:" >&2
  echo "$raw_sbuf_dict_hits" >&2
  exit 1
fi

for needle in \
  '#define tabref_acq(r)' \
  'gcref_acq((r))' \
  'LJ_FUNCA void lj_gc2_barrier_obj_pair' \
  'call extern lj_gc2_barrier_obj_pair' \
  'setgcrefmt(t->metatable, obj2gco(mt));' \
  'lj_gc_pubtabobj(sbufL(sbx), t, mt);' \
  'setgcrefnullrel(t->metatable);' \
  'lj_gc_pubtabobj(L, t, mt);' \
  'setgcrefrel(ud->env, obj2gco(env));' \
  'setgcrefmt(ud->metatable, obj2gco(env));' \
  'setgcrefmt(ud->metatable, obj2gco(mt));' \
  'lj_gc_pubobjobj(L, ud, env);' \
  'lj_gc_pubobjobj(L, ud, mt);' \
  'test_userdata_constructor_publish_barrier' \
  'setgcrefrrel(L1->env, L->env);' \
  'lj_gc_pubobjobj(L, L1, env);' \
  'test_thread_constructor_env_barrier' \
  'setgcrefrel(sbx->dict_str, obj2gco(dict_str));' \
  'setgcrefrel(sbx->dict_mt, obj2gco(dict_mt));' \
  'lj_gc_pubobjobj(L, ud, dict_str);' \
  'lj_gc_pubobjobj(L, ud, dict_mt);' \
  'test_buffer_constructor_dict_barrier' \
  'setgcrefmt(t->metatable, obj2gco(t));' \
  'test_weak_self_metatable_publish_barrier'
do
  if ! rg -F -q "$needle" "$ROOT/src" "$ROOT/tests"; then
    echo "guardrail: missing metatable publication marker: $needle" >&2
    exit 1
  fi
done

if ! awk '
  /static void newproxy_weaktable\(lua_State \*L\)/ { infn = 1; next }
  infn && /setgcrefmt\(t->metatable, obj2gco\(t\)\)/ { store = NR }
  infn && /lj_tab_storestr\(L, lj_tab_setstr/ { mode = NR }
  infn && /t->nomm =/ { nomm = NR }
  infn && /lj_gc_pubtab\(L, t\)/ { barrier = NR }
  infn && /^}/ {
    ok = store && mode && nomm && barrier &&
	 store < mode && mode < nomm && nomm < barrier
    exit ok ? 0 : 1
  }
  END { if (!ok) exit 1 }
' "$ROOT/src/lib_base.c"; then
  echo "guardrail: newproxy weak table must release-publish self metatable and publish after __mode" >&2
  exit 1
fi

if ! awk '
  /static GCtab \*ctype_fin_tab_new_l\(lua_State \*L, uint32_t hbits\)/ {
    infn = 1
    next
  }
  infn && /setgcrefmt\(t->metatable, obj2gco\(t\)\)/ { store = NR }
  infn && /lj_tab_storestr\(L, lj_tab_setstr/ { mode = NR }
  infn && /t->nomm =/ { nomm = NR }
  infn && /lj_gc_pubtab\(L, t\)/ { barrier = NR }
  infn && /^}/ {
    ok = store && mode && nomm && barrier &&
	 store < mode && mode < nomm && nomm < barrier
    exit ok ? 0 : 1
  }
  END { if (!ok) exit 1 }
' "$ROOT/src/lj_ctype.c"; then
  echo "guardrail: FFI finalizer weak table must release-publish self metatable and publish after __mode" >&2
  exit 1
fi

if ! awk '
  /static char \*serialize_get\(char \*r, SBufExt \*sbx, TValue \*o\)/ {
    infn = 1
    next
  }
  infn && /t = lj_tab_new\(sbufL\(sbx\), narray, hsize2hbits\(nhash\)\)/ {
    newtab = NR
  }
  infn && /setgcrefmt\(t->metatable, obj2gco\(mt\)\)/ { store = NR }
  infn && /lj_gc_pubtabobj\(sbufL\(sbx\), t, mt\)/ { barrier = NR }
  infn && /copyTVrel\(sbufL\(sbx\), o, &tv\)/ { publish = NR }
  infn && /^}/ { infn = 0 }
  END { exit newtab && store && barrier && publish &&
	      newtab < store && store < barrier && barrier < publish ? 0 : 1 }
' "$ROOT/src/lj_serialize.c"; then
  echo "guardrail: serializer table metatable must be release-published and barriered before table publication" >&2
  exit 1
fi

if ! awk '
  /case IRFL_TAB_META:/ { incase = 1; next }
  incase && /setgcrefnullrel\(t->metatable\)/ { nullrel = NR }
  incase && /setgcrefmt\(t->metatable, obj2gco\(mt\)\)/ { store = NR }
  incase && /lj_gc_pubtabobj\(L, t, mt\)/ { barrier = NR }
  incase && /^[[:space:]]*break;/ {
    ok = nullrel && store && barrier && store < barrier
    exit ok ? 0 : 1
  }
  END { if (!ok) exit 1 }
' "$ROOT/src/lj_snap.c"; then
  echo "guardrail: snapshot metatable restore must release-publish null/non-null stores and barrier non-null metatables" >&2
  exit 1
fi

if ! awk '
  /[|][.]ffunc_2 setmetatable/ { inff = 1; stored = 0; call = 0; legacy = 0; next }
  inff && /mov TAB:RB->metatable, TAB:RA/ { stored = NR }
  inff && /call extern lj_gc2_barrier_obj_pair/ { call = NR }
  inff && /barrierback TAB:RB, RC/ { legacy = NR }
  inff && /[|][.]ffunc_2 rawget/ { inff = 0 }
  END { exit stored && call && legacy && stored < call && call < legacy ? 0 : 1 }
' "$ROOT/src/vm_x64.dasc"; then
  echo "guardrail: x64 setmetatable fast path must publish GC2 object-pair barrier before legacy repair" >&2
  exit 1
fi

reader_hits=$(rg -n '\btabref\((tabV\([^)]*\)->metatable|udataV\([^)]*\)->metatable|basemt_obj|t->metatable|gco2ud\(o\)->metatable|gco2ud\(o\)->env|mainthread\(g\)->env|L->env|L1->env|th->env|ud->metatable|ud->env|fn->[cl]\.env|funcV\(o\)->c\.env|udataV\(o\)->env|curr_func\(L\)->c\.env|parent->env|J->fn->l\.env|sbx->dict)|\bgcref\((sbx->cowref|sbx->dict_str|sbx->dict_mt|t->metatable)' \
  "$ROOT/src/lj_gc.c" "$ROOT/src/lj_gc2.c" "$ROOT/src/lj_meta.c" \
  "$ROOT/src/lj_serialize.c" "$ROOT/src/lib_ffi.c" \
  "$ROOT/src/lj_cdata.c" "$ROOT/src/lib_threading.c" \
  "$ROOT/src/lj_api.c" "$ROOT/src/lib_base.c" \
  "$ROOT/src/lib_debug.c" "$ROOT/src/lib_buffer.c" \
  "$ROOT/src/lib_jit.c" "$ROOT/src/lj_lib.c" \
  "$ROOT/src/lj_load.c" "$ROOT/src/lj_func.c" \
  "$ROOT/src/lj_record.c" "$ROOT/src/lj_ffrecord.c" || true)
if [ -n "$reader_hits" ]; then
  echo "guardrail: shared metatable/env readers must use acquire helpers:" >&2
  echo "$reader_hits" >&2
  exit 1
fi

env_store_hits=$(rg -n '\bsetgcref\(L->env' \
  "$ROOT/src/lj_api.c" "$ROOT/src/lib_base.c" || true)
if [ -n "$env_store_hits" ]; then
  echo "guardrail: current-thread env replacement must use release stores:" >&2
  echo "$env_store_hits" >&2
  exit 1
fi

echo "M5 metatable publication guard passed"
