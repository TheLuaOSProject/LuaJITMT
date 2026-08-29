# GC2 Mark Entry Validation

## Context

Public mark helpers, sweep-root preservers, and SSB/worker mark admission can see stale or concurrently published GC object pointers. These paths previously reached `gc2_mark_base()` or direct `o->gch.gct` reads before proving that the candidate pointer still names a live GC allocation.

## Change

- Added a shared GC2 mark-entry validator that checks canonical/aligned pointers, arena/huge allocation membership, queued small-cell liveness, valid GC type tags, and cdata base validity before returning the mark base.
- Routed `lj_gc2_markobj*`, worker mark admission, SSB mark drain, sweep-root preservation/tracing, payload worker marking, and `lj_gc2_ismarked()` through that validator.
- Dropped invalid SSB mark entries without touching their headers; cleanup helpers are only used after an entry is admitted as a live object.
- Kept userdata sweep tracing as a body-preserve root while validating it before any header decisions.
- Guarded JIT-frame mark helper definitions with `LJ_HASJIT` so no-JIT `-Werror` builds do not compile unused static stubs.
- Made legacy deep mark use the root-object type validator before reading object headers during sweep bridging.

## Coverage

`tests/t-gc2-markbits.c` now sends a canonical non-object pointer through the public GC2 mark/sweep/ismarked helpers and the legacy deep mark helper. The regression catches the old failure mode where these functions could dereference the candidate header before admission.
