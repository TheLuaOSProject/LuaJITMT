# ARM64 CALLXS with a loop-carried integer overflow guard

The ARM64 CALLXS source-graph verifier now accepts an integer `ADDOV` whose type is `IRT_INT|IRT_GUARD|IRT_ISPHI`. `SUBOV` and `MULOV` retain the previous exact `IRT_INT|IRT_GUARD` requirement until their PHI forms have native witnesses. The accepted ADDOV still requires both operands to precede it and pass reference validation, with operand types exactly `IRT_INT`. Semantic and post-RA validation use the same bounded predicate independently.

The fixture adds an integer accumulator to the existing variadic no-extra-argument CALLXS loop. A copied semantic trace and an allocated post-RA copy each pass with the guard and reject after the `IRT_GUARD` bit is removed. The native run then starts the accumulator at `INT32_MAX`; it checks the exact callback count and numerical result, confirms the selected native trace is reused, and observes an overflow exit whose snapshot points to the accumulator `ADDOV`. The snapshot footer is checked against bytecode-derived CGET restart positions immediately before the accumulator `ADDVV`, after the FFI call, so the exit does not replay the callback.

Matched ordinary evidence on the frozen baseline showed x64 publishing the selected loop root (`calls=0->500`, result `125250`, root `1:67/15`), while ARM64 produced the same interpreted result and callback count without publishing that root. After the change, `tools/ci/arm64_jit_table_call_contract.sh` passed the `ffi-varargs` fixture in all four profiles: arm64/arm64e assertion and release. Captured log: `/Users/frityet/Scratch/ljmt-ffi-intacc-20261029/four-profile-final.log`.

This extends one guarded integer-overflow node shape in the existing CALLXS leaf graph proof; it does not claim general FFI-loop parity.
