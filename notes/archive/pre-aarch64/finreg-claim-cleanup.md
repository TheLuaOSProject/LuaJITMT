# FINREG claim cleanup

Two FINREG paths could leave a public claim sentinel visible if an allocating
operation longjmped while the slot was claimed.

1. `lj_cdata_setfin()` resolved enabled cdata finalizer claims after running the
   weak-key GC2 barrier. That barrier can allocate while marking during the weak
   phase. The barrier now runs through a protected call while the slot is
   claimed; on error the slot is restored to the pre-claim value, or nil for a
   new slot, before the error is rethrown. Ordered FINREG publication still
   happens only after the barrier completes.

2. `lj_gc2_finreg_cdata_finalize_pweak()` claimed an ordered FINREG slot before
   `lj_gc2_finreg_cdata_preclaim()` had ensured its side vectors. The side-vector
   ensure path can allocate. The pweak finalizer scan now prepares the fixed
   preclaim vectors before claiming the FINREG slot. After the claim, preclaim
   publication is copy/store only; if preparation was unavailable, the existing
   fallback path runs without calling the allocating preclaim helper.

The regression coverage is attached to `m7_ffi_finreg` as runtime behavior:
the weak-key barrier must be protected and precede ordered publication, and GC2
preclaim preparation must precede `lj_cdata_fin_claim_func_l()`. Deterministic
runtime reproduction would require artificial failure hooks in the middle of
GC2 weak marking or allocator failure injection at the exact post-claim point.
