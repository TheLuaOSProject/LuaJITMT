#!/bin/sh
# Guard M7 parser-driven FFI CTState mutation serialization.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

for needle in \
  'uint32_t parse_token' \
  'lj_ctype_parse_lock(CTState *cts, lua_State *L)' \
  'la_cas32(&cts->parse_token, &expect, 1, LA_ACQ_REL, LA_ACQ)' \
  'la_futex_wait(&cts->parse_token, 1, 1000000)' \
  'la_store32_rel(&cts->parse_token, 0)' \
  'la_futex_wake(&cts->parse_token, 1)' \
  'lj_ctype_new_l(cp->L, cp->cts' \
  'cp_ctype_intern(cp,' \
  'lj_ctype_parse_lock(cts, L)' \
  'lj_ctype_parse_lock(cp.cts, L)' \
  'lj_ctype_parse_lock(cp.cts, J->L)'
do
  if ! rg -F -q "$needle" "$ROOT/src"; then
    echo "guardrail: missing FFI cparse token marker: $needle" >&2
    exit 1
  fi
done

calls=$(rg -n 'lj_cparse\(&cp\)' "$ROOT/src" || true)
count=$(printf '%s\n' "$calls" | sed '/^$/d' | wc -l | tr -d ' ')
if [ "$count" != 3 ]; then
  echo "guardrail: expected exactly 3 lj_cparse(&cp) call sites, found $count:" >&2
  printf '%s\n' "$calls" >&2
  exit 1
fi

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -joff "$ROOT/tests/t-ffi-cdef-token.lua" \
  "${LJ_M7_FFI_CDEF_THREADS:-6}" "${LJ_M7_FFI_CDEF_ITERS:-120}"

echo "M7 FFI cdef token guard passed"
