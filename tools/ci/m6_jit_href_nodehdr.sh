#!/bin/sh
# Guard the M6 x64 dynamic HREF node-header hmask pairing.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
TMP=${TMPDIR:-/tmp}/lj-m6-href-nodehdr.$$
trap 'rm -f "$TMP"' EXIT

for needle in \
  'M6: dynamic HREF masks against the loaded node header, not GCtab.hmask.' \
  'emit_rmro(as, XO_MOV, dest|REX_GC64, tab, offsetof(GCtab, node));' \
  '-(int32_t)sizeof(TabNodeHdr)'
do
  if ! rg -F -q -- "$needle" "$ROOT/src/lj_asm_x86.h"; then
    echo "guardrail: missing dynamic HREF node-header marker: $needle" >&2
    exit 1
  fi
done

if ! rg -F -q 'M6: empty-hash misses fall through to HREF so x64 uses TabNodeHdr.hmask.' \
    "$ROOT/src/lj_record.c"; then
  echo "guardrail: x64 empty-hash misses must avoid the GCtab.hmask shortcut" >&2
  exit 1
fi

if awk '
  /static void asm_href\(ASMState \*as, IRIns \*ir, IROp merge\)/ { infn = 1 }
  infn && /static void asm_hrefk\(ASMState \*as, IRIns \*ir\)/ { infn = 0 }
  infn && /offsetof\(GCtab, hmask\)|IRFL_TAB_HMASK/ { bad = 1 }
  END { exit bad ? 0 : 1 }
' "$ROOT/src/lj_asm_x86.h"; then
  echo "guardrail: dynamic HREF must not use GCtab.hmask as its hmask source" >&2
  exit 1
fi

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  timeout 20s "$ROOT/src/luajit" -jdump=ir -e '
    jit.opt.start("hotloop=1", "hotexit=1")
    local keys = {"a", "b"}
    local t = {a = 10, b = 20}
    local s = 0
    local function f(k)
      return t[k]
    end
    for i = 1, 80 do
      s = s + f(keys[i % 2 + 1])
    end
    assert(s > 0)
  ' >"$TMP" 2>&1

if ! awk '
  /---- TRACE 1 IR/ { inir = 1; next }
  /---- TRACE 1 stop/ { done = 1; exit !(href && !hrefk) }
  inir && / HREF / { href = 1 }
  inir && / HREFK / { hrefk = 1 }
  END { if (!done) exit 1 }
' "$TMP"; then
  cat "$TMP" >&2
  echo "guardrail: dynamic string-key lookup must record HREF, not HREFK" >&2
  exit 1
fi

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  timeout 20s "$ROOT/src/luajit" -jdump=ir -e '
    jit.flush()
    jit.opt.start("hotloop=1", "hotexit=1")
    local t = {}
    local keys = {"missing_a", "missing_b"}
    local seen = 0
    for i = 1, 80 do
      local k = keys[i % 2 + 1]
      if t[k] == nil then seen = seen + 1 end
    end
    assert(seen == 80)
  ' >"$TMP" 2>&1

if ! awk '
  /---- TRACE 1 IR/ { inir = 1; next }
  /---- TRACE 1 stop/ { done = 1; exit !(href && !hrefk && !hmask) }
  inir && / HREF / { href = 1 }
  inir && / HREFK / { hrefk = 1 }
  inir && /tab[.]hmask/ { hmask = 1 }
  END { if (!done) exit 1 }
' "$TMP"; then
  cat "$TMP" >&2
  echo "guardrail: x64 empty-hash miss must fall through to HREF without tab.hmask" >&2
  exit 1
fi

if ! rg -F -q 'm6_jit_href_nodehdr.sh' "$ROOT/tools/ci/m6_jit.sh"; then
  echo "guardrail: m6_jit_href_nodehdr.sh is not wired into the M6 aggregate" >&2
  exit 1
fi

echo "M6 JIT dynamic HREF node-header guard passed"
