#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 3 ]; then
  printf 'usage: %s <objdump> <lj_gc2.o> <lj_thr.o>\n' "$0" >&2
  exit 2
fi

objdump=$1
gc_object=$2
thr_object=$3

if ! command -v "$objdump" >/dev/null 2>&1; then
  printf '%s is required to validate Windows GC2 TLS storage\n' "$objdump" >&2
  exit 1
fi
for object in "$gc_object" "$thr_object"; do
  if [ ! -f "$object" ]; then
    printf 'Windows GC2 thread-cell object not found: %s\n' "$object" >&2
    exit 1
  fi
done

gc_symbols=$($objdump -t "$gc_object")
thr_symbols=$($objdump -t "$thr_object")

# The five capability fields must no longer exist as independent symbols. In
# particular, MinGW's emulated TLS calls malloc/locks on first access and is
# not an admissible nonblocking GC/JIT hot-path backend.
if grep -Eq 'gc2_(reclaim_tls_g|idle_transition_gate_tls_g|idle_reclaim_gate_owned_tls|smr_reader_tls_g|smr_reader_tls_depth)|__emutls' \
    <<<"$gc_symbols"; then
  printf 'Windows GC2 gate found legacy or emulated TLS storage in %s\n' \
    "$gc_object" >&2
  exit 1
fi

gc_accessor=$(grep -E '[[:space:]]lj_thr_gc2_tls_current$' \
  <<<"$gc_symbols" | head -n 1 || true)
thr_accessor=$(grep -E '[[:space:]]lj_thr_gc2_tls_current$' \
  <<<"$thr_symbols" | head -n 1 || true)
if [ -z "$gc_accessor" ] || ! grep -Eq '\(sec[[:space:]]+0\)' \
    <<<"$gc_accessor"; then
  printf 'Windows GC2 gate lacks the thread-cell accessor import: %s\n' \
    "${gc_accessor:-missing}" >&2
  exit 1
fi
if [ -z "$thr_accessor" ] || grep -Eq '\(sec[[:space:]]+0\)' \
    <<<"$thr_accessor"; then
  printf 'Windows GC2 gate lacks the thread-cell accessor definition: %s\n' \
    "${thr_accessor:-missing}" >&2
  exit 1
fi

# GC2 may only look up an admitted cell. It must never initialize the process
# key, allocate a cell, or publish TLS while entering SMR/reclaimer paths.
for forbidden in \
  lj_thr_tg_tls_init __imp_TlsAlloc __imp_TlsSetValue \
  __imp_InitOnceExecuteOnce __emutls_get_address
do
  if grep -Eq "[[:space:]]${forbidden}$" <<<"$gc_symbols"; then
    printf 'Windows GC2 gate found lazy-admission dependency %s in %s\n' \
      "$forbidden" "$gc_object" >&2
    exit 1
  fi
done

accessor_body=$($objdump -dr "$thr_object" | awk '
  /<lj_thr_gc2_tls_current>:/ { in_accessor = 1 }
  in_accessor && /^[[:xdigit:]]+[[:space:]]+<[^>]+>:/ &&
    $0 !~ /<lj_thr_gc2_tls_current>:/ { exit }
  in_accessor { print }
')
if [ -z "$accessor_body" ] ||
    ! grep -Eq '__imp_TlsGetValue' <<<"$accessor_body"; then
  printf 'Windows GC2 gate could not prove lookup-only TlsGetValue accessor\n' >&2
  exit 1
fi
tls_get_relocs=$(grep -Ec '__imp_TlsGetValue' <<<"$accessor_body")
if [ "$tls_get_relocs" -ne 1 ]; then
  printf 'Windows GC2 accessor has %s TlsGetValue relocations, expected 1\n' \
    "$tls_get_relocs" >&2
  exit 1
fi
for error_api in __imp_GetLastError __imp_SetLastError; do
  error_relocs=$(grep -Ec "$error_api" <<<"$accessor_body")
  if [ "$error_relocs" -ne 1 ]; then
    printf 'Windows GC2 accessor has %s %s relocations, expected 1\n' \
      "$error_relocs" "$error_api" >&2
    exit 1
  fi
done
if grep -Eq 'Tls(Set|Alloc)|InitOnce|malloc|calloc|realloc|free|lj_thr_tg_tls_init|__emutls' \
    <<<"$accessor_body"; then
  printf 'Windows GC2 thread-cell accessor performs admission/allocation\n' >&2
  exit 1
fi

printf 'Windows GC2 admitted thread-cell storage gate passed\n'
