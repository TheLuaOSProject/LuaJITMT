#!/bin/sh
set -eu

root_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/luajitmt-dynasm-arm64.XXXXXX")
trap 'test -n "$tmp_dir" && rm -r -- "$tmp_dir"' EXIT HUP INT TERM

if [ -n "${MINILUA:-}" ]; then
  lua_bin=$MINILUA
elif [ -x "$root_dir/src/host/minilua" ]; then
  lua_bin=$root_dir/src/host/minilua
elif command -v luajit >/dev/null 2>&1; then
  lua_bin=$(command -v luajit)
elif command -v lua >/dev/null 2>&1; then
  lua_bin=$(command -v lua)
else
  lua_bin=$tmp_dir/minilua
  ${HOST_CC:-cc} -O2 -o "$lua_bin" "$root_dir/src/host/minilua.c" -lm
fi

dynasm=$root_dir/dynasm/dynasm.lua
fixture=$root_dir/tests/t-dynasm-arm64-ordering.dasc
generated=$tmp_dir/ordering.c

"$lua_bin" "$dynasm" -L -o "$generated" "$fixture"

expected_code_words='0x88dffc20
0xc8dffc62
0x08dffca4
0x48dffce6
0x889ffd28
0xc89ffd6a
0x089ffdac
0x489fffee
0x88dffc1f
0xc89ffc1f
0xd50331bf
0xd50332bf
0xd50333bf
0xd50335bf
0xd50336bf
0xd50337bf
0xd50339bf
0xd5033abf
0xd5033bbf
0xd5033dbf
0xd5033ebf
0xd5033fbf
0xd50330bf
0xd5033fbf
0xd5033fdf
0xd5033fdf
0xd50330df
0xd5033fdf'

expected_actions="0x00010000
$expected_code_words
0x00000000"
actual_actions=$(awk '
  /static const unsigned int arm64_ordering_actions/ { inside = 1; next }
  inside && /};/ { exit }
  inside && /0x/ { gsub(/,/, ""); print }
' "$generated")

if [ "$actual_actions" != "$expected_actions" ]; then
  printf '%s\n' "unexpected DynASM ARM64 action list" >&2
  printf '%s\n' "expected:" "$expected_actions" "actual:" "$actual_actions" >&2
  exit 1
fi

# Base-only acquire/release operands must retain DynASM virtual-register
# actions; the static Apple assembler reference cannot exercise this path.
vreg_generated=$tmp_dir/ordering-vreg.c
printf '%s\n' '|.arch arm64' '|.section code' '|.actionlist vreg_actions' \
  '|.code' '| ldar x0, [Rx(first)]' '| stlr w1, [Rx(second)]' | \
  "$lua_bin" "$dynasm" -L -o "$vreg_generated" -
vreg_actions=$(awk '
  /static const unsigned int vreg_actions/ { inside = 1; next }
  inside && /};/ { exit }
  inside && /0x/ { gsub(/,/, ""); print }
' "$vreg_generated")
expected_vreg_actions='0x00010000
0xc8dffc00
0x00110005
0x889ffc01
0x00110005
0x00000000'
if [ "$vreg_actions" != "$expected_vreg_actions" ]; then
  printf '%s\n' "unexpected virtual-register action list" >&2
  exit 1
fi
if ! grep -F 'dasm_put(Dst, 1, (first), (second));' \
    "$vreg_generated" >/dev/null; then
  printf '%s\n' "virtual-register arguments are missing or misordered" >&2
  exit 1
fi

check_reject()
{
  instruction=$1
  diagnostic=$2
  rejected=$tmp_dir/rejected.c
  errors=$tmp_dir/errors.txt
  if printf '%s\n' '|.arch arm64' '|.section code' '|.code' \
      "| $instruction" | \
      "$lua_bin" "$dynasm" -L -o "$rejected" - 2>"$errors"; then
    printf '%s\n' "DynASM unexpectedly accepted: $instruction" >&2
    exit 1
  fi
  if ! grep -F "$diagnostic" "$errors" >/dev/null; then
    printf '%s\n' "DynASM rejected '$instruction' with the wrong diagnostic" >&2
    sed -n '1,8p' "$errors" >&2
    exit 1
  fi
}

check_reject 'ldar x0, [x1, #8]' 'expected base-only address operand'
check_reject 'stlr w0, [w1]' 'bad register type'
check_reject 'ldar x0, [xzr]' 'zero register is not a valid address base'
check_reject 'dmb bogus' 'expected DMB option'
check_reject 'dmb #16' 'expected DMB option'
check_reject 'isb ish' 'expected ISB option'
check_reject 'isb #-1' 'expected ISB option'

typed_rejected=$tmp_dir/typed-rejected.c
typed_errors=$tmp_dir/typed-errors.txt
if printf '%s\n' '|.arch arm64' '|.section code' \
    '|.type BADBASE, int, xzr' '|.code' '| ldar w0, [BADBASE]' | \
    "$lua_bin" "$dynasm" -L -o "$typed_rejected" - \
      2>"$typed_errors"; then
  printf '%s\n' "DynASM unexpectedly accepted a typed xzr base" >&2
  exit 1
fi
if ! grep -F 'zero register is not a valid address base' \
    "$typed_errors" >/dev/null; then
  printf '%s\n' "typed xzr base produced the wrong diagnostic" >&2
  exit 1
fi
typed_override=$tmp_dir/typed-override.c
override_errors=$tmp_dir/override-errors.txt
if printf '%s\n' '|.arch arm64' '|.section code' \
    '|.type BASE, int, x0' '|.code' '| ldar w0, [BASE:xzr]' | \
    "$lua_bin" "$dynasm" -L -o "$typed_override" - \
      2>"$override_errors"; then
  printf '%s\n' "DynASM unexpectedly accepted a typed xzr override" >&2
  exit 1
fi
if ! grep -F 'zero register is not a valid address base' \
    "$override_errors" >/dev/null; then
  printf '%s\n' "typed xzr override produced the wrong diagnostic" >&2
  exit 1
fi

if [ "$(uname -s)" = Darwin ] && \
    command -v clang >/dev/null 2>&1 && command -v otool >/dev/null 2>&1; then
  object=$tmp_dir/ordering.o
  clang -target arm64-apple-macos11 -c -o "$object" \
    "$root_dir/tests/t-dynasm-arm64-ordering.s"
  assembler_words=$(otool -t "$object" | awk '
    $1 ~ /^[0-9a-f][0-9a-f]*$/ {
      for (i = 2; i <= NF; i++) print "0x" $i
    }
  ')
  if [ "$assembler_words" != "$expected_code_words" ]; then
    printf '%s\n' "Apple assembler encodings do not match DynASM" >&2
    printf '%s\n' "expected:" "$expected_code_words" \
      "assembler:" "$assembler_words" >&2
    exit 1
  fi
fi

printf '%s\n' "OK: DynASM ARM64 acquire/release and barrier encodings"
