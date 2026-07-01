# 11. FFI Under Multithreading

Requirement 1 includes full FFI. The FFI's mutable shared state is the C
type system (`CTState`), the cdata heap objects, the finalizer registry
(`ffi.gc`), callbacks, and library handles. Requirement 4 lets us declare
classic `lua_CFunction` C modules unsafe-by-default; FFI itself must be
fully safe.

## 11.1 CTState today
`{ CTypeTab *tabh; CTypeTab *retiredtab; CTypeID top; global_State *g;
GCtab *miscmap; GCRef *metamap; CCallback cb; uint32_t hash[CTHASH_SIZE]; }`
- `tabh` is the acquire/release-published header for the append-only CType
  records (IDs are stable); `top` is the ticket allocator; `hash[]` are chain
  anchors threaded through CType.next (intra-record links).
- `miscmap` was the old catch-all GCtab for metatypes, callback slots, and
  callback blacklist entries.
  Current implementation note: metatables have moved to the CTID-indexed
  `CTState.metamap` side root, callback function slots live in the
  `CTState.cb.func` TValue side root, and callback blacklisting uses the
  fixed `CTState.cbblack` pointer set. `miscmap` remains as a small rooted
  table for the FFI function-pointer metatable and legacy compatibility.
- `cb` is scratch for callback setup.

## 11.2 Lock-free CTState (M7)
- **tab growth = RCU vector** exactly like J->trace (08 §8.3):
  `la_loadptr_acq` readers; grower allocates 2x, memcpys, rel-publishes,
  defer_free old. CType records are immutable once their ID is published.
  Current implementation note: the published pointer is `CTState.tabh`, a
  `CTypeTab` header carrying `sizetab`, retire metadata, and `tab[]`;
  `CTState` no longer carries `tab`/`sizetab` mirrors. Code must carry
  `CTypeID`s (including child/raw-child IDs) instead of recovering IDs by
  subtracting `CType *` values from a table base.
- **ID allocation = ticket**: original sketch was `id = la_add32(&cts->top,
  n)` then bounds check vs sizetab snapshot → grow loop. Current implementation
  uses a CAS reservation loop instead: acquire `tabh`, grow/publish a bigger
  header before retrying if the candidate ID does not fit, then CAS
  `cts->top` from `id` to `id+1`. This avoids advancing `top` past capacity if
  growth allocation throws or another grower wins publication. Records are
  written *before* publication; publication = the hash insert below (or, for
  anonymous types, the store of the ID into its referencing record/cdata —
  release). Reserved records that may have been initialized through a
  pre-growth slot are first copied into the current `CTypeTab` with
  `lj_ctype_publish()`.
- **hash chains**: CAS-prepend on `hash[h]` (CTypeID1 16-bit heads — CAS
  via the containing aligned 32-bit pair? CTypeID1 is uint16; widen
  `hash[]` to uint32, low 16 bits = id, padding for CAS).
  Duplicate-define race (two threads `ffi.cdef` same struct): both
  allocate records; CAS loser re-looks-up, finds winner, abandons its
  record (IDs leak a hole — append-only array tolerates gaps; add
  CT_ABANDONED info tag so ctype_get asserts don't trip).
  Current implementation note: anonymous/non-parser interning follows this
  winner relookup and `CTA_BAD` abandon path. Parser-created named cdefs now
  publish through `lj_ctype_addname_unique()`, which rechecks the name bucket
  before CAS-prepend and abandons duplicate-name losers. The parser body is
  still serialized by `parse_token`; this helper does not claim lock-free
  struct layout/rollback publication. The stock LuaJIT 2.1 internal
  `ffi.typeinfo(id)` diagnostic remains available and reads abandoned
  `CTA_BAD` holes through the same snapshot path as internal helpers and
  stock-visible FFI readers.
  Current rollback-reader bridge: direct layout readers, string-key cdata field
  lookup, numeric cdata element-size lookup, cdata pointer arithmetic, enum
  string constant resolution, and `ffi.C` namespace lookup take the parser token
  while snapshotting ctype layout, field metadata, pointer element size, or
  name/hash-visible constants, including the recorder's `ffi.new`, field,
  numeric-index, pointer arithmetic, enum-string, and `ffi.C` specialization
  paths. This prevents a failed `ffi.cdef()` from leaking a transient struct
  definition or constant to `ffi.sizeof()`, `ffi.new()`, cdata `__index`/
  `__newindex`, numeric indexing, pointer arithmetic, enum string casts, or
  `ffi.C`; narrowing the hot cdata reader fences is deferred to M9 cleanup.
  Current recorder string-ctype bridge: stable named typedef/tag/qualified/
  pointer/fixed-array strings in `ffi.sizeof()`, `ffi.alignof()`, and
  `ffi.new()` now use recorder-side snapshot/direct construction instead of
  entering the parser-token path. If the parser token is busy, recording aborts
  with CTBUSY; anonymous declarations, functions, VLA, and other full C
  grammar still fall back to the parser path to preserve stock semantics.
- `cts->L` field: delete; pass L explicitly (it's already threaded through
  most call paths; grep `cts->L` ≈ 15 sites, mechanical).
- **cparse (ffi.cdef)** mutates parser state + tab: the original sketch
  serialized the whole cdef through a tiny CAS token (`cts->parse_token`)
  because cdef is an initialization-time API. Current implementation decision:
  this is an accepted blocking exception, not a hot-path lock-free target.
  Waiters park on a futex in `lj_ctype_parse_lock()` on Linux; the fallback path
  uses the same native sleep/STOPREQ helper discipline instead of a raw CPU
  pause. Cleanup/naming can be revisited in M9 without changing the functional
  requirement.
- `ffi.typeof/metatype/istype` read paths: pure RCU reads. `ffi.metatype`
  one-shot rule enforced with CAS on the miscmap slot (raw nil→mt).
  Current implementation note: metatypes use a CTState side root
  (`metamap[raw_ctypeid]`) instead of structural `miscmap` negative-key
  insertion. `ffi.metatype()` CAS-publishes the metatable and both collectors
  scan the side root; `miscmap` remains only for the FFI function-pointer
  metatable.

## 11.3 cdata objects
Allocation: ordinary GC objects from non-traversable arenas (04 §4.2) —
except VLA/aligned payloads >16KB → huge. `cdata_newv`/`lj_cdata_new*`
(lj_cdata.h inlines) switch from lj_mem_newgco to the TG allocator; the
GCcdata header layout is unchanged so JIT CNEW/CNEWI lowering keeps its
offsets. Interior pointers held by C are invisible to GC — unchanged
contract (anchor the cdata).

## 11.4 ffi.gc finalizers
Today: setting a finalizer flips LJ_GC_CDATA_FIN in marked and registers
in `ctype_state finalizer table` (lj_cdata.c / cdata_setfin via miscmap-
adjacent tab `GCRoot CTFIN`? verify: `grep -n finalizer lj_cdata.c
lj_clib.c lib_ffi.c`). Under MT: gcflags bit LJ_GCF_FINREG (04 §4.7) +
insert into the global FINREG registry consumed by P_WEAK (05 §5.8).
Registry = the same concurrent-table machinery (a hidden GCtab keyed by
cdata, value=finalizer) — reuse, don't invent. Order: registration order
preserved per 05 §5.8. `ffi.gc(cd, nil)` clears flag + table entry; race
with collection resolved by the registry delete CAS (collector claims the
entry by replacing value with KEYLOCK sentinel before queueing).
Current implementation note: FINREG value slots use a claim sentinel and
`TValue` CAS for registration replacement, explicit clear, and collector
claim/delete. Explicit `ffi.gc(cd, nil)` on a cdata with no registry entry
returns before structural insertion. Enabled missing-key insertion first tries
a lock-free empty-anchor path: CAS the target anchor value from nil to the
FINREG claim sentinel, publish the cdata key, then publish the finalizer
value. Collision insertion in the current hash generation claims a free node,
CAS-prepends it as a claim/nil-key placeholder, then publishes the
key/finalizer. If the current generation has no free node, `CTState.fin_head`
CAS-publishes a new weak-key FINREG generation instead of resizing/copying the
old table. The generation publisher waits out visible FINREG claim sentinels
across all generations, rechecks for the cdata key, creates a private
generation with the new key already claimed, and publishes it as the newest
head. Losing publish CAS attempts yield through the no-L FINREG wait helper
before retrying. Legacy GC and GC2 root, identify, traverse, and close-drain
every FINREG generation; traversal waits out claim sentinels before marking
finalizer values. The initial FINREG generation is created during
`lj_ctype_init()` and published directly through `CTState.fin_head`; the
legacy `GCROOT_FFI_FIN` bootstrap root has been removed. Recorded
`ffi.gc()`/ctype-`__gc` finalizer registration now emits
`IRCALL_lj_cdata_setfin` and is covered by traced direct, nil-clear, and
metatype tests plus the multi-threaded default-JIT FINREG stress. Normal
`mmudata` cdata finalization now rechecks the generation list and claims the
slot before clearing/calling it; close-time cdata drain remains exclusive.
Membership and ordering remain legacy-owned until the planned
FINREG/finqueue dispatch lands.

## 11.5 Calls & callbacks (native state discipline)
- **FFI call out** (interpreter `->vm_ffi_call`, JIT IR_CALLXS): wrap with
  native enter/leave (05 §5.4.3). Cost: two byte-stores + a poll-check on
  return — significant for tiny leaf calls (ffi_struct bench guards this).
  Current bridge: interpreted FFI calls enter native state around
  `lj_vm_ffi_call(&cc)`, preserve the native-leave action mask, and check
  `HS_STOPREQ` after callback blacklist handling and result conversion have
  restored local FFI bookkeeping. Safety-first default: cdata function calls do
  not record `IR_CALLXS`, so ordinary FFI calls naturally use the interpreted
  native-state path. `LJ_FFI_RECORD_CALLS` hard-fails at compile time until
  `IR_CALLXS` has an explicit native-state enter/leave protocol. This removes
  the previous requirement that users identify blocking functions with
  `ffi.blocking(fn)` before GC/shutdown can progress while C is blocked. The
  obsolete public marker entry point has been removed from the supported FFI
  surface; internal callback blacklisting remains for callback safety.
  Callback entry applies the same freshness rule: pre-existing sticky
  `TGF_STOPREQ` is tolerated, but a STOPREQ newly acknowledged while the
  carrier was native interrupts before the Lua callback body runs.
  Traced C-call throughput stays deferred behind the native-state protocol.
- **FFI library C spans**: `ffi.copy()`, `ffi.fill()`, and the unbounded
  `strlen()` scan behind `ffi.string(ptr)` enter native state for the raw C
  library work, then apply the same fresh-STOPREQ rule as interpreted C calls.
  Argument conversion, C type lookup, Lua string allocation, and GC checks stay
  in VM state to preserve the normal LuaJIT object/allocator invariants.
  Recorder support keeps small constant `ffi.copy`/`ffi.fill` operations as
  bounded inline loads/stores, but aborts recording before emitting raw
  `memcpy`/`memset`/`strlen` calls; those forms fall back to the interpreter
  native-state path until a JIT native-call bridge exists.
- **Callbacks (C→Lua)**: callback entry (lj_ccallback.c enter) runs
  `lj_native_leave` on the carrier thread; if the OS thread is foreign
  (created by C, never attached), auto-attach a TG (luaMT_attach path, 09
  §9.9) the first time — callbacks become legal from any thread (today:
  single VM thread assumption). Callback exit re-enters native state.
  CCallback cb scratch in CTState → per-TG copy (it's setup scratch;
  move `cb` fields used at runtime (mcode slots) read-only after init). Original
  sketch state had callback setup share the cdef token; the current
  implementation does not use that bridge.
  Current x64/Linux implementation note: runtime callback scratch now lives in
  `TGState`, callback slot arrays are preallocated at `luaopen_ffi()`, setup
  reserves a free slot with an owner-pointer CAS, lazily creates a hidden
  carrier only after a free slot is observed, stores the callback function into
  `CTState.cb.func[slot]`, then release-publishes `cbid`. Legacy GC and GC2
  scan `cb.func` as a CTState side root. Owned callback free preserves the
  original `cbid`→function→owner release order; disowned callback free nils the
  function before release-clearing `cbid` and performs no owner write after the
  slot is reusable. Callback mcode is allocated at `luaopen_ffi()` before
  concurrent callback creation, so the former `misc_token` lazy-init bridge is
  gone.
  x64 callback entry spills ABI arguments and selects the current attached TLS
  TG with `lj_ccallback_prepare()`. Callback slot owner entries now hold hidden
  attachable carrier `lua_State *` roots: attached callers still run through
  their current TG, while TLS-less foreign pthreads auto-attach the hidden
  carrier for the callback and auto-detach on normal return or unwind.
  Callback-calling C functions are blacklisted through a fixed CTState
  pointer-key CAS set, not arbitrary `miscmap` keys.
- **errno/GetLastError save** (lj_ccall) is already per-call/TLS — audit.

## 11.6 Pinning rules for C-held references
Original plan state: restate the API contract (05 §5.7.3): C code may hold
GCobj pointers only while they're anchored (stack/registry/upvalue of an
active cfunc, or a cdata payload that itself is anchored). FFI buffers passed
to async C (e.g. io_uring) must be anchored for the duration — user
responsibility; provide `ffi.pin(obj) -> pin` / `pin:release()` helper as a
registry-table insert/remove convenience, built on the concurrent registry
table.

Current implementation note: `ffi.pin(obj)` uses a dedicated `UDTYPE_FFI_PIN`
userdata whose hidden payload is one `TValue`, with the pin metatable rooted in
`CTState.pinmt`. `pin:release()` release-stores nil into that payload, and both
GC engines acquire-load and mark the payload while the pin userdata is
reachable. This preserves the original pinning contract without mutating a
shared registry table during M7; a future concurrent-registry implementation
can replace the representation if needed. Performance cleanup stays deferred
to M9.

## 11.7 clib / dlopen
`ffi.load`: dlopen is thread-safe; the CLibrary cache table is a normal
concurrent table; symbol lookup writes (lj_clib_index caching into
cl->cache GCtab) are plain table sets. One-time init of `CLIBLINKMAP` etc.
under the cdef token.

Current implementation note: the original normal-concurrent-`GCtab` target
above is preserved, but the earlier per-`CLibrary` `cache_token` bridge has
been removed. Runtime cache misses now publish immutable `CLibCacheEntry`
records to `CLibrary.cache_head` with a CAS prepend, after rooting the resolved
TValue on the active Lua stack. `lj_clib_cache_get()` is the shared acquire
lookup path for the interpreter and recorder. A successful cache-head CAS marks
the raw side-entry allocation before key/value barriers; CAS losers yield
through a no-L native sleep helper before retrying. Legacy GC and GC2 traverse
the side cache as a CLibrary userdata root. `__gc` unload and the
`lj_udata_free()` backstop detach side entries and retire them through the GC2
SMR drain; live-state retirements publish key/value root barriers, legacy GC
and GC2 also mark retired side entries as raw roots until a completed handshake
epoch reclaims them, and state close force-drains any
remaining retired entries.
Performance/cleanup of this side cache, or folding it back into a full
concurrent table protocol, is deferred to M9.

## 11.8 Tests
t-ffi-01 concurrent cdef of distinct types; t-ffi-02 same-struct cdef race
(both threads get identical ctype id semantics); t-ffi-03 cross-thread
cdata sharing + ffi.gc firing exactly once under churn; t-ffi-04 callback
from spawned thread; t-ffi-05 blocking C call (sleep via ffi) does not
stall other threads' GC progress (asserts a full cycle completes while one
thread sleeps in C); t-ffi-06 struct-field hammering from 4 threads,
TSAN-clean (cdata accesses are user-racy: M-5 only guarantees the *VM*
stays safe — test asserts no crash, values within written set per field).
Current implementation note: the same-struct/same-name cdef race also exposed
a spawned-thread stack ownership bug before the FFI parser or ctype table
itself failed. `tests/t-ffi-cdef-dup-stack.lua` keeps the original t-ffi-02
intent, but forces worker stack growth while racing duplicate `ffi.cdef()` plus
string `sizeof`/`typeof`; run it through
`tools/ci/lua_test.sh m7_ffi_cdef_dup_stack`.
