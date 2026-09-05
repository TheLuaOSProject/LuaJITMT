# Table scan-authority renewal: read-only design audit

Source boundary: coalescing commit `d680421c` and its unchanged table/activation/token paths. Reviewed the July 19 saturation note and plan 15's replacement release criterion. No production, coalescing, or plan files were edited; no runtime or timing claim is made here.

## Recommendation

Prototype a wider atomic scan stamp with a 64-bit overflow era, the existing 32-bit dirty serial, and the existing 32-bit covered GC cycle. Renew the low serial by changing the era in the same CAS. Keep the exact table token as a separate, unchanged 64-bit word.

This is the smallest proof change I found that removes the current 32-bit lifetime cliff without making rollover depend on another thread, allocation, a GC cycle boundary, or an overflow recovery owner. It adds a known memory and atomic-instruction cost; measure those costs before choosing its final storage layout. It does not establish an infinitely reusable authority namespace: the full era must remain non-wrapping, and full-era exhaustion must retain fail-closed behavior. The independent 32-bit global GC-cycle limit also remains explicit release work.

## Existing facts that constrain the design

- `LJGC2TabStamp` is 16 bytes: `state={dirty32, covered_cycle32}` plus the exact token. A small traversable arena preallocates 4,096 entries; its 64 KiB sidecar equals the 64 KiB arena size. Huge allocation headers embed the same stamp (`lj_arena.h:100,1600`).
- `gc2_table_scan_publish` currently compares only dirty32, then CASes the combined dirty/cycle word. Every mutator dirty CAS invalidates coverage. This is why resetting the serial without another identity would be unsafe (`lj_gc2.c:15519`).
- Body retention is required even to locate the stamp. Small incarnation preparation resets the proof only after claiming FREE construction/mutation and proving token NONE; token generations survive reuse (`lj_arena.c:398`, `lj_arena.h:1101`).
- Legacy INSTALLING/COUNTED and recovery PENDING/CLAIMED/REDIRTY are separate obligations. The dormant exact-token scanner publishes a stable proof before `PENDING(D)->NONE(D)` and keeps its body lease through completion. Do not reset or repurpose D (`lj_gc2.c:18253,19074,19289`, `lj_gc2token.h:755`).
- GC cycle start and close have worker/phase ownership and handshakes; a raw allocation lease alone is not a certificate that a GC cycle cannot change. The dormant exact-token pass explicitly relies on worker ownership for that stability. The current global cycle is non-wrapping and independently pins at MAX (`lj_gc2.c:4103`, `lj_obj.h:3306`).

## Proposed layout and linearization points

A straightforward representation is:

```c
typedef struct LJGC2TabStamp {
  la_u128 state;                 // lo={covered_cycle32, dirty32}; hi=era64
  LJGC2TableToken token;         // unchanged exact descriptor generation
} LJGC2TabStamp;                 // 32 bytes with 16-byte alignment
```

Define a scan ticket as `{era, dirty, captured_gc_cycle}`. `covered_cycle=0` remains invalid coverage. Do not reset dirty on ordinary GC-cycle changes in this first prototype: the non-wrapping era/serial pair survives them, reducing protocol changes.

1. **Ordinary mutation/publication.** Under the exact retained table scope, CAS `{E,D,C}` to `{E,D+1,0}`. This CAS remains after the payload store and before the SSB/recovery publication LP. Any unsuccessful CAS retries from the actual returned full state.
2. **Low-word rollover.** If D is MAX, CAS `{E,MAX,C}` to `{E+1,1,0}`. This single operation is both renewal and invalidation for the completed write. No interim reset state, reusable era, descriptor claim, memory allocation, or waiting reader count exists. Concurrent rollover attempts cannot lose increments: only one wins, and other publishers continue against its new era.
3. **Scan start.** Acquire one consistent `{E,D,C}` snapshot before reading table payload. Save E and D, plus the current GC cycle under the caller's existing scan/phase authority. Holding only the body lease does not replace that phase contract.
4. **Proof publication.** After full payload, metadata, weak and FINREG validation, CAS to `{E,D,captured_cycle}` only while the current full mutation key still equals `{E,D}`. A peer may change only coverage; retrying that full CAS is safe after rechecking the mutation key. An era mismatch is RETRY, never an invitation to adopt the new key without rescanning.
5. **Completion.** Preserve the existing proof-before-legacy-clear and post-clear repair ordering. Exact-token callers may complete only their captured D ticket under their existing phase ownership. A dirty rollover never completes, refreshes, resets or pins an exact token merely because its mutation era changed. A failed proof leaves the corresponding obligation intact.
6. **Coalescing.** A complete current-cycle proof again suffices even when dirty32 equals MAX: unlike the old absorbing maximum, the next write has a distinct era/serial key. Only exhaustion of the full `{era,dirty}` namespace lacks renewal authority. Retarget the existing saturation exclusions and explicit public MARK request bypass to that full terminal pair; a sticky veto alone must not conceal an unmarked descendant. Converter token/NEEDSCAN gates remain unchanged.
7. **Terminal namespace exhaustion.** At `{era=MAX,dirty=MAX}`, invalidate coverage and retain the existing sticky veto, without wrap. This rare terminal condition remains an explicit finite-namespace boundary, not a rationale for keeping the current 32-bit limit.

First implement exact CX16 snapshots and updates for clarity. The repository already has explicit x64 `la_cas128`, avoiding a library fallback (`lj_atomic.h:79`). A later Linux/x64 read optimization can use `era -> low_word -> era` loads: because era changes only monotonically at rollover, equal before/after eras give a consistent snapshot. That optimization relies on the repository's explicit mixed-width x64 atomic contract and needs GCC/Clang/ASan artifact checks. A mutator can also use speculative component loads only as a CAS expectation; it must not derive externally visible success or a veto from an unvalidated torn observation. Do not introduce unreviewed mixed-width stores.

## Suspended actors, cycles and allocation reuse

- An old scanner with `{E,1}` cannot publish after a rollover produces `{E+1,1}`, even in the same GC cycle. The full CAS rejects it.
- A publisher paused before invalidation must reread/retry against the new era and clear any proof completed during its pause. One paused after invalidation may publish later: only a scan which covers that invalidation or a later one may consume its duplicate.
- A rollover during an in-flight scan forces a retry even if the low dirty serial later repeats. A scanner paused after proof cannot consume a newer exact token D2 with its old D ticket. Keep the descriptor's ACTIVE-to-PENDING handoff independently intact.
- Do not stamp a captured old GC cycle as a new one. Existing worker-owned phase stability remains the proof for token completion; a mere before/after phase load is not a transferable completion lease. Preserve or explicitly reject any completion context lacking that ownership. The exceptional immediate root fallback currently follows a failed publication/fail-closed path; do not silently promote it into new reclaim authority while refactoring.
- Keep the existing non-wrapping GC cycle and activation gates. Changing cycle width/renewal is separate work; table renewal must not advance or reset global activation to obtain a fresh local identity.
- Reset the full proof for a small cell only at the existing private new-incarnation LP, preserving its token generation. An old scanner/publisher is forbidden from carrying body/stamp access past its lease. Raw queued identities re-admit the actual incarnation and carry no old scan certificate.
- Huge mappings use the identical state transitions under the retained HugeTab reader. Header-only DEFER_FREE/token completion must still avoid table payload reads. Reader denial and unadmitted requests continue using their existing recovery/veto lanes; no speculative era/stamp access is added.

## Costs and alternative designs

| Option | Expected normal-path cost | Storage cost | Assessment |
| --- | --- | --- | --- |
| Wide era stamp, same array-of-structs layout | CX16 instead of CAS64 for dirty/proof updates; exact snapshot initially adds a CX16 read, or three loads after separately proved optimization | Entry 16→32 bytes; sidecar 64→128 KiB per traversable arena | Preferred first correctness prototype; no rollover allocation or actor join. |
| Wide proof array plus separate token array | Same atomic operations, changed address calculation | 64 KiB proofs + 32 KiB tokens = 96 KiB sidecar, +32 KiB over current | Worth comparing if memory matters; changes common stamp accessors and small/huge views. |
| 64-bit per-cycle `{cycle32,serial31,clean1}` | CAS64 plus cycle comparison/renewal branch | Existing 16-byte entry | Safe lazy renewal is possible only when all captured identities include cycle. Still needs a separate same-cycle overflow protocol, including IDLE and stalled cycles. |
| Sparse extended authority after an inline limit | Normal path keeps CAS64 plus mode branch; promoted tables pay wide operations | Per-promoted-table record plus locating/retirement metadata | More complex mode publication, allocator-failure and record-lifetime proof. Do not hide those costs behind the word “rare.” |
| Join all old readers before resetting | Gate close/recheck and reader drain | Potentially small | A suspended admitted actor prevents renewal; a count-zero observation alone is unsafe. Poor fit for the requested progress goal. |

These are instruction/layout counts, not measured latency. With the straightforward wide entry, total arena+sidecar storage rises from 128 to 192 KiB before other metadata (+50%). The split layout raises it to 160 KiB (+25%). Huge header padding/field order must be measured in a layout probe: naive insertion can add more than the 16 payload bytes; reordering the descriptor before the aligned stamp can avoid excess padding. All fixed offsets, huge body bases, static assertions, sidecar allocation-failure paths and statistics accounting must be updated together.

The lower-cost per-cycle design is not rejected in principle. At serial exhaustion it would need to disable stamp-based public suppression, ensure every further semantic request obtains unsuppressible recovery before returning, and derive completion from the recovery owner's CLAIMED/REDIRTY witness. A constant saturated scan word itself is not proof of a stable scan. Integrating that witness with legacy credit, exact D-token completion, two-/three-table cycles, and live huge ownership is a larger protocol change than the wider stamp. Merely forcing another cycle cannot solve a table which exhausts the serial while that cycle is still held open.

## Counterexamples which discard shortcuts

- **Blind reset:** pause a scanner after reading child-free payload at serial 1; wrap/reset the serial back to 1 after publishing a new child; its old same-cycle proof CAS succeeds and the converter can drop the only request.
- **Separate era with a check around the old CAS64:** the scanner checks era E and pauses; another actor increments era and resets the low word; the old CAS64 then publishes false coverage. A post-check cannot undo a request already suppressed during that window.
- **Blind per-cycle reset with the current dirty-only comparer:** old scans can acquire repeated serial values across cycles and clear carried pending work despite having scanned an earlier mark plane. Full cycle identity and phase-owned completion are required; dirty equality is insufficient.
- **Saturated requests through ordinary SSB:** an old scanner can republish the ambiguous maximum after the write, letting a pre-converter legacy shortcut or converter drop work. The newly landed fixture already reproduces this failure in its saturation controls.
- **Reuse exact token generation as dirty era:** a delayed ACTIVE(D,t) helper and `NONE(D)` idempotence use D in the global descriptor namespace. Resetting or locally incrementing it can recreate completed work, reject valid transfers, or let an old completion consume a replacement request.
- **Observe reader count zero then reset:** a new reader can enter between observation and reset. Closing admission first repairs safety but makes progress depend on all prior readers returning.

## Required deterministic acceptance and negative controls

1. Force rollover with a scanner paused before proof, a new child/grandchild raw store, and available SSB capacity. Repeat rollover at least twice; require marked descendants, drained counts, no NO_RECLAIM, and a subsequent full collection of unreachable objects.
2. Pause publishers before and after dirty CAS while a peer rolls the era and completes a scan. Assert the latest request cannot reuse the pre-rollover proof, and covered duplicate requests still produce one parent scan.
3. Race two publishers at MAX and one at MAX-1. Require distinct completed mutation keys and all descendants; verify no speculative torn-snapshot success/veto.
4. Preserve the exact one-pass private self-edge test and array/hash value/hash key/metatable self edges, two-/three-table cycles, weak/FINREG semantics and mutation during scan.
5. Run MARK→SWEEP and IDLE→activation schedules with a rollover at each pause. Preserve current cycle exhaustion as a separate negative boundary; a table rollover must not reset activation or the GC cycle.
6. Capture token D, pause before proof or after proof, roll the table era and publish D2. Old completion must leave D2; failed proof must leave D. Include descriptor ACTIVE pauses and header-only terminal completion on small FREE and huge DEFER_FREE.
7. Hold exact body admission across rollover/free attempts. Then release, reuse the small address and verify the new proof starts invalid while the token generation persists. Repeat huge deletion/recreation and protected-body denial cases.
8. Negative variants: compare only dirty32; increment era separately from reset; adopt a new era without rescanning; reset token with stamp; allow full-era wrap; remove retained scope; and use a current-cycle precheck as permission to clear a newer token. Each needs a specific missed-descendant or wrong-ticket assertion, not just a timeout.
9. Validate the source prototype with the existing full traversal, recovery, coalescing, public barrier, guard, weak and huge lifetime suites under strict GCC and Clang ASan. Then compare normal hot table mutation and unchanged table-valued churn, along with allocated sidecar bytes. Avoid claiming the whole collector becomes nonblocking from this change.
