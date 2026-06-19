#!/bin/sh
# Build and run M5 table FORWARD value filtering guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
OUT=${TMPDIR:-/tmp}/lj_t-tab-forward-filter

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-tab-forward-filter.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
"$OUT"

for needle in \
  'tab_val_absent(cTValue *val)' \
  'return tvisnil(val) || tvisforward(val)' \
  'tab_slot_absent_acq(const TValue *slot)' \
  'tab_val_forward_retry_once(cTValue *val, int *retry)' \
  'tab_node_forward_hop(Node **nodep, MSize *hmaskp)' \
  'lj_tab_node_nextgen_acq(node)' \
  'tab_val_absent(&val)' \
  'tab_slot_absent_acq(tv)' \
  'tab_slot_absent_acq(&array[hi])' \
  'lj_tab_getint(t, 3) == NULL' \
  'lj_tab_getstr(t, hidden) == NULL' \
  'exercise_hash_forward_hop(L)' \
  'lj_tab_node_nextgen_acq(oldnode) == newnode' \
  't-tab-forward-filter OK'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_tab.c" "$ROOT/tests/t-tab-forward-filter.c"; then
    echo "guardrail: missing table FORWARD filtering marker: $needle" >&2
    exit 1
  fi
done

if ! rg -F -q 'm5_tab_forward_filter.sh' "$ROOT/tools/ci/m5_concurrent_objects.sh"; then
  echo "guardrail: table FORWARD filtering guard is not wired into M5 aggregate" >&2
  exit 1
fi

if ! awk '
  /static LJ_AINLINE cTValue \*lj_tab_getint\(GCtab \*t,/ { ingetint = 1 }
  ingetint && /tvisforward\(&val\)/ { forward = 1 }
  ingetint && /goto retry_array/ { retry = 1 }
  ingetint && /return NULL/ { nullret = 1 }
  ingetint && /^}/ { ingetint = 0 }
  END { exit forward && retry && nullret ? 0 : 1 }
' "$ROOT/src/lj_tab.h"; then
  echo "guardrail: integer array getter must filter FORWARD values" >&2
  exit 1
fi

if ! awk '
  /cTValue \* LJ_FASTCALL lj_tab_getinth\(GCtab \*t,/ { ininth = 1 }
  ininth && /tab_node_forward_hop\(&node, &hmask\)/ { inth = 1 }
  ininth && /tab_val_forward_retry_once\(&val, &forward_retry\)/ { inth_retry = 1 }
  ininth && /^}/ { ininth = 0 }
  /cTValue \*lj_tab_getstr\(GCtab \*t,/ { instr = 1 }
  instr && /tab_node_forward_hop\(&node, &hmask\)/ { str = 1 }
  instr && /tab_val_forward_retry_once\(&val, &forward_retry\)/ { str_retry = 1 }
  instr && /^}/ { instr = 0 }
  /cTValue \*lj_tab_get\(lua_State \*L,/ { ingen = 1 }
  ingen && /tab_node_forward_hop\(&node, &hmask\)/ { gen = 1 }
  ingen && /tab_val_forward_retry_once\(&val, &forward_retry\)/ { gen_retry = 1 }
  ingen && /^}/ { ingen = 0 }
  END { exit inth && inth_retry && str && str_retry && gen && gen_retry ? 0 : 1 }
' "$ROOT/src/lj_tab.c"; then
  echo "guardrail: hash getters must hop FORWARD values through next_gen" >&2
  exit 1
fi

if ! awk '
  /int lj_tab_next\(GCtab \*t,/ { innext = 1 }
  innext && /tab_val_absent\(&val\)/ { next_absent++ }
  innext && /^}/ { innext = 0 }
  END { exit next_absent >= 2 ? 0 : 1 }
' "$ROOT/src/lj_tab.c"; then
  echo "guardrail: table next() must filter FORWARD values from array/hash scans" >&2
  exit 1
fi

if ! awk '
  /static MSize tab_len_slow\(GCtab \*t,/ { inlen = 1 }
  /MSize LJ_FASTCALL lj_tab_len\(GCtab \*t\)/ { inlen = 1 }
  /MSize LJ_FASTCALL lj_tab_len_hint\(GCtab \*t,/ { inlen = 1 }
  inlen && /tab_slot_absent_acq|tab_val_absent/ { absent++ }
  inlen && /^}/ { inlen = 0 }
  END { exit absent >= 6 ? 0 : 1 }
' "$ROOT/src/lj_tab.c"; then
  echo "guardrail: table length helpers must treat FORWARD as absent" >&2
  exit 1
fi

echo "M5 table FORWARD filtering tests passed"
