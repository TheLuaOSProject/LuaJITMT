#!/bin/sh
# Guard C-side table value stores use release-publishing helpers.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -e '
local util = require("jit.util")
local linfo = util.funcinfo(function() return 1 end)
assert(linfo.proto ~= nil and linfo.upvalues ~= nil)
local cinfo = util.funcinfo(print)
assert(cinfo.addr ~= nil and cinfo.upvalues ~= nil)
local t = { 1, 2, 3 }
t.name = "table-value-publish"
assert(t[3] == 3 and t.name == "table-value-publish")
'

for needle in \
  'lj_tab_storetv' \
  'lj_tab_storenil' \
  'lj_tab_storebool' \
  'lj_tab_storeint' \
  'lj_tab_storeintptr' \
  'lj_tab_storestr' \
  'lj_tab_storetab' \
  'lj_tab_storethread' \
  'lj_tab_storeproto' \
  'lj_tab_storefunc' \
  'lj_tab_storeudata' \
  'copyTVrel(L, dst, src)' \
  'lj_tab_storeint(L, lj_tab_newkey(L, dict, &tv), (int32_t)(i-1))'
do
  if ! rg -F -q "$needle" "$ROOT/src"; then
    echo "guardrail: missing table value publication marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'set[a-zA-Z0-9_]*V\([^;]*lj_tab_set|lj_tab_set[^;]*\)->u64|lj_tab_newkey\([^;]*\)->u64|copyTV\([^;]*lj_tab_set' \
    "$ROOT/src" --glob '!host/*'; then
  echo "guardrail: raw table slot stores must use lj_tab_store* or copyTVrel" >&2
  exit 1
fi

echo "M5 table value publication guard passed"
