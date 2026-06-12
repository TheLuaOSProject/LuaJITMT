#!/bin/sh
# Build and run M5 lua_State owner claim guards.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
OUT=${TMPDIR:-/tmp}/lj_t-state-owner

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-state-owner.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
"$OUT"

if [ "$(rg -n "lj_state_tryclaim\\(" "$ROOT/src/lj_api.c" | wc -l)" -lt 2 ]; then
  echo "guardrail: lua_xmove must claim both foreign states" >&2
  exit 1
fi

if ! awk '
  /LUA_API int lua_status/ { infn = 1; next }
  infn && /lj_state_tryclaim\(L/ { claim = 1 }
  infn && /lj_state_dropclaim\(&claim\)/ { drop = 1 }
  infn && /^}/ { exit(claim && drop ? 0 : 1) }
  END { if (!claim || !drop) exit 1 }
' "$ROOT/src/lj_api.c"; then
  echo "guardrail: lua_status must claim the inspected state" >&2
  exit 1
fi

if ! awk '
  /LUA_API void lua_getfenv/ { infn = 1; next }
  infn && /tvisthread\(o\)/ { inthread = 1 }
  infn && inthread && /lj_state_tryclaim\(L1/ { claim = 1 }
  infn && inthread && /lj_state_dropclaim\(&claim\)/ { drop = 1 }
  infn && /^}/ { exit(claim && drop ? 0 : 1) }
  END { if (!claim || !drop) exit 1 }
' "$ROOT/src/lj_api.c"; then
  echo "guardrail: lua_getfenv must claim thread objects" >&2
  exit 1
fi

if ! awk '
  /LUA_API int lua_setfenv/ { infn = 1; next }
  infn && /tvisthread\(o\)/ { inthread = 1 }
  infn && inthread && /lj_state_tryclaim\(L1/ { claim = 1 }
  infn && inthread && /lj_state_dropclaim\(&claim\)/ { drop = 1 }
  infn && /^}/ { exit(claim && drop ? 0 : 1) }
  END { if (!claim || !drop) exit 1 }
' "$ROOT/src/lj_api.c"; then
  echo "guardrail: lua_setfenv must claim thread objects" >&2
  exit 1
fi

if ! awk '
  /LUA_API int lua_resume/ { infn = 1; next }
  infn && /lj_state_tryclaim\(L/ { claim = 1 }
  infn && /lj_vm_resume/ { resume = 1 }
  infn && /lj_state_dropclaim\(&claim\)/ { drop = 1 }
  infn && /^}/ { exit(claim && resume && drop ? 0 : 1) }
  END { if (!claim || !resume || !drop) exit 1 }
' "$ROOT/src/lj_api.c"; then
  echo "guardrail: lua_resume must claim the resumed state" >&2
  exit 1
fi

if ! awk '
  /LJLIB_CF\(coroutine_status\)/ { infn = 1; next }
  infn && /lj_state_tryclaim\(co/ { claim = 1 }
  infn && /lj_state_dropclaim\(&claim\)/ { drop = 1 }
  infn && /^}/ { exit(claim && drop ? 0 : 1) }
  END { if (!claim || !drop) exit 1 }
' "$ROOT/src/lib_base.c"; then
  echo "guardrail: coroutine.status must claim foreign coroutine state" >&2
  exit 1
fi

if ! awk '
  /static int ffh_resume\(lua_State \*L, lua_State \*co, int wrap\)/ { infn = 1; next }
  infn && /lj_state_tryclaim\(co/ { claim = 1 }
  infn && /thread busy/ { busy = 1 }
  infn && /lj_state_dropclaim\(&claim\)/ { drop = 1 }
  infn && /^}/ { exit(claim && busy && drop ? 0 : 1) }
  END { if (!claim || !busy || !drop) exit 1 }
' "$ROOT/src/lib_base.c"; then
  echo "guardrail: coroutine resume fallback must claim foreign coroutine state" >&2
  exit 1
fi

if ! awk '
  /lua_State \*LJ_FASTCALL lj_ffh_coroutine_claim/ { infn = 1; next }
  infn && /lj_state_tryclaim\(co/ { claim = 1 }
  infn && /return NULL/ { busy = 1 }
  infn && /claim.release/ { tag = 1 }
  infn && /^}/ { exit(claim && busy && tag ? 0 : 1) }
  END { if (!claim || !busy || !tag) exit 1 }
' "$ROOT/src/lib_base.c"; then
  echo "guardrail: coroutine fast path must tag claimed coroutine state" >&2
  exit 1
fi

if ! awk '
  /\.macro coroutine_resume_wrap/ { inmacro = 1; next }
  inmacro && /mov TMP1d, NARGS:RDd/ { nargs_save = 1 }
  inmacro && /call extern lj_ffh_coroutine_claim/ { claim = 1 }
  inmacro && /mov NARGS:RDd, TMP1d/ { nargs_restore = 1 }
  inmacro && /mov CARG1, TMP1/ { vmarg = 1 }
  inmacro && /and CARG1, -2/ { strip = 1 }
  inmacro && /mov CARG2, TMP1/ { wraparg = 1 }
  inmacro && /call extern lj_ffh_coroutine_wrap_err/ { wraperr = 1 }
  inmacro && /->thr_owner, 0/ { release = 1 }
  inmacro && /\.endmacro/ {
    exit(nargs_save && claim && nargs_restore && vmarg && strip &&
	 wraparg && wraperr && release ? 0 : 1)
  }
  END {
    if (!nargs_save || !claim || !nargs_restore || !vmarg || !strip ||
	!wraparg || !wraperr || !release)
      exit 1
  }
' "$ROOT/src/vm_x64.dasc"; then
  echo "guardrail: x64 coroutine fast path must claim and release coroutine state" >&2
  exit 1
fi

if ! awk '
  /static void debug_claimthread/ { infn = 1; next }
  infn && /lj_state_tryclaim\(L1/ { claim = 1 }
  infn && /thread busy/ { busy = 1 }
  infn && /^}/ { exit(claim && busy ? 0 : 1) }
  END { if (!claim || !busy) exit 1 }
' "$ROOT/src/lib_debug.c"; then
  echo "guardrail: debug library must claim foreign coroutine states" >&2
  exit 1
fi

for fn in debug_getinfo debug_getlocal debug_setlocal; do
  if ! awk -v fn="$fn" '
    $0 ~ "LJLIB_CF\\(" fn "\\)" { infn = 1; start = NR; next }
    infn && NR > start && /^LJLIB_CF\(/ {
      exit(claim && drop ? 0 : 1)
    }
    infn && /debug_claimthread\(L, L1/ { claim = 1 }
    infn && /lj_state_dropclaim\(&claim\)/ { drop = 1 }
    END { if (!claim || !drop) exit 1 }
  ' "$ROOT/src/lib_debug.c"; then
    echo "guardrail: $fn must claim and drop foreign coroutine state" >&2
    exit 1
  fi
done

for fn in lua_getlocal lua_setlocal lua_getinfo lua_getstack; do
  if ! awk -v fn="$fn" '
    /LUA_API/ && $0 ~ fn "\\(" { infn = 1; start = NR; next }
    infn && NR > start && /^LUA_API / {
      exit(claim && drop ? 0 : 1)
    }
    infn && /lj_state_tryclaim\(L/ { claim = 1 }
    infn && /lj_state_dropclaim\(&claim\)/ { drop = 1 }
    END { if (!claim || !drop) exit 1 }
  ' "$ROOT/src/lj_debug.c"; then
    echo "guardrail: $fn must claim and drop inspected lua_State" >&2
    exit 1
  fi
done

if ! awk '
  /LUALIB_API void luaL_traceback/ { infn = 1; start = NR; next }
  infn && NR > start && /^}/ {
    exit(claim && drop ? 0 : 1)
  }
  infn && /lj_state_tryclaim\(L1/ { claim = 1 }
  infn && /lj_state_dropclaim\(&claim\)/ { drop = 1 }
  END { if (!claim || !drop) exit 1 }
' "$ROOT/src/lj_debug.c"; then
  echo "guardrail: luaL_traceback must claim and drop inspected lua_State" >&2
  exit 1
fi

if ! awk '
  /LUA_API const char \*luaJIT_profile_dumpstack/ { infn = 1; next }
  infn && /lj_state_tryclaim\(L/ { claim = 1 }
  infn && /lj_debug_dumpstack\(L/ { dump = 1 }
  infn && /lj_state_dropclaim\(&claim\)/ { drop = 1 }
  infn && /^}/ { exit(claim && dump && drop ? 0 : 1) }
  END { if (!claim || !dump || !drop) exit 1 }
' "$ROOT/src/lj_profile.c"; then
  echo "guardrail: luaJIT_profile_dumpstack must claim inspected lua_State" >&2
  exit 1
fi

if ! awk '
  /static void jit_profile_callback/ { infn = 1; next }
  infn && /lj_state_tryclaim\(L2/ { claim = 1 }
  infn && /lua_pcall\(L2/ { pcall = 1 }
  infn && /lj_state_dropclaim\(&claim\)/ { drop = 1 }
  infn && /^}/ { exit(claim && pcall && drop ? 0 : 1) }
  END { if (!claim || !pcall || !drop) exit 1 }
' "$ROOT/src/lib_jit.c"; then
  echo "guardrail: jit.profile callback must claim callback coroutine" >&2
  exit 1
fi

echo "M5 lua_State owner tests passed"
