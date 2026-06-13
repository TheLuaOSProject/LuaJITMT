#!/bin/sh
# Guard recorder-side HREFK slot selection against table shape races.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -e '
jit.opt.start("hotloop=1")
local t = { stable_key = 17, other = 23 }
local sum = 0
for i = 1, 800 do
  sum = sum + t.stable_key
end
assert(sum == 800 * 17)
'

for needle in \
  'Node *hrefk_node = lj_tab_node_acq(t);' \
  'uint32_t hrefk_hmask = lj_tab_node_hmask_acq(hrefk_node);' \
  'Node *cur_node = lj_tab_node_acq(t);' \
  'hrefk_node == cur_node &&' \
  'hrefk_hmask == lj_tab_node_hmask_acq(cur_node)' \
  'uintptr_t oldvaddr = (uintptr_t)(const void *)ix->oldv;' \
  'uintptr_t nodeaddr = (uintptr_t)(const void *)&hrefk_node[0].val' \
  'lj_ir_kint(J, (int32_t)hrefk_hmask)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_record.c"; then
    echo "guardrail: missing HREFK recorder snapshot marker: $needle" >&2
    exit 1
  fi
done

for reject in \
  '(char *)&lj_tab_node_acq(t)[0].val' \
  'hrefk_hmask == t->hmask' \
  'lj_ir_kint(J, (int32_t)t->hmask)'
do
  if rg -F -n "$reject" "$ROOT/src/lj_record.c"; then
    echo "guardrail: HREFK recorder slot selection must use the stable node/hmask snapshot: $reject" >&2
    exit 1
  fi
done

echo "M5 JIT HREFK recorder snapshot guard passed"
