# AArch64 JIT third exact first-side descriptor (2026-08-28)

## Scope

The production AArch64 first-side path now admits a third independently
observed first-level side generation. The broad side recorder remains closed:
`LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED` is still `1`, so this does not admit
arbitrary first sides, side-of-side recording, stitching, calls, heap IR, or
JIT-compiled FFI.

The new row reuses the existing descriptor schema and the already certified
19-bytecode, five-slot, resumed-`CGET` child grammar. No persistent
`GCtrace`/`jit_State` field, VM assembly, prototype-size rule, or relational
admission rule was added.

## Natural trigger and exact row

The third source function is:

```lua
local function f(n, bias)
  local i = 0
  while i < n do
    if i == 0 then i = i + 1 end
    i = i + 1
  end
  return i
end
```

It has 19 bytecodes, two parameters, frame size 5, and an eleven-snapshot
integer `BC_LOOP` root. The production sequence records the childless root
with `f(3, 0)`, records the desired side with `f(4, 0)`, and executes the
published child natively with `f(3, 0)`.

The exact descriptor added to `lj_asm_arm64_side_shape()` is:

| Parent exit | Parent snapshots | Continuation PC | Child snapshot PCs | Parent register | Child SLOAD register | Head move |
| --- | ---: | ---: | --- | --- | --- | --- |
| 7 | 11 | 13 | 13, 14, 3, 17, 7 | x28 | x27 | x28 to x27 |

The child retains the existing semantic certificate: inherited integer slot
4, resumed `CGET` represented by `IR_NOP`, guarded `ADDOV +1`, guarded integer
slot-2 limit load, `GT`, and terminal `XPOLL`. It has `nins = REF_BASE+7`
before the trailing allocator `NOP`, `nk = REF_TRUE-1`, five snapshots, and
seventeen snapshot-map words.

## Abort-before-publication observation

The tuple was captured in a disposable detached worktree with the same
three-stage observation boundary used for the earlier descriptors. The
observer selected only parent exit 7, dumped the semantic child before
allocation, captured the post-register-allocation state, emitted and checked
the child head, then raised `LJ_TRERR_NYIIR` before the tail, `trace_stop()`,
machine-code commit, trace-slot publication, or parent-exit retargeting.

Four native arm64 runs produced the same allocator result:

- the parent snapshot carries slot 4 in unspilled x28;
- the child inherited `SLOAD` and limit `SLOAD` use unspilled x27;
- the child `ADDOV` uses x28;
- `BASE` uses x19; constants, `CGET`/`NOP`, `GT`, and `XPOLL` use
  `RID_INIT`; and
- there are no spills, renames, PHIs, or spill-frame growth.

The emitted head word was identical in all four runs:
`0xaa1c03fb`, the exact `MOV x27, x28`. The private arm64 body was 81 words.
An arm64e/BTI observation also passed: `BTI J` preceded the same move and the
private body was 83 words.

Every observation ended with one intentional NYI abort, no child trace slot,
the parent count changing only from 0 to 1, unchanged parent mcode/exit edge
and topology, and idle JIT-token/owner/SMR state. The disposable worktree was
removed after the captures.

Using `f(3, 0)` for the side trigger was deliberately rejected: it terminates
through `RET`, yielding a different link type and footer PCs. `f(4, 0)` reaches
the published parent loop and produces the exact root-linked grammar above.

## Production and regression proof

The pure fixture now admits all three exact rows, rejects exit 8, and checks
the exit-6/exit-7 geometry/register cross-products. The metadata fixture grows
only its synthetic parent capacity to eleven snapshots and proves that exit 7
is accepted in metadata, idle, claim, and owner contexts only with its coupled
snapshot count and continuation.

The ordinary production fixture now records three distinct root/child pairs.
It verifies the third row's immutable geometry, post-RA registers, emitted
head, authenticated edge representation, native child exit, post-token abort
cleanup, and GC-claim/scoped/full-flush retirement. With the decoy and
unsupported root, this reaches eight live traces. Root recording therefore
uses a bounded 64-call root-discovery budget because the final unsupported root
(the fifth root and eighth live trace) was not reliably published within the
previous four-call budget. The no-helper smoke similarly repeats the
unsupported trigger 64 times. These are fixture warm-up/observation bounds,
not a claim about an internal retry mechanism, and they do not relax admission
or publication policy.

Focused validation passed:

- semantic/pre-head/post-RA and assembler-consumption contracts on arm64 and
  arm64e;
- metadata ingress execution plus lifetime/authentication checks on arm64,
  arm64e compile coverage, and authenticated arm64e ingress execution through
  the root-entry contract;
- ordinary no-helper production smoke;
- two runs each of GC claim, scoped flush, and full flush for all three pairs
  on arm64 and arm64e;
- the unchanged exit-2 one-shot publication/retirement infrastructure
  regression check on arm64 and arm64e; and
- strict LOOP/FORL/JFUNCF root entry plus metadata ingress on arm64 and
  authenticated arm64e.

The only focused-build diagnostic was the existing unused
`ccall_rawchild_wait` warning.

The complete `tools/ci/arm64_jit_fail_closed_gate.sh` umbrella then passed
from commit `787f5f64`. In addition to the three-side production contract, it
covered every source/runtime contract enumerated by that umbrella, arm64 and
arm64e/BTI root entry and exit authentication, LOOP/FORL/JFUNCF recording and
native entry, mcode publication/retirement, live flush and trace-slot reuse,
and VM and recorder safepoint races.

Cross-architecture regression on the same commit also passed:

- the thin macOS x86_64 platform build and binary smoke test;
- all 509 stock tests with zero failures;
- a Rosetta x86_64 JIT loop that published trace 1 with `linktype=loop`; and
- restoration of the thin arm64 experimental-helper build, followed by a
  native health check that joined a lockless worker and then published and
  completed a loop with trace 1 published and the JIT enabled after
  multithreading activation.

The restored archive exports the ordinary trace-test helpers but not the
first-side observation seam. The x86 build showed only the existing unused
`topofs` warning; the arm64 builds showed only the existing unused
`ccall_rawchild_wait` warning. The x86 platform builder also emitted and
discarded its expected preliminary non-GC64 configuration diagnostic before
selecting the supported GC64 build.

## Remaining boundary

The three-row table is still an exact whitelist, not a general first-side
policy. Broad first sides, side-of-side recording, stitching, debug/perf
registration during side publication, and JIT FFI remain closed. The next
bounded milestone can now be selected from those gates with this complete
ARM64 and x86 regression checkpoint as its baseline.
