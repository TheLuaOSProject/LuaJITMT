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
  'tab_forwarded_int_arrayslot(GCtab *t, int32_t key)' \
  'tab_forwarded_setslot(GCtab *t, Node **nodep, MSize *hmaskp,' \
  'tab_forwarded_hash_value(GCtab *t, Node **nodep,' \
  'tab_array_slot_absent_acq(GCtab *t, TValue **arrayp,' \
  'lj_tab_array_forward_hop(const GCtab *t, TValue **arrayp,' \
  'lj_tab_array_nextgen_acq(array)' \
  'lj_tab_array_hdr_asize_acq(next)' \
  'tab_val_absent(&val)' \
  'tab_slot_absent_acq(tv)' \
  'tab_array_slot_absent_acq(t, &array, &asize, (MSize)hi)' \
  'lj_tab_getint(t, 3) == NULL' \
  'lj_tab_getstr(t, hidden) == NULL' \
  'lj_tab_len(t) == 5' \
  'lj_tab_len_hint(t, 5) == 5' \
  'exercise_array_forward_hop(L)' \
  'lj_tab_array_nextgen_acq(oldarray) == newarray' \
  'lj_tab_storeint(L, lj_tab_setint(L, t, 5), 909)' \
  'exercise_hash_forward_hop(L)' \
  'lj_tab_storeint(L, lj_tab_setstr(L, t, hopstr), 404)' \
  'lj_tab_storeint(L, lj_tab_set(L, t, &lightkey), 606)' \
  'exercise_hash_to_array_forward_hop(L)' \
  'assert_i32(&newarray[moveint], 909)' \
  'lj_tab_node_nextgen_acq(oldnode) == newnode' \
  't-tab-forward-filter OK'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_tab.c" "$ROOT/src/lj_tab.h" \
      "$ROOT/tests/t-tab-forward-filter.c"; then
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
  ingetint && /lj_tab_array_forward_hop\(t, &array, &asize\)/ { hop = 1 }
  ingetint && /goto genarray/ { gen = 1 }
  ingetint && /tvisforward\(&val\)/ { forward = 1 }
  ingetint && /goto retry_array/ { retry = 1 }
  ingetint && /return NULL/ { nullret = 1 }
  ingetint && /^}/ { ingetint = 0 }
  END { exit hop && gen && forward && retry && nullret ? 0 : 1 }
' "$ROOT/src/lj_tab.h"; then
  echo "guardrail: integer array getter must hop/filter FORWARD values" >&2
  exit 1
fi

if ! awk '
  /static LJ_AINLINE TValue \*lj_tab_setint\(lua_State \*L,/ { insetint = 1 }
  insetint && /tvisforward\(&val\)/ { forward = 1 }
  insetint && /lj_tab_array_forward_hop\(t, &array, &asize\)/ { hop = 1 }
  insetint && /goto genarray/ { gen = 1 }
  insetint && /goto retry_array/ { retry = 1 }
  insetint && /return lj_tab_setinth\(L, t, key\)/ { inth = 1 }
  insetint && /^}/ { insetint = 0 }
  END { exit forward && hop && gen && retry && inth ? 0 : 1 }
' "$ROOT/src/lj_tab.h"; then
  echo "guardrail: integer array setter must route FORWARD slots through next_gen" >&2
  exit 1
fi

if ! awk '
  /cTValue \* LJ_FASTCALL lj_tab_getinth\(GCtab \*t,/ { ininth = 1 }
  ininth && /tab_node_forward_hop\(&node, &hmask\)/ { inth = 1 }
  ininth && /tab_forwarded_int_arrayslot\(t, key\)/ { inth_array = 1 }
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
  END { exit inth && inth_array && inth_retry && str && str_retry && gen && gen_retry ? 0 : 1 }
' "$ROOT/src/lj_tab.c"; then
  echo "guardrail: hash getters must hop FORWARD values through next_gen" >&2
  exit 1
fi

if ! awk '
  /TValue \*lj_tab_setinth\(lua_State \*L,/ { ininth = 1 }
  ininth && /tab_forwarded_setslot\(t, &node, &hmask, &k\)/ { inth = 1 }
  ininth && /tab_val_forward_retry_once\(&val, &forward_retry\)/ { inth_retry = 1 }
  ininth && /^}/ { ininth = 0 }
  /TValue \*lj_tab_setstr\(lua_State \*L,/ { instr = 1 }
  instr && /tab_forwarded_setslot\(t, &node, &hmask, &k\)/ { str = 1 }
  instr && /tab_val_forward_retry_once\(&val, &forward_retry\)/ { str_retry = 1 }
  instr && /^}/ { instr = 0 }
  /TValue \*lj_tab_set\(lua_State \*L,/ { ingen = 1 }
  ingen && /tab_forwarded_setslot\(t, &node, &hmask, key\)/ { gen = 1 }
  ingen && /tab_val_forward_retry_once\(&val, &forward_retry\)/ { gen_retry = 1 }
  ingen && /^}/ { ingen = 0 }
  END { exit inth && inth_retry && str && str_retry && gen && gen_retry ? 0 : 1 }
' "$ROOT/src/lj_tab.c"; then
  echo "guardrail: hash setters must route FORWARD values through next_gen" >&2
  exit 1
fi

if ! awk '
  /int lj_tab_next\(GCtab \*t,/ { innext = 1 }
  innext && /lj_tab_array_forward_hop\(t, &nextarray, &nextasize\)/ { array_hop = 1 }
  innext && /tab_forwarded_hash_value\(t, &hopnode, &hophmask, &key, &val\)/ { hash_hop = 1 }
  innext && /tab_val_absent\(&val\)/ { next_absent++ }
  innext && /^}/ { innext = 0 }
  END { exit array_hop && hash_hop && next_absent >= 2 ? 0 : 1 }
' "$ROOT/src/lj_tab.c"; then
  echo "guardrail: table next() must hop/filter FORWARD values from array/hash scans" >&2
  exit 1
fi

if ! awk '
  /static LJ_AINLINE int tab_array_slot_absent_acq\(GCtab \*t,/ { inhelper = 1 }
  inhelper && /lj_tab_array_forward_hop\(t, &nextarray, &nextasize\)/ { helper_hop = 1 }
  inhelper && /tab_val_absent\(&val\)/ { helper_absent = 1 }
  inhelper && /^}/ { inhelper = 0 }
  /static MSize tab_len_slow\(GCtab \*t,/ { inlen = 1 }
  /MSize LJ_FASTCALL lj_tab_len\(GCtab \*t\)/ { inlen = 1 }
  /MSize LJ_FASTCALL lj_tab_len_hint\(GCtab \*t,/ { inlen = 1 }
  inlen && /tab_slot_absent_acq|tab_val_absent|tab_array_slot_absent_acq/ { absent++ }
  inlen && /^}/ { inlen = 0 }
  END { exit helper_hop && helper_absent && absent >= 6 ? 0 : 1 }
' "$ROOT/src/lj_tab.c"; then
  echo "guardrail: table length helpers must hop/filter FORWARD values" >&2
  exit 1
fi

echo "M5 table FORWARD filtering tests passed"
