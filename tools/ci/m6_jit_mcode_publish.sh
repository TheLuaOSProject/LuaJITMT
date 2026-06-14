#!/bin/sh
# Guard Linux/x64 mcode sync-core publication ordering.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

make -C "$ROOT/src" >/dev/null

for needle in \
  'uint32_t jit_mcode_synccore' \
  'void lj_mcode_init(global_State *g)' \
  'la_membarrier_register_synccore() == 0' \
  'la_store32_rel(&g->jit_mcode_synccore, 1)' \
  'void lj_mcode_sync_core(jit_State *J)' \
  'la_load32_acq(&g->jit_mcode_synccore)' \
  'la_membarrier_synccore()' \
  'lj_mcode_init(g);' \
  'lj_mcode_sync_core(J);' \
  'LJ_MCODE_EXEC_STABLE' \
  'LJ_MCODE_FRESH_AREA' \
  'mcode_area_has_published(jit_State *J)' \
  'mcode_allocarea_checked(jit_State *J, size_t sz)' \
  'lj_mcode_freeall(global_State *g)' \
  'mcode_freearea_direct(global_State *g, MCode *area, size_t size)' \
  'lj_mcode_freeall(g);' \
  'J->szallmcarea + sz > maxmcode' \
  'memfd dual-map W^X implementation'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_obj.h" "$ROOT/src/lj_mcode.h" \
      "$ROOT/src/lj_mcode.c" "$ROOT/src/lj_state.c" \
      "$ROOT/src/lj_trace.c"; then
    echo "guardrail: missing mcode publication marker: $needle" >&2
    exit 1
  fi
done

if awk '
  /#if LJ_MT && defined\(__linux__\) && LJ_TARGET_X64/ { inmt = 1 }
  inmt && /#endif/ { inmt = 0 }
  inmt && /MCPROT_RWX/ { bad = 1 }
  END { exit bad ? 0 : 1 }
' "$ROOT/src/lj_mcode.c"; then
  echo "guardrail: secure Linux/x64 LJ_MT mcode bridge must not fall back to RWX" >&2
  exit 1
fi

if ! awk '
  /MCode \*lj_mcode_reserve\(jit_State \*J, MCode \*\*lim\)/ { infn = 1 }
  infn && /#ifdef LJ_MCODE_FRESH_AREA/ { fresh = NR }
  infn && /mcode_area_has_published\(J\)/ { has = NR }
  infn && /mcode_allocarea_checked\(J, mcode_fresh_size\(J\)\)/ { alloc = NR }
  infn && /mcode_protect\(J, MCPROT_GEN\)/ && !protect { protect = NR }
  infn && /#else/ { els = NR }
  infn && /^\}/ {
    exit !(fresh && has && alloc && protect && els &&
	   fresh < has && has < alloc && alloc < protect && protect < els)
  }
  END { if (!infn) exit 1 }
' "$ROOT/src/lj_mcode.c"; then
  echo "guardrail: LJ_MT reserve must allocate a fresh area before reopening published mcode" >&2
  exit 1
fi

if ! awk '
  /static void trace_stop\(jit_State \*J\)/ { infn = 1 }
  infn && /lj_mcode_commit\(J, J->cur.mcode\)/ { commit = NR }
  infn && /lj_mcode_sync_core\(J\)/ { sync = NR }
  infn && /trace_save\(J, T\)/ { save = NR }
  infn && /proto_trace_rel\(pt, traceno\)/ { proto = NR }
  infn && /bc_publish\(patchpc, patchins\)/ { bc = NR }
  infn && /trace_exittarget_rel\(parent, J->exitno, T->mcode\)/ { exitt = NR }
  infn && /trace_nextside_rel\(root, traceno\)/ { side = NR }
  infn && /trace_link_rel\(parent, traceno\)/ { link = NR }
  infn && /Start a new root trace/ {
    done = 1
    exit !(commit && sync && save &&
	   commit < sync && sync < save &&
	   save < proto && save < bc && save < exitt &&
	   save < side && save < link)
  }
  END { if (!done) exit 1 }
' "$ROOT/src/lj_trace.c"; then
  echo "guardrail: trace_stop must sync core after mcode commit and before release publications" >&2
  exit 1
fi

if ! rg -F -q 'm6_jit_mcode_publish.sh' "$ROOT/tools/ci/m6_jit.sh"; then
  echo "guardrail: m6_jit_mcode_publish.sh is not wired into the M6 aggregate" >&2
  exit 1
fi

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  timeout 20s "$ROOT/src/luajit" -e \
  'jit.opt.start("hotloop=1","hotexit=1"); local s=0; for i=1,80 do s=s+i end; assert(s==3240)'

LUA_PATH="$ROOT/src/?.lua;$ROOT/src/jit/?.lua;;" \
  timeout 30s "$ROOT/src/luajit" "$ROOT/tests/t-jit-mcode-fresh.lua"

echo "M6 JIT mcode publication guard passed"
