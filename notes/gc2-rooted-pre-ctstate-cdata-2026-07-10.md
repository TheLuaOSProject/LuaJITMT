# GC2 strong roots for pre-CTState cdata (2026-07-10)

Fixed-size cdata can be allocated and stored in ordinary Lua roots before the
FFI `CTState` has been initialized. Such an object has a fully published arena
cell and cdata header, but `lj_cdata_validate()` cannot resolve its `CTypeID`
until CTState exists.

The GC2 table/queue TValue validator incorrectly routed every cdata edge through
`lj_gc2_obj_valid_queued()`, which requires full CType validation. A strong
registry value created before CTState was therefore ignored during the first
full GC2 collection. Sweep reclaimed its arena cell while the registry retained
the cdata TValue; later FINREG allocation could reuse that address, turning the
stale registry edge into an accidental root for an unrelated cdata object.

The fix is one phase-aware authoritative-edge rule shared by root publication,
table publication, color/GC2 traversal, and worker barriers:

- while `ctype_state == NULL`, accept only an exact live, fixed, cell-aligned
  cdata allocation with a matching `LJ_TCDATA` header;
- variable, specially aligned, or huge cdata is rejected because its real
  allocation base cannot be proved without CType metadata;
- after CTState is published, always require normal `lj_cdata_validate()`
  CType/size validation, even for a fixed live arena cell.

Conservative raw stack and weak snapshots use the same cdata branch, so the
bootstrap exception does not become a permanent validation bypass. Their other
object types retain the stricter stale-snapshot validation appropriate to those
locations.

The traversal fixture now covers strong array and hash table edges, a child
published after its parent was scanned in MARK, a synchronous SWEEP-time store,
and key/value writes into an all-weak table during P_WEAK, all before CTState
initialization. The original FINREG/registry reproduction still forces a full
collection and verifies the same live `CTID_INT32` object remains. After FFI
initialization, the fixture temporarily assigns that live cell an invalid
`CTypeID` and verifies both public and GC2 edge validators reject it before the
valid ID is restored.
