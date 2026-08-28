# AArch64 first-side parent lifetime/authentication certificate (2026-08-27)

## Scope

This tranche closed the representation and validation gap identified after the
first-side semantic, post-RA and head-shuffle certificates. It added a
token-private identity for the exact published LOOP parent borrowed by the
first-side assembler. The subsequent assembler-consumption checkpoint now
captures and revalidates it inside `lj_asm_trace()`. Recorder ingress,
`trace_stop()` publication, parent-exit retargeting and retirement remain
closed.

`LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED` remains `1`. Assembler consumption is
not side-recording admission.

## Stored identity

The publication-seal checkpoint extended `jit_State.arm64_side_parent` to
exactly eight fields in a 48-byte payload:

- `tracev`: the exact source/destination trace-vector generation;
- `body`: the exact published `GCtrace *` allocation;
- `mcode`: the raw parent machine-code identity used by a future linked tail;
- `continuation`: the selected immutable parent snapshot PC;
- `continuationins`: the exact bytecode word captured at that PC;
- `parent`: the public trace number whose slot must still name `body`; and
- `exitno`: the selected parent exit; and
- `child`: the reserved child number whose slot must still equal
  `LJ_TRACE_PENDING`.

`body == NULL` remains the sole empty representation. Pinning `tracev` is now
required because publication will consume a direct pending child-slot pointer:
even a byte-identical vector replacement is an ABA generation change and is
rejected. The certificate still does not store a redundant PAUTH target. On
arm64e, the current published `GCtrace.mcauth` is derived and checked again from
`lj_ptr_sign(mcode, body)` on every stable view.

The payload is non-atomic because it is private to the exact recorder-token
owner. Capture first requires the token word to equal the ambient TG's logical
`tid` and independently requires the current physical actor to equal that TG's
`actor_id`, before it dereferences published `J->L`. It then requires the same
live `lua_State`, TG actor/state ownership, active `LJ_TRACE_ASM`, empty
hook/request/trace-stream gates, and the canonical first-side scratch tuple:

- nonzero distinct child and parent numbers;
- `J->parent/J->exitno` equal to the selected parent exit;
- child `root == link == parent` with `LJ_TRLINK_ROOT`;
- exact current function/prototype, zero frame/return depth and base slot
  `1+LJ_FR2`; and
- child start PC equal to the stored continuation with synthetic
  `BCINS_AD(BC_JMP, 0, 0)`.

The exact owner/state predicate is repeated around the metadata work because an
asynchronous abort may clear the recorder's ACTIVE bit without taking the JIT
token.

## Bounded capture and revalidation

Both operations use only `lj_gc2_smr_read_try()`. They never wait, allocate,
raise, invoke callbacks, acquire or release the JIT token, change a snapshot
count, mutate parent topology, retarget an exit, patch bytecode or publish an
admission bit. Their distinct results are:

- `OK`: the exact identity and owner generation were proved;
- `RETRY`: the owner, scratch, slot, body, metadata, continuation, mcode or
  authentication identity was stale; and
- `SMR_RETRY`: the one-shot trace-body SMR admission was closed.

An authorized capture clears the embedded destination before starting. It
builds a local eight-field value and copies that value to `jit_State` only after
the double-captured parent view, bytecode generation, PAUTH identity, exact
pending child slot and final ASM-owner check all succeed. Thus a failed capture
is empty rather than a partially updated or replayable predecessor. An
unauthorized call cannot clear another actor's token-private value.
Revalidation copies the existing private value, admits one SMR reader, requires
the same vector generation, parent body, mcode, continuation bytecode and
pending child slot, and leaves the stored value unchanged.

The stable parent view now acquires the signed `mcauth` bits along with the raw
mcode and includes those bits in its double-capture equality. On PAUTH builds it
requires exact equality with a fresh `lj_ptr_sign(mcode, body)`. NULL, unsigned
raw, zero/wrong-discriminator and otherwise stale signed values cannot certify
the parent even when stripping produces the same raw mcode address.

## Lifecycle boundary

Initialization explicitly clears the embedded identity, and every fresh
`trace_start()` clears after asserting recorder-token ownership and before
parent/root selection. `trace_abort()` clears before mcode or scratch teardown,
so an owner-local `LJ_TRERR_MCODELM` restart begins empty; the detach-only
`lj_trace_abort_owner()` path likewise clears after exact token ownership and
before aborting or destroying selectors/scratch. Every down-recursion outcome
clears before either its terminal early return or repurposing
`J->parent/J->exitno` for a root. Generic terminal release asserts that the
exact transaction already cleared it and defensively zeros the payload before
IDLE/token handoff; VM terminal preflight also requires it empty.

The assembler-consumption checkpoint now:

1. capture afresh for every `lj_asm_trace()` attempt;
2. recapture after the existing abort clear on an owner-local
   `LJ_TRERR_MCODELM` restart;
3. revalidate before parent-map/head consumption, linked-tail finalization and
   after the last fallible snapshot fixup; and
4. consume only the certified parent body, exit and raw mcode target.

The selected parent snapshot certificate additionally proves that canonical
slot 4 exists before the assertion-only `lj_snap_regspmap()` search, keeping
that release-build scan bounded.

The later dry publication-seal checkpoint retains this exact source and
destination certificate through an atomic `ASM -> PUBLISH` state transition.
It still does not publish a child. The remaining transaction must consume the
captured raw authenticated parent fallback value through the last parent-exit
CAS, then clear the successful attempt before generic terminal release. This
checkpoint does not claim side-child/root retirement ordering or enterable
side code.

## Synthetic mutation contract

`tests/t-arm64-jit-side-ingress-metadata.c` now constructs the exact synthetic
LOOP parent and exact ASM-owner scratch, captures the embedded identity and
revalidates it. It mutates every stored field and independently exercises:

- same-slot removal and replacement by an otherwise identical body allocation;
- byte-identical TraceVec replacement as an invalid ABA generation change;
- pending child-slot removal and replacement;
- parent mcode and selected snapshot-footer generations;
- same-address continuation bytecode generation changes;
- missing, raw/unsigned and wrong-discriminator `mcauth` values on arm64e;
- owner, token and asynchronously aborted ASM-state rejection;
- closed-SMR `SMR_RETRY` with no reader leak;
- successful-capture all-or-nothing publication; and
- explicit empty-state cleanup.

`tools/ci/arm64_jit_side_ingress_metadata_contract.sh` separates the original
read-only ingress audit from the new bounded-SMR region, checks the exact
eight-field schema, PAUTH derivation, two one-shot admission/leave pairs, absence
of blocking/mutation surfaces, the init/start/downrec/abort/terminal cleanup
ordering, shutdown empty-state preflight, logical-TG-token/physical-actor split,
and the exact assembler capture/revalidation call set and ordering. It runs the fixture
natively on ARM64 and compiles both the helper and fixture with
warnings-as-errors for arm64e. The existing full arm64e root-entry contract
rebuilds the archive and executes this same fixture with `LJ_ABI_PAUTH=1`;
direct linkage against the ordinary ARM64 archive cannot execute an arm64e
object.

Local validation for this tranche:

- fresh experimental ARM64 build;
- native ARM64 mutation fixture with `-Wall -Wextra -Werror`; and
- `tools/ci/arm64_jit_side_ingress_metadata_contract.sh`, including arm64e
  warnings-as-errors compilation.
