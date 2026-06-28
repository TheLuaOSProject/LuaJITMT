#!/bin/sh
# Run the M5 table hash-node TValue snapshot guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
TAB="$ROOT/src/lj_tab.c"
LIBTAB="$ROOT/src/lib_table.c"

if hits=$(grep -nE 'tvis[[:alnum:]_]*[[:space:]]*[(][[:space:]]*&([[:alnum:]_]+->[[:space:]]*(key|val)|[[:alnum:]_]+[[][^]]+[]][.][[:space:]]*(key|val))([^[:alnum:]_]|$)' \
    "$TAB" "$LIBTAB" "$ROOT/src/lj_gc.c" "$ROOT/src/lj_gc2.c" \
    "$ROOT/src/lj_serialize.c" "$ROOT/src/lj_bcwrite.c" \
    "$ROOT/src/lj_parse.c" "$ROOT/src/lj_record.c" || true);
then
  if [ -n "$hits" ]; then
    printf '%s\n' "$hits" >&2
    printf '%s\n' 'table hash-node key/value decisions must use acquired TValue snapshots' >&2
    exit 1
  fi
fi

for helper in \
  'static LJ_AINLINE int tab_val_absent(cTValue *val)' \
  'static LJ_AINLINE int tab_slot_absent_acq(const TValue *slot)' \
  'static LJ_AINLINE int tab_array_slot_absent_acq(GCtab *t, TValue **arrayp,'
do
  if ! grep -Fq "$helper" "$TAB"; then
    printf 'required table slot snapshot helper missing: %s\n' "$helper" >&2
    exit 1
  fi
done

if ! awk '
  /^static uint32_t counthash\(/ {
    in_fn = 1
    found = 1
    saw_val_load = 0
    saw_absent = 0
    saw_key_load = 0
    saw_hidden = 0
    bad = 0
  }
  in_fn && /lj_tv_load_acq\(&val, &n->val\)/ { saw_val_load = 1 }
  in_fn && /tab_val_absent\(&val\)/ {
    if (!saw_val_load) bad = 1
    saw_absent = 1
  }
  in_fn && /lj_tv_load_acq\(&key, &n->key\)/ { saw_key_load = 1 }
  in_fn && /tab_hash_key_hidden\(&key\)/ {
    if (!saw_key_load) bad = 1
    saw_hidden = 1
  }
  in_fn && /^}/ {
    if (!(saw_val_load && saw_absent && saw_key_load && saw_hidden) || bad)
      exit 1
    in_fn = 0
  }
  END { if (!found) exit 1 }
' "$TAB"; then
  printf '%s\n' 'counthash() must decide hash visibility from acquired key/value snapshots' >&2
  exit 1
fi

if ! awk '
  /^int lj_tab_next\(GCtab \*t, cTValue \*key, TValue \*o\)/ {
    in_fn = 1
    found = 1
    saw_array_load = 0
    saw_hash_val_load = 0
    saw_absent = 0
    saw_hash_key_load = 0
    saw_hidden = 0
    bad = 0
  }
  in_fn && /lj_tv_load_acq\(&val, &array\[idx\]\)/ { saw_array_load = 1 }
  in_fn && /lj_tv_load_acq\(&val, &n->val\)/ { saw_hash_val_load = 1 }
  in_fn && /tab_val_absent\(&val\)/ {
    if (!(saw_array_load || saw_hash_val_load)) bad = 1
    saw_absent = 1
  }
  in_fn && /lj_tv_load_acq\(&key, &n->key\)/ { saw_hash_key_load = 1 }
  in_fn && /tab_hash_key_hidden\(&key\)/ {
    if (!saw_hash_key_load) bad = 1
    saw_hidden = 1
  }
  in_fn && /^}/ {
    if (!(saw_array_load && saw_hash_val_load && saw_absent &&
	  saw_hash_key_load && saw_hidden) || bad)
      exit 1
    in_fn = 0
  }
  END { if (!found) exit 1 }
' "$TAB"; then
  printf '%s\n' 'lj_tab_next() must traverse visible slots from acquired snapshots' >&2
  exit 1
fi

if ! grep -Fq 'tab_slot_absent_acq(tv)' "$TAB" ||
   ! grep -Fq 'tab_array_slot_absent_acq(t, &array, &asize,' "$TAB"; then
  printf '%s\n' 'table length must use acquire snapshot helpers for slot visibility' >&2
  exit 1
fi

if ! awk '
  /^static int table_maxn_visible_acq\(cTValue \*tv\)/ {
    in_helper = 1
    found_helper = 1
  }
  in_helper && /lj_tv_load_acq\(&val, tv\)/ { saw_helper_acq = 1 }
  in_helper && /^}/ { in_helper = 0 }
  /^LJLIB_CF\(table_maxn\)/ {
    in_fn = 1
    found = 1
    saw_key_load = 0
    saw_hash_visible = 0
  }
  in_fn && /lj_tv_load_acq\(&key, &node\[i\]\.key\)/ { saw_key_load = 1 }
  in_fn && /table_maxn_hash_visible\(L, t, &key, &node\[i\]\.val\)/ {
    if (!saw_key_load) exit 1
    saw_hash_visible = 1
  }
  in_fn && /^}/ {
    if (!(saw_key_load && saw_hash_visible)) exit 1
    in_fn = 0
  }
  END { if (!(found && found_helper && saw_helper_acq)) exit 1 }
' "$LIBTAB"; then
  printf '%s\n' 'table.maxn must snapshot hash keys and relooked-up visible slots' >&2
  exit 1
fi

if ! grep -Fq 'lj_tv_load_acq(&val, slot);' "$LIBTAB"; then
  printf '%s\n' 'table.maxn value helpers must snapshot slots before visibility checks' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m5_tab_slot_snapshot
