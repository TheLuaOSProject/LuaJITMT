#!/bin/sh
# Guard that JIT table field FLOADs are not CSE'd across mutable table shape.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -e '
jit.opt.start("hotloop=1")
local sum = 0
local t = {}
for i = 1, 80 do t[i] = i end
for r = 1, 200 do
  if r == 75 then
    for i = 81, 180 do t[i] = i end
  end
  sum = sum + (t[(r % 180) + 1] or 0)
end
assert(sum > 0)
'

if rg -n 'LJFOLDF\(fload_tab_ah\)' "$ROOT/src/lj_opt_fold.c"; then
  echo "guardrail: mutable table FLOADs must not use the old CSE fold rule" >&2
  exit 1
fi

for needle in \
  'LJFOLDF(href_ah)' \
  'LJFOLD(FLOAD any IRFL_TAB_ARRAY)' \
  'LJFOLD(FLOAD any IRFL_TAB_NODE)' \
  'LJFOLD(FLOAD any IRFL_TAB_ASIZE)' \
  'LJFOLD(FLOAD any IRFL_TAB_HMASK)' \
  'LJFOLDF(fload_tab_mut)' \
  'lj_tab_array_snapshot_acq(t, &array)' \
  'lj_tab_node_snapshot_acq(t, &hmask)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_opt_fold.c"; then
    echo "guardrail: missing mutable table FLOAD marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'ir_ktab\(IR\(fleft->op1\)\)->(asize|hmask)' \
    "$ROOT/src/lj_opt_fold.c"; then
  echo "guardrail: TDUP FLOAD folds must snapshot template table headers" >&2
  exit 1
fi

if ! awk '
  /LJFOLDF\(fload_tab_mut\)/ { infn = 1; next }
  infn && /return EMITFOLD;/ { emit = 1 }
  infn && /^}/ { infn = 0 }
  END { exit emit ? 0 : 1 }
' "$ROOT/src/lj_opt_fold.c"; then
  echo "guardrail: mutable table FLOADs must emit fresh loads" >&2
  exit 1
fi

echo "M5 JIT table FLOAD mutability guard passed"
