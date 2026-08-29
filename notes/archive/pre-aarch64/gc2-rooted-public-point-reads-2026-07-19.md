# GC2 rooted public point reads

Date: 2026-07-19

This b1.2.1 checkpoint extends the rooted table-reader protocol into public C
API and FFI initialization paths. It does not modify `plan/`, does not restore
the retired collector, and does not claim that structural readers or generated
code are complete.

## Source-edge capture

An exact child lease alone is not enough when the authoritative root can be
replaced concurrently: loading a pointer before entering SMR permits the old
allocation to be retired, reclaimed, and reused at the same address before
admission. Rooted point lookup now enters source SMR before copying both its
table and key roots, acquires the exact leases inside that interval, and keeps
the same interval through the paired table-vector snapshot.

The mutable global registry root uses the same ordering. Its snapshot is
admitted under source SMR and transferred first into the enumerated per-actor
scratch root. SMR and the allocation lease are then released before the value
is copied into a reserved durable anchor. This intermediate root is necessary:
root-publication debug hooks may run a complete collection, so neither an SMR
reader nor a rescue lease may span the hook.

Global and C-function environment pseudo-indices now materialize their source
edge under SMR plus an exact child lease before publishing the traditional TG
scratch `TValue`. Rare public raw reads clone that already-enumerated,
owner-private scratch root into a durable anchor so helper retries cannot
change the semantic parent.

## Converted consumers

- `lua_rawget()` passes the real enumerated table root and uses the consumed
  key slot as the result root.
- `lua_rawgeti()` captures its parent before extending `top` and publishes a
  result slot before rooted lookup.
- The rooted helper contract explicitly permits output aliasing key or parent.
  All input snapshots, admission failures, generation retries, and internal
  marker rejection happen before terminal result publication. This covers the
  valid extreme `lua_rawget(L, -1)`, where table, key, and output are one slot.
- `luaL_newmetatable()` captures the replaceable registry edge under source
  SMR and performs its initial winner lookup from registry/key/output anchors.
- `luaL_testudata()` roots the registry result, then reloads the selected API
  edge under source SMR and leases that exact userdata incarnation through its
  metatable and body access. This includes concurrently replaceable C upvalues.
- FFI module registration roots `_LOADED` and its result in distinct stack
  slots and starts lookup from the live registry root instead of passing a
  naked registry table pointer.

Concurrent mutation retains normal racy Lua semantics: the caller may observe
any valid value selected by a coherent current-generation lookup. It must not
observe a mixed `TValue`, retired vector storage, internal forwarding/claim
markers, or the body of a reclaimed same-address predecessor.

## Evidence

The checkpoint has focused coverage for key/result aliasing with forced
admission retry, table/key/result self-aliasing, ordinary and pseudo-index
`lua_rawget`/`lua_rawgeti`, concurrent public point reads during resize, exact
new-metatable winner handoff across forced full collections, C-closure upvalue
replacement, FFI registration, and full GC2 traversal. Strict GCC builds with
assertions and the relevant fault-injection helpers pass.

## Deliberate remaining debt

This is a correctness checkpoint and still uses `lj_tab_wait_l()` for
transient retry. The later mechanical zero-wait tranche must replace that with
bounded try/restart, helping, or conservative defer without weakening the
source-edge proof.

Table-valued `__index`/`__newindex` chains, `lua_next`, length, `table.insert`
shifts, live x64 `TGETR` and Lua `rawget`, GGET environment capture, recorder
sampling, and generated-trace operands remain raw-reader debt. Structural
length and iteration require a generation-bound read transaction; resize and
range mutation ultimately require durable helpable descriptors rather than
the current unhelpable structural owner. Active-MT JIT reads must initially use
the rooted helper universally until explicit table escape tracking can prove a
trace-local fast path safe.
