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
  'MCode *rw;		/* Writable alias of this area. */' \
  'lj_mcode_area_rw(MCode *area)' \
  'lj_mcode_rx2rw(MCode *area, MCode *rx)' \
  'lj_mcode_rw2rx(MCode *area, MCode *rw)' \
  'lj_mcode_rw(jit_State *J, MCode *rx)' \
  'mcode_register_area(jit_State *J, MCode *area' \
  'mcode_free_mapping(MCode *area, size_t sz)' \
  '((MCLink *)J->mcarea)->rw = J->mcarea;  /* 08.5: single-map write view. */' \
  'mcode_free_mapping(area, size);' \
  'asm_mcode_u8(ASMState *as, MCode **pp, MCode v)' \
  'asm_mcode_u64(ASMState *as, MCode **pp, uint64_t v)' \
  'asm_mcode_i32(ASMState *as, MCode **pp, int32_t v)' \
  'asm_mcode_ptr(ASMState *as, MCode **pp, const void *v)' \
  'asm_mcode_mem(ASMState *as, MCode **pp,' \
  'asm_mcode_put_u8(ASMState *as, MCode *p, MCode v)' \
  'asm_mcode_put_u16(ASMState *as, MCode *p, uint16_t v)' \
  'asm_mcode_put_i32(ASMState *as, MCode *p, int32_t v)' \
  'asm_mcode_put_u32(ASMState *as, MCode *p, uint32_t v)' \
  'asm_mcode_put_u64(ASMState *as, MCode *p, uint64_t v)' \
  'asm_mcode_patch_i32(jit_State *J, MCode *p, int32_t v)' \
  'emit_op(ASMState *as, x86Op xo' \
  'emit_opm(ASMState *as, x86Op xo' \
  'emit_opmx(ASMState *as, x86Op xo' \
  'lj_mcode_rw(as->J, *pp)' \
  'asm_mcode_i32(as, &mcp, jmprel(as->J, mcp + 4, target));' \
  '*lj_mcode_rw(as->J, as->mctop) = XI_NOP;' \
  'asm_mcode_put_i32(as, p+1, jmprel(as->J, p+5, target));' \
  'asm_mcode_patch_i32(J, p+len-4, jmprel(J, p+len, target));' \
  'asm_mcode_patch_i32(J, p+2, jmprel(J, p+6, target));' \
  'memfd dual-map W^X implementation'
do
  if ! rg -F -q "$needle" "$ROOT/src/lj_obj.h" "$ROOT/src/lj_jit.h" \
      "$ROOT/src/lj_mcode.h" "$ROOT/src/lj_mcode.c" "$ROOT/src/lj_state.c" \
      "$ROOT/src/lj_trace.c" "$ROOT/src/lj_emit_x86.h" \
      "$ROOT/src/lj_asm_x86.h"; then
    echo "guardrail: missing mcode publication marker: $needle" >&2
    exit 1
  fi
done

if awk '
  /#if defined\(__linux__\) && LJ_TARGET_X64/ { inx64 = 1 }
  inx64 && /#endif/ { inx64 = 0 }
  inx64 && /MCPROT_RWX/ { bad = 1 }
  END { exit bad ? 0 : 1 }
' "$ROOT/src/lj_mcode.c"; then
  echo "guardrail: secure Linux/x64 mcode bridge must not fall back to RWX" >&2
  exit 1
fi

if rg -n '#if[[:space:]]+LJ_MT|#ifdef[[:space:]]+LJ_MT|LUAJIT_THREADSAFE' \
    "$ROOT/src/lj_mcode.c"; then
  echo "guardrail: mcode publication bridge must not be hidden behind LJ_MT" >&2
  exit 1
fi

if rg -n '\*as->mcbot|\*mxp\+\+|\*\(uint64_t \*\)as->mcbot|\*\(void \*\*\)mxp|memcpy\(mxp' \
    "$ROOT/src/lj_emit_x86.h" "$ROOT/src/lj_asm_x86.h"; then
  echo "guardrail: x64 mcode bottom writes must go through lj_mcode_rw helpers" >&2
  exit 1
fi

if rg -n '\*--as->mcp|as->mcp\[[0-9]+\][[:space:]]*=[^=]|source\[[^]]+\][[:space:]]*=[^=]|(^|[^[:alnum:]_])p\[[^]]+\][[:space:]]*=[^=]|\*\(u?int(16|32|64)_t \*\)\(?p[-+0-9]*\)?[[:space:]]*=[^=]' \
    "$ROOT/src/lj_emit_x86.h"; then
  echo "guardrail: x64 core emitter writes must go through lj_mcode_rw helpers" >&2
  exit 1
fi

if awk '
  /void lj_asm_patchexit\(jit_State \*J, GCtrace \*T, ExitNo exitno, MCode \*target\)/ {
    seen_patchexit = 1
    exit bad ? 0 : 1
  }
  /\*--as->mcp|as->mcp\[[0-9]+\][[:space:]]*=[^=]|\*--p/ { bad = 1 }
  !/MCode \*patchnfpr/ && /\*patchnfpr[[:space:]]*=[^=]/ { bad = 1 }
  !/MCode \*q/ && /\*q[[:space:]]*[-+]?=[^=]/ { bad = 1 }
  /(^|[^[:alnum:]_])p\[[^]]+\][[:space:]]*=[^=]/ { bad = 1 }
  /\*\(u?int(16|32|64)_t \*\)\(?p[-+0-9]*\)?[[:space:]]*=[^=]/ { bad = 1 }
  END { if (!seen_patchexit) exit 1; exit bad ? 0 : 1 }
' "$ROOT/src/lj_asm_x86.h"; then
  echo "guardrail: x64 generation-time asm writes must go through lj_mcode_rw helpers" >&2
  exit 1
fi

if awk '
  /static void asm_tail_fixup\(ASMState \*as, TraceNo lnk\)/ { infn = 1 }
  infn && /\*mcp\+\+|mcp \+= 4|\*\(int32_t \*\)mcp|\*\(int32_t \*\)\(mcp-4\)|\*--as->mctop/ { bad = 1 }
  infn && /^\}/ { infn = 0 }
  END { exit bad ? 0 : 1 }
' "$ROOT/src/lj_asm_x86.h"; then
  echo "guardrail: x64 trace tail fixups must go through lj_mcode_rw helpers" >&2
  exit 1
fi

if awk '
  /void lj_asm_patchexit\(jit_State \*J, GCtrace \*T, ExitNo exitno, MCode \*target\)/ { infn = 1 }
  infn && /\*\(u?int(16|32|64)_t \*\)\(?p[-+0-9a-z ]*\)?[[:space:]]*=[^=]|(^|[^[:alnum:]_])p\[[^]]+\][[:space:]]*=[^=]/ { bad = 1 }
  infn && /^\}/ { infn = 0 }
  END { exit bad ? 0 : 1 }
' "$ROOT/src/lj_asm_x86.h"; then
  echo "guardrail: x64 committed-code exit patches must go through lj_mcode_rw helpers" >&2
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
  echo "guardrail: reserve must allocate a fresh area before reopening published mcode" >&2
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
