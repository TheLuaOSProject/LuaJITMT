#!/bin/sh
# Run the M7 FFI cparser rollback guard.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CPARSE="$ROOT/src/lj_cparse.c"
CPARSE_H="$ROOT/src/lj_cparse.h"

for marker in \
  'typedef struct CPRollback CPRollback;' \
  'CPRollback *rollback;' \
  'CPAlloc *newct;' \
  'CTypeID starttop;' \
  'uint8_t newtype;'
do
  if ! grep -Fq "$marker" "$CPARSE_H"; then
    printf 'required cparser rollback state marker missing: %s\n' "$marker" >&2
    exit 1
  fi
done

if hits=$(grep -nE -- 'ctype_top_rel|memcpy[[:space:]]*[(][^;]*(hash|tabh|ct)' \
    "$CPARSE" "$CPARSE_H" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'cparser rollback must not restore CTState top or memcpy old hash/table state' >&2
  exit 1
fi

if ! awk '
  /^static void cp_rollback_log\(CPState \*cp, CTypeID id\)/ {
    in_fn = 1
    found = 1
    saw_starttop_skip = 0
    saw_existing_copy = 0
    saw_link = 0
  }
  in_fn && /id == 0 \|\| id >= cp->starttop/ { saw_starttop_skip = 1 }
  in_fn && /rb->old = \*ctype_get\(cp->cts, id\)/ { saw_existing_copy = 1 }
  in_fn && /rb->next = cp->rollback/ { saw_link = 1 }
  in_fn && /^}/ {
    if (!(saw_starttop_skip && saw_existing_copy && saw_link))
      exit 1
    in_fn = 0
  }
  END { if (!found) exit 1 }
' "$CPARSE"; then
  printf '%s\n' 'cp_rollback_log() must snapshot existing CTypes only once before mutation' >&2
  exit 1
fi

if ! awk '
  /^static void cp_ctype_abandon\(CPState \*cp\)/ {
    in_fn = 1
    found = 1
    saw_bad = 0
    saw_size = 0
    saw_sib = 0
    saw_clearname = 0
    saw_publish = 0
    bad = 0
  }
  in_fn && /ctype_info_rel\(&tmp, CTINFO\(CT_ATTRIB, CTATTRIB\(CTA_BAD\)\)\)/ {
    saw_bad = 1
  }
  in_fn && /ctype_size_rel\(&tmp, 0\)/ { saw_size = 1 }
  in_fn && /ctype_sib_rel\(&tmp, 0\)/ { saw_sib = 1 }
  in_fn && /ctype_clearname\(&tmp\)/ { saw_clearname = 1 }
  in_fn && /ctype_next_rel\(&tmp/ { bad = 1 }
  in_fn && /cp_ctype_publish\(cp, id, &tmp\)/ {
    if (!(saw_bad && saw_size && saw_sib && saw_clearname)) bad = 1
    saw_publish = 1
  }
  in_fn && /^}/ {
    if (!(saw_bad && saw_size && saw_sib && saw_clearname && saw_publish) ||
	bad)
      exit 1
    in_fn = 0
  }
  END { if (!found) exit 1 }
' "$CPARSE"; then
  printf '%s\n' 'cp_ctype_abandon() must publish CTA_BAD records while preserving hash-chain next links' >&2
  exit 1
fi

if ! awk '
  /^static void cp_rollback_restore\(CPState \*cp\)/ {
    in_fn = 1
    found = 1
    saw_restore = 0
    saw_abandon = 0
    bad = 0
  }
  in_fn && /cp_ctype_publish\(cp, rb->id, &rb->old\)/ { saw_restore = 1 }
  in_fn && /cp_ctype_abandon\(cp\)/ {
    if (!saw_restore) bad = 1
    saw_abandon = 1
  }
  in_fn && /^}/ {
    if (!(saw_restore && saw_abandon) || bad)
      exit 1
    in_fn = 0
  }
  END { if (!found) exit 1 }
' "$CPARSE"; then
  printf '%s\n' 'cp_rollback_restore() must restore old CType snapshots before abandoning new IDs' >&2
  exit 1
fi

if ! awk '
  /^int lj_cparse\(CPState \*cp\)/ {
    in_fn = 1
    found = 1
    saw_err = 0
    saw_restore = 0
    saw_cleanup = 0
    bad = 0
  }
  in_fn && /if \(errcode\)/ { saw_err = 1 }
  in_fn && /cp_rollback_restore\(cp\)/ {
    if (!saw_err) bad = 1
    saw_restore = 1
  }
  in_fn && /cp_cleanup\(cp\)/ {
    if (!saw_restore) bad = 1
    saw_cleanup = 1
  }
  in_fn && /^}/ {
    if (!(saw_err && saw_restore && saw_cleanup) || bad)
      exit 1
    in_fn = 0
  }
  END { if (!found) exit 1 }
' "$CPARSE"; then
  printf '%s\n' 'lj_cparse() must restore rollback state on parser errors before cleanup' >&2
  exit 1
fi

exec "$ROOT/tools/ci/lua_test.sh" m7_ffi_cparse_rollback
