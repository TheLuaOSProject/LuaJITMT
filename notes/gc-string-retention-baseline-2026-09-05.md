# String retention and automatic GC admission, 2026-09-05

The Linux baseline separates two problems: unreachable strings remain retained
despite completed collections, and interpreted allocation can leave an
automatic collection request unconsumed while a peer remains alive.

The isolated normal/assertion/Clang-ASan builds use `e3428257`. All 224 runtime
and generator inputs match that commit. Subsequent method/receiver/optimizer
changes have not changed the GC, string, threading or API source reviewed here.
The fixture uses ordinary Lua APIs, a continuously live parked Lua peer where
requested, and zero or two configured GC workers. It keeps 32 strings rooted
and repeatedly interns then drops 4,096 distinct 64-byte strings over six
rounds. Both Lua owners check rooted canonical pointer identity and contents.
Only live rooted strings have saved C pointers.

Every explicit case completes 12 real SWEEP-to-IDLE transitions. With the
sole main TG and no workers, all 24,576 garbage bodies are reclaimed. A peer
or a GC worker pool independently prevents this reclamation: all 24,576
bodies remain, accounting for exactly 2,457,600 body bytes. Table capacity and
allocator mappings are separate costs. After stopping workers, joining the
peer and performing fresh sole-main full collections, all cases return to
exactly 300 interned strings. Unused string-count credits are accounted for;
every final matrix snapshot has zero unused credits.

Automatic collection with just the main TG completes 18 cycles but still
retains all those strings. Its separate explicit cleanup reclaims them.
The two persistent-peer automatic configurations never admit their published
IDLE request after 262,144 ordinary Lua table allocations. Request count grows
from 4 to 5 and allocation pressure reaches about 27.8 MB, while actual starts
and completions stay at 4/4. These processes stop at the first string batch and
report 4,096 retained strings; they do not prove six completed churn rounds.

With workers but no Lua peer, automatic GC does admit a cycle and reaches
SWEEP. It completes 0/1/0 cycles in the normal/assertion/ASan bounded runs,
below the required three. The workers make real progress. This is a separate
completion/scheduling failure within the tested allocation bound, not evidence
of a permanently deadlocked collector or a universally missing MT driver.

The source explains the persistent-peer IDLE case. First child attachment
saves the logical threshold and sets the legacy global threshold to
`LJ_MAX_MEM`. Allocation accounting publishes a durable request, but the
interpreted driver still tests that global threshold. A hard-only path can be
missed after slow allocation flushes the local debt. Workers and hard assist
do active phase work; they do not initialize the pending IDLE cycle.

Last-child recovery is measured independently: joining the last peer restores
the exact saved finite threshold, and the next 8,192 ordinary Lua allocations
complete three cycles without explicit collect or step. A separate controlled
experiment first compiles a numeric loop after pressure, with the peer still
alive. Trace completion calls the collector through another entrance, and all
six normal/assertion/ASan peer/worker configurations admit a new MARK cycle.
The matching JIT-disabled controls remain in IDLE. The short observation
proves admission, not three-cycle completion.

The final evidence has 38 runtime processes: 24 matrix cases (15 completed
measurements and nine preserved progress failures), six passing last-detach
recovery controls, and eight short JIT admission/control observations. All
perform normal cleanup. ASan/LSan uses `detect_leaks=1:abort_on_error=1` with no
suppressions, instrumented targets and uninstrumented host generators. Earlier
driver assumptions and incomplete experiments remain separately identified.
[The detailed source review and reproduction instructions](evidence/gc-string-retention-baseline-2026-09-05/HANDOFF.md)
and [final measured matrix](evidence/gc-string-retention-baseline-2026-09-05/summary.json)
retain exact commands, counters, timing bounds and input/binary identities.

The next admission repair belongs at existing safe VM boundaries, preserving
STOP/restart, owner roots, stack geometry and first/last-child transitions.
Calling a full collector from raw allocation accounting would violate that
boundary. Admission alone does not remove the driver's synchronous handshakes
or resolve worker-enabled SWEEP completion.

String reclamation remains a separate lifetime protocol. Do not remove the
peer/worker gates: existing sole-main batches hold exclusion through unlink,
both grace periods and physical free. Automatic entry could leave that
exclusion owned by a suspended driver. Concurrent canonical publication, late
roots and raw native/FFI byte borrowers still need durable death and retention
authority. The unused `LJ_GC2_STRING_BODY_RECLAIM` macro is not an enable switch
for a completed implementation. No runtime source or gate changes accompany
this baseline; Windows/macOS and release qualification remain deferred.
