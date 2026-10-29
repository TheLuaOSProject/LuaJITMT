# ARM64 variadic indirect-result CALLXS

The ARM64 ABI classifier already describes supported non-HFA aggregates above
16 bytes as indirect results, with the result pointer in X8. It previously
refused every variadic signature with an indirect result. The ARM64 CALLXS
lowering now removes only the hidden result pointer from the CARG vector before
planning the named parameters, variadic separator, promoted suffix, and stack
extent. It then transfers that exact rooted CNEW payload to X8 through the
existing sret emitter path. The eight visible argument registers and stack
offsets retain their original indexing. Semantic and post-RA admission each
bind the X8 payload to the same guarded CNEW, validate the hidden pointer at
CARG zero, and validate the separator after the fixed prefix. No general
indirect-result class was added: the existing ABI classifier and 24/32-byte
sret bounds remain authoritative.

## Evidence

The focused native fixture adds a variadic function returning a 24-byte
three-word struct. Its unnamed arguments are an `int32_t` CData value and a
`double` CData value. The function checks both values and returns three exact
words; the Lua runner checks all fields and a numeric loop result. The fixture
holds a live threading peer, requires the native callback to observe the
selected active trace and the exact preallocated result-root CType, and
requires the trace to contain both `CNEW` and nil-result `CALLXS`. It forces a
post-call epoch exit and checks the foreign call counter advanced exactly once
per iteration, then forces a full GC and confirms native re-entry uses the
same trace number and preserves the result. Private cloned traces prove that
semantic and post-RA admission accept the baseline and reject a substituted
hidden result pointer and a moved CARG separator.

The ordinary Lua companion compares the same sret runner with an interpreter
oracle, confirms publication and post-GC trace reuse, and labels that result as
publication evidence only. The native claim comes from the C callback and
counter checks.

The matched x86_64 control is the frozen runtime at
`/Users/frityet/Scratch/ljmt-deep-callm-integration-20261029/x64-frozen/luajit`
with the existing `Ret24` DSO and numeric-accumulator probe. Its saved dump is
`/Users/frityet/Scratch/ljmt-vararg-sret-20261029/x64-numacc-confirm.log`; it
shows `TRACE 1 stop -> loop` and `RESULT 125250.5 0ULL 500ULL`. The matching
ARM64 probe initially failed for the integer accumulator because that source
shape contains an `ADDOV` rejected by the independent CALLXS graph. Replacing
only the accumulator with numeric addition produced a native ARM64 loop trace
and 500 calls. This remains a separate source-composition gap; the sret slice
does not admit integer overflow arithmetic.

Validation command:

```sh
LJ_ARM64_TABLE_CALL_PROFILES='arm64-assert arm64e-assert arm64-release arm64e-release' \
LJ_ARM64_TABLE_CALL_CASES=ffi-varargs \
  tools/ci/arm64_jit_table_call_contract.sh
```

All four helper profiles passed the varargs C fixture and ordinary Lua
companion. The broader M7 gate remains a separate integration check.
