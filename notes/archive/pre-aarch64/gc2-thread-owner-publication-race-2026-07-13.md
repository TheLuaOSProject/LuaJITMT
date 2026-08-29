# GC2 thread-owner publication race (2026-07-13)

## Failure

The reduced `t-jit-flush-thread-stress` F-S-F sequence intermittently returned
wrong branch results, reported owner mismatches, crashed in interpreter branch
opcodes, or stalled final collection. The apparent JIT correlation was a
consequence of GC2 work from trace exit, not stale trace-slot identity.

A hardware watchpoint caught `gc2_traverse_thread()` changing a live spawned
worker's `thr_owner` from its real tid to `LJ_THREAD_GCSCAN`. Worker startup had
claimed `L` before calling `lj_tg_attach()`. During that interval
`lj_tg_find_owner(g, tid)` returned NULL, so GC2's valid stale-owner recovery
mistook the unpublished live owner for a dead one. Its terminal scan release
then stored owner zero underneath the worker.

Raising the trace/GC retirement epoch did not fix the reducer, which helped
exclude ordinary two-epoch trace reuse. A trace-generation diagnostic also saw
the worker crash without any generation mismatch.

## Required invariant

Every ordinary owner id published in `lua_State.thr_owner` must already be
resolvable in the TG registry. State teardown must retain that owner claim
while clearing `tg_hint` and other state-facing publications.

The repair does not globally disable stale-owner takeover. A global
`mt_entering` veto would couple unrelated attach attempts to stale-state
recovery and could form a progress cycle with GC completion.

## Spawned workers

The parent already initializes the worker TG with `L`, its tid, thread userdata,
and stack roots before `pthread_create`. The worker now:

1. installs TG TLS and enters a native region;
2. attaches that prepared TG to the registry;
3. waits through transient GCSCAN/GCPREP ownership and claims `L`;
4. leaves native state, servicing any handshake; and
5. only then installs the first `lj_vm_cpcall` frame.

Cancellation also waits until the worker owns `L`. This is necessary because an
owner-zero check alone does not exclude a new GCSCAN CAS before cleanup. The
retained claim protects `tg_hint` clearing and detach.

Terminal GCPREP publication is impossible after the parent-prepared TG becomes
visible; observing PENDING/DONE is therefore fail-fast corruption. Temporary
GCPREP/GCSCAN claims remain bounded waits which acknowledge TG handshakes.

## Foreign attach

Foreign attach cannot claim first for the same reason. It now publishes an
empty provisional TG, marked native, while `mt_entering` protects the VM and
private transaction. The provisional TG has the final tid but no Lua roots, so
handshake leaders can acknowledge it remotely. After claiming `L`, the carrier
publishes `tg_hint`, `cur_L`, `thread_L`, and the existing stack edges, leaves
native state, and only then enters `lj_vm_cpcall`.

A failed pre-root attach releases the state if it was claimed and detaches the
rootless TG. Recorder/callback cleanup is skipped until the state was actually
published under that TG.

Attach/detach lifecycle waits now use `lj_safepoint_poll_tg(tg)` rather than
rediscovering a TG through `L2TG(L)`. Cleanup may intentionally clear `tg_hint`
and release state ownership before monolithic detach, so an L-based ack could
otherwise acknowledge the main TG instead of the leaving TG.

## Regression coverage

`t-threading-lifecycle.c` now deterministically checks that:

- the provisional TG is registry-visible before foreign state claim;
- the claimed owner resolves to that exact TG;
- the first protected C frame starts with the exact owner claim; and
- shutdown still waits through failed-attach state release and TG cleanup.

`t-threading-spawn-native.c` was also corrected to observe `gc2.phase` and the
GC2 cycle number. The legacy `g->gc.state` remains `GCSpause` by design now that
GC2 is the sole collector and cannot identify an active cycle.

Targeted default-build validation passed both lifecycle fixtures. The original
ASAN F-S-F reducer is the primary stress gate for the silent-owner corruption.
