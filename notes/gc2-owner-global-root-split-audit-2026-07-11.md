# GC2 owner/global root split audit (2026-07-11)

Status: P0 follow-up to the current GC2 checkpoint. This is an implementation
audit, not a completed safety claim. The plan files are unchanged.

## Existing split and remaining violation

The safepoint shape is already close to the required ownership model:
`lj_safepoint_apply_tg()` asks each stopped TG to run an owner-root scan, and
the handshake leader runs one process-global scan after all acknowledgements.
The global implementation currently defeats that split by calling
`gc2_scan_tg_roots()` again. At that point acknowledged peers may already have
resumed, so the second pass is both duplicate work and an unsafe read of live
stacks, root anchors, temporary values, and TG-local state.

`gc2_scan_threading_states()` has a more direct version of the same problem.
Every spawned state has `mt_thread`, so its condition remotely scans a worker
stack even when a nonzero foreign owner is concurrently mutating it. This
bypasses the state claim/NEEDSCAN handoff.

## Required ownership boundary

The TG owner acknowledgement must scan:

- its temporary buffer backing storage;
- live JIT temporary values;
- root-anchor slots;
- `thread_L` and `cur_L` object identities and stacks when their current owner
  matches this TG;
- its positive `vmstate` trace root;
- coroutine NEEDSCAN handoffs assigned to its tid;
- future sequence-validated native/FFI frame publications.

The once-per-handshake global pass owns:

- main and VM thread identities, registry, and `gcroot[]`;
- pending root chains;
- finalizer/FINREG roots;
- native live-thread nodes and their userdata references;
- threading registry state identities, never foreign live stacks;
- global string/table retired storage, buffers, lightuserdata segments;
- CTState/callback/ctype roots;
- global JIT trace, recorder, and retired-code roots.

A dead but not yet reclaimed TG's raw temporary-buffer allocation still needs a
lifecycle root. That exception must be separated from live TG semantic scans or
the buffer must be torn down by the detaching owner before `TGF_DEAD`.

## Patch sequence

1. Add a `TGState *` owner-root entry point and call it directly from each
   safepoint acknowledgement. Keep an `L` wrapper only for tests/compatibility.
2. Move the complete live portion of `gc2_scan_one_tg_roots()` into that entry
   point and make owned-NEEDSCAN lookup tid-based rather than `L2TG(L)`-based.
3. Make the threading-state registry scan object-only.
4. Remove live TG scanning from the global pass, preserving only explicitly
   protected dead-storage lifetime.
5. Replace every post-handshake `gc2_scan_tg_roots()` fallback with a bounded
   defer/reopen/retry. Failure to establish freshness is not permission for a
   leader to read a foreign stack.
6. Replace the duplicate sweep root scanner with the same owner/global
   handshake. Sweep must not scan peers before their acknowledgement.
7. Add a nonwaiting TG-registry read/reclaim gate. Today a dead heap TG can be
   unlinked and freed while a post-ack leader is still walking the registry;
   thread-count and handshake-pending counters do not protect that reader.

The FFI XSAVE staging fields are not remotely published and must not be added
to the generic TG scan. The generic FFI bridge needs a separate stable,
sequence-validated native-frame publication.

## Required regressions

- A global-only scan must not mark an auxiliary TG's stack/root-anchor value;
  that TG's owner scan must mark it exactly once.
- A global scan of a foreign-owned threading state must mark only the state
  identity and leave its stack epoch unchanged; owner/NEEDSCAN processing must
  mark the stack.
- One two-TG handshake must mark both private root sets once and the global set
  once.
- Sweep preparation must not make a foreign stack fresh without that TG's root
  acknowledgement.
- ASan stress must race child detach/TG reclamation against global scans and
  freshness walks without a registry use-after-free.

Besides removing a correctness race, this split removes one complete all-TG
private-root pass from every root handshake.
