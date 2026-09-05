# Independent correctness review of cdata first-function-method capture v2

Result: **no concrete blocker found** for frozen lj_meta.c SHA256
`c355b30c7978b31b499b8fe41fff1c03e8ff00f4a2a8293e1e4c74ee3823161e`.
This review covers the cdata-capture production patch against dd2c4391.
Evidence scope and limitations are in README.md; hashes are in
source-manifest.json. No shared source edit or new test hook was made here.

## Source authority and cleanup

- **Optional helper, lj_meta.c:501.** The caller has already acquired its exact
  authoritative receiver/key and opened source SMR before entering. The helper
  loads the current cdata base metatable under that same interval, so replacement
  cannot turn a captured pointer into an unobserved same-address incarnation.
  It acquires the exact table lease before any body lookup. Both existing pause
  hooks remain at their original meaningful boundaries.
- **Method lookup, :520.** The helper uses the held one-shot string getter. It
  retains existing table generation/retirement and key-lock/publication-claim
  refusal. The function tag test touches the copied word only; exact method
  admission follows before a header use or source-SMR release. It does not call
  a cached FFI C address or assume an immutable metatable/method.
- **Refusal, :514–525.** An unsuccessful mt admission owns no token. An absent,
  nonfunction or failed method admission releases the successful mt lease. The
  lease API initializes its output and cleans its internal scope on failure,
  so the helper's failed result admission does not leave a second obligation.
  The caller copies no optional method anchor when captured_method is false.
- **Capture, :544.** The original MetaSourceRef stack offsets and source-SMR
  loads remain unchanged. Anchor allocation still precedes admission. The extra
  work is excluded from function-environment mode and attempted only for the
  admitted cdata receiver. A changing original cell does not substitute a new
  receiver after the exact snapshot has been selected.
- **Publication, :611–635.** All receiver/key/method anchor words are copied while
  their exact scopes still retain them. Source SMR closes before publication.
  The method root barrier runs first under every relevant scope. Extra method
  and mt leases then release before the existing arbitrary-key barriers. The
  method_ready flag is assigned only after all scopes close, so neither caller
  owns a new token cleanup obligation.
- **First-hop dispatch, :805/:940.** The ready flag is used only for loop==0 on
  the initial non-table path. It denotes an already-rooted function, not an
  unleased raw method cache. Ordinary absent/table-valued/invalid/later-hop
  behavior remains the existing lookup. Existing method-frame construction and
  stack publication run only after the optional helper releases its scopes.
- **Aliases and setter, :987.** Optional capture writes only new private anchors;
  input/output aliases keep their words until the existing terminal path acts.
  RHS publication, saved stack offsets and caller-side RHS materialization are
  unchanged. The fixture verifies direct alias inputs/frame preparation, and
  root's Lua script exercises normal setter dispatch with aliased RHS.

## Allocation/exception boundary

The implementation includes the necessary compile-time guard:
`LJ_HASFFI && LJ_GC2_INTERNAL_ALLOCATOR_ONLY`. Under the default allocator policy,
lua_newstate forces the internal allocator and lua_setallocf is a no-op. A build
overriding that policy compiles the optional helper to refusal; it does not gain
extra leases across an arbitrary lua_Alloc callback. This source proof is not a
claim about the dormant custom-callback branch's own legacy safety.

For the function-valued method publication, lj_gc_pubroot reaches the GC2
function mark/rescan path. Function direct-body preservation may mark its proto
and upvalues but does not invoke Lua; semantic scheduling uses SSB/recovery.
Writer-side SSB recycling owns the worker token, converts bounded SSB identities
and may call gc2_grey_grow. That grow uses lj_mem_new_nothrow; allocator NULL
returns without lj_err_mem or queue replacement, after which the identity is
represented by recovery or sticky NO_RECLAIM. Successful growth accounting cannot
recursively acquire worker_active because the converter already holds it.

Therefore both successful growth and an allocator-NULL return reach the explicit
method/mt release tail. The new helper does not wait, allocate a Lua frame, parse
C types or call Lua while extra scopes are held. Fatal invariant aborts terminate
the process; they are not catchable exceptions with an unwind guarantee.

The ordering matters: generic table-valued key publication has a much broader
preexisting immediate-traversal fallback after failed rescan publication.
Publishing/releasing the extra method scopes first avoids extending those
obligations across that path. This change does not certify or repair every
existing public-barrier fallback. The full function queue route/source policy
audit is recorded separately in the packaged FFI diagnosis evidence.

## Functional observations relevant to the proof

All 13 exact-schedule modes pass with the supplied assert archive. In successful
ordinary captures, receiver/key/mt/method each acquire once; forced optional
failure explicitly invokes the expected older second-receiver path. The
metatable replacement schedule preserves source SMR before exact old-target
admission and selects the method from that retained table. Real queue pressure
observes all four arenas retained as 1/1/1/2 with no outer SMR, exact anchors and
the worker/consumer tokens held. Forced grow failure produces one recovery
identity instead of dropping publication or skipping cleanup. The method remains
callable after the observer deletes its original edges and a later full GC.

All 13 modes and the exact root-authored Lua/FFI semantic script in interpreter
and JIT modes also pass on an identical-source target-only Clang ASan build,
both with leak detection disabled and in a separate leak-enabled repeat. Actual
OS OOM and exhaustive concurrent collector schedules remain untested; the
failure hook exercises the allocator-failure return branch before allocation.
