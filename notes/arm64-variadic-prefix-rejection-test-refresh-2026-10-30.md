# Named variadic aggregate prefix test expectation

The complete experimental gate at `9581ed718` reached M7 FFI after the
preceding contracts passed, then stopped in the integer-aggregate fixture:
`unexpectedly accepted unsupported ABI signature: __arm64_pair_stack_vararg_fn`.
The earlier `5a74660e1` implementation intentionally supports this named
integer aggregate after all eight GPRs are occupied. Its focused variadic
runtime contract passed, but the older aggregate fixture still expected the
signature to be rejected. This was a stale expectation, not evidence of a
completed gate or a native-call failure.

The test now checks the accepted nine-parameter fixed prefix: eight 32-bit
integer GPR arguments followed by the eight-byte integer aggregate at stack
offset zero, total named stack size eight, and `is_vararg` set. Anonymous
arguments retain the independent variadic proof. The aggregate and variadic
runtime fixtures both pass ARM64/ARM64e assertion and release profiles.
No production code changes in this follow-up.

Evidence is in `/Users/frityet/Scratch/ljmt-byte-num-prefix-integration-20261030/`:
the original failure remains in `complete-gate.log`; its successful ordinary
restoration is in `gate-restoration.log`, profile
`44fd58f91e3d8b323eeae7bdf25b8be4a7e4632aacedfb51bf4d1c3112a57dc4`.
`aggregate-varargs-four-profiles.log` records the updated focused matrix.
The complete gate will be rerun on this test correction; main parity remains
on the completed `89226814d` production checkpoint until it passes.
