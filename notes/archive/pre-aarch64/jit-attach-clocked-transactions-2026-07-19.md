# Clocked `jit.attach` transactions

Date: 2026-07-19

## Scope

This b1.2.1 tranche makes the public `jit.attach()` writer consume the held
keyed-slot resolver, prepared exact table-store transaction, and per-event
attachment clocks. It replaces the old naked-slot `lua_rawseti()` attach path
and `lua_next()` detach path. `plan/` is unchanged.

This is writer-side publication only. The VM-event reader still needs its
separate tri-state rooted lookup and double clock snapshot, and TRACE delivery
still needs the production callback cut. Direct mutation of
`debug.getregistry()._VMEVENTS` remains intentionally racy and unclocked.

## One captured event table

The observable entry order remains:

1. a `LUAJIT_DISABLE_VMEVENT` build raises `vmevent API disabled` before
   validating any argument;
2. an enabled build validates argument 1 as a function;
3. it validates argument 2 as an optional string; and
4. it calls `luaL_findtable()` exactly once.

`lj_lib_optstr()` may convert a numeric argument in place to a freshly
allocated GC string. That exact stack cell is published immediately on a
non-null return, before `luaL_findtable()` can allocate again; hashing later
reloads the string only through its saved stack offset.

The table left by that one call is rooted on the Lua stack and retained for
the whole attach or detach operation. Every retry, including confirmation
after a committed-plus-stale store, uses that same table. A concurrent registry
replacement can therefore orphan the captured table and lose the update in
the ordinary racy way; the operation never silently retargets a later registry
incarnation.

There is one intentional safety divergence from stock LuaJIT. If
`_VMEVENTS` is already a non-table, stock `jit.attach()` ignores the
`luaL_findtable()` conflict and can proceed with invalid stack assumptions
(including a crash or invalid `next()` behavior). This fork raises the
established `LJ_ERR_BADMODN` error instead:

```text
name conflict for module '_VMEVENTS'
```

The returned conflicting name component is passed directly to
`lj_err_callerv()`. No table dereference or traversal follows the failed
lookup.

## Attach transaction

The event registry key preserves stock hashing exactly: the full explicit
string length seeds the hash, while byte mixing stops at the first NUL. Thus
embedded-NUL names and string collisions retain stock behavior. Clock choice
uses the final signed 32-bit numeric table key, so the known collision
`"2694847501"` selects the TEXIT lane.

The table, numeric key, and desired function remain authoritative stack roots.
Only saved stack offsets cross an L-aware wait, allocation, insertion, or
stack relocation. Each attempt:

1. resolves or inserts the structural slot and obtains only an integerized
   address candidate;
2. prepares a snapshot transaction, completing barriers, GC2 guard admission,
   leases, and all potentially throwing work;
3. for a known JIT event, claims the exact attachment-clock lane;
4. once claimed, performs only `lj_tab_keyed_store_commit()` immediately
   followed by unconditional `lj_jit_event_attachment_writer_publish()`; and
5. finishes a committed transaction or aborts an uncommitted transaction only
   after the clock is even again.

A busy clock aborts all prepared authority before waiting. Exhaustion aborts
before a deterministic Lua error. Corrupt clock/handle state and impossible
transaction cleanup fail-stop rather than leaving an odd lane or discarded
GC2 authority. An uncommitted post-claim collision still publishes a
conservative generation before retrying.

`{committed, STALE}` remains successful publication authority: it publishes
and finishes, then resolves and prepares afresh against the same captured
table. Only a fresh snapshot whose expected raw value is the desired function
confirms completion. An initially identical function does not short-circuit;
it still performs a store and invalidation, matching the observable stock
attach contract.

Unknown numeric keys and every no-JIT build use the same prepared transaction
without a clock. Every committed attach still release-stores
`VMEVENT_NOCACHE`, including committed-plus-stale attempts.

## Detach transaction

Detach reserves four contiguous stack roots:

```text
[captured table, LJ_KEYINDEX control, actual key, exact value]
```

`lj_tab_itern_rooted()` advances the structural cursor. It can tolerate a
concurrent generation change without carrying an invalid public `next()` key.
Matching uses exact `GCfunc` identity. All matching entries are considered,
including arbitrary string, object, and numeric keys; detach is not restricted
to the five known VM-event shapes.

For each match, detach performs an existing-only rooted resolve and prepares
an exact `(expected function -> nil)` transaction. A changed expected value
means a replacement won and is never erased. Known clock lanes are recognized
from either an integer key or an exactly integral int32 Lua-number key, which
covers the iterator representation of negative hash keys. Unknown/object keys
remain unclocked.

An uncommitted `CHANGED` result advances without touching the replacement.
`STALE` or `FORWARD` re-resolves the same rooted key. A committed-plus-stale
delete publishes and finishes, then retries that exact key only while its
current value is still the target function. Concurrent resize may make the
overall structural scan skip or repeat entries, which is natural racy table
iteration; no exact delete can remove a competing replacement.

## Configuration and verification

Clock use is gated by `LJ_HASJIT`, not by the runtime `jit.on` flag. A
JIT-enabled build with the engine disabled still publishes clocks. A
`LUAJIT_DISABLE_JIT` build retains working legacy VM-event attachment through
the unclocked prepared path. A `LUAJIT_DISABLE_VMEVENT` build compiles out all
writer helpers and preserves the disabled error as the first operation.

Completed integration verification before the deterministic fixture landed:

- clean default and `LUAJIT_DISABLE_JIT` builds with `XCFLAGS=-Werror` passed;
- a clean `LUAJIT_DISABLE_VMEVENT` build passed with only the existing
  `gc2_errfin_vmevent_cp()` unused-variable warning demoted, and public calls
  with missing or deliberately invalid arguments both returned
  `vmevent API disabled` first;
- the amalgamated static and dynamic builds passed with `-Werror` plus the
  repository's established `unused-function` and `stringop-overflow`
  demotions, and an amalgamated attach/detach smoke passed;
- public default-build smokes covered an actual TRACE callback, repeated known
  attach, unknown and numeric-name attach, embedded NUL, the
  `"2694847501"`/TEXIT collision, detach of arbitrary string/table/boolean and
  nonintegral-number keys, replacement preservation, and the exact non-table
  module-conflict error;
- the equivalent no-JIT public attach/detach/conflict smoke passed; and
- reachable `newproxy` finalizers called attach then detach during
  `lua_close()` successfully in both default and no-JIT builds.

A whole-tree `-Wdeclaration-after-statement -Werror` build is not presently a
valid gate: existing violations occur in `lj_bcread.c`, `lj_serialize.c`,
`lj_profile.c`, `lj_str.c`, `lj_tab.c`, `lj_gc.c`, `lj_gc2.c`, and
`lj_arena.c`. Compiling `lib_jit.c` with that diagnostic produced only its
pre-existing declaration at line 1043, after this new integration section;
the new code itself retains declaration-before-statement style. The focused
deterministic race fixture and broader stock suites remain separate landing/CI
evidence.
