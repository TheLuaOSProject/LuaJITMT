# GC2 quarantine EOF LIVE rearm (2026-07-11)

This note records a bounded-sweep progress fix and the terminal protocol that
now makes its retry safe. The plan files are unchanged.

## Observed livelock

A full stock run remained in `P_SWEEP` with one traversable quarantine arena as
the sole pending work. A live-process snapshot established all of the terminal
conditions below at the same arena head:

- `reclaim_cell == LJ_ARENA_CELLS`;
- `reclaim_deferred == 0`;
- `remote_active == CLOSED` and `remote_free == NULL`;
- a non-sentinel retire epoch;
- 266 allocated cells, of which 265 were `WHITE` and marked;
- one valid, marked `GCtab` allocation at cell 2216 in `LIVE` state.

The dump proves that an actionable `LIVE` state remained behind an EOF cursor.
It does not distinguish whether the publisher was a late
`RETIRED -> LIVE` preservation rescue or a late marked-root detach. Both are
legal inputs to the bounded reclaimer and require the same cursor backedge.

`lj_gc_reclaim_gc2_arena()` previously rewound at EOF only when that invocation
set its local `pending` flag or when `reclaim_deferred` was nonzero. A late
rescue can reduce `reclaim_deferred` back to zero, and a state published behind
the cursor is not observed by the empty EOF invocation. It therefore reported
`done`.

`lj_arena_alloc_quarantine_finish()` independently and correctly rejected the
remaining `LIVE` state, but its failure path only cleared `PREPSWEEP`. It did
not move `reclaim_cell`. The owner consequently repeated
`step == 0`, `done == 1`, `finish == 0` without changing any state.

## Implemented progress backedge

The final bitmap-readiness scan now returns the exact first `LIVE` or `RETIRED`
allocation cell. If terminal finish is rejected for that actionable state and
the cell is behind the owner cursor, the owner-only `reclaim_cell` is lowered
to that exact cell.

The GC2 owner recognizes a failed finish with a rearmed cursor as one unit of
bounded real work. This prevents a false worker-idle declaration and lets the
same bounded owner schedule consume the cell. The following blockers do not
rearm the cell cursor:

- `WHITE` and unmarked, because the state reclaimer cannot classify it;
- a remote publisher or queue, which belongs to remote-generation handling;
- a fresh-grace retire-epoch sentinel, which belongs to grace scheduling;
- any other finish failure with different owner progress.

This distinction is not permission to declare every non-cursor reason idle. A
durable queue or epoch sentinel published after the owner's earlier precheck is
actionable on the next bounded iteration and must be reported as such; only an
active publisher is waiting for external completion. The SEALED protocol must
return an explicit finish reason so queue, grace, rearm, active-publisher and
unclassified-WHITE outcomes receive the correct scheduling behavior.

Thus the normal successful finish path gains no additional scan, CAS, fence,
or allocation. The failure path reuses the cell already found by the existing
readiness scan and restarts there rather than at the beginning of the arena.
The cursor remains owner-local under the GC2 worker token; no lock or wait is
introduced.

## Terminal admission ordering

The same audit found that readiness validation alone could race ordinary frees
and mark/rescue publication. The completed protocol packs `CLOSED`, `SEALED`,
`PENDING`, and a 61-bit publisher count into one naturally aligned 64-bit gate.
Every small-object mark/rescue joins the gate, including while OPEN. A producer
which observes a terminal generation atomically publishes count plus PENDING
before its bit/status intent. Terminal finish first seals a zero-count gate,
reconciles every durable intent, clears PENDING only through an exact CAS, and
commits only through exact `CLOSED|SEALED -> SEALED`.

A terminal/grace-late free publishes only `late[]`; it never overwrites the
still-SMR-visible body with an intrusive queue record. A late bit pins block1
through this generation and is converted to FREEING only by a later
PREPSWEEP, before a fresh grace. The last publisher release edge wakes sweep
progress. After commit, rescue is read-only: block decisions are release-
published before the sidecar reset, so the reader sees either pre-reset
FREEING or post-reset WHITE plus the committed block bit.

Reclaimed adoption and abort restore rebuild bins privately while SEALED and
open only through exact `SEALED -> 0`. A publisher winning that arbitration
defeats OPEN; owner-visible staging is rolled back and the arena remains
CLOSED. No path waits for a publisher.

## Deterministic regression

`tests/t-arena-gcsweep.c` now creates a valid table header in an isolated target
arena, retains every other `WHITE` allocation, and drives bounded reclamation
to EOF. It then unlinks the table and publishes `WHITE -> LIVE` at its exact
cell, reproducing the observed post-cursor condition without relying on stock
test ordering or timing.

The fixture requires terminal finish to reject once and rearm to the table
cell. It drives that rejection through `lj_gc2_test_sweep_owner_progress()`
with a one-unit budget and requires the rearm discovery itself to be reported
as bounded work rather than a false idle declaration. The next bounded owner
pass must reanchor the table and publish `LIVE -> WHITE`; later bounded passes
must reach EOF and complete terminal finish.

## Scope of the proof

The EOF backedge and packed gate now cover terminal small-arena publication,
including a rescue which arrives behind the numeric cursor. They do not prove
all GC2 object lifetime callers. Variable/interior cdata, huge containing-mark
publication, weak/FINREG snapshots, and the epoch-tagged mark-generation work
remain separate P0 items recorded in the current audit notes.

## Separate invalid detached-header assertion

While reproducing the stock sequence with the then-current assertion/debug
binary, a distinct failure was observed:

```
cd tests/stock/test
timeout 20s ../../../src/luajit test.lua
LuaJIT ASSERT lj_gc.c:949: lj_gc_reclaim_gc2_arena:
  cannot reconstruct detached arena GC header
```

That run's recorder diagnostics dominated the captured output and the stock
runner's case line was not line-buffered, so no reliable case attribution or
arena/cell value was recovered. A subsequent line-buffered repetition
completed all 509 cases. The focused arena GC-sweep suite now passes with the
packed admission protocol, but this historical observation remains recorded
until hostile sanitizer and stock stress have made it reproducible or provided
enough repeated clean coverage to retire it.
