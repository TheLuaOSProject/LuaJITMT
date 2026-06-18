#!/bin/sh
# Guard M5 release publication for gcroot/base-metatable roots.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

hits=$(
  cd "$ROOT/src" && \
  rg -n "setgcref\\([^;]*(gcroot|basemt_)" \
    --glob '*.c' --glob '*.h' || true
)

if [ -n "$hits" ]; then
  printf '%s\n' "$hits"
  echo "guardrail: gcroot publications must use setgcrefroot()" >&2
  exit 1
fi

reader_hits=$(
  cd "$ROOT/src" && \
  rg -n 'gcref\(g->gcroot\[|gcref\(G\([^)]*\)->gcroot|tabref\(g->gcroot|tabref\(G\([^)]*\)->gcroot|&gcref\([^)]*gcroot' \
    --glob '*.c' --glob '*.h' || true
)

if [ -n "$reader_hits" ]; then
  printf '%s\n' "$reader_hits"
  echo "guardrail: gcroot readers must acquire-load release-published roots" >&2
  exit 1
fi

if rg -n 'GCROOT_FFI_FIN|lj_ctype_initfin' "$ROOT/src"; then
  echo "guardrail: FINREG bootstrap must not use a legacy gcroot slot" >&2
  exit 1
fi

for needle in \
  'GCobj *o = gcref_acq(g->gcroot[i])' \
  'gco2ud(gcref_acq(G(L)->gcroot[(id)]))'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_gc.c" "$ROOT/src/lj_gc2.c" \
      "$ROOT/src/lj_cdata.c" "$ROOT/src/lib_ffi.c" "$ROOT/src/lib_io.c"; then
    echo "guardrail: missing gcroot acquire-load marker: $needle" >&2
    exit 1
  fi
done

echo "M5 gcroot publication guard passed"
