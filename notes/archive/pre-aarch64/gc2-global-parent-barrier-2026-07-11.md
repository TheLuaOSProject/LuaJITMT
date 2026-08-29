# GC2 global parent barrier (2026-07-11)

`lj_gc_barrierf(global_State *, parent, child)` is an ABI entry which may be
called without a trustworthy current `lua_State` or TG. It must therefore not
route through raw TLS, borrow `G2TG(g)`, or require a TG-local `mark_active`
acknowledgement.

The g-only GC2 object-pair barrier now samples the global phase once and:

- preserves the child during MARK or WEAK even when `mark_active == 0`;
- dirties/rescans a valid table parent as an additional conservative repair;
- uses the sweep-resurrection path during SWEEP;
- retains the historical no-op behavior in non-generational IDLE.

The `lua_State *` object-pair barrier remains separate. It keeps the existing
TG-owned active/remembered-set behavior, including generational IDLE filtering.
Conflating these two entry points previously made the ABI barrier depend on the
main-TG fallback and could drop MARK/WEAK edges during activation startup.

The focused allocation/barrier fixture forces MARK and WEAK with the main TG's
`mark_active` cleared and verifies that `lj_gc_barrierf()` still preserves both
children.

This remains a point liveness barrier, not a phase-admission lease. The exact
activation gate and root descriptors are still required to close a writer that
starts across a phase transition.
