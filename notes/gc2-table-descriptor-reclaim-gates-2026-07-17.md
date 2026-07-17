# GC2 table-descriptor reclamation gates (2026-07-17)

This checkpoint couples the already-landed dormant `LJGC2TableDesc` authority
to small-arena physical lifetime. Descriptor producers remain disabled. The
purpose of this tranche is to make later producer enablement incapable of
creating an unguarded destruction, reuse, or unmap window.

## Stable mapping attachment

The final eight bytes of `GCAhdr` now hold `gc2_tabledesc` instead of padding.
All existing offsets and the 128-byte header size are unchanged. A production
arena binding publishes `&g->gc2.table_rescan_desc` before `progress_g` and
never rebinds either pointer to another global state. Same-global dead-TG
transfer retains the existing binding. A cross-global rebind is a fatal
invariant violation because the old descriptor could still name the mapping.

An unpublished/private arena with both pointers null has no descriptor
publisher and is therefore clear. Once `progress_g` is visible, a missing
descriptor pointer is malformed and fails closed. Tests may install a local
descriptor directly while the mapping remains fixture-owned.

## Reclamation classification

`lj_arena_gc2_desc_clear_acq()` classifies one exact small allocation:

- `IDLE` permits the descriptor half of reclamation;
- `ACTIVE(table)` vetoes only the exact matching table address;
- an unrelated `ACTIVE` identity permits independent progress; and
- `PINNED`, malformed encodings, or inconsistent attachment veto
  conservatively.

`lj_arena_gc2_reclaim_clear_acq()` combines that result with the exact
per-table token being `NONE`. This is a physical-ownership query, not semantic
liveness: logical `FREE` may still coexist with a token.

Mapping-wide teardown separately rejects an `ACTIVE` address in the mapping
and rejects `PINNED`, malformed, or inconsistent attachment globally. The
check happens before small-arena registry deletion, not merely in the final
unmap helper, so a failed certificate cannot erase the only lookup locator.

The descriptor is also converted to bitmap-word geometry on cold sweep and
free-run rebuild paths. Exact `ACTIVE` splits the affected reusable range;
global veto states block every traversable range. Intrusive free-run body
reads/writes and range coalescing revalidate the descriptor before touching
logically free storage.

## Atomic-read decision

This checkpoint deliberately uses the exact 128-bit CX16 snapshot already
implemented by `lj_gc2_tabledesc_snapshot()`. The design note allowed an
aligned acquire load of the low word under the explicit x86-64 GCC/Clang
machine contract. We are not taking that optimization for b1.2.0 yet:

- exact CX16 avoids overlapping mixed-width atomic language-model ambiguity;
- it is hardware lock-free on the supported x86-64 target;
- descriptor checks are currently concentrated in destruction, sweep,
  rebuild, teardown, and defensive structural-reuse paths; a multi-cell or
  free-run allocation can still reach one of those checks, so eliminating the
  locked snapshot there remains an explicit performance follow-up; and
- replacing the snapshot with a measured low-word classifier later does not
  change any caller or reclamation semantics.

This is an intentional safety-first divergence, not a rejection of the later
performance optimization.

## Small-object terminal coverage

The combined authority is checked by:

- ordinary arena destructive claims and the final `DESTRUCT -> FREE` helper;
- exclusive leaf and adjacent closure-pair reclamation, including a late
  fail-closed conversion to the ordinary post-grace reanchor ticket;
- scalar and packed pre-grace typed destructors before body dispatch and final
  commit;
- late/quarantine reclamation and terminal-word certificates;
- terminal lifetime cleanup;
- free-run enumeration, coalescing, and intrusive node access; and
- direct and allocator-list mapping teardown before registry removal.

If a descriptor becomes globally vetoing after `DESTRUCT -> FREE` but before
free-run-node publication, ordinary free now retains `block + FREEING + late`
for quarantine retry. It no longer aborts on this legitimate authority
crossover or attempts to restore an already-terminal lifetime lane.

A lawful publisher takes readable-lifetime admission before `ACTIVE`, installs
the exact token before clearing `ACTIVE`, and releases admission last. A
destroyer claims `DESTRUCT`, performs the reciprocal sequentially-consistent
admission proof, then checks descriptor and token. Thus the two sides cannot
both miss physical ownership.

## Deterministic validation

The runtime typed-destructor fixture now drives otherwise-dormant states with
the real descriptor primitives and proves:

- exact `ACTIVE` preserves body bytes, lifetime, and accounting while peers in
  the same arena reclaim;
- unrelated same-arena `ACTIVE` does not become a global stop gate;
- same-address generation reuse rejects a stale helper completion;
- reclamation resumes only after the current generation reaches `IDLE`;
- `PINNED` retains every candidate; and
- a malformed descriptor retains every candidate.

Standalone sweep coverage proves that exact `ACTIVE` splits free-run
enumeration, vetoes coalescing without arena mutation, and prevents direct
unmap until exact completion.

Validated commands:

- `tools/ci/lua_test.sh m2_arena_gcsweep`
- `tools/ci/lua_test.sh m2_arena_sweep`
- `tools/ci/lua_test.sh m2_arena_alloc`
- the descriptor fixture compiled and ran under Clang as well as GCC

## Still intentionally incomplete

The header now exposes pointer-form descriptor/token helpers and the final
huge-unmap backstop recognizes the embedded huge token. That is groundwork,
not a claim that huge lifetime is complete. Every HugeTab claim, destructor,
deferred-free, realloc, delete, transfer, fini, and pre-delete unmap path still
needs the reciprocal authority audit and deterministic crossover coverage.
Bounded token enumeration, generation/cycle wrap handling, storage compaction,
and actual descriptor producers also remain disabled work.
