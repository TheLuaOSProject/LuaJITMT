# M6 MCode Native Boundary

Linux/x64 secure mcode allocation creates a memfd and maps it twice, once RX
and once RW. Those syscalls now run while the recorder's current TG is marked
native, and the fd is closed before leaving native state. The sync-core
membarrier used before trace publication is also marked native.

The mcode path intentionally does not call `lj_safepoint_checkstop()` inside
the allocation or trace commit transaction. Native leave records pending
STOPREQ on the TG, and the existing dispatch path checks the sticky STOPREQ
after the trace recorder has returned to idle and released/cleaned up its
state. This keeps shutdown observable without throwing from the middle of
assembly or mcode publication.

Validation:

- `tools/ci/m6_jit_mcode_native.sh`
- `tools/ci/m6_jit_mcode_publish.sh`
- `tools/ci/lua_test.sh m6_jit`
