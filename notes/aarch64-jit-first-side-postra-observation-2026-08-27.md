# AArch64 first-side live post-RA observation (2026-08-27)

## Scope and safety boundary

This checkpoint replaces the synthetic first-side post-register-allocation
layout with a native Apple Silicon observation. It does **not** open the
production side recorder, publish a child trace, patch a parent exit, or make a
side trace enterable. `LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED` remains `1` in
the real tree and `TRACE_ARM64_INT_SIDE_ADMITTED` still has no writer.

The observation ran from commit `0d607af9` in a detached disposable worktree.
A test-only overlay admitted only the already-certified parent 1 / exit 2
grammar, completed register allocation, emitted a bounded diagnostic record,
and synchronously raised `LJ_TRERR_NYIIR` immediately after the final allocator
free-set check.
The overlay was removed with the disposable worktree; no observation macro or
runtime bypass remains in the branch.

The unconditional abort was before the production ARM64 post-RA marker block,
`T->mcode`, tail fixup, final snapshot fixup, code synchronization, return to
`trace_stop()`, `lj_mcode_commit()`, `trace_save()`, root topology updates,
the `trace_stop()` parent-snapshot `SNAPCOUNT_DONE` publication, and
authenticated parent-exit retargeting. The private child's terminal snapshot
was already marked done before assembly; it was discarded with the aborted
recorder and was never parent/child publication.

## Canonical trigger

The observed Lua function was:

```lua
local function f(n, bias)
  local i = 0
  while i < n do
    i = i + 1
    if bias ~= 0 then
      i = i + 1
    end
  end
  return i
end
```

It has 19 bytecodes, two parameters, frame size 5, and the certified integer
`BC_LOOP` root. A bias-zero call published root 1. After the one-shot observer
was armed, `f(3, 1)` selected parent exit 2 and the offset-13 `CGET`
continuation, recorded child 2, reached completed allocation, and aborted.

## Exact live allocator result

Four fresh native ARM64 processes produced the same normalized record. The
only differences were ASLR-dependent addresses and packed snapshot footer
addresses.

Trace and allocator header:

- child 2, parent/root/link 1, exit 2;
- `startins = BCINS_AD(BC_JMP, 0, 0)`, root link type, zero sink tags;
- base slot 2 and packed base delta 0;
- semantic `nins = REF_BASE+7`, final `nins = REF_BASE+8`;
- `nk = REF_TRUE-1`, four snapshots, 13 snapshot-map words;
- child and parent top slot 5, child and parent stack adjustment 0;
- `stopins = REF_BASE+1`, `orignins = REF_BASE+7`;
- one trailing `IR_NOP`; no `IR_RENAME`, spills, PHIs, or spill-frame growth;
- `evenspill = SPS_FIRST`, `oddspill = 0`, and the final free set equals
  `RSET_ALL`.

Exact post-RA register and spill bytes:

| IR | register byte | spill byte |
|---|---:|---:|
| `KINT +1`, true, false, nil | `RID_INIT` | `SPS_NONE` |
| `BASE` | `RID_BASE` / x19 | `SPS_NONE` |
| inherited parent slot-4 `SLOAD` | x27 | `SPS_NONE` |
| guarded slot-5 `SLOAD` | x28 | `SPS_NONE` |
| guarded `ADDOV` | x28 | `SPS_NONE` |
| guarded slot-2 `SLOAD` | x27 | `SPS_NONE` |
| guarded `GT` | `RID_INIT` | `SPS_NONE` |
| terminal `XPOLL` | `RID_INIT` | `SPS_NONE` |
| trailing `NOP` | zero | zero |

`IRIns.prev` is a pre-RA chain field which aliases these two post-RA bytes; it
is not an additional allocator property. The trailing `NOP` therefore has a
zeroed allocation word rather than an allocated x0 register.

The parent snapshot register map for inherited slot 4 is exactly unspilled
x28. This is the one material correction to the synthetic certificate: the
child does not retain that value in the same register. `asm_head_side()` emits
the required x28-to-x27 shuffle, while the child uses x28 for the live value and
`ADDOV` result and x27 for the inherited value and loop limit.

These fields prove the allocator-facing source and target and force the current
`asm_head_side()` algorithm onto its x28-to-x27 shuffle path. The pure post-RA
helper does not by itself certify the live parent-map provenance or the emitted
move; both remain production gates.

The four temporary snapshot machine-code offsets were 65, 61, 59, and 48
words. They are assembler products, not semantic identity, and remain outside
the immutable grammar. The private side body occupied 77 words at the
observation seam. Its would-be linked tail was aligned and directly encodable;
the pure B26 encoder returned host-order `0x1400002e`, targeting the already
published parent 46 instructions ahead. This is diagnostic evidence only: tail
fixup was deliberately unreachable.

## Abort and no-runnable-child proof

A C fixture armed the observer only after validating root 1, then asserted:

- exactly one post-RA observation and one `LJ_TRERR_NYIIR` abort;
- one private child exit-table allocation and one free, both five slots;
- trace slot 2 empty, `J->curfinal == NULL`, recorder state IDLE, token zero,
  owner null, and `J->mctop` unchanged;
- root 1 still runnable with unchanged mcode, zero children, and no next side;
- parent snapshot 2 advanced only through the ordinary hot-exit claim from 0
  to 1, never `SNAPCOUNT_DONE`;
- parent exit 2 still decoded to its original shared fallback; and
- a subsequent bias-zero call continued to execute root 1 correctly.

The observation sources and binary were deleted with the disposable worktree.
The overlay and fixture also compiled cleanly for arm64e with branch protection;
native arm64 execution is the behavioral authority.

Cleanup published the compact scratch only as an explicitly unpublished,
nonsemantic retire-list node covered by the retirement epoch. It never
published trace slot 2, committed machine code, or made the child runnable.

## Production certificate update

`lj_asm_arm64_side_postra_admit()` now encodes the repeated live layout above,
including the exact x28 parent map and x27 child inheritance target. Its pure
fixture mutates every register byte, spill byte, suffix, top-slot, adjustment,
and parent-map field. The helper remains dormant: production `lj_asm_trace()`
does not call it, no side admission bit is set, and the side recorder stays
closed. Parent-map provenance and the actual x28-to-x27 head move still require
their own exact certificate before this helper can participate in production.

The next production tranche still needs an exact parent lifetime/generation
certificate through assembly, acquire-loaded parent mcode identity, linked-tail
target revalidation, authenticated last publication of the parent exit slot,
and child/root retirement ordering before side recording can open.
