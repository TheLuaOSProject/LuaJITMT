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

## Recorder normalization and boundary

`rec_upvalue()` uses its direct `func_uv_acq()` path only when
`lj_record_mt_runtime_shared()` is false. In the shared/active-MT case it calls
`rec_upvalue_rooted()` and records the rooted helper path instead. The first MT
activation calls `threading_mt_active_prepare_traces()` before setting the
one-way `mt_active` latch; that routine retires existing traces. This is the
transition that separates a pre-MT direct-upvalue trace from later shared
recording.

`rec_upvalue()` now normalizes one narrow no-peer case to the existing rooted
IR path: a closed UGET cell in a root recorder whose current value is the C
fast function `FF_string_sub`. The recorder re-resolves the value through
`lj_func_getuv_rooted_try`; existing exact source-call and function-identity
checks still decide whether the trace may publish. The semantic and post-RA
passes and emitter did not gain raw `IR_UREFC` or `IR_ULOAD` permissions.
Other no-peer upvalues retain the old path, and shared/active-MT recording
continues to use the rooted path as before.

The helper reads the current closed cell on every execution and its existing
guard checks the function identity. A Lua `debug.setupvalue` replacement
therefore observes the new function or exits to the interpreter instead of
using a recorder-time cached pointer. First peer activation also retires the
no-peer root before shared execution; the transition test confirms a new root
works after activation and collection.

This deliberately pays for a rooted helper call at each captured `string.sub`
load in the ARM64 trace. x86-64 keeps its raw upvalue load. The ARM64 helper
path has more runtime overhead; this slice prioritizes a reusable enumerated
root and exact dynamic cell semantics over matching the x86-64 load cost. No
performance comparison was run. The mechanism avoids adding a lock and avoids
teaching ARM64 admission to trust raw UREFC pointers.

Validation on the candidate source:

- `tools/ci/arm64_jit_rooted_upvalue_contract.sh` passed ARM64 and arm64e,
  assertion and release profiles. The contract includes no-peer FORL/LOOP
  string.sub admission clones, which independently accept the baseline and
  reject corrupted helper/source/map/register evidence.
- The no-peer transition script observes a published loop root containing
  `lj_func_getuv_rooted_try` and no UREFC, changes the captured function with
  `debug.setupvalue`, collects garbage, then starts a peer. It verifies the
  old root is retired and a rooted peer trace executes after GC. The helper
  profile reported 40 no-peer and 40 peer selected exits in each run.
- An ordinary ARM64 build without test-helper flags passed the same transition
  script. Its output was `no-peer UREFC normalization: JLOOP exits=40
  peer-exits=40`. It was built with
  `MACOSX_DEPLOYMENT_TARGET=13.0` and
  `-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL`; the
  binary SHA-256 was
  `51f12c4a52c46d77e2afcdb4481ca56933b870ec519c9b2ec61b84b2366c1051`.
  Output is preserved at
  `/Users/frityet/Scratch/ljmt-no-peer-urefc-20261030/ordinary-transition.log`.

The slice is restricted to this closed captured fast-function shape. Other
closed/open upvalues, USET stores, nested recorder contexts and raw UREFC
admission remain outside it. Any future raw UREFC proof must separately bind
the current closure, upvalue index, exact UGET and typed ULOAD to source, and
cover native lifetime across XPOLL with the GC2 phase/root proof; a register or
spill is not itself a root.

The integrated transition test additionally requires a self-linked loop root
with nonempty machine code, verifies code identity across each GC phase, and
binds the currently published no-peer root immediately before peer startup.
This prevents a prior callee-mutation invalidation from making the retirement
assertion pass vacuously. The strengthened test passes on the ordinary
`67da4e268` candidate (40 selected exits in each phase); this preliminary run
is saved under `/Users/frityet/Scratch/ljmt-no-peer-upvalue-integrated-20261030/`.
