#!/bin/sh
# Guard M5 JIT trace-slot and trace-link publication.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

for needle in \
  'LJ_TRACE_PENDING' \
  'traceref_fromgco(GCobj *o)' \
  'gcref_acq(tracevec_acq(J)[(n)])' \
  'traceslot_pending(J, n)' \
  'traceslot_publish(J, n, T)' \
  'traceslot_clear(J, n)' \
  'traceno16_acq(const uint16_t *p)' \
  'trace_link_acq(T)' \
  'trace_nextroot_acq(T)' \
  'trace_nextside_acq(T)' \
  'proto_trace_acq(pt)'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_jit.h"; then
    echo "guardrail: missing trace publication helper: $needle" >&2
    exit 1
  fi
done

hits=$(rg -n -- 'setgcrefp\(J->trace|setgcrefnull\(J->trace|gcref\(J->trace' \
  "$ROOT/src/lj_trace.c" "$ROOT/src/lj_jit.h" || true)
if [ -n "$hits" ]; then
  echo "guardrail: J->trace slots must use acquire/release trace helpers:" >&2
  echo "$hits" >&2
  exit 1
fi

hits=$(rg -n -- 'pt->trace\b|->link\b|->nextroot\b|->nextside\b' \
  "$ROOT/src/lj_trace.c" "$ROOT/src/lj_gc.c" "$ROOT/src/lj_gc2.c" \
  "$ROOT/src/lib_jit.c" "$ROOT/src/lj_bcwrite.c" || true)
if [ -n "$hits" ]; then
  echo "guardrail: shared trace-number fields must use acquire/release helpers:" >&2
  echo "$hits" >&2
  exit 1
fi

if ! awk '
  /static void trace_stop\(jit_State \*J\)/ { infn = 1 }
  infn && /lj_mcode_commit\(J, J->cur.mcode\)/ { commit = NR }
  infn && /trace_save\(J, T\)/ { save = NR }
  infn && /bc_publish\(patchpc, patchins\)/ { bc = NR }
  infn && /lj_asm_patchexit\(J, parent, J->exitno, T->mcode\)/ { side = NR }
  infn && /trace_link_rel\(parent, traceno\)/ { stitch = NR }
  END { exit(commit && save && bc && side && stitch &&
	     commit < save && save < bc && save < side && save < stitch ? 0 : 1) }
' "$ROOT/src/lj_trace.c"; then
  echo "guardrail: trace_stop must publish final trace before bytecode/exit/link go-signals" >&2
  exit 1
fi

if ! rg -F -q 'lnk == as->T->traceno ? as->T : traceref(as->J, lnk)' \
  "$ROOT/src/lj_asm_x86.h"; then
  echo "guardrail: x86 assembler self-links must not read the pending trace slot" >&2
  exit 1
fi

if ! rg -F -q 'm5_jit_trace_publish.sh' "$ROOT/tools/ci/m5_concurrent_objects.sh"; then
  echo "guardrail: m5_jit_trace_publish.sh is not wired into the M5 aggregate" >&2
  exit 1
fi

make -C "$ROOT/src" -j"$JOBS" >/dev/null

"$ROOT/src/luajit" -e '
local util = require"jit.util"
local function tracecount()
  local n = 0
  for i = 1, 200 do
    if util.traceinfo(i) then n = n + 1 end
  end
  return n
end
jit.off(tracecount, true)

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function f(n)
  local s = 0
  for i = 1, n do s = s + i end
  return s
end
for _ = 1, 40 do
  assert(f(200) == 20100)
end
assert(tracecount() > 0, "no root trace was published")
jit.flush()
assert(tracecount() == 0, "trace slots were not cleared")

jit.flush()
jit.opt.start("hotloop=1")
local function f1(a)
  if a > 0 then
    local b = f1(a - 1)
    return function()
      if type(b) == "function" then return a + b() end
      return a + b
    end
  end
  return a
end
local function f2(a) return f1(a)() end
for _ = 1, 41 do
  assert(f2(4) + f2(4) == 20)
end
print("jit-trace-publish-smoke OK")
'

echo "M5 JIT trace publication guard passed"
