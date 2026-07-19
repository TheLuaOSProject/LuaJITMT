#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
bad=0

# These reader/recorder domains have completed their one-shot SMR cutover.
# Keep the list narrow until each additional lifecycle domain has matching
# retry/defer semantics and runtime evidence; this is not an allowlist for the
# blocking call sites that remain elsewhere.
for rel in \
  src/lib_jit.c \
  src/lj_asm_x86.h \
  src/lj_bcwrite.c \
  src/lj_debug.c \
  src/lj_record.c \
  src/lj_asm.c \
  src/lj_gdbjit.c
do
  hits=$(rg -n 'lj_gc2_smr_read_enter[[:space:]]*\(' "$ROOT/$rel" || true)
  if [ -n "$hits" ]; then
    printf 'forbidden looping SMR admission returned in %s:\n%s\n' \
      "$rel" "$hits" >&2
    bad=1
  fi
done

unpublished=$(sed -n \
  '/^static void trace_retire_unpublished(/,/^}/p' "$ROOT/src/lj_trace.c")
if printf '%s\n' "$unpublished" | \
   rg -n 'trace_preservebody|lj_gc2_smr_read_enter' >/dev/null; then
  printf '%s\n' \
    'unpublished scratch retirement regained semantic/looping preservation' >&2
  bad=1
fi
if ! printf '%s\n' "$unpublished" | \
     rg -q 'trace_preserve_unpublished_publish'; then
  printf '%s\n' 'unpublished scratch retirement lost tactical raw marks' >&2
  bad=1
fi

record_lookup=$(sed -n '/^static GCtrace \*rec_traceref_live(/,/^}/p' \
  "$ROOT/src/lj_record.c")
asm_lookup=$(sed -n '/^static GCtrace \*asm_traceref_live(/,/^}/p' \
  "$ROOT/src/lj_asm.c")
asm_tail=$(sed -n '/^static void asm_tail_fixup(/,/^}/p' \
  "$ROOT/src/lj_asm_x86.h")
if ! printf '%s\n' "$record_lookup" | rg -Uq \
     '!lj_gc2_smr_read_try\(g\)\)+\s+lj_trace_err\(J, LJ_TRERR_SMRRETRY\)'; then
  printf '%s\n' 'recorder SMR admission no longer raises SMRRETRY' >&2
  bad=1
fi
if ! printf '%s\n' "$asm_lookup" | rg -Uq \
     '!lj_gc2_smr_read_try\(g\)\)+\s+lj_trace_err\(as->J, LJ_TRERR_SMRRETRY\)'; then
  printf '%s\n' 'assembler lookup SMR admission no longer raises SMRRETRY' >&2
  bad=1
fi
if ! printf '%s\n' "$asm_tail" | rg -Uq \
     '!lj_gc2_smr_read_try\(g\)\)+\s*\{[^}]*LJ_TRERR_SMRRETRY'; then
  printf '%s\n' 'x86 tail-link SMR admission no longer raises SMRRETRY' >&2
  bad=1
fi

trace_start=$(sed -n '/^static TraceStartResult trace_start(/,/^}/p' \
  "$ROOT/src/lj_trace.c")
if printf '%s\n' "$trace_start" | rg -q 'lj_trace_flushall_hs'; then
  printf '%s\n' 'trace_start regained a token-held full flush' >&2
  bad=1
fi

terminal=$(sed -n '/^static void trace_terminal_release(/,/^}/p' \
  "$ROOT/src/lj_trace.c")
terminal_order=$(printf '%s\n' "$terminal" | rg -o \
  'setvmstate|lj_trace_state_store|lj_jit_token_release_l|lj_dispatch_update' | \
  tr '\n' ' ')
if [ "$terminal_order" != \
     "setvmstate lj_trace_state_store lj_jit_token_release_l lj_dispatch_update " ]; then
  printf 'unexpected terminal recorder release order: %s\n' \
    "$terminal_order" >&2
  bad=1
fi

if rg -q 'lj_buf_wmem\([^\n]*proto_bc|PROTO_ILOOP.*proto_trace' \
     "$ROOT/src/lj_bcwrite.c"; then
  printf '%s\n' 'bytecode writer regained a racy bulk/gated bytecode copy' >&2
  bad=1
fi

tracek=$(sed -n '/^LJLIB_CF(jit_util_tracek)/,/^}/p' \
  "$ROOT/src/lib_jit.c")
if ! printf '%s\n' "$tracek" | rg -q 'lj_gc2_tv_lease_acquire' || \
   ! printf '%s\n' "$tracek" | rg -q 'lj_state_stack_pubtv' || \
   ! printf '%s\n' "$tracek" | rg -q 'lj_gc2_lease_release'; then
  printf '%s\n' 'jit.util tracek lost its KGC child lifetime handoff' >&2
  bad=1
fi

traceexitstub=$(sed -n '/^LJLIB_CF(jit_util_traceexitstub)/,/^}/p' \
  "$ROOT/src/lib_jit.c")
if printf '%s\n' "$traceexitstub" | rg -q 'MCode \*addr' || \
   ! printf '%s\n' "$traceexitstub" | rg -q 'intptr_t addr'; then
  printf '%s\n' 'jit.util traceexitstub regained a post-lease pointer value' >&2
  bad=1
fi

if [ "$bad" -ne 0 ]; then
  exit 1
fi

remaining=$(rg -l 'lj_gc2_smr_read_enter[[:space:]]*\(' "$ROOT/src" \
  --glob '*.[ch]' | wc -l | tr -d ' ')
printf 'nonblocking JIT SMR reader gate passed; %s source files remain outside this cutover\n' \
  "$remaining"
