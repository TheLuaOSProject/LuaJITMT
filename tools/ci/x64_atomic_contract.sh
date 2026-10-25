#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
if test "$(uname -s)" != Darwin; then
  echo "x64_atomic_contract SKIP: requires Darwin x86-64 or Rosetta"
  exit 0
fi
cc=${CC:-clang}
minver=${MACOSX_DEPLOYMENT_TARGET:-13.0}
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-x64-atomic.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

for name in cas128 tab-pair; do
  object=$tmpdir/$name.o
  fixture=$tmpdir/$name
  "$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -Watomic-alignment \
    -arch x86_64 -mcx16 -mmacosx-version-min="$minver" -pthread \
    -DLUAJIT_DISABLE_JIT -I"$root/src" \
    -c "$root/tests/t-arm64-$name-contract.c" -o "$object"
  "$cc" -arch x86_64 -mmacosx-version-min="$minver" -pthread \
    "$object" -o "$fixture"
  codesign --force --sign - "$fixture"
  "$fixture"
  test "$(lipo -archs "$object")" = x86_64
  if nm -u "$object" | grep -E '(__atomic|libatomic)' >/dev/null; then
    echo "x86-64 $name imports an atomic runtime helper" >&2
    exit 1
  fi
  otool -tvV "$object" >"$tmpdir/$name.disasm"
done

# The paired compare/exchange must stay inline, including when reached through
# the GCtab wrapper. The x86-64 half-width table helpers retain their existing
# TSO implementation; the ARM64 companion checks every exact-pair helper.
for item in 'cas128 arm64_cas128_probe' 'tab-pair tab_pair_probe_full_cas'; do
  set -- $item
  if ! awk -v label="_$2:" '
    $0 == label { inside = 1; next }
    inside && /^_[[:alnum:]_.]+:$/ { exit }
    inside && /cmpxchg16b/ { found = 1 }
    inside && /[[:space:]]callq?[[:space:]]/ { bad = 1 }
    END { if (!inside || !found || bad) exit 1 }
  ' "$tmpdir/$1.disasm"; then
    echo "x86-64 $2 did not emit an inline CMPXCHG16B" >&2
    exit 1
  fi
done
echo "x64_atomic_contract OK: inline CMPXCHG16B and concurrent table pair"
