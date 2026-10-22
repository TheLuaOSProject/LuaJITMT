# FNEW SSB fixture reopen capacity

The pre-existing `t-jit-fnew-bump.c` failure at the positive two-slot setup is a
fixture assumption. No production change is needed for this boundary.

`lj_gc2_preserve_abort_to_idle` deliberately retains active and published SSB
work. The next MARK start requests a flush while holding the worker token. If
the TG's second built-in node is still published, no free node exists; recycling
that node requires the same worker token and cannot occur inside MARK start.
The reopened MARK can therefore legitimately retain a full active SSB. The
no-drain FNEW certificate must continue to refuse this full buffer atomically.

A native ARM64 extraction of the unchanged boundary fixture failed the same
capacity assertion as the earlier Rosetta run. An explicit ordinary owner
`lj_gc2_flush_ssb` after reopening MARK makes the complete extracted boundary
pass. The fixture now prepares capacity this way before its positive two-slot
case; its foreign-owner, one-slot, and full-buffer rejection assertions remain.

`tests/t-gc2-ssb-preserve-reopen.c` separately proves the retained-pressure case.
It keeps a real published predecessor plus a full active node across abort and
restart, verifies both queue identities and entries, checks no-drain refusal,
and verifies that the normal owner flush preserves the full node and permits
later pair publication. It runs before the FNEW fixture in `m6_jit_fnew_bump`.
Native ARM64 validation used an assertion-enabled experimental JIT archive;
the fixture itself needs no private test-helper macro or native trace execution.

This is not a full FNEW-suite pass. The later persistent root-snapshot fixture
also fails natively after its fixed 64 one-item MARK-close attempts. Its setup
creates 20,000 rooted tables, while `gc2_fixpoint_round` now deliberately defers
the root snapshot until older queued work is drained (except owner NEEDSCAN
handoffs). That separate setup assumption remains under investigation; no
assertion was relaxed and no production snapshot state was forced.
