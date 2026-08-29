# ARM64 fixed-function trace publication (2026-08-27)

## Scope

This tranche permits publication of the first fixed-function ARM64 root while
deliberately keeping native `BC_JFUNCF` entry closed. The admitted bytecode is
the pure three-word family represented by:

```lua
function f(a, b)
  return true
end
```

The exact certificate is more important than the source spelling: immutable
`FUNCF`, `KPRI true`, `RET1`; the primitive is written to and returned from the
last fixed-frame slot. Fixed functions returning false, nil, a number, or a
computed result remain interpreted. Vararg functions, calls, heap operations,
side traces, stitches and multi-frame returns are not admitted.

`LJ_ARM64_JIT_FUNCF_RECORDER_FAIL_CLOSED` is independently open in the
experimental macOS ARM64 build. `LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED`
remains `1`, so this milestone proves recording and immutable publication only.

## Recorder and generation proof

Recorder ingress recognizes `BC_FUNCF` separately from LOOP/FORL. At initial
capture and on START/RECORD preflight it requires:

- bytecode position zero and an unpatched full `FUNCF` word;
- exactly three bytecode words and a non-vararg, non-empty fixed frame;
- `FUNCF.A == framesize` and `FUNCF.D == 0`;
- `KPRI true` in `framesize - 1` followed by `RET1` of the same slot;
- enough non-result frame slots for all fixed parameters;
- acquire rereads of the complete three-word generation.

The assembler independently repeats the prototype/bytecode proof before mcode
reservation. A trace must be a root with no parent or exit, `link == 0`,
`linktype == LJ_TRLINK_RETURN`, and no loop reference. This does not weaken the
self-link/LOOP topology required by existing LOOP and FORL roots.

## Semantic and allocator certificate

The semantic IR is exact:

1. canonical primitive constants only, with `nk == REF_TRUE`;
2. `IR_BASE`;
3. the recorder's snapshot-separator `IR_NOP`;
4. terminal guarded `IR_XPOLL` with ARM64 poll operand `1`.

The post-register-allocation view adds exactly one allocator suffix `IR_NOP`.
There are no values requiring a register or spill, `spadjust == 0`, and
`mcloop == 0`.

The two snapshots and five map entries are also exact. The first snapshot is at
the separator and resumes at the `KPRI`; the second is at XPOLL and resumes at
`RET1`. Its sole restored value is `REF_TRUE` in the result slot. Both encoded
PC/base records must name those exact positions in the same three-word
prototype. Publication sets only `TRACE_ARM64_TRUE_FUNCF_ADMITTED` (`0x40`),
not either integer loop certificate.

## Closed native-entry behavior

Generic trace publication patches the header to `JFUNCF` and preserves the
original `FUNCF` in the immutable prototype sidecar. Calls then reach the ARM64
header entry helper, but the independent native-entry policy rejects `JFUNCF`
before any `TG.jit_base` publication. The VM reloads the header, resolves the
original `FUNCF` through the sidecar and redispatches the static interpreter
handler.

The ordinary ARM64 and ARM64e disassemblies are pinned to this boundary: they
clear any impossible successful lease, contain no raw or authenticated branch
to function-root mcode, and do not reserve the 16-byte native spill frame.
Missing fixed arguments are filled before the admission attempt and calls with
zero, one and two supplied arguments all return correctly through recovery.

## Validation

The focused `arm64_jit_funcf_record_contract.sh` passed ordinary ARM64 and
ARM64e/BTI builds, repeated native runs, randomized mcode placement, fixture
compilation with warnings as errors, and VM disassembly checks. It covers:

- exact bytecode, topology, IR, snapshots, admission flag and mcode metadata;
- pre-`jit_base` rejection with zero publication/cleanup/exit counters;
- missing-argument calls and exact stale-sidecar recovery deltas;
- full flush, original-header restoration, hotcount regeneration, trace-slot 1
  reuse and a second publication of the same immutable prototype;
- false, nil, integer and computed-result functions remaining unpublished;
- ordinary ARM64 and ARM64e restoration to the normal experimental build.

The existing IR admission and FORL recorder contracts pass after the new
certificate selector was added. The complete ARM64 umbrella also passes. The
post-change lifecycle sweep revalidated strict root entry, scalar arithmetic,
bounded integer spills, IDLE/MARK/SWEEP phase gates, live flush and trace-slot
reuse, retired-mcode leases, ordinary/ARM64e exit recovery, and malformed
ARM64e trace signatures. The only compiler diagnostics were the pre-existing
unused `szmcode` and `ccall_rawchild_wait` warnings.

## Next boundary

Opening `JFUNCF` entry is a separate change. The stock ARM64 path previously
reached function mcode through the JLOOP funnel; the dedicated lockless handler
must therefore execute `sub sp, sp, #16` after successful admission and before
the raw/authenticated branch. It also needs a function-specific two-view entry
certificate for RETURN/zero-link/zero-mcloop topology and the `0x40` admission
bit. The publication-only split prevents an incomplete macro flip from making
the currently generated function trace executable.
