# Weak overflow test bridge owns its graph work

The exported test helper entered weak bridge tracing without the worker claim
required by its grey/recovery drains. Production weak-completion callers
already hold that claim. The helper now takes the real claim, returns zero on
refusal, and releases it after either successful or deferred bridge work.
This changes only the test-helper build path.

The existing overflow retry/success fixture now pauses an actual worker inside
its SSB consumer. A competing bridge call must fail its claim, retain the
unconsumed value-retry hook and weak entries, and leave the worker's ownership
intact. After release and join, both the value-refusal pass and successful clear
must release their own claim. The old helper fails this new refusal oracle.
No synthetic worker-active store substitutes for ownership.

Ten isolated assertion/ASan processes pass across weak-only traversal, phase,
weak resize, recovery and interpreted stock. The new canonical
`m3_gc2_weak_helper_claim` case also passes and restores the default build.
The existing weak-only fixture mode needs `-Wno-unused-function` for its
unselected static tests; runtime flags and other warnings stay intact.

The full traversal suite remains red at a separate assertion in
`test_stack_admission_tristate`: immediate dead-TG reclamation must return one.
The unchanged f9ec archive and unchanged fixture reproduce it at original
line 7151. Corrected-helper full runs stop there too; those processes are not
counted as passes. The assertion is preserved for a separate stability repair.
The broader GC work-class scheduler candidate remains unlanded.

See the [exact source and validation review](evidence/gc-weak-helper-claim-2026-09-05/root/REVIEW.md),
[production/test caller audit](evidence/gc-weak-helper-claim-2026-09-05/source-review/AUTHORITY.md),
[old-helper negative control](evidence/gc-weak-helper-claim-2026-09-05/root/baseline-controls/results.json),
[canonical result](evidence/gc-weak-helper-claim-2026-09-05/root/canonical.json),
and [archive manifest](evidence/gc-weak-helper-claim-2026-09-05/manifest.json).
