# Rooted table-reader cutover

This b1.2.1 tranche starts replacing table reads which retain a naked `GCtab *`
or vector-slot pointer across GC2 progress with an authoritative-root interface.
It extends the lifetime protocol described by the plans without modifying any
file under `plan/`.

## Reader contract

`lj_tab_gettv_rooted()` and `lj_tab_getinttv_rooted()` start from a live table
`TValue` root and copy the semantic result into an already enumerated output
root. A read:

1. snapshots and leases the exact table incarnation before dereferencing it;
2. snapshots and leases the exact key used for hashing and comparison;
3. enters table-vector SMR and resolves one paired current generation;
4. leases a copied GC result before closing the vector read interval; and
5. release-publishes that result while all required leases are still held.

Transient lifetime or generation conflicts close every lease/SMR scope before
using the existing non-blocking retry path. Stack-backed roots are represented
by saved offsets and restored after a retry can grow or move the stack. The
helper never exports a raw array/hash slot.

This is a GC2-only protocol. It does not re-enable or fall back to the legacy
collector.

## First converted consumers

- `unpack()` reads each requested integer key through the rooted helper and
  keeps its copied result in a TG root anchor until the Lua stack publication is
  complete.
- `table.concat()` retains each copied element in a TG root anchor. It
  reacquires that anchor after buffer growth before dereferencing a string and
  carries the protected element type to the error path instead of rereading a
  raced table slot.
- The generated-trace ABI of the five-argument `lj_buf_puttab()` helper remains
  unchanged. A separate rooted entry point serves interpreter callers.

Concurrent writes retain ordinary Lua race semantics: a reader may observe any
valid current value selected by its generation snapshot. It must not observe a
forwarding marker, publication claim, reclaimed allocation, mixed `TValue`, or
retired vector storage.

## Deliberate remaining debt

This is the first reader batch, not a claim that all table reads are converted.
The compatibility `lj_tab_gettv_forjit()` entry still begins with a naked table
pointer because existing generated traces pass that ABI and several pre-cutover
C consumers in the API, FFI and metatable paths still call it. It protects work
after entry but cannot retroactively prove the parent edge against pointer
reuse; those consumers and the JIT IR/helper operands still need a rooted
carrier.

Other important remaining conversions include global/environment lookup,
`table.insert()` shifting, `next()`/iteration, length discovery, VM/JIT fast
reads, and recorder samples. In particular, the default end index in
`unpack()` and `table.concat()` still uses the existing length reader before
their element-by-element rooted loop. Structural descriptor work is required
to make these paths generation-safe without serializing resize.

Focused C coverage injects parent, key, and result lifetime retries and checks
stack/TG-anchor outputs. Lua stress coverage races resize and GC against
`unpack()` and `table.concat()`, including nil and bad-element semantics.
