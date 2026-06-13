#!/bin/sh
# Guard M5 upvalue publication wrapper conversions.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
LEGACY='lj_gc_(objbarrier|objbarriert|anybarriert|barrieruv|barriert|barrier)'

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
  'LUA_API void lua_upvaluejoin' \
  'LUA_API const char \\*lua_setupvalue'
do
  if ! check_clean_function "$ROOT/src/lj_api.c" "$fn"; then
    echo "guardrail: converted lj_api.c upvalue function regressed: $fn" >&2
    exit 1
  fi
done

if ! check_clean_function "$ROOT/src/lib_debug.c" 'LJLIB_CF\(debug_upvaluejoin\)'; then
  echo "guardrail: debug.upvaluejoin must use M5 publication wrappers" >&2
  exit 1
fi

if ! awk '
  /static void copy_slot/ { infn = 1; seen = 1 }
  infn && /idx < LUA_GLOBALSINDEX/ { upidx = 1 }
  infn && /copyTVrel\(L, o, f\)/ { rel = 1 }
  infn && /lj_gc_pubobjtv\(L, curr_func\(L\), f\)/ { pub = 1 }
  infn && /lj_gc_barrier\(L, curr_func\(L\), f\)/ { bad = 1 }
  infn && /^}/ { exit(seen && upidx && rel && pub && !bad ? 0 : 1) }
  END { if (!seen || !upidx || !rel || !pub || bad) exit 1 }
' "$ROOT/src/lj_api.c"; then
  echo "guardrail: C closure upvalue pseudo-index stores must be release-published" >&2
  exit 1
fi

for needle in \
  'setgcrefrel(fn1->l.uvptr[n1], uv)' \
  'lj_gc_pubobjobj(L, fn1, uv)' \
  'setgcrefrel(*p[0], uv)' \
  'lj_gc_pubobjobj(L, fn[0], uv)' \
  'copyTVrel(L, val, L->top)' \
  'lj_gc_pubobjtv(L, o, L->top)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_api.c" "$ROOT/src/lib_debug.c"; then
    echo "guardrail: missing upvalue publication marker: $needle" >&2
    exit 1
  fi
done

if ! rg -F -q 'copyTVrel(mainthread(g), &uv->tv, uvval(uv))' \
    "$ROOT/src/lj_gc.c"; then
  echo "guardrail: lj_gc_closeuv must release-copy into uv->tv" >&2
  exit 1
fi

for file in "$ROOT/src/lj_gc.c" "$ROOT/src/lj_gc2.c"; do
  if ! rg -F -q 'lj_tv_load_acq(&tv, uvval' "$file"; then
    echo "guardrail: GC upvalue traversal must snapshot uv payloads: $file" >&2
    exit 1
  fi
  if ! rg -F -q 'lj_tv_load_acq(&tv, &fn->c.upvalue[i])' "$file"; then
    echo "guardrail: GC C-closure upvalue traversal must snapshot payloads: $file" >&2
    exit 1
  fi
done

if rg -n 'gc_marktv\(g, uvval|gc2_mark_tv(_worker)?\(g, uvval|gc_marktv\(g, &fn->c\.upvalue\[i\]\)|gc2_mark_tv_worker\(g, &fn->c\.upvalue\[i\]\)|copyTV\(mainthread\(g\), &uv->tv, uvval\(uv\)\)' \
    "$ROOT/src/lj_gc.c" "$ROOT/src/lj_gc2.c"; then
  echo "guardrail: upvalue payloads must use release stores and GC snapshots" >&2
  exit 1
fi

legacy_all=$(rg -n "$LEGACY\\b" "$ROOT/src"/*.c | grep -v "$ROOT/src/lj_gc.c" || true)
legacy_count=$(printf '%s\n' "$legacy_all" | sed '/^$/d' | wc -l | tr -d ' ')
if [ "$legacy_count" -gt 3 ]; then
  echo "guardrail: legacy barrier count regressed above 3:" >&2
  echo "$legacy_all" >&2
  exit 1
fi

echo "M5 upvalue publication guard passed"
