# 11. FFI Under Multithreading

Requirement 1 includes full FFI. The FFI's mutable shared state is the C
type system (`CTState`), the cdata heap objects, the finalizer registry
(`ffi.gc`), callbacks, and library handles. Requirement 4 lets us declare
classic `lua_CFunction` C modules unsafe-by-default; FFI itself must be
fully safe.

## 11.1 CTState today (lj_ctype.h:174–183)
`{ CType *tab; CTypeID top; MSize sizetab; lua_State *L; global_State *g;
GCtab *miscmap; CCallback cb; CTypeID1 hash[CTHASH_SIZE]; }`
- `tab` is an append-only array of CType records (IDs are stable);
  `top`/`sizetab` grow it; `hash[]` are chain anchors threaded through
  CType.next (intra-record links).
- `miscmap` maps -CTypeID→metatable and callback slots→funcs — an ordinary
  GCtab (becomes concurrent for free via 06).
  Current implementation note: metatables have moved to the CTID-indexed
  `CTState.metamap` side root; `miscmap` remains for callback slots, the
  function-pointer metatable, and legacy compatibility.
- `cb` is scratch for callback setup.

## 11.2 Lock-free CTState (M7)
- **tab growth = RCU vector** exactly like J->trace (08 §8.3):
  `la_loadptr_acq` readers; grower allocates 2x, memcpys, rel-publishes,
  defer_free old. CType records are immutable once their ID is published.
  Current implementation note: the published pointer is `CTState.tabh`, a
  `CTypeTab` header carrying `sizetab`, retire metadata, and `tab[]`;
  `cts->tab`/`cts->sizetab` are compatibility mirrors, not the reader
  correctness boundary.
- **ID allocation = ticket**: original sketch was `id = la_add32(&cts->top,
  n)` then bounds check vs sizetab snapshot → grow loop. Current implementation
  uses a CAS reservation loop instead: acquire `tabh`, grow/publish a bigger
  header before retrying if the candidate ID does not fit, then CAS
  `cts->top` from `id` to `id+1`. This avoids advancing `top` past capacity if
  growth allocation throws or another grower wins publication. Records are
  written *before* publication; publication = the hash insert below (or, for
  anonymous types, the store of the ID into its referencing record/cdata —
  release).
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
  struct layout/rollback publication.
- `cts->L` field: delete; pass L explicitly (it's already threaded through
  most call paths; grep `cts->L` ≈ 15 sites, mechanical).
- **cparse (ffi.cdef)** mutates parser state + tab: serialize whole cdef
  through a tiny CAS token (`cts->parse_token`) — cdef is initialization-
  time API; token-not-lock per 02 §2.2 (busy ⇒ la_cpu_pause retry loop is
  acceptable here? That IS waiting on a specific thread. DECIDED: cdef
  parks on a futex — cdef is explicitly allowed to block, added to the
  §2.2 whitelist; it is never on a hot path).
- `ffi.typeof/metatype/istype` read paths: pure RCU reads. `ffi.metatype`
  one-shot rule enforced with CAS on the miscmap slot (raw nil→mt).
  Current implementation note: metatypes use a CTState side root
  (`metamap[raw_ctypeid]`) instead of structural `miscmap` negative-key
  insertion. `ffi.metatype()` CAS-publishes the metatable and both collectors
  scan the side root; `miscmap` remains for callback function slots.

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
Current implementation note: existing `GCROOT_FFI_FIN` value slots use a
FINREG claim sentinel and `TValue` CAS for registration replacement, explicit
clear, and collector claim/delete. Close-time table disable now runs without
`fin_token` after `lua_close()` shutdown has joined secondary threads.
Explicit `ffi.gc(cd, nil)` on a cdata with no registry entry now returns before
that fallback. Enabled missing-key insertion first tries a lock-free
empty-anchor path: CAS the target anchor value from nil to the FINREG claim
sentinel, publish the cdata key, then publish the finalizer value. Collision
insertion in the current hash generation claims a free node, CAS-prepends it
as a claim/nil-key placeholder, then publishes the key/finalizer. Legacy GC and
GC2 traversal wait out that claim sentinel for the hidden FFI finalizer table.
Active lock-free FINREG insert claims are counted separately, and the remaining
structural fallback waits for that count to drain after taking `fin_token`;
new lock-free insert claims decline while the token is held.
Table growth/full-table resize still uses the `fin_token` structural fallback
because `lj_tab_newkey()` is not yet a multi-writer-safe resize path: the
current inserter still owns `freetop` and resize publication as a single
writer. Removing this last bridge requires either FINREG-specific resize
semantics that all lookups understand, or the original
`NHdr`/KEYLOCK/freecount/CAS-prepend table protocol from §6.3. Do not replace
the token with an unlocked call to the legacy table inserter. Recorded
`ffi.gc()`/ctype-`__gc` finalizer registration is currently NYI for the same
reason: JIT-enabled code falls back to the interpreter until FINREG insertion
is fully lock-free. Normal `mmudata` cdata finalization and close-time cdata
drain now share the same owned slot-clear/callback helper, but membership and
ordering remain legacy-owned until the planned FINREG/finqueue dispatch lands.

## 11.5 Calls & callbacks (native state discipline)
- **FFI call out** (interpreter `->vm_ffi_call`, JIT IR_CALLXS): wrap with
  native enter/leave (05 §5.4.3). Cost: two byte-stores + a poll-check on
  return — significant for tiny leaf calls (ffi_struct bench guards this).
  Optimization (DECIDED, v1): calls *recorded as fast* (no callback risk,
  trivial signatures — the JIT already classifies) skip native-state and
  instead rely on poll-at-backedge; only interpreter calls and JIT calls
  to unanalyzed pointers do enter/leave. The GC then must tolerate a
  mutator stuck ≤1 FFI call without acking: it already does — handshakes
  wait, hard-pacing throttles only the offender (05 §5.4.2/5.11). A truly
  blocking C function called via the fast path can stall GC completion:
  document `ffi.blocking(fn)` wrapper that forces native-state per call.
  Current bridge: interpreted FFI calls enter native state around
  `lj_vm_ffi_call(&cc)`, preserve the native-leave action mask, and check
  `HS_STOPREQ` after callback blacklist handling and result conversion have
  restored local FFI bookkeeping. `ffi.blocking(fn)` currently marks the
  function pointer in the existing CTState pointer-key blacklist and returns
  the same cdata function, forcing future calls through the interpreted
  native-enter path instead of recorded `IR_CALLXS` fast calls.
- **Callbacks (C→Lua)**: callback entry (lj_ccallback.c enter) runs
  `lj_native_leave` on the carrier thread; if the OS thread is foreign
  (created by C, never attached), auto-attach a TG (luaMT_attach path, 09
  §9.9) the first time — callbacks become legal from any thread (today:
  single VM thread assumption). Callback exit re-enters native state.
  CCallback cb scratch in CTState → per-TG copy (it's setup scratch;
  move `cb` fields used at runtime (mcode slots) read-only after init,
  setup token = cdef token).
  Current x64/Linux implementation note: runtime callback scratch now lives in
  `TGState`, callback slot arrays are preallocated at `luaopen_ffi()`, setup
  reserves a free slot with an owner-pointer CAS, stores the callback function
  before release-publishing `cbid`, and free clears `cbid` before niling the
  function slot and releasing the owner. Callback mcode is allocated at
  `luaopen_ffi()` before concurrent callback creation, so the former
  `misc_token` lazy-init bridge is gone. Callback-calling C functions are
  blacklisted through a fixed CTState pointer-key CAS set, not arbitrary
  `miscmap` keys.
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
the raw side-entry allocation before key/value barriers. Legacy GC and GC2
traverse the side cache as a CLibrary userdata root; `__gc` unload and the
`lj_udata_free()` backstop both free side entries idempotently.
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
string `sizeof`/`typeof`; `tools/ci/m7_ffi_cdef_dup_stack.sh` guards the
worker-stack rehome that moves child stacks into their TG arena before thread
publication.
