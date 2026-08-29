# IDLE table-key publication fast return

## Profile result

The b1.2.0 Linux/x64 profile separated the growing-live-table slowdown into an
active-cycle/JIT scheduling cost and a base key-publication cost. With
automatic collection stopped, every freshly interned string key still entered
`lj_gc_tv_gcref_valid()`. That generic edge validator acquired exact arena
lifetime admission, including the shared HugeTab reader path, before
`lj_gc2_barrier_key_g()` discovered that non-generational IDLE had no marking,
SWEEP-rescue, weak-key, or remembered-set work.

`perf` attributed roughly 11% directly to small-object admission, another 9%
to its HugeTab reader acquire/release, and additional time to the sequential
fence and type validation reached from `lj_gc_pubtabkey_()`.

## Retained boundary

`lj_gc_pubtabkey_()` is now one out-of-line publication boundary. It samples
the exact publishing TG's `mark_active` acknowledgement, GC2 phase, and
generational mode before validating the key. It returns only for the complete
no-op conjunction:

- the publishing TG has not acknowledged an active mark barrier;
- authoritative GC2 phase is IDLE; and
- generational remembered barriers are disabled.

Every table insertion call site invokes this wrapper after release-publishing
the key slot. If MARK begins after the no-op sample, that slot publication
precedes the TG's activation acknowledgement and root snapshot. If activation
has already completed, `mark_active` prevents the fast return even if a racing
phase load is conservative. MARK, WEAK, SWEEP, and generational IDLE retain
the exact key validation and existing barrier paths.

Combining the phase gate and validation in one C entry also avoids the old
two-call validation-then-barrier sequence on active paths without changing the
barrier's public semantics.

## Measurements and verification

Pinned 200,000-key GC-stopped samples measured roughly 184--218 ns/key for the
fork versus 93--97 ns/key for the system LuaJIT in the same session. Before
the change the fork was roughly 409--423 ns/key. This removes the base
new-key row's greater-than-5x gap; active-cycle cost remains a separate
cooperative-JIT release blocker.

Focused verification passed:

- `m9_newkey_barrier_scope`;
- the direct `m5_tab_resize_stress` aggregate;
- `m5_gc2_pacing_atomic`;
- `m3_gc_root_pending`; and
- `m3_gc_root_pending_race`.

`m2_arena_gcclose` still fails its weak `kv` assertion at fixture line 35. The
unchanged `6cc0c583`/`d2d9a9b1` baseline has the identical failure, so it is
tracked independently and is not evidence for or against this IDLE fast
return.
