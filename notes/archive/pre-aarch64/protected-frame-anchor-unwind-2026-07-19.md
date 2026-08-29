# Protected-frame dynamic-anchor unwind

## Closed failure mode

`lj_vm_pcall_unwind()` and `lj_vm_cpcall()` already restored the TG dynamic
root-anchor depth when their own protected C boundary returned an error. That
was insufficient for a Lua fast `pcall()`/`xpcall()` nested inside the boundary:
the VM fast frame can catch STOPREQ, OOM or a semantic allocation error and let
the outer C boundary return success, leaving roots abandoned by the failed
operation permanently published.

On x86-64/FR2, the fast protected delta frame now stores its entry anchor depth
in the unused upper 32 bits of the 64-bit `ftsz` word. Delta consumers mask to
the low word. `err_unwind()` rolls the current physical TG back to this depth
when `FRAME_PCALL`/`FRAME_PCALLH` actually catches a non-yield error. Rollback
only release-nils existing slots; it cannot allocate, wait or throw. Successful
return discards the packed word before doing frame-delta arithmetic.

The checkpoint is owned by the VM's physical `DISPATCH` TG. Error cleanup and
the JIT observation helper therefore use `G2TG(G(L))`, not the coroutine's
migratable `L->tg_hint`/`L2TG(L)` metadata.

## Recorded pcall frames

Snapshots preserve the complete recorder-time frame word. A trace can execute
on another lexical anchor depth, so `recff_pcall` and `recff_xpcall` emit a
non-throwing runtime depth read and equality guard before logical call setup.
The normal depth is zero. A mismatch exits before a stale protected-frame
checkpoint can be installed; interpreter replay captures the current depth.

## Deterministic coverage

`t-meta-rooted-chain` deliberately makes a dynamic anchor the only strong root,
forces a full collection to prove it remains live, and then abandons it through
nested fast `pcall`/`xpcall` throws representing STOPREQ, OOM and table
overflow. The outer protected call succeeds, while the test verifies the inner
catch restored depth and that a later collection clears the weak value.

With `hotloop=1`, separate `pcall(math.sqrt, x)` and `xpcall(math.sqrt, ...)`
loops are recorded. Test-only telemetry proves both generated depth helpers run.
Each trace is then entered beneath an extra sentinel anchor; the mismatch helper
runs and interpreter replay catches a type error without consuming the outer
sentinel.

## Remaining topology limits

- The packed fast-frame checkpoint is intentionally x86-64/FR2-only, matching
  the current platform scope. Other VM backends still rely on the outer C
  protected wrappers and explicit cleanup.
- Yield does not roll anchors back: the protected frame remains active and may
  later catch an error after resume. Current dynamic-anchor users must finish or
  transfer their value to natural stack roots before any yield. If a future
  helper deliberately carries anchors across a coroutine/TG handoff, depth alone
  is not a transferable identity; it needs a state-owned root stack or explicit
  migration protocol.
- This checkpoint owns only TG dynamic root anchors. Table-reader epochs,
  allocation leases, native regions and other lexical authorities retain their
  own unwind protocols; they are not silently repaired by anchor rollback.
- External-unwind search passes (`errcode == 0`) are observational and do not
  mutate roots. Rollback occurs only in the cleanup pass which selects the
  protected fast frame. Unprotected panic/termination paths do not resume Lua
  and therefore do not require a reusable anchor depth.
