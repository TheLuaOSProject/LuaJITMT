#!/bin/sh
# M0 guardrails from plan/12_implementation_plan.md.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
PIN=b925b3e3fc6771171602323b45fbe9fb8fc90369

fail=0

if ! git -C "$ROOT" cat-file -e "$PIN^{commit}" 2>/dev/null; then
  echo "guardrail: pinned LuaJIT commit is not present locally: $PIN" >&2
  fail=1
elif ! git -C "$ROOT" merge-base --is-ancestor "$PIN" HEAD; then
  echo "guardrail: HEAD is not descended from pinned LuaJIT commit $PIN" >&2
  fail=1
fi

mutex_hits=$(git -C "$ROOT" grep -n 'pthread_mutex' -- \
  ':!src/lj_profile.c' \
  ':!src/lj_thr.c' \
  ':!src/lj_chan.c' \
  ':!tests/c/*' \
  ':!tools/*' \
  ':!notes/*' \
  ':!plan/*' \
  ':!tests/stock/*' || true)
if [ -n "$mutex_hits" ]; then
  echo "guardrail: pthread_mutex appears outside the M0 whitelist:" >&2
  echo "$mutex_hits" >&2
  fail=1
fi

added_files=$(git -C "$ROOT" diff --name-only --diff-filter=A "$PIN"..HEAD -- || true)
volatile_hits=
for f in $added_files; do
  case "$f" in
    aux/bench/*|bench/*|docs/*|tests/stock/*|tools/*|notes/*|plan/*|src/lj_atomic.h)
      continue
      ;;
    *.c|*.h|*.dasc|*.lua)
      if [ -f "$ROOT/$f" ]; then
        hit=$(grep -n 'volatile' "$ROOT/$f" 2>/dev/null || true)
        if [ -n "$hit" ]; then
          volatile_hits="${volatile_hits}${volatile_hits:+
}$f:$hit"
        fi
      fi
      ;;
  esac
done
if [ -n "$volatile_hits" ]; then
  echo "guardrail: raw volatile appears in new runtime files:" >&2
  echo "$volatile_hits" >&2
  fail=1
fi

barrier_hits=$(git -C "$ROOT" grep -n -E 'lj_gc_(objbarrier|objbarriert|anybarriert|barrieruv|barriert|barrier)\b' -- 'src/*.c' ':!src/lj_gc.c' || true)
barrier_count=$(printf '%s\n' "$barrier_hits" | sed '/^$/d' | wc -l | tr -d ' ')
if [ "${LJ_MT_REQUIRE_NO_LEGACY_BARRIERS:-0}" = 1 ] && [ "$barrier_count" -ne 0 ]; then
  echo "guardrail: legacy lj_gc_barrier call sites remain under strict mode:" >&2
  echo "$barrier_hits" >&2
  fail=1
else
  echo "guardrail: legacy lj_gc_barrier call sites currently detected: $barrier_count"
fi

x64_dispatch_gl_gc_hits=$(grep -n 'DISPATCH_GL(gc\.' \
  "$ROOT/src/vm_x64.dasc" "$ROOT/src/lj_asm_x86.h" 2>/dev/null || true)
if [ -n "$x64_dispatch_gl_gc_hits" ]; then
  echo "guardrail: x86-64 VM/JIT must not load legacy gc fields via DISPATCH_GL:" >&2
  echo "$x64_dispatch_gl_gc_hits" >&2
  fail=1
fi

if [ "$fail" -ne 0 ]; then
  exit 1
fi

echo "guardrail: M0 checks passed"
