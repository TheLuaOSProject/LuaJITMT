# AArch64 first-side compact-body round trip (2026-08-28)

## Scope

This checkpoint extracts the compact `GCtrace` body construction formerly
embedded in `trace_save()` into a generic private plan/initialize split. It does
not open the AArch64 side-recorder gate and does not publish a child trace.

The extraction is intentionally shared with the ordinary LuaJIT trace path:

- `lj_trace_alloc()` and rollback use one constructor-header helper;
- `trace_save()` uses the same compact geometry plan and private initializer;
- the existing root link, trace-slot, GC publication, GDB and perf suffix stays
  in its original order after recorder-scratch ownership is transferred.

## Contract

`trace_compact_body_plan()` is read-only. It accepts only the exact
`J->curfinal` constructor allocation whose `nins`, `nk`, `nsnap` and
`nsnapmap` still match `J->cur`. It reconstructs and checks the biased compact
IR base, snapshot array, snapshot map and allocation end before returning any
plan.

`trace_compact_body_init()` is finite and non-failing after a successful plan.
It copies the semantic header and snapshot payloads into the private body, but
does not allocate, wait, enter SMR, invoke callbacks, commit mcode, publish a GC
root or trace slot, mutate parent topology, or transfer `J->cur` ownership.
On arm64e it signs the raw mcode pointer with the final `GCtrace *` body as the
discriminator; it never copies the recorder scratch signature.

Rollback restores the exact semantic constructor header with release stores:
all semantic pointers and topology are cleared, compact geometry and post-RA IR
are preserved, native pins and retirement state are zeroed, and the retire-list
link returns to `TRACE_RETIRED_LINK_UNLINKED`. The reserved snapshot/snapmap
tail is deliberately not byte-restored. It was opaque allocator storage before
initialization and becomes opaque again once its header pointers are cleared.

## Real first-side proof

The existing one-shot `LJ_ARM64_SIDE_ASM_TEST` probe now runs the production
compact plan/initializer/reset against the real parent-1/exit-2 side assembler
result before its dry publication seal.

The fixture proves:

- incrementing only the compact body's `nsnapmap` is rejected before any plan
  write, while recorder state, certificate, pending slot and SMR count remain
  unchanged;
- all semantic header fields and snapshot payloads match `J->cur` after init;
- the complete compact post-RA IR byte range is unchanged by init and reset;
- `J->cur`, `J->curfinal`, the exact parent certificate and child `PENDING`
  slot retain ownership through the rollback-capable window;
- reset restores the constructor contract accepted by the existing independent
  dry publication seal;
- arm64e uses the exact body-discriminated signed entry and strips back to the
  raw child mcode;
- ordinary abort still frees the exit table exactly once and leaves no child
  slot, SMR reader, recorder token or parent-topology mutation.

## Validation

- `tools/ci/arm64_jit_side_asm_consumption_contract.sh`
  - two ordinary arm64 executions;
  - two arm64e + BTI executions;
  - restores an ordinary experimental helper build;
- `tools/ci/arm64_jit_exit_contract.sh`
  - ordinary arm64 exit targets and trace retirement;
  - arm64e signed-target negative cases and trace retirement;
  - restores an ordinary experimental helper build.
- `tools/ci/arm64_jit_forl_record_contract.sh`
  - bounded FORL roots publish and execute on arm64 and arm64e;
- `tools/ci/arm64_jit_funcf_record_contract.sh`
  - exact true-FUNCF roots publish and execute on arm64 and arm64e;
- `tools/ci/arm64_jit_gdbjit_prepare_contract.sh`
  - bounded prepare/commit/abort runs twice on arm64 and twice on arm64e;
  - restores an ordinary experimental helper build.

The only compiler diagnostic in these runs is the pre-existing unused
`ccall_rawchild_wait` helper warning.

## Required publication ordering after this checkpoint

The production first-child path must keep all recoverable work before its
`ASM -> PUBLISH` transition. In particular:

1. capture the exact absent TRACE vmevent snapshot;
2. validate the current side body and parent destinations;
3. prepare mcode and optional GDB metadata while `curfinal->traceno == 0`;
4. capture the compact plan and initialize the private body;
5. prepare the sealed GC root and exact vmevent absence reservation;
6. retain the exact SMR view and transition `ASM -> PUBLISH`;
7. publish mcode, GC membership, child slot, topology and snapshot metadata;
8. publish the raw parent exit target last as the runnable linearization point;
9. release the vmevent reservation, certificate, SMR reader and recorder token.

The current dry seal still validates constructor-form `curfinal`, so production
integration must split pre-init child planning from post-init revalidation. It
must not simply call the existing dry-seal helper after compact initialization.
