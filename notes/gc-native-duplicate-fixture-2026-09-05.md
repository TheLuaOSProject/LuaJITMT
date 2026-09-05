# Native duplicate fixture accepts exact repeat publication

A late signal or remote refusal can republish the same counted native request
after its owner consumes the mask and before the epoch claim. The fixture's
zero-only observation failed on both the prior runtime and the worker candidate
under a controlled real schedule.

The corrected observation accepts a nonzero mask only when the exact actions
and published epoch match. All actual scan, consumed-poll wait, teardown and
final pending-count assertions remain. Runtime acknowledgement rules are
unchanged.

Eight owner runs pass on the two original source generations. Four additional
assertion/ASan runs pass on current runtime `eb8a5b2f`, including the forced
repeat-publication schedule. The registered native-completion suite passes all
11 processes and restores the default build. Original failures remain archived.

See the [root review](evidence/gc-native-duplicate-fixture-2026-09-05/root/review.md),
[owner handoff](evidence/gc-native-duplicate-fixture-2026-09-05/owner/DUPLICATE-V2-HANDOFF.md),
[canonical result](evidence/gc-native-duplicate-fixture-2026-09-05/root/canonical.json)
and [artifact manifest](evidence/gc-native-duplicate-fixture-2026-09-05/manifest.json).
The inherited synchronous native-root protocol remains separate progress work.
