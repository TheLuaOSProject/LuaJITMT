#!/bin/sh
# Guard M5 runtime publication wrapper conversions.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
LEGACY='lj_gc_(objbarrier|objbarriert|anybarriert|barrieruv|barriert|barrier)'

if ! rg -q 'lj_gc_pubtabobj' "$ROOT/src/lj_gc.h"; then
  echo "guardrail: missing table-object publication wrapper" >&2
  exit 1
fi

for needle in \
  'static LJ_AINLINE void lj_gc_barriertv_' \
  'static LJ_AINLINE void lj_gc_barrierobjtv_' \
  'lj_tv_load_acq(&snap, tv)' \
  'lj_gc2_barrier_tv_pair(L, obj2gco(t), &snap)' \
  'lj_gc2_barrier_tv_pair(L, p, &snap)' \
  'tviswhite(&snap)' \
  'gcV(&snap)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_gc.h"; then
    echo "guardrail: publication TValue barriers must snapshot inputs: $needle" >&2
    exit 1
  fi
done

if awk '
  /#define lj_gc_(barriert|barrier|pubtabtv|pubobjtv)\(/ { infn = 1; next }
  infn && /lj_gc2_barrier_tv\(\(L\), \(tv\)\)|tviswhite\(tv\)|gcV\(tv\)/ {
    print FILENAME ":" FNR ":" $0; bad = 1
  }
  infn && /^#define/ { infn = 0 }
  END { exit bad ? 0 : 1 }
' "$ROOT/src/lj_gc.h"; then
  echo "guardrail: publication wrappers must not inspect caller TValue slots directly" >&2
  exit 1
fi

for file in \
  "$ROOT/src/lib_base.c" \
  "$ROOT/src/lib_buffer.c" \
  "$ROOT/src/lib_ffi.c" \
  "$ROOT/src/lib_table.c" \
  "$ROOT/src/lj_buf.c" \
  "$ROOT/src/lj_ccallback.c" \
  "$ROOT/src/lj_cdata.c" \
  "$ROOT/src/lj_clib.c" \
  "$ROOT/src/lj_lib.c" \
  "$ROOT/src/lj_meta.c"
do
  hits=$(rg -n "$LEGACY\\b" "$file" || true)
  if [ -n "$hits" ]; then
    echo "guardrail: $file must use M5 publication wrappers:" >&2
    echo "$hits" >&2
    exit 1
  fi
done

check_clean_function() {
  file=$1
  start=$2
  awk -v start="$start" -v legacy="$LEGACY" '
    $0 ~ start { infn = 1; seen = 1 }
    infn && $0 ~ legacy { print FILENAME ":" FNR ":" $0; bad = 1 }
    infn && /^}/ { exit bad ? 1 : 0 }
    END { if (!seen) exit 2 }
  ' "$file"
}

for fn in \
  'LUALIB_API int luaL_newmetatable' \
  'LUA_API void lua_rawset\(' \
  'LUA_API void lua_rawseti' \
  'LUA_API int lua_setmetatable' \
  'LUA_API int lua_setfenv'
do
  if ! check_clean_function "$ROOT/src/lj_api.c" "$fn"; then
    echo "guardrail: converted lj_api.c publication function regressed: $fn" >&2
    exit 1
  fi
done

if ! rg -q 'lj_gc_pubobjtv\(L, fn, f\)' "$ROOT/src/lj_api.c"; then
  echo "guardrail: LUA_ENVIRONINDEX store must use publication wrapper" >&2
  exit 1
fi

if ! awk '
  /static void base_storetab_str\(lua_State \*L,/ { infn = 1; next }
  infn && /lj_tab_storetab\(L, dst,/ { raw = 1 }
  infn && /for \(;;\)/ { loop = 1 }
  infn && /lj_tab_setstr\(L, tab, key\)/ { resolve = 1 }
  infn && /lj_tab_trystoretv_cas\(L, dst, &tv\) == LJ_TAB_STORE_CAS_OK/ { cas = 1 }
  infn && /base table store saw FORWARD after lookup\./ { retry = 1 }
  infn && /^}/ { exit(!raw && loop && resolve && cas && retry ? 0 : 1) }
  END { if (raw || !loop || !resolve || !cas || !retry) exit 1 }
' "$ROOT/src/lib_base.c"; then
  echo "guardrail: base string table helper must CAS-publish and retry forwarded slots" >&2
  exit 1
fi

if ! awk '
  /LUALIB_API int luaopen_base\(lua_State \*L\)/ { infn = 1; next }
  infn && /lj_tab_storetab\(L, lj_tab_setstr\(L, env, lj_str_newlit\(L, "_G"\)\), env\)/ { raw = 1 }
  infn && /base_storetab_str\(L, env, lj_str_newlit\(L, "_G"\), env\)/ { cas = 1 }
  infn && /^}/ { exit(!raw && cas ? 0 : 1) }
  END { if (raw || !cas) exit 1 }
' "$ROOT/src/lib_base.c"; then
  echo "guardrail: luaopen_base must CAS-publish _G into the environment" >&2
  exit 1
fi

legacy_all=$(rg -n "$LEGACY\\b" "$ROOT/src"/*.c | grep -v "$ROOT/src/lj_gc.c" || true)
legacy_count=$(printf '%s\n' "$legacy_all" | sed '/^$/d' | wc -l | tr -d ' ')
if [ "$legacy_count" -ne 0 ]; then
  echo "guardrail: legacy barrier call sites regressed outside lj_gc.c:" >&2
  echo "$legacy_all" >&2
  exit 1
fi

echo "M5 runtime publication guard passed"
