# GC2 semantic-edge retry and table-rescan hardening (2026-07-17)

This tranche closes several correctness gaps found while auditing the b1.2.0
GC2/JIT path. It does not change `plan/` and does not claim that the table
rescan publication protocol is yet the final lock-free design.

## Semantic table lookup

`lj_tab_gettv_forjit()` now retains exact GC2 leases for both the table and a
collectable key through hashing and current-generation resolution. Raw vector
slots are read only inside an SMR interval. A fallback miss captures the array
and hash roots as one logical snapshot and validates both roots and their size
metadata before returning. This prevents a false nil when an integral key moves
from an old hash generation into a replacement array generation.

Nil keys are a terminal absent lookup, rather than a retry. Both integer and
integral-number keys check the current array before the current hash. The common
successful lookup still uses the ordinary fast getter; the paired manual scan
is a miss/stale-generation path.

`meta_tset()` now uses tri-state table/key admission and retries transient
RECOVERY/MUTATING/DESTRUCT observations before making an existing/new-key or
`__newindex` decision. Its leased copied lookup is semantic evidence only. A
fresh current write slot is resolved afterwards and the existing keyed CAS
performs final SMR-backed validation, so no decision-time vector pointer crosses
the resize boundary. `__index` and `__newindex` lookup now use the common safe
metamethod-value path; a function-valued regression proves a forced semantic
retry dispatches the metamethod exactly once.

The copied result itself now has an independent tri-state lease acquired before
the table-vector SMR interval closes. A valid GC result stays leased through
header validation and root/table publication, RETRY restarts the semantic read,
and stable STALE is normalized to nil instead of spinning forever. This is
necessary for weak values: table and key leases do not pin a weak child. The
test fixture uses a huge userdata so its result-only HugeTab reader can be
observed independently of the small source table and scalar key.

Metamethod lookup also closes the receiver -> metatable -> value lifetime
chain. Each attempt atomically copies the receiver TValue once, publishes that
copy, and uses it exclusively for exact admission and type selection; a racy
overwrite can no longer make the leased incarnation differ from the body being
read. Receiver admission precedes its metatable field load, and source-side SMR
spans captured-pointer admission into an independent exact metatable lease.
Replacement can therefore select the old or new table as a valid
linearization, but cannot cross into a reused table at the same address.

The nested metamethod-name scan is a bounded, one-shot held-table operation.
It never invokes the ordinary L-aware retry loop while receiver/metatable
leases are held: every transient structural or result-admission collision
first releases SMR and all exact leases, then services the caller-visible
safepoint/retry. This ordering is required because a STOPREQ longjmp from an
L-aware wait would otherwise strand arena/HugeTab readers permanently. A found
GC value receives its own exact lease before the vector SMR interval closes and
is published before that result lease is released.

That nonwaiting primitive is now named `lj_tab_getstr_held_try()` rather than
the collector-specific `lj_tab_getstr_gc_held()`: its contract is exact table
scope plus SMR, and it is intentionally shared by collector, finalizer, and
mutator-side semantic readers without embedding any GC-phase assumption.

## Sweep TValue classification

All TValue-based SWEEP root, barrier, table-child, thread-root and upvalue paths
now enter a shared tri-state admission helper:

- `STALE` (unmapped, wrong-tag, or terminal old incarnation) is ignored.
- `RETRY` (a transient lifetime owner) is transferred to exact recovery, with
  sticky `NO_RECLAIM` only if that mandatory handoff cannot be represented.
- `VALID` retains its exact allocation lease through the existing sweep tracer.

Raw `GCobj *` root paths are unchanged: their callers already own an
authoritative object identity and do not represent conservative TValue words.

## Queue admission and table rescan membership

SSB and grey consumers now distinguish a post-admission lifetime race from a
terminal dead object. The mark operation returns the lifetime value which
actually rejected it; a later DESTRUCT/MUTATING/RECOVERY -> LIVE restoration
cannot erase that retry witness. If the lifetime changes after candidate
validation, the SSB slot remains owned or the grey item is republished exactly;
table rescan membership stays counted until a later live drain completes.

A mark bit may win before final lifetime validation reports RETRY. Semantic
expected-type callers therefore publish exact work while their counted scope is
still held: otherwise the retry would observe LIVE_ALREADY and could leave a
marked container graph undispatched. The deterministic fixture forces precisely
mark-CAS -> DESTRUCT -> RETRY -> LIVE during MARK and verifies that the parent's
child is subsequently marked.

The temporary per-table membership byte uses `NONE`, `INSTALLING`, `COUNTED`
and `CANCELLED` transitions so a racing consumer cannot decrement a publisher's
provisional aggregate reservation. Unheld requeue now retains the table's exact
allocation scope across token clearing, phase selection and republication; a
transient admission transfers to recovery instead of reusing an unleased raw
pointer. Duplicate finish/ABA states fail closed in release builds and assert in
diagnostic builds.

The SSB stale-table shortcut now retains the table body scope through all
header/stamp/token observations and may consume an already-scanned entry only
when exact `gc2_rescan_state` is also `NONE`. A delayed prior-generation hint
clear can temporarily erase advisory `LJ_GC_NEEDSCAN` while a new COUNTED token
and its sole SSB locator exist; a deterministic pause proves that entry is sent
through traversal and discharges the exact token instead of being dropped.

The current aggregate reservation is still incremented before `INSTALLING` is
visible. That window is a safe phase-close veto, but a descheduled publisher is
not helpable and therefore it is not the final lock-free protocol. The complete
replacement design, including the necessary pre-store root-operation
descriptor, is in
`notes/gc2-table-rescan-helpable-token-design-2026-07-17.md`.

## Layout and diagnostics

`gc2_rescan_state` occupies existing `GCtab` padding on x86-64. DynASM now
asserts offsets `nomm=10`, `colo=11`, `gc2_rescan_state=12`, `array=16`, and
`sizeof(GCtab)=80`. Every C/inline empty-table constructor initializes both
`weak_cycle` and `gc2_rescan_state`; the inline TNEW fixture pre-poisons recycled
bytes to prove the assembly path overwrites them. The shared-field contract is
documented in `src/lj_mtfields.md`.

Test-only pause APIs were added under `LJ_GC2_TEST_HELPERS` for exact schedules:

- table lease held while a key enters MUTATING;
- first-generation lookup miss and hash-to-array publication between the
  fallback's array/hash observations;
- successful queue admission and mark-bit test followed by LIVE -> DESTRUCT;
- a captured retry witness held across DESTRUCT -> LIVE restoration;
- a result-only lease held while table-vector SMR remains open;
- receiver metatable replacement after field capture, followed by a second
  pause after exact admission of the now-unrooted old target in a different
  arena;
- a delayed stale table-hint clear with a new exact COUNTED token; and
- `INSTALLING` table-rescan publication before settlement.

These APIs and their storage do not exist in release builds.

## Verification

- clean normal helper build;
- clean `LUA_USE_ASSERT` + `LJ_GC2_PARANOIA=1` helper build;
- focused `t-gc2-traverse` normal: pass, plus 50 consecutive runs;
- focused `t-gc2-traverse` paranoia: pass, plus 20 consecutive runs;
- target-only Clang ASan+UBSan build (`alignment`, `function`,
  `pointer-overflow`, and `shift` disabled for LuaJIT's established intentional
  low-level idioms): pass, plus 10 consecutive focused runs;
- target-only GCC TSan build: pass, plus 10 consecutive focused runs;
- `m3_gc2_recovery`, normal and paranoia: pass;
- `m5_x64_tnew_empty_inline`: pass;
- clean release build and a JIT-on table-read smoke: pass;
- `git diff --check`: pass.

The custom `lua_Alloc` omission remains temporary and separately documented in
`notes/lua-alloc-temporarily-disabled-2026-07-10.md`.
