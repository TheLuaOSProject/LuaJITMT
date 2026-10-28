# Negative dynamic string-byte boundary validation

Production source `b33802e93` includes the bounded negative-index byte-load
certificate and the independent exact positive subtraction proof. The C
contract now compares 28 additional calls through the negative-origin native
trace against a separately JIT-disabled interpreter oracle. Every call proves
entry and exit through the selected negative-origin trace.

The cases include fractional truncation at both string ends, the first invalid
negative index, signed 32-bit boundaries, huge finite numbers, infinities, NaN,
both signed zeroes, a positive fractional index, short and empty strings,
embedded NUL bytes, and a 65,536-byte string. Successful values and error status
must match the interpreter. Existing private-copy semantic and post-register-
allocation malformed-graph tests remain in place.

`tools/ci/arm64_jit_string_byte_contract.sh` passed all four ARM64/arm64e
assertion and release profiles with these additions. Evidence is in
`/Users/frityet/Scratch/ljmt-negative-nested-20261028/byte-oracle-four-profile.log`.
This is a focused contract result, not a full gate result for the combined
negative-byte and nested-call changes. The previous complete gate checkpoint
and the unresolved harness TDUP investigation retain their separate scope.
