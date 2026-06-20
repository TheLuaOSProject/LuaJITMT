# VM-event native stdio

Date: 2026-06-20

## Problem

`lj_vmevent_call()` reports failing VM-event handlers to `stderr`. That path
has a live `lua_State *`, but it still used raw `fputs()` / `fputc()` directly.
A blocked or slow diagnostic write could therefore sit outside native state and
delay a STOPREQ handshake.

## Fix

The failure reporter now enters native state before writing to `stderr`, leaves
native state afterward, and carries the native-leave action mask back to
`lj_vmevent_call()`.

The STOPREQ check is intentionally deferred until after `cur_L`, JIT `L`, hook
state, and `vmevmask` are restored. Throwing while those VM-event invariants are
still temporarily changed would leave the VM in event-call state.

## Guard

`tools/ci/m3_vmevent_native_stdio.sh` rejects raw VM-event stdio outside the
`vmevent_report_failure()` native boundary and runs the behavior smoke
`m3_vmevent_native_stdio`, which triggers an erroring `jit.attach(..., "bc")`
handler and verifies the reporter still fires.
