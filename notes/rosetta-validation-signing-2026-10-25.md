# Local Rosetta validation signing (2026-10-25)

Two freshly linked unsigned x86-64 executables entered uninterruptible `U`
state on this host before producing output: a focused FFI errno harness and
`src/luajit -e 'print(jit.arch, jit.version)'`. The system x86-64 `true` control
still ran. These launch stalls do not establish a runtime test failure or pass.

Ad hoc signed copies launched. After correcting the temporary harness to load
FFI before accessing its CType state, the shared parser-wait and automatic-GC
errno checks passed under Rosetta. The signed LuaJIT executable printed `x64`
and its version successfully. No entitlement, Developer ID credential,
notarization, system setting, or runtime source change was required.

The two macOS multi-architecture contracts now explicitly ad hoc sign their
x86-64 fixture before execution. All required ARM64, arm64e and x86-64 branches
remain enabled. The unwind fixture requests a linker-generated ad hoc signature;
the callback fixture signs its fresh executable before the run loop. This is a
local validation workaround demonstrated on this host, not a claim that every
unsigned Rosetta program or every macOS installation needs signing.

The first temporary signed errno harness crash was a harness setup error:
`luaL_openlibs()` alone does not initialize the FFI CType state; requiring `ffi`
resolved the null-state access. That crash was separate from the unsigned launch
stall and is not evidence of a LuaJIT runtime regression.
