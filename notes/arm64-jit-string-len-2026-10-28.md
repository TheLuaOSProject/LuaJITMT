# ARM64 native immutable-string length — 2026-10-28

This slice admits only the exact `IR_FLOAD IRT_INT, IRFL_STR_LEN` operation on
an earlier, valid `IRT_STR` reference. Semantic IR provenance and post-register-
allocation provenance are checked independently. String sources must remain
guarded stack/rooted loads or validated proto constants; the existing root
snapshot, native-entry, and GC2 phase gates continue to govern their lifetime.
The ARM64 assembler already lowers this field load with the target's normal
32-bit field load, so no emitter or GC helper changes were needed.

The new native fixture exercises a dynamic string argument through the compiled
loop, runs a full collection while the string remains rooted, verifies native
reentry, and passes a table to force the string guard exit while checking Lua's
table-length result. It also clones a live trace and independently rejects
wrong fields, result types/flags, invalid/forward sources, non-string sources,
and out-of-range source slots in both admission passes. The ordinary Lua test
checks that the native IR contains `FLOAD str.len`, survives collection, and
exits on a table argument.

Validation on Apple ARM64 used the experimental profile with assertions and
trace/GC2 test helpers. `make` succeeded; `tests/t-arm64-jit-string-len.lua`
passed; and `t-arm64-jit-string-len-admission` passed with
`FLOAD=32772`, two mapped source snapshots, native root-entry witnesses, and
forced-exit witnesses. The suite entry is `m6_jit_string_len_ir` in
`tests/suites/m6_jit.lua`.

The focused `tools/ci/arm64_jit_string_len_contract.sh` now rebuilds and runs
the C admission fixture and Lua companion under arm64 and arm64e/BTI, each in
assertion and release modes. It binds fixture compilation to each archive
profile and restores the enclosing gate's requested arm64 profile afterward.
All four profile runs passed. The C fixture checks dynamic, empty, embedded-NUL,
and 65,536-byte strings, then verifies native reentry after full GC and a native
exit on a table argument. The Lua companion checks the same length boundaries
and IR/GC/type-exit semantics; its plain `OK` output is a semantic result and
does not claim a native entry counter witness.

`string.byte` remains outside this slice. Its recorder emits `IR_STRREF` and
`IR_XLOAD IRT_U8`, plus per-index bounds guards and potentially several byte
results. It needs its own source/snapshot and post-RA proof before ARM64
admission.
