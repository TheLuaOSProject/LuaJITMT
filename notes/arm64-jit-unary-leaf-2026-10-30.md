# ARM64 numeric unary leaf coverage

This slice adds numeric `BC_UNM` to the bounded fixed-leaf proof. The independent recorder-side and post-RA replayers require the D-byte source register to be defined and numeric, then bind that source to an earlier numeric `IR_NEG` with the exact global sign mask. NEG type matching permits only `IRT_NUM` and the `IRT_ISPHI` marker. Integer-specialized unary forms remain closed.

The native C fixture uses a two-formal leaf `return -x`, called with a numeric first argument and `false` as the second. Its selected loop root is asserted to contain a numeric `IR_NEG` and pass native entries, selected identity exits, GC, and same-trace reuse. Independent semantic and post-RA clone mutations cover the child argument snapshot, CALL arity, wrong/out-of-frame UNM source, a nonnumeric second-formal source, unrelated numeric NEG input, and sign-mask corruption. The unary child input begins negative and later reaches signed zero, exercising both caller branch outcomes. No direct-root source-recognition rule was broadened.

The numeric and type range/path walkers now propagate the D-byte source for `BC_UNM`, with explicit source/destination bounds and numeric-source checks. The reverse live-value walks retain the unary input even when the output is dead.

## Validation

- Four-profile leaf contract: `arm64`, `arm64e`, `arm64` release, and `arm64e` release; final log: `/Users/frityet/Scratch/ljmt-unary-leaf-work-20261030/final-four-profile-negative-input.log`.
- Ordinary ARM64 runtime without trace-test helpers: root 1, 159 selected exits across nine edge inputs, oracle match, live peer, GC and code reuse; log: `/Users/frityet/Scratch/ljmt-unary-leaf-work-20261030/ordinary-arm64.log`.
- Preserved ordinary x64 runtime at `/Users/frityet/Scratch/ljmt-side-ancestry-20261029/dae-x64/luajit`: root 1, four observed exits, nine edge inputs, oracle match, live peer, GC and code reuse; log: `/Users/frityet/Scratch/ljmt-unary-leaf-work-20261030/ordinary-x64.log`.

The ordinary Lua loop exercises unary negation as a direct root operation. Child-leaf coverage is supplied by the native C fixture, which proves the active trace includes the leaf's `IR_NEG` and applies its mutations to that trace's cloned semantic and allocated IR views.
