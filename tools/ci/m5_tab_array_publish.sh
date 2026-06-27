#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if hits=$(grep -nE -- '(^|[^[:alnum:]_])(ret|aret)[[:space:]]*->[[:space:]]*(next|node|hmask|array|acap|retire_epoch|armed)' \
    "$ROOT/src/lj_tab.c" \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/src/lj_gc2.c" \
    "$ROOT/tests/t-tab-retire.c" \
    "$ROOT/tests/t-tab-array-publish.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw table retire record access is forbidden; use lj_tab_*_retired_* helpers' >&2
  exit 1
fi
for helper in lj_tab_node_retired_head_acq lj_tab_node_retired_head_cas \
    lj_tab_node_retired_head_xchg_acqrel lj_tab_array_retired_head_acq \
    lj_tab_array_retired_head_cas lj_tab_array_retired_head_xchg_acqrel; do
  if ! grep -qE "^[[:space:]]*static LJ_AINLINE .*[*[:space:]]${helper}[[:space:]]*[(]|^[[:space:]]*${helper}[[:space:]]*[(]" \
      "$ROOT/src/lj_tab.h"; then
    printf 'required table retired head helper missing: %s\n' "$helper" >&2
    exit 1
  fi
done
if hits=$(grep -nE -- 'g->[[:space:]]*tab[.]retired_(nodes|arrays)|&g->[[:space:]]*tab[.]retired_(nodes|arrays)' \
    "$ROOT/src/lj_tab.c" \
    "$ROOT/src/lj_gc.c" \
    "$ROOT/tests/t-tab-retire.c" \
    "$ROOT/tests/t-tab-array-publish.c" || true); [ -n "$hits" ]; then
  printf '%s\n' "$hits" >&2
  printf '%s\n' 'raw table retired head access is forbidden; use lj_tab_*_retired_head_* helpers' >&2
  exit 1
fi
exec "$ROOT/tools/ci/lua_test.sh" m5_tab_array_publish
