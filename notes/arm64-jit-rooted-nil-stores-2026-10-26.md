# Rooted nil stores

The experimental ARM64 LOOP/FORL path now records nil assignments through
`lj_tab_storenil_rooted`. It deletes present keys and accepts absence only with
a current NULL metatable, or after this same operation already observed a
present value or committed a deletion. Other absent assignments side-exit
into ordinary Lua semantics. Nil/NaN assignment keys and internal table
sentinels remain rejected. New-key insertion and general metamethod traces
are not opened.

The helper reserves two nil anchors before retaining table/key inputs, then
leases, confirms and publishes their exact values before releasing temporary
authority. Cooperative retries retain the anchors, not incoming stack/scratch
addresses. The held resolver copies the old value inside a paired-generation
SMR interval; a found node with nil value is semantic absence. A retained hash
key does not authorize skipping `__newindex`.

Present values use the existing keyed CAS. Its private optional commit output
distinguishes an uncommitted retry from a committed old-generation CAS whose
post-currentness check returns STALE. The helper re-resolves the current
generation before completion. An overlapping competing deletion may satisfy
the same operation without invoking a newly installed metamethod. Stable
FORWARD markers go through storage repair and re-resolution, never a public
new-key setter. Retry/error cleanup, nominal errno preservation, and string
key NOMM invalidation follow the established rooted-store protocol.

The recorder uses the existing positive nil/no-metatable sample on ARM64 to
avoid unsafe metatable sampling. Runtime absence is checked again by the
helper. Its two distinct TMPREF inputs, CARG pair, immediate status guard,
input provenance, allocated registers and post-effect snapshots are checked
independently by semantic and post-RA admission. There are no emitter changes.
The helper and nil-store recording path are shared with x64; x64 keeps its
existing conservative metamethod sampling rule.

## Validation

The final focused regression ran from base
`843d861b42f357250556224cec9cd97dd6a4c389`, with frozen binary diff SHA-256
`5ad86e219689ea4d651347b05730593661e31c74d472135e64a86d1a5c97212e`.
It passed:

- The complete rooted-GC contract on ARM64 and arm64e/BTI, assertion and
  release. Each profile's nil matrix observed 590 present-key deletions under
  a published JIT frame. Dedicated LOOP/FORL clones observed 20/30 native
  present deletions and passed independent malformed-graph/register checks.
- LOOP/FORL numeric, boolean, string, table, function, thread, userdata,
  cdata and lightuserdata keys; no raw ASTORE/HSTORE/FSTORE operations;
  deletion after insertion/collection; exact metamethod call counts; and
  exits through the nil helper's status-guard snapshots.
- Direct present/absent helpers, retained nil hash nodes and empty array
  slots with metatables, invalid keys/internal markers, reservation failures,
  source-cell removal, stack relocation, GC/resize, errno and error rollback.
- Deterministic post-CAS successor publication and competing deletion plus
  metatable installation. Synthetic stable array/hash FORWARD cases repair
  storage without inserting a key. These are controlled interleavings, not
  sustained concurrent mutation stress or OOM-during-repair evidence.
- Existing CAS publication/forwarding fixtures on ARM64 and signed
  x86-64/Rosetta. The signed x64 run also passed rooted stores/readers and the
  raw nil-store regression, including `__newindex` bypass for raw stores.
- ARM64 and signed x64 stock source and original-v2 import: 509 cases per
  corpus on each target. The experimental ARM64 assertion profile was restored.

The FORL fixture skips deletion in its first interpreted body iteration and
refills the key outside the trace before every present-key invocation. This
prevents interpreted deletion from satisfying the native claim. The native
observer counts only actual present-key helper operations with a published
JIT frame. FORL exit counts varied with diagnostic timing (normally 11 over
20 present-key calls, versus 20 for LOOP); the exact reason for deferred entry
was not established. The test requires native progress rather than a fixed
FORL cadence. All successful calls still have the required Lua result.

Initial isolated drafts exposed and corrected a nil-valued-node/metamethod
bug, an inappropriate ACTIVE-descriptor assertion for a nil CAS, shared test
counter assumptions, and an admission fixture that warmed its supposedly
unpatched bytecode clone. Those drafts were not committed.

Driver: `/tmp/ljmt-nil-store-regression-final.sh`. Logs:

- `/tmp/ljmt-nil-store-regression-final.log`
- `/tmp/ljmt-nil-store-rooted-gc-contract-final.log`
- `/tmp/ljmt-nil-store-arm64-stock-source-final.log`
- `/tmp/ljmt-nil-store-arm64-stock-import-final.log`
- `/tmp/ljmt-nil-store-shared-regression-final.log`

After the frozen run, two unrelated pre-existing line wraps were restored in
`lj_tab.c`, with identical whitespace-normalized source SHA-256
`10b1242ec36c218257728e40e9676bf00c7aef5774dc9d9f6828d1006b17e056`.
Documentation was then added. This is a focused checkpoint after the float
CALLXS combined validation; it does not claim a new complete experimental/
bootstrap, sanitizer or performance result.
