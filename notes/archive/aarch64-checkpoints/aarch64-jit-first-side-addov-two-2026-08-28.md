# AArch64 JIT exact `ADDOV +2` first side (2026-08-28)

## Scope

The production AArch64 first-side path now admits one additional, independently
observed child grammar: the existing exit-6 descriptor accepts an integer
`ADDOV` addend of either 1 or 2. This is an exact descriptor-owned set, not a
general constant or side-trace widening.

The other two production descriptors remain `+1`-only, and the admission gate
still requires the exact root-linked `IR_GT` grammar, immutable snapshot
geometry, register assignment, head bytes, parent identity, and first-level
topology. `LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED` remains `1`; arbitrary first
sides, side-of-side recording, stitching, calls, heap IR, and JIT-compiled FFI
remain closed.

No persistent `GCtrace` or `jit_State` field, VM assembly, publication rule,
retirement rule, or prototype-size rule changed. `LJArm64SideShape` gained a
two-element `addends` exact set. Singleton rows repeat their first value, so
the existing `+1` cases take the first comparison and retain the same backend
and runtime paths.

## Natural trigger and rejected prediction

The added production source is:

```lua
local function f(n, bias)
  local i = 0
  while i < n do
    i = (i ~= 0 and i or i) + 2
  end
  return i
end
```

The initial prediction that repeating `f(3, 0)` would record the desired side
was wrong and was kept as a negative control. The `n=3` and `n=4` paths leave
the loop through a return-linked `IR_LE` child with snapshot PCs
`{10, 11, 3, 6, 18}`. That is a distinct grammar and remains rejected.

The natural root-linked sequence is:

- establish the childless root with `f(3, 0)`, which returns 4;
- trigger the exact first side with `f(5, 0)`, which returns 6; and
- execute the published child natively with `f(3, 0)`.

The `n=5` through `n=8` variants all produced the same root-linked grammar.
Only the minimal `n=5` case was advanced through register allocation during
the abort-before-publication observation.

## Exact descriptor and observation

The existing exit-6 row now carries addends `{1, 2}`:

| Parent exit | Parent snapshots | Continuation PC | Child snapshot PCs | Parent register | Child SLOAD register | Addends |
| --- | ---: | ---: | --- | --- | --- | --- |
| 6 | 9 | 10 | 10, 11, 3, 17, 7 | x27 | x28 | 1, 2 |

The other descriptor rows carry `{1, 1}`. A `+3` source with the same exit-6
geometry is the production rejection control.

The `+2` tuple was captured in a disposable detached worktree before it was
admitted. Four fresh arm64 processes and two fresh arm64e/BTI processes agreed
on all of the following:

- the child is linked to its parent with `LJ_TRLINK_ROOT` and uses `IR_GT`;
- its constant is exact integer `KINT 2`;
- the five child snapshots have PCs `{10, 11, 3, 17, 7}` and slot counts
  `{5, 6, 6, 5, 5}`;
- the parent map carries the inherited value in x27, the inherited child
  `SLOAD` uses x28, `ADDOV` uses x27, and the limit `SLOAD` uses x28;
- there are no spills, renames, PHIs, or spill-frame growth; and
- the compact body is 82 words.

The arm64 head begins `aa1b03fc 5280005e d5033bbf b9084f3e`, including the
exact x27-to-x28 head move. The arm64e head adds `d503249f` (`BTI J`) before
the same sequence.

Each observation intentionally raised `LJ_TRERR_NYIIR` immediately after the
post-RA capture. It emitted no tail or publication marker, consumed no trace
slot, did not retarget the parent edge, and left the token, owner, SMR, VM
state, certificate, and machine-code frontier quiescent. The disposable
worktree was removed afterward.

## Admission and production proof

The pure fixture proves `+2` at the semantic, pre-head, and post-RA boundaries
for exit 6. It independently rejects:

- exit 2 with `+2`;
- exit 6 with `+3`;
- exit 7 with `+2`;
- exit 6 `+2` with a return link;
- exit 6 `+2` with `IR_LE`; and
- the complete observed return-linked `IR_LE` tuple.

The ordinary production fixture now records four independent root/child pairs:
the three prior `+1` descriptors and exit 6 with `+2`. It verifies the new
child's immutable IR, snapshots, allocator state, head, authenticated edge,
native exit, post-token cleanup, and retirement. Before recording that child,
two `n=3` calls prove two NYIIR aborts, no child or topology change, an
unchanged fallback edge, and only the expected snapshot-count increments.

The `+3` exit-6 source establishes a root with `n=4` and uses `n=7` as its
root-linked rejection trigger. It remains childless. With the decoy and this
unsupported root, the fixture has ten live traces: six roots and four first
sides. Full flush correspondingly proves ten retirement publications. The
ordinary no-helper smoke test independently reaches the same 10/6/4 totals.

Implementation commit `7438ab5a` and its test-wording correction `27608770`
were pushed to `origin/codex/aarch64-macos-port` before the full regression.
An independent patch review found no functional, fail-closed, assertion-count,
or cross-architecture issue; its sole stale three-versus-four comment finding
is the correction in `27608770`.

## Validation checkpoint

Focused validation passed on both arm64 and authenticated arm64e/BTI:

- pure semantic, pre-head, and post-RA admission;
- assembler consumption without publication;
- ordinary no-helper production smoke;
- GC-claim, scoped-flush, and full-flush production runs;
- first-side publication and retirement;
- side-ingress metadata and parent lifetime/authentication; and
- strict LOOP/FORL/JFUNCF root entry.

The complete `tools/ci/arm64_jit_fail_closed_gate.sh` then passed from
`27608770`. It revalidated the full source/runtime contract set, exact
first-side production, arm64e authentication, LOOP/FORL/JFUNCF recording and
native entry, exit-table rejection, machine-code publication and retirement,
live flush/trace-slot reuse, and VM/recorder safepoint races.

Cross-architecture regression also passed:

- thin macOS x86_64 build and binary smoke;
- all 509 stock tests with zero failures;
- a Rosetta x86_64 JIT loop with `jit.os == "OSX"`, `jit.arch == "x64"`, and
  trace 1 linked as a loop; and
- restoration of the ordinary thin arm64 experimental-helper build, followed
  by a native worker spawn/join and a post-threading JIT loop with trace 1
  linked as a loop and the JIT still enabled.

The x86 builder emitted and discarded its expected preliminary non-GC64
configuration diagnostic, then selected the supported GC64 build. Its only
build warning was the existing unused `topofs`; arm64 builds showed only the
existing unused `ccall_rawchild_wait` warning.

## Remaining boundary

The table is still three geometry rows and four exact production pairs, not a
general first-side policy. Return-linked `+2`, broad first sides,
side-of-side recording, stitching, debug/perf registration during side
publication, and JIT FFI remain closed. The next bounded inventory candidate
is the GDBJIT/debug-registration surface; it needs its own observation,
publication, cleanup, and retirement proof before any gate is opened.
