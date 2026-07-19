# Rooted structural table-reader checkpoint

Date: 2026-07-19

This b1.2.1 checkpoint moves the public `lua_objlen()` and `lua_next()` table
paths, the x64 base `next` fast function, and the x64 `BC_LEN`/`BC_ITERN`
interpreter paths onto authoritative `TValue` roots. It is a bounded lifetime
and structural-generation tranche. It is not a claim that all table/JIT
readers are nonblocking yet.

## Reader contract

`lj_tab_next_rooted()` and `lj_tab_len_rooted()` enter vector SMR before
loading the authoritative table root, acquire an exact lease for that table,
and inspect one paired array/hash generation. Traversal resolves the current
key and selects the returned key/value from that same generation. Length uses
a bounded binary/widening search and validates that the paired generation is
still current before returning. Neither helper waits, allocates, invokes Lua,
or calls an L-aware service while SMR and its exact leases are retained.

`next()` also leases its input key and both selected results. Stack-backed
roots and outputs are represented by saved offsets and restored after every
L-aware retry point. This permits the public input-key/output-key alias, a
receiver which is also the current table-valued key, and physical Lua-stack
relocation between attempts without retaining a naked stack address.

LuaJIT's array slot zero remains a real traversal key: a cursor at integer zero
advances to array slot one. It is deliberately excluded from the positive
integer length boundary. A stable cursor miss remains the stock "invalid key
to 'next'" error during live multithreading; active concurrent-mode
suppression is no longer part of the rooted public path. The resize stress
therefore accepts that natural error when a peer deletes the current cursor,
while continuing to reject malformed values or any other traversal failure.

The x64 base fast function, `BC_LEN`, and specialized `BC_ITERN` pass frame
slots rather than decoded `GCtab *` values and restore `BASE` after the helper
returns. The old unreachable inline `BC_ITERN` array/hash traversal and its
forwarding fallbacks were deleted, so future audits do not mistake dead raw
loads for an alternate active reader. Under Lua 5.2 semantics, table length
routes through the rooted meta helper before choosing raw length, avoiding an
unretained metatable-body inspection in the bytecode path.

## Evidence

`tests/t-tab-rooted-reader.c` has a one-shot retry hook which runs only after
SMR and all exact leases are closed. The hook resizes the table, physically
relocates the Lua stack when requested, and completes a full GC before the
reader's next attempt. Focused cases cover:

- empty, key-zero-only, and first-positive length boundaries;
- `lua_objlen()`, `lua_next()`, base `next`, `BC_LEN`, and `BC_ITERN` retries;
- input/output key aliasing and receiver/current-key aliasing;
- key-zero traversal and stable invalid-cursor errors during live MT; and
- an additional full collection after returned results are published.

An isolated clean helper build with
`LJ_GC2_TEST_HELPERS`, `LJ_TAB_TEST_HELPERS`, and `LUA_USE_ASSERT` passes this
fixture. Production/helper strict compilation of the touched API code, helper
strict compilation of the table and fixture code, and x64 DynASM expansion
also pass. The production table translation unit retains one unrelated
pre-existing `-Wextra -Werror` unused-parameter warning; the normal `-Wall`
production build is unaffected.

## Explicit residual debt

The rooted helpers' inner snapshot work is bounded, but their outer retry loops
still call `lj_tab_wait_l()` after failed SMR admission, transient lease state,
or a structural-generation collision. That service may yield/wait and is an
explicit violation of the final fully nonblocking target. It must be replaced
by a bounded status contract whose API/VM callers safepoint, redispatch, or
return a documented transient observation without holding structural or GC
authority. The present code only establishes that no wait occurs while SMR or
an exact lease is held.

Legacy/internal naked readers also remain: `lj_tab_next()`, `lj_tab_len()`,
`lj_tab_len_hint()`, `lj_tab_vmnext_forward()`, the x64 `vm_next` helper, and
JIT recorder/emitter paths that still depend on those ABIs or inline table
structure. They need their own rooted/generation-bearing IR and exit contracts;
routing the public interpreter paths in this tranche does not make those JIT
paths safe by implication.
