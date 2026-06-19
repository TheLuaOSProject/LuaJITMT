#!/bin/sh
# Build and run M5 table KEYLOCK lookup filtering guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CC=${CC:-cc}
CFLAGS=${CFLAGS:-"-std=gnu99 -O2 -Wall -Wextra -Werror -mcx16"}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
OUT=${TMPDIR:-/tmp}/lj_t-tab-keylock-lookup

make -C "$ROOT/src" clean >/dev/null
make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$CC" $CFLAGS -I"$ROOT/src" "$ROOT/tests/t-tab-keylock-lookup.c" \
  "$ROOT/src/libluajit.a" -lm -ldl -pthread -o "$OUT"
timeout 20s "$OUT"

for needle in \
  'tab_key_islocked(cTValue *key)' \
  'tab_key_retry_once(cTValue *key, int *retry)' \
  'tab_try_claim_nil_key(TValue *dst)' \
  'tab_claim_free_node_scan(Node *nodebase, MSize hmask,' \
  'tab_findkey_or_keylock(Node *anchor, cTValue *key, int *locked)' \
  'tab_findkey_or_keylock(n, key, &locked)' \
  'tab_try_claim_nil_key(&n->key)' \
  'tab_claim_free_node_scan(nodebase, hmask, n, &locked)' \
  'tab_release_claimed_free(freenode)' \
  'if (tab_key_retry_once(&nk, &retry))' \
  'if (tab_key_islocked(&key))' \
  'if (tab_key_islocked(&nk))' \
  'tviskeylock(&key)' \
  'tviskeylock(&out[0])' \
  'lj_tab_newkey(L, t, &keyv) == &node[0].val' \
  'exercise_tombstone_anchor_insert(L)' \
  'strV(&node[0].key) == anchor' \
  'getfreetop(t, node) == freetop0' \
  'assert_tabnum(t, replacement, 33)'
do
  if ! rg -F -q "$needle" "$ROOT/src" "$ROOT/tests/t-tab-keylock-lookup.c"; then
    echo "guardrail: missing table KEYLOCK lookup marker: $needle" >&2
    exit 1
  fi
done

if ! awk '
  /TValue \*lj_tab_newkey\(lua_State \*L,/ { innewkey = 1 }
  innewkey && /tab_try_claim_nil_key\(&n->key\)/ { anchor = 1 }
  innewkey && /tab_claim_free_node_scan\(nodebase, hmask, n, &locked\)/ { free = 1 }
  innewkey && /tab_release_claimed_free\(freenode\)/ { release++ }
  innewkey && /tab_storekeyrel\(L, &n->key, key\)/ { anchorpub = 1 }
  innewkey && /tab_storekeyrel\(L, &freenode->key, key\)/ { freepub = 1 }
  innewkey && /setfreetop\(t, nodebase, freenode\)/ { bad = 1 }
  innewkey && /^}/ { innewkey = 0 }
  END { exit anchor && free && release >= 2 && anchorpub && freepub && !bad ? 0 : 1 }
' "$ROOT/src/lj_tab.c"; then
  echo "guardrail: lj_tab_newkey must KEYLOCK-claim nil anchor/free keys without freetop mutation" >&2
  exit 1
fi

if ! rg -F -q 'm5_tab_keylock_lookup.sh' "$ROOT/tools/ci/m5_concurrent_objects.sh"; then
  echo "guardrail: table KEYLOCK lookup guard is not wired into M5 aggregate" >&2
  exit 1
fi

echo "M5 table KEYLOCK lookup filtering tests passed"
