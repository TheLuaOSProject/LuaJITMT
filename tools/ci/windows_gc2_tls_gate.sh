#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 2 ]; then
  printf 'usage: %s <objdump> <lj_gc2.o>\n' "$0" >&2
  exit 2
fi

objdump=$1
object=$2

if ! command -v "$objdump" >/dev/null 2>&1; then
  printf '%s is required to validate Windows GC2 TLS storage\n' "$objdump" >&2
  exit 1
fi
if [ ! -f "$object" ]; then
  printf 'Windows GC2 TLS object not found: %s\n' "$object" >&2
  exit 1
fi

symbols=$($objdump -t "$object")
sections=$($objdump -h "$object")
backend=

for name in \
  gc2_reclaim_tls_g \
  gc2_idle_transition_gate_tls_g \
  gc2_idle_reclaim_gate_owned_tls \
  gc2_smr_reader_tls_g \
  gc2_smr_reader_tls_depth
do
  line=$(grep -E "[[:space:]]${name}$" <<<"$symbols" | head -n 1 || true)
  if [ -n "$line" ]; then
    sec=$(sed -n 's/.*(sec[[:space:]]*\([0-9][0-9]*\)).*/\1/p' <<<"$line")
    if [ -z "$sec" ] || [ "$sec" -le 0 ]; then
      printf 'Windows GC2 TLS gate could not classify %s: %s\n' \
        "$name" "$line" >&2
      exit 1
    fi
    section_index=$((sec - 1))
    section=$(awk -v section_index="$section_index" \
      '$1 == section_index { print $2; exit }' <<<"$sections")
    case "$section" in
      .tls|.tls\$*) field_backend=native ;;
      *)
        printf 'Windows GC2 TLS gate found process-global %s in section %s\n' \
          "$name" "${section:-unknown}" >&2
        exit 1
        ;;
    esac
  elif grep -Eq "[[:space:]]__emutls_v[.]${name}$" <<<"$symbols"; then
    field_backend=emutls
  else
    printf 'Windows GC2 TLS gate could not find %s in %s\n' \
      "$name" "$object" >&2
    exit 1
  fi

  if [ -z "$backend" ]; then
    backend=$field_backend
  elif [ "$backend" != "$field_backend" ]; then
    printf 'Windows GC2 TLS gate found mixed %s/%s backends at %s\n' \
      "$backend" "$field_backend" "$name" >&2
    exit 1
  fi
done

if [ "$backend" = emutls ] &&
    ! grep -Eq '[[:space:]]__emutls_get_address$' <<<"$symbols"; then
  printf 'Windows GC2 emutls descriptors lack __emutls_get_address\n' >&2
  exit 1
fi

printf 'Windows GC2 TLS storage gate passed (%s)\n' "$backend"
