# ARM64 FFI integer-to-double argument validation

The generated ABI matrix now includes a native mixed-signature call whose Lua
arguments are integers but whose first, third, and fifth C arguments are
`double`. The loop runs 300 calls and requires at least 200 additional calls
observed with the active native-frame lease. This exercises the recorder's
`IR_CONV` path in a published native trace rather than only classifying a
signature.

No production conversion change was needed. Inspection of the trace showed
`IRT_NUM` output, `IRCONV_NUM_INT` mode (`op2 == 467`), and an `IRT_INT|IRT_GUARD`
source (`t == 147`). The existing semantic and post-RA checks already compare
the source's `irt_type` and allow its guard bit; the earlier suspected guard
rejection came from misreading the IR type enum. In this tree `IRT_INT=19`,
`IRT_U32=20`, and `IRT_NUM=14`; this source is signed `IRT_INT`, not `IRT_U32`.

The existing synthetic double-profile semantic mutation rejects
`IRCONV_CHECK`. A matching post-RA mutation now checks the same unsupported
conversion mode independently. The ABI-class fixture validates the positive
native call, while the callxs-admission fixture validates both malformed-mode
paths.

Validation used an assertion-enabled Darwin ARM64 experimental build with
matching helper defines:

```
make -j2 TARGET_SYS=Darwin TARGET_FLAGS='-arch arm64' \
  XCFLAGS='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS -DLJ_XSAVE_TEST_HELPERS -DLJ_GC2_TEST_HELPERS -DLJ_TAB_TEST_HELPERS'
```

Both `tests/t-arm64-jit-callxs-admission.c` and
`tests/t-arm64-jit-ffi-abi-classes.c` were compiled with the same defines and
run against that archive. They reported `t-arm64-jit-callxs-admission OK` and
`t-arm64-jit-ffi-abi-classes OK`; the latter's native-call delta assertion
passed.
