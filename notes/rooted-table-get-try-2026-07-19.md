# Bounded rooted table point reads

## Purpose

The VM-event reader needs to distinguish a certified missing attachment from a
structural/lifetime collision without parking inside a callback eligibility
check.  The existing `lj_tab_gettv_rooted()` deliberately hides that distinction:
it waits until a point read completes and normalizes a terminally stale value to
nil.  `lj_tab_getstr_held_try()` is bounded, but it accepts a naked retained
table, handles only string keys, and does not publish an exact result root.

This tranche adds the production substrate used by the subsequent VM-event
reader cut:

```c
int lj_tab_gettv_rooted_try(lua_State *L, cTValue *tabroot,
                            cTValue *keyroot, TValue *outroot);
int lj_tab_getinttv_rooted_try(lua_State *L, cTValue *tabroot,
                               int32_t key, TValue *outroot);
```

Both use `LJ_TAB_ROOTED_GET_RETRY == -1`,
`LJ_TAB_ROOTED_GET_ABSENT == 0`, and `LJ_TAB_ROOTED_GET_FOUND == 1`.

## Bounded authority transaction

One attempt performs the following ordered transaction:

1. Snapshot the exact physical actor, TG, thread id, `lua_State` owner word, and
   universe using the keyed resolver's owner certificate.  The narrow existing
   main-state/main-TG exception remains valid while `lua_close()` runs user
   finalizers after `cur_L` has been cleared.  Secondary states do not receive
   that exception.
2. Enter GC2 SMR before loading either mutable TValue root.  Failed admission
   returns immediately; it does not wait or retry internally.
3. Acquire an exact table lease before dereferencing the table body, then an
   exact key lease before hashing or equality.  The fixed `int32_t` form has a
   non-collectable local key value, but passes through the same lease/result
   machinery.
4. Resolve the key once with `tab_resolve_current_keyed_held()`.  That resolver
   snapshots both array and hash roots as one logical generation, distinguishes
   stable-current separated-array/hash FORWARD from current colocated or live-
   handoff FORWARD, rejects retiring/KEYLOCK/finalizer-claim state, bounds
   collision traversal, and confirms the paired generation before returning.
   The rooted-read wrapper maps the stable raw FORWARD result to logical
   absence; the live forms remain RETRY.
5. Convert a nil value in either a structural array cell or an existing hash
   node to semantic ABSENT.  A non-nil result receives its own exact TValue
   lease before the source vector can leave SMR.
6. Reload and raw-compare every mutable input root which was used, and confirm
   the exact owner again.  Only then is the result release-published.  This
   ordering permits `outroot == tabroot`, `outroot == keyroot`, and the maximal
   parent/key/output alias without a post-publication input read.
7. Drop all vector pointer provenance, leave SMR, perform the stack/root GC2
   publication barrier while the exact result lease remains live, and finally
   release result, key, and table leases.

Production builds contain no callback or test callout in the authority
interval.  The path has no wait, allocation, safepoint, throw, Lua metamethod,
or structural help call.  It makes one bounded attempt and exports no
table-vector pointer.  Helper-enabled builds retain the pre-existing atomic
integral-array pause injection inside the shared resolver; this is a spin-only
test instrument, not a callback, and is absent from production builds.

## Exact classification

| Observation | Status | Output when authority is current |
| --- | --- | --- |
| Current non-nil array/hash value with valid exact result lease | FOUND | Exact value, release-published |
| In-range nil array cell | ABSENT | nil |
| Existing hash key whose value is nil | ABSENT | nil |
| Missing ordinary key, including nil or NaN | ABSENT | nil |
| Stable non-table parent root | ABSENT | nil |
| Closed SMR admission | RETRY | nil after a fresh owner check |
| Stale/transient table, key, or result lease | RETRY | nil |
| Stable-current separated-array/hash FORWARD with no live hand-off | ABSENT | nil |
| Current colocated FORWARD, retiring/mixed generation, live-handoff FORWARD, KEYLOCK, finalizer claim, malformed chain, or hidden `LJ_KEYINDEX` | RETRY | nil |
| Mutable table/key root changed before confirmation | RETRY | nil |
| Invalid operands, failed initial owner admission, or owner lost before terminal publication | RETRY | untouched |

The last row is a necessary safety qualification to the usual “non-FOUND is
nil” rule.  Once exact state authority is absent, even a nil store through a
possibly foreign stack pointer would be an unauthorized concurrent mutation.
Cleanup of already-acquired leases/SMR remains owned by the original physical
TLS actor and does not touch the output cell.

Unlike the allocation-capable keyed-slot insertion API, nil and NaN are not
invalid point-read keys.  Lua table lookup defines them as misses.  Conversely,
a table-tagged stale incarnation is not treated as a stable non-table parent:
failed exact table admission is RETRY, preventing address reuse from becoming a
false certified absence.

## Focused coverage

`tests/t-tab-rooted-get-try.c`, registered as `m5_tab_rooted_get_try`, covers:

- integer-array and generic string-key FOUND reads;
- GC-valued function result publication, removal of the source table edge, then
  a full collection and call through the returned stack root;
- in-range array nil, existing hash-node nil, ordinary missing, nil, NaN, and
  stable non-table ABSENT classifications;
- key/output, table/output, and table/key/output aliases;
- deterministic closed-SMR, table-lease, key-lease, and result-lease RETRY;
- stable-current separated-array FORWARD ABSENT, but current colocated
  FORWARD, published-retiring array root, and `LJ_KEYINDEX` RETRY;
- zero leaked SMR readers/root descriptors/root anchors and unchanged counters
  for `wait_no_l`, `wait_l`, and `store_wait_l` around every bounded call;
- wrong/unclaimed state refusal without modifying output;
- the exact main terminal-owner exception both synthetically and from a real
  `lua_close()` user finalizer.

The case uses `LJ_GC2_TEST_HELPERS` only to force allocator-admission outcomes
before calling the production API.  No fixture callback runs while the API owns
SMR or a lease.  The M5 aggregate includes the case so later VM-event reader
work cannot silently regress the substrate.

Validation completed for this tranche:

- `src/luajit tools/test.lua m5_tab_rooted_get_try` passed;
- the focused fixture compiled with `-Wall -Wextra -Werror` and passed;
- clean default and `LUAJIT_DISABLE_JIT` production builds passed with
  `XCFLAGS=-Werror`;
- a clean `LUAJIT_DISABLE_FFI` production build passed; its stricter `-Werror`
  variant reaches pre-existing no-FFI unused/switch warnings in `lj_gc2.c` and
  `lj_opt_fold.c`, before any diagnostic from this tranche; and
- the normal amalgamated static/dynamic build passed, after which a clean
  default `-Werror` build was restored.

## Follow-on use

The VM-event reader can now keep registry key, captured event table, event key,
and handler in ordinary stack roots and classify each hop without parking.  It
must still pair the resulting handler with the attachment sequence/generation
snapshot and cache-bit ordering described by the JIT event-clock tranche.  This
table API alone certifies only the exact point read; it does not certify the
separate attachment clock or registry-table identity across multiple calls.
