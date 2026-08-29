# ARM64 fixed-function native entry (2026-08-27)

## Scope

This tranche opens native `BC_JFUNCF` entry for the exact function-root shape
published by the preceding fixed-function tranche:

```lua
function f(a, b)
  return true
end
```

The admitted contract is the bytecode and trace shape, not the spelling of the
source. It remains restricted to a fixed, non-vararg, three-word prototype with
an immutable original `FUNCF`, `KPRI true`, and `RET1`. The primitive result is
written to and returned from the final fixed-frame slot. False, nil, numeric and
computed returns remain unpublished, as do calls, allocations, side traces,
stitches, vararg functions and multi-frame return paths.

`LJ_ARM64_JIT_JFUNCF_NATIVE_ENTRY_FAIL_CLOSED` is now `0` only in the
experimental macOS ARM64 JIT configuration. The normal ARM64 build remains
fail-closed unless both experimental bootstrap macros are supplied.

## Entry certificate

Function roots cannot reuse the LOOP/FORL topology. `lj_trace_enter_root`
therefore classifies the source and requires a function-specific view:

- the trace is live, is the exact slot named by `JFUNCF.D`, and has no retirement
  epoch;
- `root == 0`, `link == 0`, `linktype == LJ_TRLINK_RETURN`, `mcloop == 0`, and
  `spadjust == 0`;
- `nchild == 0`, `nextside == 0`, and `topslot == framesize`;
- the only admission flag is `TRACE_ARM64_TRUE_FUNCF_ADMITTED` (`0x40`);
- `startpt` is the current Lua function prototype, `startpc` is exactly its
  first bytecode word, and immutable `startins` is the original `FUNCF` with
  `A == framesize` and `D == 0`;
- the live header is `JFUNCF` with the same A and the requested nonzero trace
  number, followed by the exact `KPRI true` and `RET1` words;
- the IR, snapshots and spill-free post-register-allocation view still match the
  literal-true certificate used at publication.

The helper takes two complete metadata views and compares them. It acquire-loads
and rechecks all three live bytecode words during each view and once more after
the final request/admission checks. This makes a concurrent header, constant or
return-word generation change reject through the one cleanup path rather than
entering mcode from a mixed prototype generation.

Assembly-time validation still sees the original FUNCF header. A separate
entry-aware post-RA API accepts only the exact original-FUNCF/live-JFUNCF
relationship. This preserves real prototype PCs in snapshots; copying a patched
header into a temporary prototype would have made snapshot resume addresses
invalid. The exported entry validator repeats the complete pointer/count/range
preflight itself, so a malformed future caller fails closed before any IR or
snapshot dereference even though the current trace-entry caller has already
proved those fields.

## VM stack and transfer contract

The link-zero ARM64 tail emitted for this root always restores the four fixed
spill slots before returning to the interpreter:

```text
add sp, sp, #16
b   lj_vm_exit_interp
```

When the VM label is outside direct-branch range, the terminal branch becomes a
K64 load followed by `br x30` on ordinary ARM64 or `braaz x30` on ARM64e. The
function handler must therefore reserve the matching 16 bytes after both words
of the helper result have been checked and immediately before entering mcode:

```text
cbz trace, reject
cbz target, reject
sub sp, sp, #16
br target                         ordinary ARM64
braa target, trace                ARM64e
```

Rejection does not adjust SP. Guard and XPOLL exits restore the raw C-frame SP
in the existing exit handler, while a normal RETURN balances the explicit
function-entry reserve with the link-zero tail's add.

The focused fixture decodes generated mcode. It requires exactly one fixed
`add sp, #16`, no fixed subtraction inside mcode, and either the exact direct
branch to `lj_vm_exit_interp` or the exact K64-load/indirect terminal sequence.
Normal placement is required to exercise the direct form; `LUAJIT_MCODE_TEST=R`
is required to exercise the far form.

## Pointer authentication

Publication signs the mcode pointer with the function-pointer/IA key and the
exact `GCtrace *`. The entry helper strips the pointer only to compare its raw
identity with `T->mcode`; it returns the original signed bits. Darwin's 16-byte
aggregate result places the exact trace in x0 and the signed target in x1, and
the VM enters with `braa x1, x0`.

This discriminator is intentionally different from the zero discriminator used
for K64 VM labels. A raw target, an IA target signed with zero, or a target signed
with a different trace may have the same stripped address and pass the software
identity check, but must fault at the authenticated VM branch. The ARM64e
supervisor covers those cases separately from its LOOP and FORL probes. All
nine malformed LOOP/FORL/JFUNCF combinations reached a post-validation
readiness marker and terminated with `SIGBUS`; all three corresponding control
children entered and returned normally.

## Native execution evidence

The normal return path does not call `lj_trace_exit`, so successful Lua results
alone would not distinguish native execution from recovery. The fixture instead
pauses the root-entry helper after admission and publishes a profile request
while `TG.jit_base` is live. The terminal `IR_XPOLL` then exits trace 1 at
snapshot 1. Recovery resumes at `RET1`, returns true without re-entry, and
leaves exactly:

- one root-entry publication and no cleanup;
- one trace exit with parent 1 and exit number 1;
- no remaining poll, profile or request bits;
- `in_native == 0`, `jit_base == NULL`, and the original VM state, base, C frame,
  Lua stack top and GC handshake epoch;
- the original trace still runnable.

A second post-admission probe publishes the counted STOPREQ order at the same
boundary. The native XPOLL again exits trace 1 at snapshot 1, the VM returns
`LUA_ERRRUN`, acknowledges the incremented handshake epoch, consumes pending and
poll state, clears the fresh-request bit, restores the C frame and leaves the
admitted function trace runnable. A subsequent ordinary native call succeeds
before the fixture flushes the trace.

Zero-, one- and two-argument calls separately enter the same root after the VM
fills missing fixed parameters. A three-argument call proves that the normal
fixed-function frame ignores an extra actual argument without upsetting the
native return tail. A full flush restores FUNCF, retires slot 1, then permits
the same prototype to republish and enter again in trace slot 1.

## Validation

The focused `arm64_jit_funcf_record_contract.sh` passes on this macOS AArch64
host for ordinary ARM64 and ARM64e/BTI. Both configurations pass repeated direct
placements and forced-far randomized placement. The contract also covers:

- exact publication metadata, IR, snapshots and mcode tail;
- direct helper success and mutations of link topology, admission bits, child
  metadata and allocator suffix spill state;
- concurrent post-metadata mutation of each of the three live bytecode words;
- the post-admission native XPOLL witness and complete state restoration;
- the post-admission counted STOPREQ error unwind and subsequent re-entry;
- flush/header restoration, slot reuse and native re-entry;
- negative false, nil, integer and computed-result functions;
- ordinary `br x1` versus ARM64e `braa x1, x0` disassembly.

The strict root-entry, native FORL/LOOP, scalar-loop, integer-spill, phase-gate,
live-flush/reuse, mcode-retirement, exit and full VM-safepoint contracts all pass
with the new function topology. The full `arm64_jit_fail_closed_gate.sh`
umbrella also passes, including recorder safepoints, the VM safepoint runtime,
and the dynamic-step FORL negative. The full safepoint object checker now
selects the matching no-JIT or experimental DISPATCH layout and, for a JIT VM,
inspects the ordinary I-label bodies behind public hotcount stubs.

The only build diagnostics were the pre-existing unused `szmcode` warning in
`lj_trace.c` and unused `ccall_rawchild_wait` warning in `lj_ccall.c`. These
results validate this exact function root and its interaction with the existing
constrained JIT surfaces; they are not proof of general function compilation.

## Remaining boundary

This is still one deliberately tiny root language. It does not admit general
function bodies, function calls, heap access, side traces, trace stitching, FFI
or allocating IR. Those surfaces require their own exact IR and lifetime
certificates. The next JIT dependency is side-trace exit-table infrastructure;
opening a wider recorder or entry macro before that work would bypass the
current fail-closed boundary. Function-specific generation races and sequential
flush/slot reuse are covered here; concurrent real flush/retirement remains
covered by the shared root lifecycle suites rather than a dedicated JFUNCF race
fixture.
