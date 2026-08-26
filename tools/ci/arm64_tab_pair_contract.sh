#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_tab_pair_contract SKIP: requires native Apple AArch64"
  exit 0
fi

cc=${CC:-clang}
minver=${MACOSX_DEPLOYMENT_TARGET:-11.0}
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-tab-pair.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

fixture="$tmpdir/tab-pair"
object="$tmpdir/tab-pair.o"
disasm="$tmpdir/tab-pair.disasm"
flags="-std=gnu11 -O2 -Wall -Wextra -Werror -Watomic-alignment -pthread -arch arm64 -mmacosx-version-min=$minver"

# shellcheck disable=SC2086 # flags intentionally expands to arguments.
"$cc" $flags -I"$root/src" -DLUAJIT_MT_ARM64_BOOTSTRAP \
  -DLUAJIT_DISABLE_JIT -c "$root/tests/t-arm64-tab-pair-contract.c" \
  -o "$object"
"$cc" -arch arm64 -mmacosx-version-min="$minver" -pthread \
  "$object" -o "$fixture"
"$fixture"

if nm -u "$object" | grep -E '(__atomic|libatomic)' >/dev/null; then
  echo "Apple AArch64 GCtab pair imports an atomic runtime helper" >&2
  exit 1
fi
otool -tvV "$object" >"$disasm"

require_caspal_symbol() {
  symbol=$1
  if ! awk -v label="_$symbol:" '
    $0 == label { inside = 1; next }
    inside && /^[^0-9].*:$/ { exit(found && !bad ? 0 : 1) }
    inside && /[[:space:]]caspal[[:space:]]/ { found = 1 }
    inside && /^[0-9]/ && /\[[^]]*\]/ &&
      !/[[:space:]]caspal[[:space:]]/ { bad = 1 }
    inside && /^[0-9]/ && $2 ~ /^blr?$/ { bad = 1 }
    END { if (!inside || !found || bad) exit 1 }
  ' "$disasm"; then
    echo "Apple AArch64 GCtab helper $symbol is not CASPAL-only" >&2
    exit 1
  fi
}

for symbol in \
  tab_pair_probe_control_acq \
  tab_pair_probe_control_cas \
  tab_pair_probe_control_rel \
  tab_pair_probe_weak_acq \
  tab_pair_probe_weak_acap_rel \
  tab_pair_probe_weak_store \
  tab_pair_probe_weak_cas \
  tab_pair_probe_full_cas
do
  require_caspal_symbol "$symbol"
done

echo "arm64_tab_pair_contract OK: exact GCtab pair operations are inline"
