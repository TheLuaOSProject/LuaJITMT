# ARM64 VM root-entry rejection checkpoint (2026-08-26)

This checkpoint wires the real macOS ARM64 interpreter entry sites to the
strict C root-entry validator without enabling recording or generated-code
execution. It is deliberately a rejection checkpoint: patched bytecode now
exercises the production ABI and recovery paths, while both sides of native
success remain compile-time closed.

## Boundary split

The former aggregate `LJ_ARM64_JIT_FAIL_CLOSED` setting is now represented by
two explicit production predicates:

- `LJ_ARM64_JIT_RECORDER_ADMISSION_FAIL_CLOSED` guards root/side recorder
  admission and defensive recorder ingress.
- `LJ_ARM64_JIT_NATIVE_ENTRY_FAIL_CLOSED` guards interpreter-to-mcode entry and
  native-only stitch continuation handling.

The aggregate name remains only as a conservative out-of-tree compatibility
signal: it is true when either boundary is closed. Every in-tree branch and
fixture names the precise boundary it depends on, including the meaningful
future middle stage where recording is open but native entry is closed. Both
explicit predicates are still true in the experimental desktop ARM64 build. No
recorder, side trace, stitch, trace publication, or machine-code execution
surface is opened here.

The earlier synthetic-exit note described its focused exit contract as also
running a zero-trace recorder smoke. That assertion now lives in this
root-entry contract and the combined fail-closed gate; the exit fixture names
only the native-entry boundary it actually exercises. This keeps the focused
contracts meaningful when recorder admission is opened before native entry.

## VM call and rejection protocol

`BC_JLOOP` and `BC_JFUNCF` now call
`lj_trace_enter_root(J, PC-4, bc_d(INS), L, BASE, sourceop)` using the Darwin
AAPCS64 register contract. The 16-byte result is consumed directly as the exact
`GCtrace *` in x0 and target in x1. `BC_JFUNCF` performs stack growth and fills
all missing fixed arguments before the helper call. `BC_JFUNCV` remains its
existing explicit `BRK`/NYI path because replay-safe vararg-frame admission has
not been ported.

The helper rejects when any of the TG's `poll`, counted `reqmask`, or
`profile_request` words is nonzero. It checks that set before publishing
`jit_base`, immediately after the gate/fence publication handshake, and again
after reacquiring all trace metadata. A deterministic pause at that final
window separately publishes each of the three request words in the runtime
fixture. Thus a pending owner request cannot be hidden by metadata validation.
The normal interpreted fallback reaches the existing opcode poll path and
services the request; this checkpoint does not call a generic safepoint helper
from a patched-PC state.

Every helper rejection acquire-reloads the current 32-bit bytecode. A still
patched instruction uses `lj_trace_stale_startins()` and its retry sentinel;
an already changed word is dispatched from the acquired value. Recovered words
use the static dispatch table so rejection does not immediately re-enter a
dynamic recorder path. A retry acquire-reloads and reclassifies the opcode
before another helper call: if a peer restored the original instruction in the
retry window, the VM dispatches that exact acquired word instead.

`BC_JFORI`, `BC_JFORL`, `BC_JITERL`, and `BC_JLOOP` share the existing JLOOP
handler tail, but only a direct `BC_JLOOP` is a root-helper admission site. The
numeric/iterator handlers have already tested their condition and, for JFORL,
incremented the loop index before reaching that tail. They therefore recover
the original FORL/ITERL only to decode its branch target, poll, and continue;
they must never statically execute the recovered instruction. Doing so would
double-increment JFORL and skip the first JFORI body. JITERL uses the same
branch-only rule after saving its control variable.

The fixed-function case has one important ABI distinction: x28 still carries
the actual argument byte count across both C calls. JFUNCF.D is the trace
number, not the call argument count, so its rejection dispatcher deliberately
does not decode RD back into x28. Replaying the recovered FUNCF header is then
idempotent and preserves already-filled nil arguments.

## Native and PAUTH closure

After complete metadata validation, the C helper is forced through its single
release-clear rejection label while
`LJ_ARM64_JIT_NATIVE_ENTRY_FAIL_CLOSED` is true. The VM independently rejects a
synthetic non-null result: it release-clears TG `jit_base` before recovery.
This makes a test helper or future partial change unable to leak a trace lease
or execute mcode accidentally.

The future success instruction is nevertheless present for object review. On
arm64e it is `BRAA x1, x0`, authenticating the returned target with the exact
returned `GCtrace *` modifier. It is placed behind an unconditional
native-entry-disabled branch and cannot execute in this build. `BRAAZ` would
use the wrong modifier and is rejected by the contract.

## Validation

`tests/t-arm64-jit-root-entry.c` now covers:

- direct pre-publication rejection for each pending request word;
- a deterministic request published after `jit_base` publication;
- deterministic poll, reqmask, and profile publications after complete trace
  metadata reacquisition;
- the existing two gate-close orderings;
- an actual patched while-loop executing `BC_JLOOP`, including a forced
  stale-startins retry where the bytecode is restored inside the retry window
  and the helper call count proves immediate static redispatch;
- an actual patched fixed-argument function executing `BC_JFUNCF` with two
  missing arguments, proving nil fill and x28 argument preservation;
- an actual patched generic iterator executing `BC_JITERL`, proving the shared
  JLOOP tail bypasses root admission and still recovers correctly; and
- separate actual `BC_JFORL` and paired `BC_JFORI`/`BC_JFORL` executions whose
  exact integer and floating-path `1234` results detect both double increments
  and a skipped first body. The floating JFORI path now publishes its FORL PC
  before loading the trace number, matching the integer and x64 edge contract.

All five patched VM executions produce correct Lua results and leave
`tg->jit_base` clear. The synthetic final-window race temporarily installs a
fully validated test metadata view, then restores the originally absent
`TraceVec`. The two strict-root cases increment matching publish/cleanup
counters; JITERL and both numeric tails prove zero root-helper publications.

`tools/ci/arm64_jit_root_entry_contract.sh` verifies the source ordering, both
VM BR26 relocations and exact argument shapes, acquire bytecode reloads,
retry reclassification, branch-only FORL/ITERL recovery, synthetic-success
lease clearing, JFUNCV's retained trap, precise boundary predicates, and
absence of an x28 decode in the JFUNCF fallback. It runs the native runtime
fixture, rebuilds and runs it as arm64e/BTI, verifies exactly two
`BRAA x1, x0` instructions, then restores the ordinary native experimental
build.

Validated on this macOS ARM64 host:

- native ARM64 clean experimental build and runtime fixture;
- arm64e plus BTI clean build, Werror fixture compile, runtime fixture, and VM
  disassembly contract; and
- the full ARM64 bootstrap gate (387 stock tests plus threading, hooks,
  coroutines, and FFI callbacks), recorder fail-closed smoke execution with
  zero published traces, and an x86_64 assertion build under Rosetta (509 stock
  tests) to guard the untouched x64 path.

This is not end-to-end JIT enablement. No successful `lj_trace_enter_root`
result, generated trace body, side/stitch entry, exit restore, or real mcode
execution is exercised by this checkpoint.
