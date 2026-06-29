# Safepoint Consumed Native Ack Gate

Native handshakes allow the leader to acknowledge a TG whose owner is blocked
outside the VM. That leader may scan the TG's current Lua stack before it clears
the TG poll word. A racing owner can observe the same poll, find that the
request mask was already consumed by the leader, and otherwise return to the VM
while the leader is still scanning its stack.

The poll word is therefore also the completion gate for the consumed-request
case: if `lj_safepoint_ack()` finds `reqmask == 0` while `poll != 0`, it waits
until the poll word is cleared before returning. This is fork-local threading
behavior only. It does not add a public API and does not change stock LuaJIT
single-threaded semantics.

The root scanner also treats a remote current thread specially before it walks
any frame links. Remote frame chains are unstable around VM safepoints and JIT
exits, so the scan conservatively covers the stack up to `L->maxstack` and
avoids both `frame_prev()`/`frame_func()` and any attempt to derive a tighter
bound from another active OS thread.

Detach has one more handshake edge: a signaler can observe a TG as live, then
the owner can mark it dead before the request is acknowledged. Dead TGs cannot
poll again, so detach and signal publication both retire a dead TG's pending
request by claiming the current epoch and decrementing the pending count once.

`tests/t-safepoint-handshake.c` exercises the contract directly by setting the
consumed-request state and clearing the poll word from another pthread after a
short delay, and by detaching an extra TG with an outstanding request. This is
intentionally runtime synchronization coverage, not a source-search guard.
