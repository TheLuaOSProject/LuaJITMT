# No-peer `string.sub` upvalue refusal

## Ordinary evidence

The candidate is a loop which calls `string.sub(s, -4, -5)` and returns its
empty result. The candidate function is explicitly enabled for JIT recording;
the oracle is JIT-disabled. The no-peer probe runs both FORL and while forms,
checks empty results and cold type/error behavior, collects garbage between
warm execution phases, and records selected-root exits where a root exists.

- ARM64: no selected root; `-jv` repeatedly reports `NYI: cannot assemble IR
  instruction 63`, which is `IR_UREFC`. Log:
  `/Users/frityet/Scratch/ljmt-negative-first-substring-20261030/negative-end-candidate-ordinary/arm64-empty-no-peer.log`.
- x86_64: selected root 1 for both forms, with 476 and 482 selected exits
  respectively after collection. Log:
  `/Users/frityet/Scratch/ljmt-negative-first-substring-20261030/negative-end-candidate-ordinary/x86_64-empty-no-peer.log`.
- The x86_64 IR has `UREFC` from the current function's SLOAD, `ULOAD` of its
  captured function, and an equality guard. The loop-backedge reloads the
  upvalue after XPOLL. The trace folds the empty-result operation away and has
  no STRREF or SNEW.

The first no-peer fallback check in the preceding substring work explicitly
disabled the candidate function and therefore did not measure JIT behavior.
That check is superseded by `empty-no-peer.lua`, which calls `jit.on(f, true)`.
The fixed JIT-enabled fallback probes are preserved separately from the earlier
forced-off logs.

With a live peer and candidate JIT enabled, both ordinary architectures publish
JFORL/JLOOP roots and take selected exits after peer GC. ARM64 selected-exit
counts are 395/402; x86_64 counts are 396/402. ARM64 uses a rooted
`TMPREF -> CALLS -> VLOAD` upvalue path and has no SNEW; x86_64 folds the result
operation away. The texit-counting logs are:

- `/Users/frityet/Scratch/ljmt-negative-first-substring-20261030/negative-end-candidate-ordinary/arm64-empty-live-peer.log`
- `/Users/frityet/Scratch/ljmt-negative-first-substring-20261030/negative-end-candidate-ordinary/x86_64-empty-live-peer.log`

Observer-free live-peer IR logs:

- `/Users/frityet/Scratch/ljmt-negative-first-substring-20261030/negative-end-candidate-ordinary/arm64-empty-peer-noobserver.log`
- `/Users/frityet/Scratch/ljmt-negative-first-substring-20261030/negative-end-candidate-ordinary/x86_64-empty-peer-noobserver.log`

The no-peer JIT-enabled oracle/exit probe is `empty-no-peer.lua`; its per-arch
logs are cited above. The earlier no-peer opcode dump is `empty-ir.lua`, with
`arm64-empty-jv.log` and `x86_64-empty-ir.log`.

## Existing boundary and possible narrow extension

`rec_upvalue()` uses its direct `func_uv_acq()` path only when
`lj_record_mt_runtime_shared()` is false. In the shared/active-MT case it calls
`rec_upvalue_rooted()` and records the rooted helper path instead. The first MT
activation calls `threading_mt_active_prepare_traces()` before setting the
one-way `mt_active` latch; that routine retires existing traces. This is the
transition that separates a pre-MT direct-upvalue trace from later shared
recording.

The ARM64 semantic and post-RA opcode gates currently have no `IR_UREFC` or
`IR_ULOAD` admission case, so the no-peer trace fails closed at `IR_UREFC`.
`asm_uref()` can emit the operation, but backend emission alone does not prove
its source, lifetime, snapshot behavior, or allocator locations.

A safe extension would need a bounded certificate for this exact class of
source operation: the UREFC owner must be the current closure rooted from
SLOAD slot 0, its upvalue index must map to the exact UGET, the ULOAD must be
typechecked and feed the exact `FF_string_sub` equality guard, and no source
write or control-flow edge may bypass that guard. Semantic and post-RA passes
must prove this independently. Any live UREFC value crossing XPOLL must also be
covered by the GC2 phase/root proof; a register or spill is not itself a root.
This note records the measured gap and candidate boundary, not an implementation
or a safety approval for admitting raw UREFC.
