#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "arm64_cas128_contract SKIP: requires native Apple AArch64"
  exit 0
fi

cc=${CC:-clang}
minver=${MACOSX_DEPLOYMENT_TARGET:-11.0}
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-arm64-cas128.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

object="$tmpdir/cas128.o"
generic_object="$tmpdir/cas128-generic.o"
fixture="$tmpdir/cas128"
disasm="$tmpdir/cas128.disasm"
probe="$tmpdir/cas128.probe"
generic_disasm="$tmpdir/cas128-generic.disasm"
generic_probe="$tmpdir/cas128-generic.probe"

common_flags="-std=gnu11 -O2 -Wall -Wextra -Werror -Watomic-alignment -arch arm64 -mmacosx-version-min=$minver"

# shellcheck disable=SC2086 # common_flags intentionally expands to arguments.
"$cc" $common_flags \
  -I"$root/src" -c \
  "$root/tests/t-arm64-cas128-contract.c" -o "$object"
# shellcheck disable=SC2086 # common_flags intentionally expands to arguments.
"$cc" $common_flags -mcpu=generic -I"$root/src" -c \
  "$root/tests/t-arm64-cas128-contract.c" -o "$generic_object"
"$cc" -arch arm64 -mmacosx-version-min="$minver" "$object" -o "$fixture"
"$fixture"

if nm -u "$object" "$generic_object" | grep -E '(__atomic|libatomic)' >/dev/null; then
  echo "Apple AArch64 CAS128 imports an atomic runtime helper" >&2
  exit 1
fi

if file "$object" "$generic_object" | \
   grep -Ev 'Mach-O 64-bit object arm64' >/dev/null; then
  echo "Apple AArch64 CAS128 fixture has the wrong Mach-O architecture" >&2
  exit 1
fi
if ! otool -l "$object" | awk -v expected="$minver" \
  '$1 == "minos" && $2 == expected { found = 1 } END { exit !found }'; then
  echo "Apple AArch64 CAS128 fixture has the wrong deployment target" >&2
  exit 1
fi

otool -tvV "$object" >"$disasm"
otool -tvV "$generic_object" >"$generic_disasm"
awk '
  /^_arm64_cas128_probe:$/ { copying = 1; next }
  copying && /^_[[:alnum:]_.]+:$/ { exit }
  copying { print }
' "$disasm" >"$probe"
awk '
  /^_arm64_cas128_probe:$/ { copying = 1; next }
  copying && /^_[[:alnum:]_.]+:$/ { exit }
  copying { print }
' "$generic_disasm" >"$generic_probe"
test -s "$probe"
test -s "$generic_probe"
if ! grep -E '[[:space:]]caspal[[:space:]]' "$probe" >/dev/null; then
  echo "Apple AArch64 CAS128 did not lower to acquire-release CASP" >&2
  exit 1
fi
if ! grep -E '[[:space:]]ldaxp[[:space:]]' "$generic_probe" >/dev/null ||
   ! grep -E '[[:space:]]stlxp[[:space:]]' "$generic_probe" >/dev/null; then
  echo "generic ARMv8 CAS128 did not lower to an inline exclusive pair" >&2
  exit 1
fi
if grep -E '[[:space:]](bl|blr)[[:space:]]' "$probe" "$generic_probe" >/dev/null; then
  echo "Apple AArch64 CAS128 artifact contains an out-of-line call" >&2
  exit 1
fi

echo "arm64_cas128_contract OK: CASPAL and generic LL/SC are inline"
