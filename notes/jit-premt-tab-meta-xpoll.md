# JIT pre-MT tab.meta forwarding across XPOLL, 2026-07-03

`IR_XPOLL` remains a hard alias boundary for mutable field stores and for
active/entering-MT traces. The optimizer now lets `IRFL_TAB_META` field loads
forward or CSE across `XPOLL` only while `mt_entering || mt_active` is false.

The safety boundary matches the existing pre-MT array-read and primitive
upvalue-forwarding rules. Before `mt_entering`, there is no secondary Lua thread
that can mutate a table metatable concurrently with traced code. The first
threading activation raises `mt_entering`, flushes existing traces, and only then
allows worker Lua code to run. If a poll is actually pending, `XPOLL` exits the
trace before any post-poll value is used.

This recovers the common no-metatable table-store loop shape after the
`tab.nomm` cleanup: the trace still guards `tab.meta == NULL`, but the guard is
not repeated after each loop `XPOLL` in pre-MT traces. Active-MT and
entering-MT traces keep the conservative post-poll metatable load.

Coverage remains behavioral and traceability-based. Generated IR dumps are useful
for local diagnosis, but they are not pass/fail contracts.
