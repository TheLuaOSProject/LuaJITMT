# ARM64 open CALLM arity normalization

The CALLM consumer's actual argument vector is the producer's complete open
result vector. Its fixed-formal frame is normalized separately: missing
formals are nil-padded and extra actual values are ignored by a non-vararg
callee. The recorder preflight had incorrectly used `pt->numparams` (the root
function's formal count) as the CALLM argument count, required it to equal
`J->maxslot` at consumer entry, and used the post-return `J->maxslot` as though
it still described input arguments.

`trace_arm64_lua_leaf_frame()` now checks the exact bounded vector using the
live `J->maxslot` only at the consumer's FUNCF/JFUNCF entry. At later
instructions, it validates the CALL/CALLM source pair and minimum frame
geometry without treating normalized `maxslot` as source arity. The returned
call check likewise uses structural CALLM validation because `lj_record_ret()`
has replaced `maxslot` with the caller's result high-water mark. Full vector
length and each result's source/map binding remain independently proved by
semantic and post-RA walks in `lj_asm_arm64_admit.h`; those walks initialize
fixed formals to nil, fill only supplied actual slots, and keep extra actuals
in the bounded vector without treating them as formal values.

The ordinary probe at
`/Users/frityet/Scratch/ljmt-callm-integrated-20261029/parameter-count-probe.lua`
showed all four shapes compiling/executing on x64 and no ARM64 selected root
before this change. The dedicated open-CALLM consumer-exit fixture now covers
the original same-arity 1/2/3 cases and all four cross-arity shapes:

| Produced values | Fixed formals | Case |
|---:|---:|---|
| 1 | 3 | missing `b`, `c` become nil and are unused |
| 2 | 3 | missing `c` becomes nil and is unused |
| 3 | 1 | extra `b`, `c` are ignored |
| 3 | 2 | extra `c` is ignored |

The fixture checks an independent interpreted oracle, a live peer, exact
selected root and consumer-identity exit, the complete vector at the exit,
frame/MULTRES restoration, full-GC survival, and native reentry using identical
mcode bytes and generation. The leaf admission fixture additionally mutates
private prototypes/views: normalized missing and extra formal counts are
accepted independently by semantic and post-RA admission; changing source to
read an unavailable missing/extra slot is rejected in both passes. Existing
CALLM source/vector mutations still exercise missing result-map entries,
incorrect slots, producer-result arity, and bytecode geometry.

Focused evidence:

- `/Users/frityet/Scratch/ljmt-callm-normalization-20261029/open-exits-four-profile.log`
  — all four ARM64/arm64e assertion/release profiles; seven native exit cases
  per profile.
- `/Users/frityet/Scratch/ljmt-callm-normalization-20261029/leaf-four-profile.log`
  — full leaf contract, including all four profiles, seven open-CALLM source
  arities, and independent semantic/post-RA mutation checks.

The slice does not cover a mutable captured consumer whose per-iteration
identity guard is CSE-removed; that is a separate source/IR provenance case.
