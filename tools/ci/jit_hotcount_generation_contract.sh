#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
bad=0

require_source() {
  pattern=$1
  file=$2
  message=$3
  if ! rg -q "$pattern" "$root/$file"; then
    printf '%s\n' "$message" >&2
    bad=1
  fi
}

require_source '^#define LJ_GC2_HS_RESET_HOTCOUNT[[:space:]]+0x00002000u$' \
  src/lj_gc2.h 'hotcount reset lost its dedicated handshake bit'
require_source 'uint64_t hotcount_reset_word;' src/lj_tg.h \
  'main-TG packed desired hotcount publication is missing'
require_source 'uint64_t hotcount_applied_generation;' src/lj_tg.h \
  'per-TG applied hotcount generation is missing'
require_source 'lj_tg_hotcount_reset_word_cas' src/lj_dispatch.c \
  'desired hotcount template/generation is no longer one atomic publication'
require_source 'lj_tg_hotcount_applied_generation_rel' src/lj_dispatch.c \
  'owner fill lost its release-published applied generation'
require_source 'LJ_GC2_HS_REDISPATCH\|LJ_GC2_HS_RESET_HOTCOUNT' \
  src/lj_dispatch.c 'JIT off-to-on no longer combines redispatch and reset'
require_source 'native_parked \|\| tg == lj_thr_get_tg\(\)' \
  src/lj_safepoint.c \
  'foreign hotcount fill is no longer restricted to native-park/TLS authority'
require_source 'G2TG\(g\) != tg' src/lj_dispatch.h \
  'runtime bucket writes no longer require the exact physical TG actor'
require_source 'after the successful list CAS closes that window' src/lj_tg.c \
  'TG attach lost its post-legacy-list-CAS generation closure'

if rg -n 'hotcount_setg[[:space:]]*\(' "$root/src" --glob '*.[ch]' \
    >/dev/null; then
  printf '%s\n' 'runtime hotcount writes regained ambient/shared hotcount_setg' >&2
  bad=1
fi
if rg -n 'G2GG\([^)]*\)->hotcount\[[^]]+\][[:space:]]*=' \
    "$root/src" --glob '*.[ch]' >/dev/null; then
  printf '%s\n' 'runtime source writes shared GG.hotcount buckets' >&2
  bad=1
fi

update=$(sed -n \
  '/^void LJ_FASTCALL lj_dispatch_update(/,/^\/\* -- JIT mode setting/p' \
  "$root/src/lj_dispatch.c")
mode_line=$(printf '%s\n' "$update" | \
  rg -n 'dispatchmode_store_rel\(g, mode\)' | head -n 1 | cut -d: -f1)
publish_line=$(printf '%s\n' "$update" | \
  rg -n 'lj_dispatch_hotcount_publish\(g\)' | head -n 1 | cut -d: -f1)
if test -z "$mode_line" || test -z "$publish_line" || \
   test "$mode_line" -ge "$publish_line"; then
  printf '%s\n' \
    'off-to-on hotcount publication moved back under DISPMODE_UPDATE' >&2
  bad=1
fi

attach=$(sed -n '/^void lj_tg_attach(/,/^int lj_tg_registry_detach_begin/p' \
  "$root/src/lj_tg.c")
cas_line=$(printf '%s\n' "$attach" | \
  rg -n 'gc2_tg_list_cas' | tail -n 1 | cut -d: -f1)
apply_line=$(printf '%s\n' "$attach" | \
  rg -n 'lj_dispatch_hotcount_apply_tg' | tail -n 1 | cut -d: -f1)
if test -z "$cas_line" || test -z "$apply_line" || \
   test "$cas_line" -ge "$apply_line"; then
  printf '%s\n' 'hotcount catch-up does not follow the successful list CAS' >&2
  bad=1
fi

if test -f "$root/src/lj_dispatch.o" && \
   nm "$root/src/lj_dispatch.o" | rg -q 'lj_dispatch_hotcount_publish'; then
  for symbol in lj_dispatch_hotcount_publish lj_dispatch_hotcount_apply_tg; do
    if ! nm "$root/src/lj_dispatch.o" | rg -q "[[:space:]]_?$symbol$"; then
      printf 'JIT object is missing %s\n' "$symbol" >&2
      bad=1
    fi
  done
fi

if test "$bad" -ne 0; then
  exit 1
fi

printf '%s\n' \
  'JIT hotcount generation contract passed: TG-local reset and attach closure intact'
