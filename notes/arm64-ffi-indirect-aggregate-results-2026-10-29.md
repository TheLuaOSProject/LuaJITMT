# ARM64 Darwin indirect aggregate results

This slice adds one ARM64 Darwin FFI result class: ordinary non-union numeric
structs of 17–32 bytes with alignment at most 8, whose bounded CType walk finds
at least one integer or enum leaf. A positive integer leaf is required because
failure of the existing HFA classifier alone does not prove the type is
non-HFA. Over-aligned/ambiguous HFA layouts, unions, references, vectors,
variable-length members, unsupported leaves, varargs, and larger aggregates
remain interpreted. Existing direct HFA3/HFA4 and small direct aggregate
results retain their prior classification.

The recorder allocates and roots the declared CData result first, forms the
hidden result address from that exact CNEW payload, and records it as the first
hidden CALLXS argument. The CALLXS result is NIL; the returned value is the
rooted CData box. Semantic and post-RA admission independently bind the CNEW
type/size, payload offset, hidden first argument, CALLXS signature, snapshot
result, and absence of a post-call store. The aggregate is filled by the callee
through X8; visible arguments keep their original X0–X7 and stack positions.

The emitter reserves X8 from visible GPR argument allocation, including stack
argument temporaries, and checks already allocated visible values before setup.
The indirect target allocation excludes X8 as well; because `ra_alloc1()` may
retain an existing allocation outside its allowed set, emission explicitly
refuses an indirect target found in X8. The hidden pointer is moved into X8
before the visible argument moves execute in machine order. A native test uses
eight visible GPR arguments plus two stack arguments, alternate callees,
selected-root entry and finish evidence, full GC/re-entry, and interpreter
result/effect comparisons.

Private cloned-trace tests first admit the unmodified semantic and post-RA
proofs, then independently reject wrong result snapshot refs, CNEW CType,
payload base/offset, removed or reordered hidden argument, wrong CALLXS result
type, wrong post-RA payload class, and an X8 alias for a visible GPR argument.

Validation: `tools/ci/arm64_jit_integer_aggregate_args_contract.sh` passed all
four ARM64/arm64e assert and release profiles after the final fixture changes;
full log: `/Users/frityet/Scratch/sret-profile-tmp/final-four-profile7.log`.
