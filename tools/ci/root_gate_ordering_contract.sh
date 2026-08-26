#!/bin/sh
set -eu

root=${LJ_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}

if test "$(uname -s)" != Darwin || test "$(uname -m)" != arm64; then
  echo "root_gate_ordering_contract SKIP: requires native Apple AArch64"
  exit 0
fi

cc=${CC:-clang}
minver=${MACOSX_DEPLOYMENT_TARGET:-11.0}
rounds=${LJ_ROOT_GATE_ORDERING_ROUNDS:-2000000}
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/lj-root-gate-ordering.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

object="$tmpdir/root-gate-ordering.o"
fixture="$tmpdir/root-gate-ordering"
disasm="$tmpdir/root-gate-ordering.disasm"
publisher="$tmpdir/root-gate-ordering.publisher"
scanner="$tmpdir/root-gate-ordering.scanner"
flags="-std=gnu11 -O2 -Wall -Wextra -Werror -Watomic-alignment -pthread -arch arm64 -mmacosx-version-min=$minver"

# shellcheck disable=SC2086 # flags intentionally expands to arguments.
"$cc" $flags -I"$root/src" -c \
  "$root/tests/t-gc2-root-gate-ordering.c" -o "$object"
"$cc" -arch arm64 -mmacosx-version-min="$minver" -pthread \
  "$object" -o "$fixture"

if nm -u "$object" | grep -E '(__atomic|libatomic)' >/dev/null; then
  echo "root descriptor/gate fixture imports an atomic runtime helper" >&2
  exit 1
fi
if file "$object" | grep -Ev 'Mach-O 64-bit object arm64' >/dev/null; then
  echo "root descriptor/gate fixture has the wrong Mach-O architecture" >&2
  exit 1
fi
if ! otool -l "$object" | awk -v expected="$minver" \
  '$1 == "minos" && $2 == expected { found = 1 } END { exit !found }'; then
  echo "root descriptor/gate fixture has the wrong deployment target" >&2
  exit 1
fi

otool -tvV "$object" >"$disasm"
awk '
  /^_arm64_root_gate_publisher_boundary:$/ { copying = 1; next }
  copying && /^_[[:alnum:]_.]+:$/ { exit }
  copying { print }
' "$disasm" >"$publisher"
awk '
  /^_arm64_root_gate_closing_scan_boundary:$/ { copying = 1; next }
  copying && /^_[[:alnum:]_.]+:$/ { exit }
  copying { print }
' "$disasm" >"$scanner"
test -s "$publisher"
test -s "$scanner"

# The publisher has an earlier descriptor-payload fence as well as the
# StoreLoad fence under test. Require the second full fence after the second
# descriptor CAS and before the exact gate snapshot. Without that boundary,
# only the payload-publication fence remains in this function.
if ! awk '
  /[[:space:]]casal[[:space:]]/ { after_cas = 1; fenced = 0 }
  after_cas && /[[:space:]]dmb[[:space:]]+ish([[:space:]]|$)/ { fenced = 1 }
  fenced && /[[:space:]]caspal[[:space:]]/ { ordered = 1 }
  END { exit !ordered }
' "$publisher"; then
  echo "publisher artifact lacks descriptor CAS -> DMB ISH -> gate CASP" >&2
  exit 1
fi

# The runtime closer first creates CLOSING. This dedicated scan probe verifies
# that snapshot_closing independently exact-acquires that authority before its
# own fence and descriptor load. The rule therefore also covers helper threads
# that did not create CLOSING themselves.
if ! awk '
  /[[:space:]]caspal[[:space:]]/ { acquired = 1 }
  acquired && /[[:space:]]dmb[[:space:]]+ish([[:space:]]|$)/ { fenced = 1 }
  fenced && /[[:space:]](ldar|ldapr)[[:space:]]/ { ordered = 1 }
  END { exit !ordered }
' "$scanner"; then
  echo "closer artifact lacks exact CLOSING acquire -> DMB -> descriptor load" >&2
  exit 1
fi

"$fixture" "$rounds"
echo "root_gate_ordering_contract OK: native ARM boundaries retain full fences"
