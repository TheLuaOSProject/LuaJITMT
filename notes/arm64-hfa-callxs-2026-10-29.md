# ARM64 two-float HFA CALLXS arguments

This checkpoint adds one Darwin ARM64 fixed-argument HFA shape: a direct
struct of exactly two `float` fields at offsets 0 and 4, size 8, alignment 4.
Both members occupy successive SIMD argument registers. The ABI signature keeps
Lua formal-parameter count separate from flattened physical carrier count, and
each carrier records its source parameter and HFA member index.

The recorder loads each member from the exact CData payload offset. The CData
TValue stays rooted in the Lua call frame. Semantic and post-register-allocation
admission independently bind each `IRT_FLOAT` XLOAD to that source, its guarded
CType ID, the expected field offset and HFA member, the corresponding CALLXS
carrier, and a valid FPR allocation. The CALLXS-specific fixture mutates type,
address source, CType guard, field offset, CALLXS binding, physical register,
spill, raw-XLOAD snapshot publication, and post-RA type; both admission passes
reject their applicable malformed views. The standalone ABI probe also rejects
the HFA in a variadic function signature. Before unwrapping a source CData type,
the recorder retains and checks the source CType's reference flag. A live
reference-typed HFA candidate is checked against its interpreter oracle and
asserted to publish no HFA root; the direct by-value candidate remains a
separate positive.

The permanent Lua fixture keeps a live MT peer attached, requires a published
HFA root, correlates native finish events to that exact candidate proto, checks
an exit from that root, and checks native re-entry plus another root exit after
full GC. Its HFA C effect count is checked before and after collection. An
initial counter bug placed the HFA finish check inside the integer-candidate
predicate, making the HFA witness impossible; the predicates are now separate.
That was a test attribution correction, not a runtime lifecycle change.

## Limits

- Only the direct two-`float` struct is classified. Nested structs, arrays,
  other HFA sizes, padding, unions, vectors, and HFA results remain closed.
- HFA formals in variadic function signatures are rejected. This does not open
  HFA variadic tails, stack-passed HFA values, or stack crossing.
- The positive trace tests the HFA in the first named parameter, and another
  positive tests a following `double` parameter in the next SIMD register.
- This is an isolated branch checkpoint and is not integrated into the root
  parity branch yet.

## Validation

`tools/ci/arm64_jit_integer_aggregate_args_contract.sh` passed all four ARM64
profiles: arm64 and arm64e, each with assertions and without assertions. This
contract includes semantic/post-RA malformed HFA views, candidate-specific
native finish/exit evidence, live peer coverage, and post-GC native re-entry.
The run log is `/Users/frityet/Scratch/hfa-final-20261029/four-profile.log`.

Strict x86_64 compilation with `-Werror` passed for `lj_crecord.o`, `lj_asm.o`,
`lj_ccall.o`, and `lj_ctype.o` using the Darwin x86_64 target. This checks that
the target-local additions do not introduce an unused-variable warning in the
x86_64 build; it is not an x86_64 runtime HFA claim.
