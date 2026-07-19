# Bounded rooted and clocked VM-event preparation (2026-07-19)

## Scope

This tranche replaces the naked table-vector reads in
`lj_vmevent_prepare()` with a bounded, rooted, tri-state observation. It is the
reader half of the previously landed `jit.attach()` publication clocks. It does
not yet change production callback delivery or the JIT event-session schema;
that is the next cut, where INITIAL attachment generation zero needs an
explicit session representation rather than a fabricated generation.

The legacy internal ABI remains:

```c
ptrdiff_t lj_vmevent_prepare(lua_State *L, VMEvent ev);
```

It is now a wrapper over:

```c
int lj_vmevent_prepare_try(lua_State *L, VMEvent ev,
                           LJVMEVENTPrepareResult *result);
```

The status is `RETRY`, `ABSENT`, or `READY`. The result contains the saved
argument base, exact clock slot, all three attachment-clock fields, and one of
`INVALID`, `INITIAL`, `PUBLISHED`, or `UNCLOCKED`. Every RETRY resets the
snapshot, reports `INVALID`, and leaves the slot as `NONE`. A compile-time
no-JIT build reports accepted results as `UNCLOCKED`; calling `jit.off()` at
runtime does not bypass clocks.

## Nonwaiting canonical registry key

The former reader called `lj_str_newlit(L, "_VMEVENTS")` for every event.
Although the string normally already existed, canonical string lookup can
enter `strtab_wait()` behind a resize or sweep owner. That is incompatible with
a bounded observational callback path.

State bootstrap now calls `lj_vmevent_init()` immediately after
`lj_str_init()`. It interns `_VMEVENTS` once, marks the canonical string fixed,
and release-publishes its pointer in a new append-only tail field of the main
TG. Every secondary TG initializes its copy to NULL. Fixed lifetime makes the
main-TG pointer a permanent comparison/reference value, not a separate GC
edge. The reader acquire-loads it and publishes a stack TValue root without
touching the string table. Disabled-vmevent builds make initialization a no-op.

The focused fixture claims the current string-table resize word and performs a
successful handler preparation while the claim remains held. Reintroducing
runtime interning would hang this case and hit its timeout.

## Stack and lifetime transaction

After the one possible stack reservation and before clock A, preparation
constructs this exact enumerated layout:

```text
[handler] [FR2 when enabled] [fixed registry key] [captured event table]
```

Every address is retained as a saved stack offset. Test hooks may relocate the
stack or leave scratch values above the roots; the reader restores the root-end
offset after every hook and never carries a raw stack address across one.

The first `lj_tab_gettv_rooted_try()` reads
`registry["_VMEVENTS"]` into the captured-table root. The second
`lj_tab_getinttv_rooted_try()` reads the event hash from that same captured
root into the handler root. Both are raw table operations: no `__index` or
`__newindex` call is possible. Every table/key/result lease and SMR interval is
closed inside the bounded getter before the next step or test hook.

`READY` drops the two temporary lookup roots and leaves exactly handler plus
FR2, preserving the historical argument-base geometry. `ABSENT` and `RETRY`
restore the exact entry top. A returned collectable handler is therefore a real
published stack root, not a C-local pointer surviving past reclamation.

## Clocked algorithm

For JIT-capable builds, the reader:

1. derives and validates the exact event lane;
2. reads canonical clock snapshot A (`INITIAL` or `PUBLISHED`);
3. performs the two bounded rooted table reads against one captured event
   table;
4. for a miss or non-function, clears only this event's cache bit with one
   acquire-release atomic fetch-and operation;
5. executes an acquire fence and reads clock snapshot B; and
6. accepts only if A and B have the same canonical classification and identical
   `{sequence, next_generation, generation}`.

Odd, malformed, changing, or classification-changing clocks return RETRY. If
the reader cleared the mask bit before discovering that retry, it restores only
that bit with one acquire-release atomic fetch-or. The single RMW operations
replace the inherited unbounded `vmevmask_update()` CAS loop and preserve every
unrelated concurrent bit.

`INITIAL` is an accepted stable state. In particular, `luaL_newstate()` installs
its default ERRFIN handler directly before any attachment writer advances the
clock. Raw registry mutation remains deliberately unclocked and racy, but all
values it exposes are lifetime-safe. A registry replacement after the first
hop does not redirect the in-progress read: the captured old table remains the
one authority through handler lookup and snapshot B.

The cache ordering closes the lost-clear race with the writer's existing
publication order:

```text
semantic table CAS -> vmevmask = NOCACHE -> generation -> even sequence
```

Whether the writer invalidates just before or just after the reader's bit
clear, a changed B forces the reader to restore the event bit and return RETRY.

## Compile-time no-JIT and disabled-vmevent behavior

A no-JIT build bypasses clocks but still uses both bounded rooted hops. On a
first miss it clears the event bit, then performs a fresh full registry and
event-table lookup. A second `READY` or `RETRY` restores the bit and returns
RETRY; only a second certified miss returns ABSENT. This prevents an unclocked
`jit.attach()` publication from being lost behind the clear.

A disabled-vmevent build does not allocate or publish the fixed key. Direct
calls to the companion API report a stable `ABSENT/UNCLOCKED` result for a
valid event and leave the stack unchanged. Existing send macros remain
compiled out and `jit.attach()` retains its disabled error behavior.

## Test hooks

Helper builds expose one-shot hooks at:

- `AFTER_CLOCK_A`;
- `AFTER_REGISTRY_LOOKUP`;
- `AFTER_EVENT_LOOKUP`;
- `BEFORE_MASK_CLEAR`; and
- `AFTER_MASK_CLEAR`.

The hook and userdata are taken and cleared before callback invocation, so a
reentrant prepare cannot recursively trigger the same hook or pair mismatched
state. A staged fixture explicitly rearms itself for the next point. Every hook
asserts zero leaked SMR readers, an idle root descriptor, an unchanged root
anchor depth, and the live fixed-key stack root.

## Focused coverage

`tests/t-vmevent-prepare-clocked.c`, registered as
`m5_vmevent_prepare_clocked`, covers:

- fixed key type, bytes, pointer identity, fixed mark, and secondary-TG NULL;
- default ERRFIN as `READY/INITIAL` and raw initial READY/ABSENT/non-function;
- all five BC/TRACE/RECORD/TEXIT/ERRFIN lanes as published observations;
- same-value attach publication and runtime `jit.off()` remaining clocked;
- exact legacy wrapper stack geometry;
- raw metatable bypass and a missing `_VMEVENTS` registry entry;
- forced stack relocation at clock A and one-shot recursive preparation;
- registry replacement plus full GC after the first hop, proving one captured
  table and retained old handler;
- isolated lease-admission RETRY for the registry parent, fixed key, first-hop
  event-table result, captured event-table second-hop parent, and handler
  result, plus closed-SMR RETRY, unchanged top/mask, and no wait-counter
  movement;
- a real writer after handler lookup, before mask clear, and after mask clear;
- post-lookup exact detach plus full GC;
- bit-only retry restoration preserving a concurrently changed unrelated bit;
- odd and malformed attachment clocks;
- detach of the last table/global edge, full GC, then invocation through the
  prepared handler stack root; and
- preparation from a real `lua_close()` finalizer.

The registered case builds and runs four profiles: default, compile-time
no-JIT, disabled-vmevent, and both disabled. The default production build is
also checked with `-Werror`; the normal amalgamated build and the pre-existing
`m6_jit_token` stack-growth/closed-SMR cases remain regression gates.

## Follow-on

Production VM-event macros still consume the legacy argbase wrapper and
serialize the mutable callback path. The next tranche will carry this result's
exact handler root, slot, classification, and full attachment snapshot into an
immutable event session. That work must add an explicit INITIAL attachment
state to the session schema before accepting generation zero; this reader does
not weaken the current nonzero published-generation invariant.
