#!/bin/sh
# Guard that CTState does not regain a shared lua_State carrier.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

if rg -n 'cts->L' "$ROOT/src"; then
  echo "guardrail: CTState must not carry or dereference a shared lua_State" >&2
  exit 1
fi

if awk '
  /typedef struct CTState/ { in_cts = 1 }
  in_cts && /lua_State[[:space:]]*\*L/ {
    print
    bad = 1
  }
  in_cts && /^} CTState;/ { in_cts = 0 }
  END { exit bad ? 0 : 1 }
' "$ROOT/src/lj_ctype.h"; then
  echo "guardrail: CTState must not embed lua_State *L" >&2
  exit 1
fi

for needle in \
  'global_State *g;	/* Global state. */' \
  'mref_acq((g)->ctype_state, CTState)' \
  'setmrefrel(G(L)->ctype_state, cts)' \
  'cp.L = L' \
  'cp.L = J->L'
do
  if ! rg -F -q "$needle" "$ROOT/src"; then
    echo "guardrail: missing explicit FFI context marker: $needle" >&2
    exit 1
  fi
done

if rg -n 'mref\([^)]*ctype_state|setmref\([^)]*ctype_state' \
    "$ROOT/src" -g '*.c' -g '*.h' -g '!**/host/*'; then
  echo "guardrail: C-side CTState global pointer must use acquire/release MRef helpers" >&2
  exit 1
fi

echo "M7 FFI no shared CTState lua_State guard passed"
